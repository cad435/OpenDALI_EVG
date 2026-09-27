namespace EvgFlasher.Services;

/// <summary>Locations of the external tools and of the OpenDALI_EVG working copy.</summary>
public sealed class ToolPaths
{
    public required string Wlink { get; init; }
    public required string PlatformIo { get; init; }
    public required string RepoRoot { get; init; }

    public string FirmwareDir       => Path.Combine(RepoRoot, "Firmware");
    public string BootloaderDir     => Path.Combine(RepoRoot, "Bootloader");
    public string PlatformIoIni     => Path.Combine(FirmwareDir, "platformio.ini");
    public string ConfigureBootBin  => Path.Combine(BootloaderDir, "configurebootloader.bin");
    public string BootloaderPioBin  => Path.Combine(BootloaderDir, ".pio", "build", "dali_bootloader", "firmware.bin");
    public string BootloaderLegacy  => Path.Combine(BootloaderDir, "dali_bootloader.bin");

    public string FirmwareBin(string environment) =>
        Path.Combine(FirmwareDir, ".pio", "build", environment, "firmware.bin");

    public static ToolPaths Discover()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return new ToolPaths
        {
            Wlink      = Path.Combine(home, ".platformio", "packages", "tool-wlink", "wlink.exe"),
            PlatformIo = Path.Combine(home, ".platformio", "penv", "Scripts", "platformio.exe"),
            RepoRoot   = FindRepoRoot(),
        };
    }

    /// <summary>Walk up from the executable until a folder holds both Firmware/ and Bootloader/.</summary>
    private static string FindRepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (Directory.Exists(Path.Combine(dir.FullName, "Firmware")) &&
                Directory.Exists(Path.Combine(dir.FullName, "Bootloader")))
                return dir.FullName;
            dir = dir.Parent;
        }
        throw new DirectoryNotFoundException(
            "Could not locate the OpenDALI_EVG root (no parent folder holds both Firmware/ and Bootloader/).");
    }

    /// <summary>Every path that must exist before the first chip is plugged in.</summary>
    public IEnumerable<string> MissingPrerequisites()
    {
        if (!File.Exists(Wlink))            yield return Wlink;
        if (!File.Exists(PlatformIo))       yield return PlatformIo;
        if (!File.Exists(PlatformIoIni))    yield return PlatformIoIni;
        if (!File.Exists(ConfigureBootBin)) yield return ConfigureBootBin;
    }
}
