namespace EVG_Updater;

/// <summary>
/// Firmware build identity: build time and git commit, stamped by
/// Firmware/scripts/build_info.py. The device serves it in DALI memory
/// bank 2 (Firmware/src/dali/nvm/dali_bank2.c); firmware.bin carries the
/// same bytes behind the magic "EVGBUILD". Comparing the two tells which
/// build a device actually runs after an update.
/// </summary>
public sealed record BuildInfo(uint UnixTime, uint GitHash, bool Dirty)
{
    // Bank 2 layout: 0x00 last addr | 0x01 indicator | 0x02 lock |
    // 0x03..0x06 build time (MSB first) | 0x07..0x0A git | 0x0B flags
    public const byte Bank          = 2;
    public const byte FirstLocation = 0x03;
    public const int  Length        = 9;    // 0x03..0x0B

    private static readonly byte[] Magic = "EVGBUILD"u8.ToArray();

    /// <summary>Decode the 9 bytes read from bank 2 locations 0x03..0x0B.</summary>
    public static BuildInfo FromBankBytes(ReadOnlySpan<byte> b) => new(
        (uint)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]),
        (uint)(b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7]),
        (b[8] & 0x01) != 0);

    /// <summary>
    /// Find the build record in a firmware image. Returns null for images
    /// built before bank 2 existed.
    /// </summary>
    public static BuildInfo? FromFirmware(byte[] image)
    {
        int i = image.AsSpan().IndexOf(Magic);
        int payload = i + Magic.Length + FirstLocation;     // record = magic + bank image
        if (i < 0 || payload + Length > image.Length) return null;
        return FromBankBytes(image.AsSpan(payload, Length));
    }

    public override string ToString()
    {
        var local = DateTimeOffset.FromUnixTimeSeconds(UnixTime).ToLocalTime();
        return $"{local:yyyy-MM-dd HH:mm:ss} {GitHash:x8}{(Dirty ? "+" : "")}";
    }
}
