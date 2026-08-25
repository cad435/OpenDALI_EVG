namespace EVG_Updater;

/// <summary>
/// IEC 62386-105 firmware update protocol implementation.
/// Drives the bootloader via DALI 32-bit forward frames sent through the gateway.
/// Mirrors the proven Python full_update.py 4-phase flow.
/// </summary>
public class DaliBootloader
{
    // IEC 62386-105 opcodes
    private const byte OP_STANDARD     = 0xFB;
    private const byte OP_BEGIN_BLOCK  = 0xCB;
    private const byte OP_BLOCK_DATA   = 0xBD;
    private const byte ADDR_FW_UPDATE  = 0xBF;

    // Commands (byte 2 of standard frame)
    private const byte CMD_START_FW_TRANSFER = 0x00;
    private const byte CMD_FINISH_FW_UPDATE  = 0x03;
    private const byte CMD_QUERY_FW_FEATURES = 0x05;
    private const byte CMD_QUERY_FW_RUNNING  = 0x07;
    private const byte CMD_QUERY_BLOCK_FAULT = 0x08;
    private const byte CMD_START_BL_UPDATE   = 0x10;  // vendor: handled by user FW, not the BL

    // Bootloader-update target (vendor extension, see dali_bl_update.c)
    public const byte BlDeviceKey = 0xB0;             // Block-0 key for "bootloader" target
    public const int  BlMaxSize   = 1920;             // CH32V003 boot area size

    // Block 0 layout
    private const int BLOCK0_SIZE                = 64;
    private const int BLOCK0_GTIN_OFFSET         = 5;
    private const int BLOCK0_GTIN_LENGTH         = 6;
    private const int BLOCK0_DEVICE_KEY_OFFSET   = 0x2B;
    private const int BLOCK0_FLETCHER_FA_OFFSET  = 0x2C;
    private const int BLOCK0_FLETCHER_FB_OFFSET  = 0x2D;

    // Block 1 layout: [size_hi][size_lo][13 zeros][firmware][2 CRC zeros]
    private const int BLOCK_HEADER_SIZE = 15;
    private const int BYTES_PER_FRAME   = 3;

    private readonly DaliGateway _gateway;
    private byte _deviceAddress;

    public event Action<string>? OnLog;
    public event Action<int, int>? OnProgress; // (current, total)

    public DaliBootloader(DaliGateway gateway)
    {
        _gateway = gateway;
    }

    /// <summary>
    /// Run the full firmware update sequence (Phases 1–4).
    /// </summary>
    public async Task<bool> UpdateFirmwareAsync(
        byte[] firmwareBytes,
        byte shortAddress,
        byte[] gtin,
        byte evgModeId,
        CancellationToken ct = default)
    {
        _deviceAddress = (byte)(shortAddress << 1); // 32-bit gear address byte

        OnLog?.Invoke($"Device addr=0x{_deviceAddress:X2}  GTIN={Convert.ToHexString(gtin)}  "
                    + $"mode=0x{evgModeId:X2}  fw={firmwareBytes.Length} bytes");

        if (!await Phase1EnterBootloaderAsync(ct)) return false;
        if (!await Phase2Block0Async(firmwareBytes, gtin, evgModeId, ct)) return false;
        if (!await Phase3FirmwareAsync(firmwareBytes, ct)) return false;
        if (!await Phase4FinishAsync(ct)) return false;

        OnLog?.Invoke("*** Firmware update sequence complete ***");
        return true;
    }

    /// <summary>
    /// Update the DALI BOOTLOADER in the BOOT area (0x1FFFF000) via the
    /// firmware's BL-update engine (dali_bl_update.c). The device is NOT
    /// rebooted — the app stays alive; the firmware stages the image in
    /// EEPROM, Fletcher-verifies it, then erases/programs/readback-verifies
    /// the boot area synchronously at FINISH.
    /// </summary>
    /// <summary>File name of the pre-built BL emergency flasher image.</summary>
    public const string EmergencyFlasherFileName = "BL-Emergency-Flasher.bin";

    /// <summary>
    /// Locates the pre-built BL emergency flasher image: next to the
    /// executable first (that is how it ships), then the dev build tree.
    /// Returns null when nothing is found.
    /// </summary>
    public static string? FindEmergencyFlasher()
    {
        var exeDir = AppContext.BaseDirectory;
        var candidates = new[]
        {
            Path.Combine(exeDir, EmergencyFlasherFileName),
            // dev tree: bin/<cfg>/<tfm>/ -> EVG_Updater -> EVG-Updater -> OpenDALI_EVG
            Path.Combine(exeDir, "..", "..", "..", "..", "..",
                         "BL-Emergency-Flasher", ".pio", "build",
                         "genericCH32V003F4P6", "firmware.bin"),
        };

        foreach (var c in candidates)
        {
            var full = Path.GetFullPath(c);
            if (File.Exists(full)) return full;
        }
        return null;
    }

