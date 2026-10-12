namespace EVG_Updater;

/// <summary>
/// One discovered DALI gear on the bus, as seen by the scanner.
/// Fields are null when the corresponding query failed / returned nothing.
/// </summary>
public sealed record ScannedDevice
{
    public byte ShortAddress { get; init; }
    public byte[]? Gtin { get; init; }          // bank0 0x03..0x08, 6 bytes
    public byte? FwMajor { get; init; }         // bank0 0x09
    public byte? FwMinor { get; init; }         // bank0 0x0A
    public byte[]? Serial { get; init; }        // bank0 0x0B..0x12, 8 bytes
    public byte? HwMajor { get; init; }         // bank0 0x13
    public byte? HwMinor { get; init; }         // bank0 0x14
    public byte? DeviceType { get; init; }      // QUERY_DEVICE_TYPE (cmd 153)
    public uint? RandomAddress { get; init; }   // QUERY_RANDOM_H/M/L combined 24-bit
    public BuildInfo? Build { get; init; }      // bank2 0x03..0x0B (null: older FW / foreign gear)

    public string GtinHex => Gtin == null ? "—" : Convert.ToHexString(Gtin);
    public string BuildLabel => Build?.ToString() ?? "—";
    public string FwVersion => (FwMajor, FwMinor) switch
    {
        ({ } a, { } b) => $"{a}.{b}",
        _              => "—"
    };
    public string HwVersion => (HwMajor, HwMinor) switch
    {
        ({ } a, { } b) => $"{a}.{b}",
        _              => "—"
    };
    public string RandomHex => RandomAddress is { } r ? $"0x{r:X6}" : "—";
    public string DeviceTypeLabel => DeviceType switch
    {
        null  => "—",
        0xFF  => "multi",      // IEC 62386-102: 0xFF = multiple device types, see QUERY NEXT DEVICE TYPE
        var x => $"DT{x}"      // 0=fluorescent, 6=LED, 8=color control, ...
    };

    /// <summary>
    /// EVG mode label decoded from the serial field, which our firmware
    /// fills with ASCII mode name by default ("RGBW", "ONOFF", "CCT", ...).
    /// Returns "?" when serial is missing or doesn't look like printable ASCII.
    /// </summary>
    public string ModeLabel
    {
        get
        {
            if (Serial == null) return "?";
            int len = 0;
            while (len < Serial.Length && Serial[len] != 0) len++;
            if (len == 0) return "?";
            for (int i = 0; i < len; i++)
                if (Serial[i] < 0x20 || Serial[i] > 0x7E) return "?";
            return System.Text.Encoding.ASCII.GetString(Serial, 0, len);
        }
    }

    /// <summary>True if GTIN matches the user-configured "ours" value.</summary>
    public bool IsOurs(byte[] expectedGtin)
    {
        if (Gtin == null || expectedGtin.Length != Gtin.Length) return false;
        for (int i = 0; i < Gtin.Length; i++)
            if (Gtin[i] != expectedGtin[i]) return false;
        return true;
    }
}

/// <summary>
/// Scans the DALI bus by walking shorts 0..63 with QUERY_GEAR_PRESENT, then
/// pulling bank-0 identity (GTIN, FW/HW versions, EVG mode via serial field),
/// the bank-2 build identity and the 24-bit random address for each present
/// gear. Read-only; never
/// changes addressing or any persistent state on the bus.
/// </summary>
public class DaliBusScanner
{
    // IEC 62386-102 cmd numbers (resolved via Firmware/src/dali/dali_physical.h)
    private const byte CMD_QUERY_GEAR_PRESENT = 145;
    private const byte CMD_QUERY_DEVICE_TYPE  = 153;
    private const byte CMD_QUERY_RANDOM_H     = 194;
    private const byte CMD_QUERY_RANDOM_M     = 195;
    private const byte CMD_QUERY_RANDOM_L     = 196;
    private const byte CMD_READ_MEMORY        = 197;

    // Special-command address bytes (target ALL gear)
    private const byte SPECIAL_DTR0 = 0xA3;
    private const byte SPECIAL_DTR1 = 0xC3;

    // Bank 0 read window: 0x03..0x14 inclusive = 18 bytes
    //   0x03..0x08 GTIN | 0x09..0x0A FW | 0x0B..0x12 Serial | 0x13..0x14 HW
    private const int BANK0_START = 0x03;
    private const int BANK0_BYTES = 18;

    // Diagnostic: extra delay between consecutive READ_MEMORY queries.
    // Set to 0 for "as fast as the gateway can send" (the historical default).
    // Higher values give the gear more recovery time between forward frames —
    // use to diagnose suspected gear-side back-to-back reception problems.
    private const int READ_INTER_FRAME_DELAY_MS = 0;

    private readonly DaliGateway _gateway;

    public event Action<string>? OnLog;
    // Fires twice per scan: first (current,total)=(s+1,64) during the 0..63
    // probe phase, then resets to (0, deviceCount) and ticks (i+1, deviceCount)
    // as each present gear's bank 0 + random address is read.
    public event Action<int, int>? OnProgress;

    public DaliBusScanner(DaliGateway gateway) { _gateway = gateway; }

