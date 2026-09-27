namespace EvgFlasher.Models;

/// <summary>One [env:...] section of Firmware/platformio.ini.</summary>
/// <param name="Name">Environment name, e.g. "EVG_WS2812B".</param>
/// <param name="IsDigitalStrip">True for the SPI+DMA strip modes, which take a LED count.</param>
/// <param name="IsDefault">True if listed in [platformio] default_envs.</param>
public sealed record EvgEnvironment(string Name, bool IsDigitalStrip, bool IsDefault)
{
    /// <summary>Label for the dropdown.</summary>
    public string Display => IsDefault ? $"{Name}  (default)" : Name;

    public override string ToString() => Display;
}
