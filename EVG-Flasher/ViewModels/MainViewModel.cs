using System.Collections.ObjectModel;
using System.Text;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using EvgFlasher.Models;
using EvgFlasher.Services;

namespace EvgFlasher.ViewModels;

public partial class MainViewModel : ViewModelBase
{
    private const int MaxLogLines = 2000;

    private readonly ToolPaths _paths;
    private readonly FlashService _flash;
    private readonly StringBuilder _log = new();
    private readonly Queue<int> _logLineLengths = new();
    private CancellationTokenSource? _runCts;

    public MainViewModel()
    {
        try
        {
            _paths = ToolPaths.Discover();
            _flash = new FlashService(_paths);
            ReloadEnvironments();

            var missing = _paths.MissingPrerequisites().ToList();
            if (missing.Count > 0)
            {
                Append("Missing prerequisites:");
                foreach (var m in missing) Append("  " + m);
                SetStatus("Prerequisites missing — see log", StatusKind.Error);
            }
            else
            {
                SetStatus("Ready. Connect a chip.", StatusKind.Idle);
            }

            _ = PollChipAsync();
        }
        catch (Exception ex)
        {
            _paths = null!;
            _flash = null!;
            Append("Startup failed: " + ex.Message);
            SetStatus("Startup failed", StatusKind.Error);
        }
    }

    // ── Environment selection ──────────────────────────────────────────────

    public ObservableCollection<EvgEnvironment> Environments { get; } = [];

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowLedCount))]
    public partial EvgEnvironment? SelectedEnvironment { get; set; }

    /// <summary>The strip length only applies to the SPI+DMA modes.</summary>
    public bool ShowLedCount => SelectedEnvironment?.IsDigitalStrip == true;

    [ObservableProperty]
    public partial decimal LedCount { get; set; } = 30;

    [RelayCommand]
    private void ReloadEnvironments()
    {
        var keep = SelectedEnvironment?.Name;
        Environments.Clear();
        foreach (var e in PlatformIoConfig.Read(_paths.PlatformIoIni)) Environments.Add(e);
        SelectedEnvironment = Environments.FirstOrDefault(e => e.Name == keep)
                              ?? Environments.FirstOrDefault(e => e.IsDefault)
                              ?? Environments.FirstOrDefault();
        Append($"Loaded {Environments.Count} environments from {_paths.PlatformIoIni}");
    }

    // ── Run state ──────────────────────────────────────────────────────────

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(FlashCommand))]
    [NotifyCanExecuteChangedFor(nameof(CancelCommand))]
    public partial bool IsBusy { get; set; }

    [ObservableProperty] public partial bool ChipPresent { get; set; }
    [ObservableProperty] public partial bool AutoMode { get; set; }
    [ObservableProperty] public partial int CurrentStep { get; set; }
    [ObservableProperty] public partial int ChipsDone { get; set; }
    [ObservableProperty] public partial string StatusMessage { get; set; } = "Starting...";
    [ObservableProperty] public partial StatusKind Status { get; set; } = StatusKind.Idle;
    [ObservableProperty] public partial string LogText { get; set; } = "";

    private bool CanFlash => !IsBusy && SelectedEnvironment is not null;

    [RelayCommand(CanExecute = nameof(CanFlash))]
    private async Task FlashAsync()
    {
        if (SelectedEnvironment is null) return;

        _runCts = new CancellationTokenSource();
        IsBusy = true;
        CurrentStep = 0;
        var env = SelectedEnvironment;
        var leds = env.IsDigitalStrip ? (int)LedCount : (int?)null;

        Append("");
        Append($"############# EVG #{ChipsDone + 1} [{env.Name}] - START #############");
        SetStatus($"Flashing {env.Name}...", StatusKind.Busy);
        var started = DateTime.UtcNow;

        try
        {
            var steps = await _flash.RunAsync(env, leds, Append, s => CurrentStep = s, _runCts.Token);
            var failed = steps.FirstOrDefault(s => !s.Ok);
            var elapsed = (DateTime.UtcNow - started).TotalSeconds;
            ChipsDone++;

            if (failed is null)
            {
                Append($"############# EVG #{ChipsDone} [{env.Name}] - PASS in {elapsed:N1}s #############");
                SetStatus($"PASS — {env.Name} in {elapsed:N1}s", StatusKind.Pass);
            }
            else
            {
                Append($"############# EVG #{ChipsDone} [{env.Name}] - FAIL: {failed.Error} #############");
                SetStatus($"FAIL at step {failed.Number} ({failed.Title}): {failed.Error}", StatusKind.Fail);
            }
        }
        catch (OperationCanceledException)
        {
            Append("Cancelled.");
            SetStatus("Cancelled", StatusKind.Idle);
        }
        catch (Exception ex)
        {
            Append("ERROR: " + ex.Message);
            SetStatus(ex.Message, StatusKind.Fail);
        }
        finally
        {
            IsBusy = false;
            CurrentStep = 0;
            _runCts?.Dispose();
            _runCts = null;
        }
    }

    [RelayCommand(CanExecute = nameof(IsBusy))]
    private void Cancel() => _runCts?.Cancel();

    [RelayCommand]
    private void ClearLog()
    {
        _log.Clear();
        _logLineLengths.Clear();
        LogText = "";
    }

    // ── Chip polling ───────────────────────────────────────────────────────

    /// <summary>
    /// Polls the WCH-Link once a second. In auto mode a newly attached chip
    /// starts a run by itself; afterwards the chip has to be removed before
    /// the next one is picked up, mirroring flash_blank_evg.ps1.
    /// </summary>
    private async Task PollChipAsync()
    {
        var waitingForRemoval = false;
        while (true)
        {
            try
            {
                if (!IsBusy)
                {
                    var present = await _flash.IsChipPresentAsync(CancellationToken.None);
                    ChipPresent = present;

                    if (!present) waitingForRemoval = false;
                    else if (AutoMode && !waitingForRemoval && SelectedEnvironment is not null)
                    {
                        waitingForRemoval = true;
                        await FlashAsync();
                    }
                }
            }
            catch { /* keep polling */ }
            await Task.Delay(1000);
        }
    }

    // ── Log ────────────────────────────────────────────────────────────────

    private void Append(string line)
    {
        void Write()
        {
            _log.AppendLine(line);
            _logLineLengths.Enqueue(line.Length + Environment.NewLine.Length);
            while (_logLineLengths.Count > MaxLogLines)
                _log.Remove(0, _logLineLengths.Dequeue());
            LogText = _log.ToString();
        }

        if (Dispatcher.UIThread.CheckAccess()) Write();
        else Dispatcher.UIThread.Post(Write);
    }

    private void SetStatus(string message, StatusKind kind)
    {
        if (Dispatcher.UIThread.CheckAccess()) { StatusMessage = message; Status = kind; }
        else Dispatcher.UIThread.Post(() => { StatusMessage = message; Status = kind; });
    }
}

public enum StatusKind { Idle, Busy, Pass, Fail, Error }
