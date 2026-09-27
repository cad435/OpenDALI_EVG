using System.Text.RegularExpressions;
using EvgFlasher.Models;

namespace EvgFlasher.Services;

/// <summary>
/// The provisioning sequence, ported from flash_blank_evg.ps1:
///   1. Bootloader          -> 0x1FFFF000 (boot area, 1920 B fixed)
///   2. configurebootloader -> 0x08000000 (writes the option bytes once)
///   3. Firmware            -> 0x08000000 (overwrites configurebootloader)
///   4. Verify option bytes (RDPR, USER complement, FLASH_OBR)
/// </summary>
public sealed partial class FlashService(ToolPaths paths)
{
    public sealed record StepResult(int Number, string Title, bool Ok, string? Error = null);

    /// <summary>True while a CH32V003 is attached to the WCH-Link.</summary>
    public async Task<bool> IsChipPresentAsync(CancellationToken ct)
    {
        try
        {
            var (_, output) = await ProcessRunner.CaptureAsync(paths.Wlink, ["status"], ct)
                                                 .ConfigureAwait(false);
            return AttachedChipRegex().IsMatch(output);
        }
        catch (OperationCanceledException) { throw; }
        catch { return false; }
    }

    /// <summary>Picks the bootloader image, preferring the PlatformIO build.</summary>
    public (string Path, string Source)? ResolveBootloader()
    {
        if (File.Exists(paths.BootloaderPioBin)) return (paths.BootloaderPioBin, "PIO build");
        if (File.Exists(paths.BootloaderLegacy)) return (paths.BootloaderLegacy, "build.bat legacy");
        return null;
    }

    public async Task<IReadOnlyList<StepResult>> RunAsync(
        EvgEnvironment environment,
        int? ledCount,
        Action<string> log,
        Action<int> onStepStarted,
        CancellationToken ct)
    {
        var results = new List<StepResult>();
        var bootloader = ResolveBootloader();
        if (bootloader is null)
        {
            results.Add(new StepResult(1, "Bootloader", false,
                $"No bootloader image found ({paths.BootloaderPioBin} or {paths.BootloaderLegacy})."));
            return results;
        }

        // ---- 1/4 bootloader -------------------------------------------------
        onStepStarted(1);
        log($"--- Step 1/4: bootloader ({bootloader.Value.Source}) -> 0x1FFFF000 ---");
        var code = await ProcessRunner.RunAsync(paths.Wlink,
            ["flash", "--address", "0x1FFFF000", bootloader.Value.Path],
            null, null, log, ct).ConfigureAwait(false);
        results.Add(new StepResult(1, "Bootloader", code == 0,
            code == 0 ? null : $"wlink exited with {code}"));
        if (code != 0) return results;

        // ---- 2/4 configurebootloader ---------------------------------------
        onStepStarted(2);
        log("--- Step 2/4: configurebootloader -> 0x08000000 ---");
        code = await ProcessRunner.RunAsync(paths.Wlink,
            ["flash", paths.ConfigureBootBin], null, null, log, ct).ConfigureAwait(false);
        results.Add(new StepResult(2, "configurebootloader", code == 0,
            code == 0 ? null : $"wlink exited with {code}"));
        if (code != 0) return results;
        await Task.Delay(500, ct).ConfigureAwait(false);

        // ---- 3/4 firmware ---------------------------------------------------
        onStepStarted(3);
        var extra = new Dictionary<string, string>();
        if (environment.IsDigitalStrip && ledCount is > 0)
        {
            // PLATFORMIO_BUILD_FLAGS is appended to the environment's build_flags,
            // so the strip length overrides hardware.h without touching the .ini.
            extra["PLATFORMIO_BUILD_FLAGS"] = $"-DWS2812_NUM_LEDS={ledCount}";
            log($"--- Step 3/4: firmware {environment.Name} ({ledCount} LEDs) -> 0x08000000 ---");
        }
        else
        {
            log($"--- Step 3/4: firmware {environment.Name} -> 0x08000000 ---");
        }
        code = await ProcessRunner.RunAsync(paths.PlatformIo,
            ["run", "-e", environment.Name, "-t", "upload"],
            paths.FirmwareDir, extra, log, ct).ConfigureAwait(false);
        results.Add(new StepResult(3, $"Firmware {environment.Name}", code == 0,
            code == 0 ? null : $"pio exited with {code}"));
        if (code != 0) return results;

        // ---- 4/4 option bytes ----------------------------------------------
        onStepStarted(4);
        log("--- Step 4/4: verify option bytes ---");
        var (ok, detail) = await VerifyOptionBytesAsync(log, ct).ConfigureAwait(false);
        results.Add(new StepResult(4, "Option bytes", ok, ok ? null : detail));
        return results;
    }