    /// <summary>
    /// OBSOLETE — transfers a bootloader image over the bus for the running
    /// firmware to write into the boot area. That path is dead: writing the
    /// CH32V003 boot area from user code only works while a debug module is
    /// active, so the application firmware can never do it in the field
    /// (without a debugger the FPEC reports success and changes nothing).
    /// Use the emergency flasher firmware instead — see FindEmergencyFlasher
    /// and BL-Emergency-Flasher/README.md.
    /// </summary>
    [Obsolete("Boot-area writes need an active debug module; use the BL emergency flasher firmware.")]
    public async Task<bool> UpdateBootloaderAsync(
        byte[] blBytes,
        byte shortAddress,
        byte[] gtin,
        CancellationToken ct = default)
    {
        _deviceAddress = (byte)(shortAddress << 1);

        OnLog?.Invoke($"BL update: addr=0x{_deviceAddress:X2}  GTIN={Convert.ToHexString(gtin)}  "
                    + $"image={blBytes.Length} bytes");
        if (blBytes.Length > BlMaxSize)
        {
            OnLog?.Invoke($"  ERROR: image exceeds boot area ({blBytes.Length} > {BlMaxSize} bytes)");
            return false;
        }

        // Phase 1: arm the firmware's BL-update engine (no reboot, app keeps running)
        OnLog?.Invoke("[Phase 1] START BL UPDATE (sendTwice)...");
        var yes = await _gateway.SendQueryAsync(
            new byte[] { _deviceAddress, OP_STANDARD, CMD_START_BL_UPDATE, 0x00 },
            sendTwice: true, timeoutMs: 3000, ct: ct);
        if (yes != 0xFF)
        {
            OnLog?.Invoke($"  ERROR: no YES (got {FormatByte(yes)}) — firmware lacks BL-update support?");
            return false;
        }
        OnLog?.Invoke("  firmware armed for BL update");

        // Phases 2+3 are byte-identical to the app update, just with the
        // BL device key and the BL image as payload.
        if (!await Phase2Block0Async(blBytes, gtin, BlDeviceKey, ct)) return false;
        if (!await Phase3FirmwareAsync(blBytes, ct, interFrameDelayMs: 60)) return false;

        // Phase 4: FINISH — the firmware now erases + programs + verifies the
        // boot area synchronously (up to 3 attempts, ~0.5–5 s). An immediate
        // 0xFF = Fletcher/size fault (boot area untouched). The 8 s timeout
        // covers the whole flash window.
        OnLog?.Invoke("[Phase 4] FINISH (sendTwice; firmware flashes the BOOT area now)...");
        var res = await _gateway.SendQueryAsync(
            new byte[] { ADDR_FW_UPDATE, OP_STANDARD, CMD_FINISH_FW_UPDATE, 0x00 },
            sendTwice: true, timeoutMs: 8000, ct: ct);
        if (res == 0xFF)
        {
            OnLog?.Invoke("  ERROR: device signalled FAULT — image rejected or flash/verify failed.");
            return false;
        }

        // Definitive result: the firmware answers QUERY BLOCK FAULT for 30 s
        // after FINISH (post-FINISH result window). Silence = no fault. Then
        // prove the app is alive with a plain 16-bit QUERY STATUS.
        var fault = await _gateway.SendQueryAsync(
            new byte[] { ADDR_FW_UPDATE, OP_STANDARD, CMD_QUERY_BLOCK_FAULT, 0x00 }, ct: ct);
        if (fault == 0xFF)
        {
            OnLog?.Invoke("  ERROR: BLOCK FAULT after FINISH — boot-area flash/verify failed.");
            return false;
        }

        var status = await _gateway.SendQueryAsync(
            new byte[] { (byte)((shortAddress << 1) | 1), 0x90 }, ct: ct);   // 16-bit QUERY STATUS
        if (status == null)
        {
            OnLog?.Invoke("  ERROR: app does not answer QUERY STATUS — result unconfirmed.");
            return false;
        }

        OnLog?.Invoke($"  app alive (status 0x{status:X2}), no fault -> BOOT area flashed + readback-verified");
        OnLog?.Invoke("*** Bootloader update complete ***");
        return true;
    }

