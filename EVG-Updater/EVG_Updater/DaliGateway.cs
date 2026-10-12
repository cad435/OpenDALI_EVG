using System.Net.WebSockets;
using System.Text;
using System.Text.Json;

namespace EVG_Updater;

/// <summary>
/// WebSocket client for the OpenKNX GW-REG1-Dali gateway.
/// Mirrors the proven Python full_update.py flow exactly:
///   - Background reader task continuously drains all incoming messages.
///   - Sender uses a semaphore-based sliding window so the gateway's
///     outgoing TCP buffer never deadlocks.
///   - Backward frames are delivered to a single pending TaskCompletionSource.
/// </summary>
public class DaliGateway : IDisposable
{
    // Max number of unacked daliFrame frames in flight (matches Python PACE_INFLIGHT)
    private const int PACE_INFLIGHT = 4;

    private ClientWebSocket? _ws;
    private SemaphoreSlim? _sendSlots;
    private TaskCompletionSource<byte?>? _backwardTcs;
    private Task? _readerTask;
    private CancellationTokenSource? _readerCts;
    private readonly byte[] _rxBuf = new byte[4096];
    private volatile bool _closed;

    public event Action<string>? OnLog;
    /// <summary>
    /// Fired exactly once when the connection drops (gracefully or unexpectedly).
    /// Argument is a short human-readable reason. UI uses this to reset state.
    /// </summary>
    public event Action<string>? Disconnected;

    public bool IsConnected => _ws?.State == WebSocketState.Open && !_closed;

    public async Task ConnectAsync(string ip, CancellationToken ct = default)
    {
        _ws = new ClientWebSocket();
        // Python uses websockets.connect(..., ping_interval=None). .NET 8's
        // ClientWebSocket otherwise sends an unsolicited PONG every 30s, which
        // the OpenKNX gateway may treat as garbage and close the connection.
        _ws.Options.KeepAliveInterval = Timeout.InfiniteTimeSpan;
        await _ws.ConnectAsync(new Uri($"ws://{ip}"), ct);
        _sendSlots = new SemaphoreSlim(PACE_INFLIGHT, PACE_INFLIGHT);
        _closed = false;
        OnLog?.Invoke($"Connected to ws://{ip}/");

        // Greeting and all subsequent traffic are handled by the reader task.
        // (Reading the greeting on this thread with a CancelAfter-based CTS
        // could abort the WS if the timer fires after a successful read but
        // before the using-scope ends, which then makes IsConnected go false.)
        _readerCts = new CancellationTokenSource();
        _readerTask = Task.Run(() => ReaderLoopAsync(_readerCts.Token));
    }

    public async Task DisconnectAsync()
    {
        _closed = true;
        _readerCts?.Cancel();
        if (_readerTask != null)
        {
            try { await _readerTask; } catch { /* swallow */ }
            _readerTask = null;
        }
        if (_ws?.State == WebSocketState.Open)
        {
            try { await _ws.CloseAsync(WebSocketCloseStatus.NormalClosure, "bye", CancellationToken.None); }
            catch { /* swallow */ }
        }
        _ws?.Dispose();
        _ws = null;
        _sendSlots?.Dispose();
        _sendSlots = null;
        _readerCts?.Dispose();
        _readerCts = null;
        OnLog?.Invoke("Disconnected");
    }

