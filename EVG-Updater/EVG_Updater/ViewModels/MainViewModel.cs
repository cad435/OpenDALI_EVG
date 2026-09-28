using System.Collections.ObjectModel;
using System.Text;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using EVG_Updater.Models;

namespace EVG_Updater.ViewModels;

public partial class MainViewModel : ViewModelBase
{
    private const int MaxFirmwareBytes = 32640;

    /// <summary>Mode name → EVG_MODE_ID, mirrors Firmware/src/config/hardware.h.</summary>
    private static readonly Dictionary<string, byte> ModeNameToId = new(StringComparer.OrdinalIgnoreCase)
    {
        ["ONOFF"]    = 0x01,
        ["SINGLE"]   = 0x02,
        ["CCT"]      = 0x03,
        ["RGB"]      = 0x04,
        ["RGBW"]     = 0x05,
        ["WS2812"]   = 0x06,
        ["SK68RGB"]  = 0x07,
        ["SK68RGBW"] = 0x08,
    };

    private readonly StringBuilder _log = new();
    private DaliGateway? _gateway;
    private DaliBootloader? _bootloader;
    private DaliBusScanner? _scanner;
    private CancellationTokenSource? _updateCts;

    /// <summary>Set by the view — the VM has no business knowing about windows.</summary>
    public Func<Task<string?>>? PickFirmwareFile { get; set; }

    // ── Connection ─────────────────────────────────────────────────────────