    // ─── Phase 1: Query + Start FW Transfer ───────────────────────────────
    private async Task<bool> Phase1EnterBootloaderAsync(CancellationToken ct)
    {
        OnLog?.Invoke("[Phase 1] Query FW features...");
        var features = await _gateway.SendQueryAsync(
            new byte[] { _deviceAddress, OP_STANDARD, CMD_QUERY_FW_FEATURES, 0x00 }, ct: ct);
        if (features == null)
        {
            OnLog?.Invoke("  ERROR: no answer to QUERY FW UPDATE FEATURES");
            return false;
        }
        OnLog?.Invoke($"  features = 0x{features:X2}");

        OnLog?.Invoke("[Phase 1] START FW TRANSFER (sendTwice)...");
        var yes = await _gateway.SendQueryAsync(
            new byte[] { _deviceAddress, OP_STANDARD, CMD_START_FW_TRANSFER, 0x00 },
            sendTwice: true, timeoutMs: 3000, ct: ct);
        if (yes != 0xFF)
        {
            OnLog?.Invoke($"  ERROR: device did not respond YES (got {FormatByte(yes)})");
            return false;
        }
        OnLog?.Invoke("  device entered bootloader");

        OnLog?.Invoke("[Phase 1] Wait 500ms for bootloader init...");
        await Task.Delay(500, ct);

        OnLog?.Invoke("[Phase 1] QUERY FW UPDATE RUNNING...");
        var running = await _gateway.SendQueryAsync(
            new byte[] { ADDR_FW_UPDATE, OP_STANDARD, CMD_QUERY_FW_RUNNING, 0x00 }, ct: ct);
        if (running != 0xFF)
            OnLog?.Invoke($"  WARNING: no YES (got {FormatByte(running)}) — continuing anyway");
        else
            OnLog?.Invoke("  bootloader confirmed running");

        return true;
    }

    // ─── Phase 2: Send Block 0 (validation) ───────────────────────────────
    private async Task<bool> Phase2Block0Async(byte[] firmware, byte[] gtin, byte evgModeId, CancellationToken ct)
    {
        OnLog?.Invoke("[Phase 2] BEGIN BLOCK 0...");
        await _gateway.SendDataAsync(new byte[] { OP_BEGIN_BLOCK, 0x00, 0x00, 0x00 }, ct: ct);

        // Pre-compute Fletcher-16 over the firmware payload — bootloader checks
        // this against fa/fb it accumulates while receiving Block 1, before the
        // flash commit. Mismatch -> BLOCK FAULT, commit aborted.
        byte fa = 0, fb = 0;
        foreach (var x in firmware)
        {
            fa = (byte)((fa + x) & 0xFF);
            fb = (byte)((fb + fa) & 0xFF);
        }
        OnLog?.Invoke($"[Phase 2] Fletcher-16(fw) = fa=0x{fa:X2} fb=0x{fb:X2}");

        var block0 = new byte[BLOCK0_SIZE];
        Array.Fill(block0, (byte)0xFF);
        block0[0] = 0x00;
        block0[1] = (byte)(BLOCK0_SIZE - 2); // 0x3E
        Array.Copy(gtin, 0, block0, BLOCK0_GTIN_OFFSET, Math.Min(gtin.Length, BLOCK0_GTIN_LENGTH));
        block0[BLOCK0_DEVICE_KEY_OFFSET]  = evgModeId;
        block0[BLOCK0_FLETCHER_FA_OFFSET] = fa;
        block0[BLOCK0_FLETCHER_FB_OFFSET] = fb;

        OnLog?.Invoke("[Phase 2] Send 22 BLOCK_DATA frames...");
        for (int i = 0; i < block0.Length; i += BYTES_PER_FRAME)
        {
            var chunk = new byte[]
            {
                OP_BLOCK_DATA,
                block0[i],
                (i + 1 < block0.Length) ? block0[i + 1] : (byte)0xFF,
                (i + 2 < block0.Length) ? block0[i + 2] : (byte)0xFF,
            };
            await _gateway.SendDataAsync(chunk, ct: ct);
        }

        await _gateway.DrainInflightAsync(ct: ct);
        await Task.Delay(50, ct);

        OnLog?.Invoke("[Phase 2] QUERY BLOCK FAULT...");
        var fault = await _gateway.SendQueryAsync(
            new byte[] { ADDR_FW_UPDATE, OP_STANDARD, CMD_QUERY_BLOCK_FAULT, 0x00 }, ct: ct);
        if (fault == 0xFF)
        {
            OnLog?.Invoke("  ERROR: BLOCK FAULT detected (GTIN/mode mismatch?)");
            return false;
        }
        OnLog?.Invoke("  no fault -> Block 0 validated");
        return true;
    }