    /// <summary>
    /// Background reader: dispatches incoming JSON messages.
    ///   daliFrame.result    -> release one in-flight send slot
    ///   daliMonitor (8 bit, !isEcho) -> resolve pending backward TCS
    ///   daliAnswer          -> resolve pending backward TCS (older protocol form)
    /// </summary>
    private async Task ReaderLoopAsync(CancellationToken ct)
    {
        string exitReason = "shutdown";
        try
        {
            while (!ct.IsCancellationRequested && _ws?.State == WebSocketState.Open)
            {
                string? raw;
                try
                {
                    raw = await ReceiveRawAsync(ct);
                }
                catch (OperationCanceledException) { exitReason = "cancelled"; break; }
                catch (WebSocketException ex) { exitReason = $"WS exception: {ex.Message}"; break; }

                if (raw == null) { exitReason = "WS closed by gateway"; break; }
                if (string.IsNullOrEmpty(raw)) continue;

                try { ProcessMessage(raw); }
                catch (Exception ex)
                {
                    // Bad/unexpected message format — log and keep reader alive.
                    OnLog?.Invoke($"  [reader] parse error (continuing): {ex.GetType().Name}: {ex.Message}");
                }
            }
        }
        catch (Exception ex)
        {
            exitReason = $"unexpected: {ex.GetType().Name}: {ex.Message}";
        }
        finally
        {
            _closed = true;
            _backwardTcs?.TrySetException(new InvalidOperationException("WS closed"));

            // Wake any sender currently blocked on _sendSlots.WaitAsync.
            // They will then re-check WS state and throw "Not connected".
            try { _sendSlots?.Release(PACE_INFLIGHT); }
            catch (SemaphoreFullException) { }
            catch (ObjectDisposedException) { }

            if (exitReason != "shutdown" && exitReason != "cancelled")
            {
                OnLog?.Invoke($"  [reader] exited: {exitReason}");
                Disconnected?.Invoke(exitReason);
            }
        }
    }

    private void ProcessMessage(string raw)
    {
        using var doc = JsonDocument.Parse(raw);
        var root = doc.RootElement;
        if (!root.TryGetProperty("type", out var typeEl)) return;
        var type = typeEl.GetString();
        if (!root.TryGetProperty("data", out var d)) return;

        switch (type)
        {
            case "info":
                // Gateway greeting / device-info push
                OnLog?.Invoke($"Gateway: {raw}");
                break;

            case "daliFrame":
                // Gateway confirmed the frame went out -> release a slot
                if (d.TryGetProperty("result", out _))
                {
                    try { _sendSlots?.Release(); }
                    catch (SemaphoreFullException) { /* spurious extra ack — ignore */ }
                    catch (ObjectDisposedException) { /* shutting down */ }
                }
                break;

            case "daliMonitor":
            {
                bool isEcho = d.TryGetProperty("isEcho", out var e)
                              && e.ValueKind == JsonValueKind.True;
                int bits = (d.TryGetProperty("bits", out var b)
                            && b.ValueKind == JsonValueKind.Number) ? b.GetInt32() : 0;
                if (!isEcho && bits == 8
                    && d.TryGetProperty("data", out var md)
                    && md.ValueKind == JsonValueKind.Array
                    && md.GetArrayLength() > 0
                    && md[0].ValueKind == JsonValueKind.Number)
                {
                    byte val = (byte)md[0].GetInt32();
                    _backwardTcs?.TrySetResult(val);
                }
                break;
            }

            case "daliAnswer":
            {
                int res = (d.TryGetProperty("result", out var rEl)
                           && rEl.ValueKind == JsonValueKind.Number) ? rEl.GetInt32() : -1;
                int val = (d.TryGetProperty("daliData", out var vEl)
                           && vEl.ValueKind == JsonValueKind.Number) ? vEl.GetInt32() : 0;
                var tcs = _backwardTcs;
                if (tcs != null && !tcs.Task.IsCompleted)
                {
                    if (res == 8) tcs.TrySetResult((byte)val);
                    else if (res == 0) tcs.TrySetResult(null);
                }
                break;
            }
        }
    }

    /// <summary>
    /// Send a fire-and-forget DALI frame; pace via the in-flight semaphore.
    /// </summary>
    public async Task SendDataAsync(byte[] frameBytes, bool sendTwice = false, CancellationToken ct = default)
    {
        if (_closed || _ws?.State != WebSocketState.Open || _sendSlots == null)
            throw new InvalidOperationException("Not connected");

        await _sendSlots.WaitAsync(ct);
        // Re-check after wait — reader may have exited while we were waiting
        // and woken us via the bulk-release in finally.
        if (_closed || _ws?.State != WebSocketState.Open)
            throw new InvalidOperationException("Not connected");

        var msg = BuildFrameMessage(frameBytes, waitForAnswer: false, sendTwice: sendTwice);
        await _ws.SendAsync(Encoding.UTF8.GetBytes(msg), WebSocketMessageType.Text, true, ct);
    }