    public async Task<List<ScannedDevice>> ScanAsync(CancellationToken ct = default)
    {
        OnLog?.Invoke("[Scan] Probing shorts 0..63 with QUERY_GEAR_PRESENT...");
        var presentShorts = new List<byte>();
        for (byte s = 0; s < 64; s++)
        {
            ct.ThrowIfCancellationRequested();
            byte addr = (byte)((s << 1) | 1);
            var resp = await _gateway.SendQueryAsync(
                new byte[] { addr, CMD_QUERY_GEAR_PRESENT }, timeoutMs: 200, ct: ct);
            if (resp == 0xFF)
            {
                presentShorts.Add(s);
                OnLog?.Invoke($"[Scan]   found gear at short {s}");
            }
            OnProgress?.Invoke(s + 1, 64);
        }
        OnLog?.Invoke($"[Scan] probe done — {presentShorts.Count} gear present: "
                   + string.Join(", ", presentShorts));

        if (presentShorts.Count == 0) return new List<ScannedDevice>();

        // Probe filled the progress bar to 100% — reset it and now drive it
        // again, one tick per device, while pulling bank 0 + random address.
        var devices = new List<ScannedDevice>();
        OnProgress?.Invoke(0, presentShorts.Count);
        for (int i = 0; i < presentShorts.Count; i++)
        {
            ct.ThrowIfCancellationRequested();
            devices.Add(await ScanGearAsync(presentShorts[i], ct));
            OnProgress?.Invoke(i + 1, presentShorts.Count);
        }
        return devices;
    }

    private async Task<ScannedDevice> ScanGearAsync(byte shortAddr, CancellationToken ct)
    {
        OnLog?.Invoke($"[Scan]   short={shortAddr}: reading bank 0 + bank 2 + random address...");
        byte gearAddr = (byte)((shortAddr << 1) | 1);

        // DTR setup per gear — special commands target ALL gear, and the
        // bank-2 read below moves DTR1/DTR0 for everybody.
        // IEC 62386-102 §9.8: DTR1 = bank, DTR0 = offset (auto-incremented).
        await SelectMemoryAsync(0, BANK0_START, ct);

        // Read up to BANK0_BYTES from bank 0 starting at offset 0x03.
        // If the very first read times out, the gear doesn't implement bank 0
        // (or uses non-standard DTR ordering) — skip the remaining 17 reads
        // to save ~7 s of waiting.
        var bank0 = new byte?[BANK0_BYTES];
        for (int i = 0; i < BANK0_BYTES; i++)
        {
            if (i > 0 && READ_INTER_FRAME_DELAY_MS > 0)
                await Task.Delay(READ_INTER_FRAME_DELAY_MS, ct);
            bank0[i] = await _gateway.SendQueryAsync(
                new byte[] { gearAddr, CMD_READ_MEMORY }, timeoutMs: 400, ct: ct);
            if (i == 0 && bank0[0] == null)
            {
                OnLog?.Invoke("[Scan]     first READ_MEMORY timed out — skipping bank0 for this gear");
                break;
            }
        }

        static byte[]? Slice(byte?[] src, int off, int len)
        {
            var buf = new byte[len];
            for (int i = 0; i < len; i++)
            {
                if (src[off + i] is not { } b) return null;
                buf[i] = b;
            }
            return buf;
        }

        var gtin   = Slice(bank0, 0x03 - BANK0_START, 6);
        var serial = Slice(bank0, 0x0B - BANK0_START, 8);

        var build = await ReadBuildInfoAsync(shortAddr, ct);

        var deviceType = await _gateway.SendQueryAsync(
            new byte[] { gearAddr, CMD_QUERY_DEVICE_TYPE }, timeoutMs: 300, ct: ct);

        var rh = await _gateway.SendQueryAsync(
            new byte[] { gearAddr, CMD_QUERY_RANDOM_H }, timeoutMs: 300, ct: ct);
        var rm = await _gateway.SendQueryAsync(
            new byte[] { gearAddr, CMD_QUERY_RANDOM_M }, timeoutMs: 300, ct: ct);
        var rl = await _gateway.SendQueryAsync(
            new byte[] { gearAddr, CMD_QUERY_RANDOM_L }, timeoutMs: 300, ct: ct);

        uint? random = (rh, rm, rl) switch
        {
            ({ } h, { } m, { } l) => (uint)((h << 16) | (m << 8) | l),
            _                     => null
        };

        return new ScannedDevice
        {
            ShortAddress  = shortAddr,
            Gtin          = gtin,
            FwMajor       = bank0[0x09 - BANK0_START],
            FwMinor       = bank0[0x0A - BANK0_START],
            Serial        = serial,
            HwMajor       = bank0[0x13 - BANK0_START],
            HwMinor       = bank0[0x14 - BANK0_START],
            DeviceType    = deviceType,
            RandomAddress = random,
            Build         = build
        };
    }

    /// <summary>
    /// Read the firmware build identity from memory bank 2. Returns null if
    /// the gear does not answer — firmware older than bank 2, foreign gear,
    /// or a device still sitting in the bootloader.
    /// </summary>
    public async Task<BuildInfo?> ReadBuildInfoAsync(byte shortAddr, CancellationToken ct = default)
    {
        byte gearAddr = (byte)((shortAddr << 1) | 1);
        await SelectMemoryAsync(BuildInfo.Bank, BuildInfo.FirstLocation, ct);

        var buf = new byte[BuildInfo.Length];
        for (int i = 0; i < buf.Length; i++)
        {
            var b = await _gateway.SendQueryAsync(
                new byte[] { gearAddr, CMD_READ_MEMORY }, timeoutMs: 400, ct: ct);
            if (b is not { } v) return null;
            buf[i] = v;
        }
        return BuildInfo.FromBankBytes(buf);
    }

    private async Task SelectMemoryAsync(byte bank, byte location, CancellationToken ct)
    {
        await _gateway.SendDataAsync(new byte[] { SPECIAL_DTR1, bank }, ct: ct);
        await _gateway.SendDataAsync(new byte[] { SPECIAL_DTR0, location }, ct: ct);
        await _gateway.DrainInflightAsync(ct: ct);
    }
}