    // ─── Phase 3: Send Block 1 = firmware ─────────────────────────────────
    private async Task<bool> Phase3FirmwareAsync(byte[] firmware, CancellationToken ct,
        int interFrameDelayMs = 0)
    {
        OnLog?.Invoke($"[Phase 3] BEGIN BLOCK 1 ({firmware.Length} bytes)...");
        await _gateway.SendDataAsync(new byte[] { OP_BEGIN_BLOCK, 0x00, 0x00, 0x01 }, ct: ct);

        var block1 = new byte[BLOCK_HEADER_SIZE + firmware.Length + 2];
        block1[0] = (byte)((firmware.Length >> 8) & 0xFF);
        block1[1] = (byte)(firmware.Length & 0xFF);
        Array.Copy(firmware, 0, block1, BLOCK_HEADER_SIZE, firmware.Length);
        // Last 2 CRC bytes left as 0 (not verified by bootloader)

        int totalFrames = (block1.Length + BYTES_PER_FRAME - 1) / BYTES_PER_FRAME;
        OnLog?.Invoke($"[Phase 3] Send {totalFrames} BLOCK_DATA frames...");

        // No mid-transfer or end-of-Phase-3 QUERY_BLOCK_FAULT polls here:
        // BEGIN BLOCK 1 clears the BL's blockFault flag, and during Block 1
        // reception nothing sets it again — the Fletcher-16 check runs only
        // inside the FINISH handler. The BL also stays silent on a healthy
        // poll (no answer when flag is clear), so a "null" reply can't even
        // distinguish "alive and OK" from "dead bus". Payload integrity is
        // confirmed or rejected in Phase 4 via the FINISH response.
        int sent = 0;
        for (int i = 0; i < block1.Length; i += BYTES_PER_FRAME)
        {
            var chunk = new byte[]
            {
                OP_BLOCK_DATA,
                block1[i],
                (i + 1 < block1.Length) ? block1[i + 1] : (byte)0xFF,
                (i + 2 < block1.Length) ? block1[i + 2] : (byte)0xFF,
            };
            await _gateway.SendDataAsync(chunk, ct: ct);
            sent++;

            // BL-update path only: the receiver is the RUNNING firmware, whose
            // main loop blocks ~11 ms on each 64-byte EEPROM staging flush.
            // 32-bit frames arrive back-to-back (~27 ms frame, ~7 ms gap) and
            // can overrun the single-frame RX buffer -> lost/mixed frames
            // (observed: Fletcher fault at FINISH; bus capture proved every
            // frame byte-perfect on the wire).
            // The delay must EXCEED the ~34 ms bus frame period — anything
            // shorter is absorbed by the gateway's in-flight queue and the
            // bus stays back-to-back. Windows quantizes Task.Delay to
            // ~15.6 ms ticks, so 40 ms -> ~47 ms real loop period -> ~20 ms
            // bus gap per frame, and no queue ever builds (no initial burst).
            // The app-update path (real bootloader, tight RX loop) stays at
            // full speed with interFrameDelayMs = 0.
            if (interFrameDelayMs > 0)
                await Task.Delay(interFrameDelayMs, ct);

            if (sent % 50 == 0 || sent == totalFrames)
                OnProgress?.Invoke(sent, totalFrames);
        }

        await _gateway.DrainInflightAsync(ct: ct);
        await Task.Delay(100, ct);
        OnLog?.Invoke($"[Phase 3] {totalFrames} BLOCK_DATA frames sent (payload Fletcher verified at FINISH).");
        return true;
    }

    // ─── Phase 4: FINISH FW UPDATE ────────────────────────────────────────
    // Bootloader semantics (dali_bootloader.c CMD_FINISH):
    //   no fault → silent commit + jump to user FW  → SendQueryAsync returns null (timeout)
    //   fault    → tx_byte(0xFF), stay in BL        → SendQueryAsync returns 0xFF
    // So 0xFF here is an explicit FAIL signal, NOT a success ACK.
    private async Task<bool> Phase4FinishAsync(CancellationToken ct)
    {
        OnLog?.Invoke("[Phase 4] FINISH FW UPDATE (sendTwice; EVG reboots on success)...");
        var res = await _gateway.SendQueryAsync(
            new byte[] { ADDR_FW_UPDATE, OP_STANDARD, CMD_FINISH_FW_UPDATE, 0x00 },
            sendTwice: true, timeoutMs: 3000, ct: ct);
        OnLog?.Invoke($"  finish answer: {FormatByte(res)}");

        if (res == 0xFF)
        {
            OnLog?.Invoke("  ERROR: bootloader signalled FAULT at FINISH — Fletcher-16 mismatch.");
            OnLog?.Invoke("  Payload corrupted on the DALI bus during Block 1 transfer.");
            OnLog?.Invoke("  Flash was NOT modified; EVG is still in bootloader, re-run flash to retry.");
            return false;
        }
        // null (timeout) is the expected success path — EVG jumped into user FW silently.
        return true;
    }

    private static string FormatByte(byte? b) => b == null ? "<null>" : $"0x{b:X2}";
}
