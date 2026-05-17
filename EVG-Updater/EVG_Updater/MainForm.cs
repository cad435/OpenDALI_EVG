namespace EVG_Updater;

public partial class MainForm : Form
{
    private DaliGateway? _gateway;
    private DaliBootloader? _bootloader;
    private DaliBusScanner? _scanner;
    private CancellationTokenSource? _updateCts;
    // Mode-name → EVG_MODE_ID mapping, mirrors Firmware/src/config/hardware.h
    private static readonly Dictionary<string, byte> ModeNameToId = new(StringComparer.OrdinalIgnoreCase)
    {
        ["ONOFF"]       = 0x01,
        ["SINGLE"]      = 0x02,
        ["CCT"]         = 0x03,
        ["RGB"]         = 0x04,
        ["RGBW"]        = 0x05,
        ["WS2812"]      = 0x06,
        ["SK68RGB"]     = 0x07,
        ["SK68RGBW"]    = 0x08,
    };

    public MainForm()
    {
        InitializeComponent();
    }

    private async void btnConnect_Click(object? sender, EventArgs e)
    {
        if (_gateway?.IsConnected == true)
        {
            await _gateway.DisconnectAsync();
            _gateway.Dispose();
            _gateway = null;
            btnConnect.Text = "Connect";
            SetControlsEnabled(false);
            return;
        }

        try
        {
            _gateway = new DaliGateway();
            _gateway.OnLog += msg => SafeInvoke(() => Log(msg));
            _gateway.Disconnected += reason => SafeInvoke(() => HandleUnexpectedDisconnect(reason));

            await _gateway.ConnectAsync(txtGatewayIp.Text);

            btnConnect.Text = "Disconnect";
            SetControlsEnabled(true);

            _ = RunScanAsync();
        }
        catch (Exception ex)
        {
            Log($"Connection failed: {ex.Message}");
            _gateway?.Dispose();
            _gateway = null;
        }
    }

    private void HandleUnexpectedDisconnect(string reason)
    {
        // Reader detected the WS dropped. Don't cancel _updateCts — the
        // bootloader's pending Send/Query calls already wake up via the
        // semaphore-bulk-release in the reader's finally and throw
        // "Not connected", which surfaces a more accurate error message.
        if (_gateway != null)
        {
            try { _gateway.Dispose(); } catch { }
            _gateway = null;
        }
        btnConnect.Text = "Connect";
        SetControlsEnabled(false);
        btnStartUpdate.Enabled = false;
        btnCancel.Enabled = false;
        Log($"Connection lost: {reason}");
    }

    private void SafeInvoke(Action action)
    {
        if (IsDisposed || Disposing || !IsHandleCreated) return;
        // ObjectDisposedException is a subclass of InvalidOperationException —
        // a single catch covers both "form closing" and "form disposed".
        try { BeginInvoke(action); }
        catch (InvalidOperationException) { /* form closing/disposed */ }
    }

    private void btnBrowse_Click(object? sender, EventArgs e)
    {
        using var ofd = new OpenFileDialog
        {
            Filter = "Binary files (*.bin)|*.bin|All files (*.*)|*.*",
            Title = "Select firmware binary"
        };
        if (ofd.ShowDialog() == DialogResult.OK)
        {
            txtFirmwarePath.Text = ofd.FileName;
            var info = new FileInfo(ofd.FileName);
            Log($"Selected: {info.Name} ({info.Length} bytes)");
        }
    }

    private async void btnStartUpdate_Click(object? sender, EventArgs e)
    {
        if (_gateway == null || !_gateway.IsConnected)
        {
            Log("ERROR: Not connected to gateway");
            return;
        }

        if (string.IsNullOrWhiteSpace(txtFirmwarePath.Text) || !File.Exists(txtFirmwarePath.Text))
        {
            Log("ERROR: Select a valid firmware file");
            return;
        }

        if (!byte.TryParse(txtShortAddress.Text, out byte shortAddr) || shortAddr > 63)
        {
            Log("ERROR: Short address must be 0-63");
            return;
        }

        byte[] firmware;
        try
        {
            firmware = await File.ReadAllBytesAsync(txtFirmwarePath.Text);
        }
        catch (Exception ex)
        {
            Log($"ERROR reading file: {ex.Message}");
            return;
        }

        if (firmware.Length > 32640)
        {
            Log($"ERROR: Firmware too large ({firmware.Length} bytes, max 32640)");
            return;
        }

        // Parse GTIN from hex textbox
        byte[] gtin;
        try
        {
            gtin = Convert.FromHexString(txtGtin.Text.Replace(" ", "").Replace("0x", ""));
            if (gtin.Length != 6) throw new FormatException("Must be 6 bytes");
        }
        catch
        {
            Log("ERROR: GTIN must be 6 bytes hex (e.g. 3452334E0CAD)");
            return;
        }

        byte evgModeId = (byte)numEvgMode.Value;

        btnStartUpdate.Enabled = false;
        btnRescan.Enabled = false;
        btnCancel.Enabled = true;
        progressBar.Value = 0;
        _updateCts = new CancellationTokenSource();

        _bootloader = new DaliBootloader(_gateway);
        _bootloader.OnLog += msg => SafeInvoke(() => Log(msg));
        _bootloader.OnProgress += (cur, total) => SafeInvoke(() =>
        {
            if (total > 0)
                progressBar.Value = Math.Min(100, cur * 100 / total);
        });

        try
        {
            var success = await _bootloader.UpdateFirmwareAsync(
                firmware, shortAddr, gtin, evgModeId, _updateCts.Token);

            progressBar.Value = success ? 100 : 0;
            Log(success ? "=== UPDATE SUCCESSFUL ===" : "=== UPDATE FAILED ===");
        }
        catch (OperationCanceledException)
        {
            Log("Update cancelled by user");
            progressBar.Value = 0;
        }
        catch (Exception ex)
        {
            Log($"Update error: {ex.Message}");
            progressBar.Value = 0;
        }
        finally
        {
            // Only re-enable Start Update if the gateway is actually still
            // connected. If it dropped mid-update, HandleUnexpectedDisconnect
            // already disabled the controls and we must not re-enable them
            // against a dead gateway.
            bool stillConnected = _gateway?.IsConnected == true;
            btnStartUpdate.Enabled = stillConnected;
            btnRescan.Enabled = stillConnected;
            btnCancel.Enabled = false;
            _updateCts = null;
        }
    }

