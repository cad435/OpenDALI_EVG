using EvgFlasher.Models;

namespace EvgFlasher.Services;

/// <summary>
/// Minimal reader for platformio.ini — enough to list the build environments
/// and tell which of them drive an addressable strip.
///
/// The environment list is deliberately NOT hardcoded: adding an [env:...]
/// section to platformio.ini is all it takes to make it appear in the UI.
/// </summary>
public static class PlatformIoConfig
{
    /// <summary>Build flags that mark an environment as an addressable-strip mode.</summary>
    private static readonly string[] DigitalStripFlags =
    [
        "EVG_MODE_WS2812", "EVG_MODE_SK6812_RGB", "EVG_MODE_SK6812_RGBW"
    ];

    public static IReadOnlyList<EvgEnvironment> Read(string iniPath)
    {
        var sections = ParseSections(iniPath);

        var defaults = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (sections.TryGetValue("platformio", out var pio) &&
            pio.TryGetValue("default_envs", out var envs))
        {
            foreach (var e in envs.Split([',', '\n'], StringSplitOptions.RemoveEmptyEntries
                                                     | StringSplitOptions.TrimEntries))
                defaults.Add(e);
        }

        var result = new List<EvgEnvironment>();
        foreach (var (section, keys) in sections)
        {
            if (!section.StartsWith("env:", StringComparison.OrdinalIgnoreCase)) continue;
            var name = section[4..].Trim();
            if (name.Length == 0) continue;

            keys.TryGetValue("build_flags", out var flags);
            var digital = flags is not null &&
                          DigitalStripFlags.Any(f => flags.Contains(f, StringComparison.Ordinal));

            result.Add(new EvgEnvironment(name, digital, defaults.Contains(name)));
        }
        return result;
    }

    /// <summary>
    /// Section -> key -> value. Handles full-line ';'/'#' comments (also when
    /// indented inside a multi-line value, which is how PlatformIO's own parser
    /// treats them) and indented continuation lines.
    /// </summary>
    private static Dictionary<string, Dictionary<string, string>> ParseSections(string path)
    {
        var sections = new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
        Dictionary<string, string>? current = null;
        string? currentKey = null;

        foreach (var raw in File.ReadLines(path))
        {
            var trimmed = raw.Trim();
            if (trimmed.Length == 0) { currentKey = null; continue; }
            if (trimmed.StartsWith(';') || trimmed.StartsWith('#')) continue;

            if (trimmed.StartsWith('[') && trimmed.EndsWith(']'))
            {
                var name = trimmed[1..^1].Trim();
                current = sections.TryGetValue(name, out var existing)
                    ? existing
                    : sections[name] = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                currentKey = null;
                continue;
            }

            if (current is null) continue;

            var eq = trimmed.IndexOf('=');
            var indented = char.IsWhiteSpace(raw[0]);

            if (!indented && eq > 0)
            {
                currentKey = trimmed[..eq].Trim();
                current[currentKey] = trimmed[(eq + 1)..].Trim();
            }
            else if (currentKey is not null)
            {
                current[currentKey] = (current[currentKey] + "\n" + trimmed).Trim();
            }
        }
        return sections;
    }
}