    /// <summary>
    /// Send a DALI frame with waitForAnswer; await the backward 8-bit frame.
    /// Returns the byte value, or null on timeout / no answer / connection loss.
    /// </summary>
    public async Task<byte?> SendQueryAsync(byte[] frameBytes, bool sendTwice = false,
        int timeoutMs = 2000, CancellationToken ct = default)
    {
        if (_closed || _ws?.State != WebSocketState.Open || _sendSlots == null)
            throw new InvalidOperationException("Not connected");

        // Set up TCS BEFORE sending so the reader can resolve it (matches Python)
        var tcs = new TaskCompletionSource<byte?>(TaskCreationOptions.RunContinuationsAsynchronously);
        _backwardTcs = tcs;

        try
        {
            await _sendSlots.WaitAsync(ct);
            if (_closed || _ws?.State != WebSocketState.Open)
                throw new InvalidOperationException("Not connected");

            var msg = BuildFrameMessage(frameBytes, waitForAnswer: true, sendTwice: sendTwice);
            await _ws.SendAsync(Encoding.UTF8.GetBytes(msg), WebSocketMessageType.Text, true, ct);

            using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeoutCts.CancelAfter(timeoutMs);
            using (timeoutCts.Token.Register(() => tcs.TrySetResult(null)))
            {
                try { return await tcs.Task; }
                catch { return null; }
            }
        }
        finally
        {
            _backwardTcs = null;
        }
    }

    /// <summary>
    /// Wait until all in-flight slots have been released (i.e. all daliFrame
    /// results received). Returns false on timeout. <paramref name="precise"/>
    /// polls without Task.Delay, whose ~15.6 ms Windows granularity would
    /// otherwise add up to one tick after the last result arrived.
    /// </summary>
    public async Task<bool> DrainInflightAsync(int timeoutMs = 5000, CancellationToken ct = default,
        bool precise = false)
    {
        if (_sendSlots == null) return true;
        long deadline = Environment.TickCount64 + timeoutMs;
        while (_sendSlots.CurrentCount < PACE_INFLIGHT)
        {
            if (Environment.TickCount64 > deadline) return false;
            if (precise) await Task.Yield();
            else await Task.Delay(10, ct);
            ct.ThrowIfCancellationRequested();
        }
        return true;
    }

    private static string BuildFrameMessage(byte[] frameBytes, bool waitForAnswer, bool sendTwice)
    {
        return JsonSerializer.Serialize(new
        {
            type = "daliFrame",
            data = new
            {
                line = 0,
                numberOfBits = frameBytes.Length * 8,
                mode = new { sendTwice, waitForAnswer, priority = 3 },
                daliData = frameBytes.Select(b => (int)b).ToArray()
            }
        });
    }

    private async Task<string?> ReceiveRawAsync(CancellationToken ct)
    {
        if (_ws == null) return null;
        using var ms = new MemoryStream();
        WebSocketReceiveResult result;
        do
        {
            result = await _ws.ReceiveAsync(_rxBuf, ct);
            if (result.MessageType == WebSocketMessageType.Close) return null;
            ms.Write(_rxBuf, 0, result.Count);
        } while (!result.EndOfMessage);
        return Encoding.UTF8.GetString(ms.GetBuffer(), 0, (int)ms.Length);
    }

    public void Dispose()
    {
        _closed = true;
        _readerCts?.Cancel();
        try { _readerTask?.Wait(500); } catch { /* swallow */ }
        _ws?.Dispose();
        _sendSlots?.Dispose();
        _readerCts?.Dispose();
    }
}