    private void btnCancel_Click(object? sender, EventArgs e)
    {
        _updateCts?.Cancel();
    }

    private void btnClearLog_Click(object? sender, EventArgs e)
    {
        txtLog.Clear();
    }

    private void Log(string message)
    {
        txtLog.AppendText($"[{DateTime.Now:HH:mm:ss.fff}] {message}\r\n");
    }

    private void SetControlsEnabled(bool connected)
    {
        btnStartUpdate.Enabled = connected;
        grpUpdate.Enabled = connected;
        grpDevices.Enabled = connected;
        if (!connected) gridDevices.Rows.Clear();
    }

    private async void btnRescan_Click(object? sender, EventArgs e)
    {
        await RunScanAsync();
    }

    private async Task RunScanAsync()
    {
        if (_gateway == null || !_gateway.IsConnected) return;

        // Scan and flash share the DALI bus — disable both controls during scan
        // to prevent concurrent bus traffic.
        btnRescan.Enabled = false;
        btnStartUpdate.Enabled = false;
        gridDevices.Rows.Clear();
        progressBar.Value = 0;
        try
        {
            _scanner = new DaliBusScanner(_gateway);
            _scanner.OnLog += msg => SafeInvoke(() => Log(msg));
            _scanner.OnProgress += (cur, total) => SafeInvoke(() =>
            {
                if (total > 0)
                    progressBar.Value = Math.Min(100, cur * 100 / total);
            });

            byte[] oursGtin;
            try
            {
                oursGtin = Convert.FromHexString(
                    txtOursGtin.Text.Replace(" ", "").Replace("0x", ""));
                if (oursGtin.Length != 6) throw new FormatException();
            }
            catch
            {
                Log("WARNING: Ours-GTIN invalid (need 6 bytes hex); using empty match.");
                oursGtin = new byte[6];
            }

            var devices = await _scanner.ScanAsync();
            foreach (var d in devices)
            {
                gridDevices.Rows.Add(
                    d.ShortAddress,
                    d.RandomHex,
                    d.GtinHex,
                    d.ModeLabel,
                    d.DeviceTypeLabel,
                    d.FwVersion,
                    d.HwVersion,
                    d.IsOurs(oursGtin) ? "✓" : "✗");
            }
            Log($"[Scan] done — {devices.Count} gear listed.");
            progressBar.Value = 100;
        }
        catch (Exception ex)
        {
            Log($"Scan failed: {ex.Message}");
            progressBar.Value = 0;
        }
        finally
        {
            bool stillConnected = _gateway?.IsConnected == true;
            btnRescan.Enabled = stillConnected;
            btnStartUpdate.Enabled = stillConnected;
        }
    }

    private void gridDevices_CellDoubleClick(object? sender, DataGridViewCellEventArgs e)
    {
        if (e.RowIndex < 0 || e.RowIndex >= gridDevices.Rows.Count) return;
        var row = gridDevices.Rows[e.RowIndex];
        // Column order matches Designer: 0=Short, 1=Long, 2=GTIN, 3=Mode, 4=DT, 5=FW, 6=HW, 7=Updatable
        txtShortAddress.Text = row.Cells[0].Value?.ToString() ?? "";
        var gtin = row.Cells[2].Value?.ToString() ?? "";
        if (gtin != "—") txtGtin.Text = gtin;
        var modeName = row.Cells[3].Value?.ToString() ?? "";
        if (ModeNameToId.TryGetValue(modeName, out var id))
        {
            numEvgMode.Value = id;
            Log($"[Select] short={txtShortAddress.Text} mode={modeName} id={id}");
        }
        else
        {
            Log($"[Select] short={txtShortAddress.Text} (mode '{modeName}' unknown — leave EVG Mode as is)");
        }
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        _updateCts?.Cancel();
        _gateway?.Dispose();
        base.OnFormClosing(e);
    }
}