    [ObservableProperty] public partial string GatewayIp { get; set; } = "192.168.178.131";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ConnectButtonText))]
    [NotifyCanExecuteChangedFor(nameof(StartUpdateCommand), nameof(UpdateAllCommand), nameof(RescanCommand))]
    public partial bool IsConnected { get; set; }

    public string ConnectButtonText => IsConnected ? "Disconnect" : "Connect";

    [RelayCommand]
    private async Task ToggleConnectAsync()
    {
        if (_gateway?.IsConnected == true)
        {
            await _gateway.DisconnectAsync();
            _gateway.Dispose();
            _gateway = null;
            IsConnected = false;
            Devices.Clear();
            return;
        }

        try
        {
            _gateway = new DaliGateway();
            _gateway.OnLog += Log;
            _gateway.Disconnected += HandleUnexpectedDisconnect;
            await _gateway.ConnectAsync(GatewayIp);
            IsConnected = true;
            await RescanAsync();
        }
        catch (Exception ex)
        {
            Log($"Connection failed: {ex.Message}");
            _gateway?.Dispose();
            _gateway = null;
            IsConnected = false;
        }
    }

    /// <summary>
    /// The reader noticed the socket dropped. Deliberately does NOT cancel
    /// _updateCts — pending bootloader calls already wake up through the
    /// reader's semaphore release and throw a more accurate "Not connected".
    /// </summary>
    private void HandleUnexpectedDisconnect(string reason) => OnUi(() =>
    {
        if (_gateway is not null)
        {
            try { _gateway.Dispose(); } catch { /* already gone */ }
            _gateway = null;
        }
        IsConnected = false;
        IsBusy = false;
        Log($"Connection lost: {reason}");
    });

    // ── Device grid ────────────────────────────────────────────────────────

    public ObservableCollection<DeviceRow> Devices { get; } = [];

    [ObservableProperty] public partial string OursGtin { get; set; } = "3452334E0CAD";

    [ObservableProperty]
    public partial DeviceRow? SelectedDevice { get; set; }

    /// <summary>Picking a row fills in the update fields — the old grid double-click.</summary>
    partial void OnSelectedDeviceChanged(DeviceRow? value)
    {
        if (value is null) return;
        ShortAddress = value.ShortAddress;
        if (value.GtinHex != "—") Gtin = value.GtinHex;
        if (ModeNameToId.TryGetValue(value.ModeLabel, out var id))
        {
            EvgMode = id;
            Log($"[Select] short={value.ShortAddress} mode={value.ModeLabel} id={id}");
        }
        else
        {
            Log($"[Select] short={value.ShortAddress} (mode '{value.ModeLabel}' unknown - EVG Mode left as is)");
        }
    }

    private bool CanUseBus => IsConnected && !IsBusy;

    [RelayCommand(CanExecute = nameof(CanUseBus))]
    private async Task RescanAsync()
    {
        if (_gateway is null || !_gateway.IsConnected) return;

        IsBusy = true;
        Devices.Clear();
        Progress = 0;
        try
        {
            _scanner = new DaliBusScanner(_gateway);
            _scanner.OnLog += Log;
            _scanner.OnProgress += ReportProgress;

            var ours = ParseGtin(OursGtin);
            if (ours is null)
            {
                Log("WARNING: Ours-GTIN invalid (need 6 bytes hex); using empty match.");
                ours = new byte[6];
            }

            var devices = await _scanner.ScanAsync();
            OnUi(() =>
            {
                foreach (var d in devices) Devices.Add(new DeviceRow(d, d.IsOurs(ours)));
            });
            Log($"[Scan] done - {devices.Count} gear listed.");
            Progress = 100;
        }
        catch (Exception ex)
        {
            Log($"Scan failed: {ex.Message}");
            Progress = 0;
        }
        finally
        {
            IsBusy = false;
        }
    }

    // ── Update ─────────────────────────────────────────────────────────────

    [ObservableProperty] public partial string FirmwarePath { get; set; } = "";
    [ObservableProperty] public partial decimal ShortAddress { get; set; }
    [ObservableProperty] public partial string Gtin { get; set; } = "3452334E0CAD";
    [ObservableProperty] public partial decimal EvgMode { get; set; } = 5;

    /// <summary>Take the Block-0 mode from the scanned device instead of the field,
    /// which is what lets one of our EVGs be reflashed to a different mode.</summary>
    [ObservableProperty] public partial bool OverrideModeCheck { get; set; }
    [ObservableProperty] public partial int Progress { get; set; }

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(StartUpdateCommand), nameof(UpdateAllCommand),
                                nameof(RescanCommand), nameof(CancelCommand))]
    public partial bool IsBusy { get; set; }

    [RelayCommand]
    private async Task BrowseAsync()
    {
        if (PickFirmwareFile is null) return;
        var path = await PickFirmwareFile();
        if (string.IsNullOrEmpty(path)) return;
        FirmwarePath = path;
        var info = new FileInfo(path);
        Log($"Selected: {info.Name} ({info.Length} bytes)");
    }

    [RelayCommand(CanExecute = nameof(CanUseBus))]
    private async Task StartUpdateAsync()
    {
        var firmware = await LoadFirmwareAsync();
        if (firmware is null) return;

        if (ShortAddress is < 0 or > 63) { Log("ERROR: Short address must be 0-63"); return; }

        var gtin = ParseGtin(Gtin);
        if (gtin is null) { Log("ERROR: GTIN must be 6 bytes hex (e.g. 3452334E0CAD)"); return; }

        var modeId = ResolveModeId();
        if (modeId is null) return;

        IsBusy = true;
        Progress = 0;
        _updateCts = new CancellationTokenSource();
        try
        {
            var ok = await RunOneUpdateAsync(firmware, (byte)ShortAddress, gtin, modeId.Value);
            Progress = ok ? 100 : 0;
            Log(ok ? "=== UPDATE SUCCESSFUL ===" : "=== UPDATE FAILED ===");
        }
        catch (OperationCanceledException)
        {
            Log("Update cancelled by user");
            Progress = 0;
        }
        catch (Exception ex)
        {
            Log($"Update error: {ex.Message}");
            Progress = 0;
        }
        finally
        {
            _updateCts = null;
            IsBusy = false;
        }
    }

    /// <summary>
    /// Which EVG mode id goes into Block 0. The bootloader checks it against the
    /// identity block the *running* firmware wrote, so changing a device's mode
    /// means announcing the mode it has now, not the one it is about to get —
    /// the new firmware rewrites the identity block on its first boot.
    /// With the override on, that current mode is taken from the scan instead of
    /// the input field. Returns null when the run must not start.
    /// </summary>
    private byte? ResolveModeId()
    {
        if (!OverrideModeCheck) return (byte)EvgMode;

        var row = Devices.FirstOrDefault(d => d.ShortAddress == (byte)ShortAddress);
        if (row is null)
        {
            Log($"ERROR: override needs a scanned device at short {(byte)ShortAddress} — rescan first");
            return null;
        }
        if (!ModeNameToId.TryGetValue(row.ModeLabel, out var current))
        {
            Log($"ERROR: short {row.ShortAddress} reports mode '{row.ModeLabel}', which is not a known EVG mode");
            return null;
        }
        if (current != (byte)EvgMode)
            Log($"[Override] Block 0 announces mode {current} ({row.ModeLabel}) as the device reports it, " +
                $"not {(byte)EvgMode}. The new firmware writes its own mode on first boot.");
        return current;
    }

    [RelayCommand(CanExecute = nameof(CanUseBus))]
    private async Task UpdateAllAsync()
    {
        var firmware = await LoadFirmwareAsync();
        if (firmware is null) return;

        var gtin = ParseGtin(OursGtin);
        if (gtin is null) { Log("ERROR: Ours GTIN must be 6 bytes hex (e.g. 3452334E0CAD)"); return; }

        var targets = new List<(byte Short, byte ModeId, string ModeName)>();
        foreach (var row in Devices.Where(r => r.IsUpdatable))
        {
            if (!ModeNameToId.TryGetValue(row.ModeLabel, out var modeId))
            {
                Log($"[UpdateAll] skipping short {row.ShortAddress}: unknown mode '{row.ModeLabel}'");
                continue;
            }
            targets.Add((row.ShortAddress, modeId, row.ModeLabel));
        }

        if (targets.Count == 0) { Log("[UpdateAll] no updatable devices in the grid"); return; }
        Log($"[UpdateAll] starting batch for {targets.Count} device(s)");

        IsBusy = true;
        _updateCts = new CancellationTokenSource();
        int ok = 0, fail = 0;
        try
        {
            for (var i = 0; i < targets.Count; i++)
            {
                if (_updateCts.IsCancellationRequested) break;
                var t = targets[i];
                Log($"[UpdateAll] [{i + 1}/{targets.Count}] short={t.Short} mode={t.ModeName}");
                Progress = 0;

                var success = false;
                try
                {
                    success = await RunOneUpdateAsync(firmware, t.Short, gtin, t.ModeId);
                }
                catch (OperationCanceledException) { Log("[UpdateAll] cancelled by user"); break; }
                catch (Exception ex) { Log($"[UpdateAll] short {t.Short} error: {ex.Message}"); }

                if (success) { ok++;   Log($"[UpdateAll] [{i + 1}/{targets.Count}] OK"); }
                else         { fail++; Log($"[UpdateAll] [{i + 1}/{targets.Count}] FAILED"); }

                if (_gateway?.IsConnected != true) break;   // gateway dropped mid-batch
            }
            Log($"=== UPDATE ALL DONE: {ok} ok / {fail} failed / {targets.Count} total ===");
        }
        finally
        {
            _updateCts = null;
            IsBusy = false;
        }
    }

    private async Task<bool> RunOneUpdateAsync(byte[] firmware, byte shortAddr, byte[] gtin, byte modeId)
    {
        _bootloader = new DaliBootloader(_gateway!);
        _bootloader.OnLog += Log;
        _bootloader.OnProgress += ReportProgress;
        return await _bootloader.UpdateFirmwareAsync(firmware, shortAddr, gtin, modeId, _updateCts!.Token);
    }

    private async Task<byte[]?> LoadFirmwareAsync()
    {
        if (_gateway is null || !_gateway.IsConnected) { Log("ERROR: Not connected to gateway"); return null; }
        if (string.IsNullOrWhiteSpace(FirmwarePath) || !File.Exists(FirmwarePath))
        {
            Log("ERROR: Select a valid firmware file");
            return null;
        }
        try
        {
            var fw = await File.ReadAllBytesAsync(FirmwarePath);
            if (fw.Length > MaxFirmwareBytes)
            {
                Log($"ERROR: Firmware too large ({fw.Length} bytes, max {MaxFirmwareBytes})");
                return null;
            }
            return fw;
        }
        catch (Exception ex)
        {
            Log($"ERROR reading file: {ex.Message}");
            return null;
        }
    }

    [RelayCommand(CanExecute = nameof(IsBusy))]
    private void Cancel() => _updateCts?.Cancel();

    // ── Log ────────────────────────────────────────────────────────────────

    [ObservableProperty] public partial string LogText { get; set; } = "";

    [RelayCommand]
    private void ClearLog()
    {
        _log.Clear();
        LogText = "";
    }

    private void Log(string message) => OnUi(() =>
    {
        _log.Append($"[{DateTime.Now:HH:mm:ss.fff}] {message}{Environment.NewLine}");
        LogText = _log.ToString();
    });

    private void ReportProgress(int current, int total) => OnUi(() =>
    {
        if (total > 0) Progress = Math.Min(100, current * 100 / total);
    });

    private static void OnUi(Action action)
    {
        if (Dispatcher.UIThread.CheckAccess()) action();
        else Dispatcher.UIThread.Post(action);
    }

    private static byte[]? ParseGtin(string text)
    {
        try
        {
            var g = Convert.FromHexString(text.Replace(" ", "").Replace("0x", ""));
            return g.Length == 6 ? g : null;
        }
        catch { return null; }
    }

    public void Shutdown()
    {
        _updateCts?.Cancel();
        _gateway?.Dispose();
    }
}