    private async Task<(bool Ok, string Detail)> VerifyOptionBytesAsync(Action<string> log, CancellationToken ct)
    {
        var (_, dump) = await ProcessRunner.CaptureAsync(paths.Wlink,
            ["dump", "0x1FFFF800", "4"], ct).ConfigureAwait(false);
        var (_, obrd) = await ProcessRunner.CaptureAsync(paths.Wlink,
            ["dump", "0x4002201C", "4"], ct).ConfigureAwait(false);

        var ob  = ParseDump(dump, "1ffff800", 4);
        var obr = ParseDump(obrd, "4002201c", 4);
        if (ob is null)  return (false, "Could not parse the option byte dump");
        if (obr is null) return (false, "Could not parse the FLASH_OBR dump");

        int rdpr = ob[0], rdprN = ob[1], user = ob[2], userN = ob[3];
        long obrVal = obr[0] | (obr[1] << 8) | (obr[2] << 16) | ((long)obr[3] << 24);
        int opterr = (int)(obrVal & 1), rdprt = (int)((obrVal >> 1) & 1), startMode = (int)((obrVal >> 14) & 1);

        log($"  RDPR=0x{rdpr:X2}  ~RDPR=0x{rdprN:X2}  USER=0x{user:X2}  ~USER=0x{userN:X2}  " +
            $"OBR=0x{obrVal:X8}  STARTMODE={startMode}");

        var ok = rdpr == 0xA5 && rdpr + rdprN == 0xFF
              && user == 0xEF && user + userN == 0xFF
              && opterr == 0 && rdprt == 0 && startMode == 1;
        return (ok, ok ? "" : "Option bytes deviate from the expected values");
    }

    /// <summary>
    /// Pulls <paramref name="count"/> bytes out of a wlink hex dump. Matches the
    /// data line by its leading address so that INFO lines merely mentioning the
    /// address are skipped, and strips wlink's ANSI colouring first.
    /// </summary>
    internal static int[]? ParseDump(string text, string address, int count)
    {
        var clean = AnsiRegex().Replace(text, "");
        var wanted = new Regex(@"^\s*" + Regex.Escape(address) + @"\s*:", RegexOptions.IgnoreCase);

        foreach (var line in clean.Split('\n'))
        {
            if (!wanted.IsMatch(line)) continue;
            var idx = line.IndexOf(':');
            if (idx < 0) continue;

            var hex = HexByteRegex().Matches(line[(idx + 1)..]);
            if (hex.Count < count) continue;

            var bytes = new int[count];
            for (var i = 0; i < count; i++) bytes[i] = Convert.ToInt32(hex[i].Value, 16);
            return bytes;
        }
        return null;
    }

    [GeneratedRegex(@"Attached chip:\s*CH32V003")]
    private static partial Regex AttachedChipRegex();

    [GeneratedRegex(@"\x1b\[[0-9;]*m")]
    private static partial Regex AnsiRegex();

    [GeneratedRegex(@"(?<![0-9a-fA-F])[0-9a-fA-F]{2}(?![0-9a-fA-F])")]
    private static partial Regex HexByteRegex();
}
