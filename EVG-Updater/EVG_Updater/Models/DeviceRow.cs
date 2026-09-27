namespace EVG_Updater.Models;

/// <summary>A scan result plus the "is this one of ours" verdict for the grid.</summary>
public sealed class DeviceRow(ScannedDevice device, bool updatable)
{
    public ScannedDevice Device { get; } = device;

    public byte   ShortAddress    => Device.ShortAddress;
    public string RandomHex       => Device.RandomHex;
    public string GtinHex         => Device.GtinHex;
    public string ModeLabel       => Device.ModeLabel;
    public string DeviceTypeLabel => Device.DeviceTypeLabel;
    public string FwVersion       => Device.FwVersion;
    public string HwVersion       => Device.HwVersion;
    public string Updatable       { get; } = updatable ? "\u2713" : "\u2717";
    public bool   IsUpdatable     { get; } = updatable;
}
