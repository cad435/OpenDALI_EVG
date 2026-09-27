using System.Diagnostics;

namespace EvgFlasher.Services;

/// <summary>Runs a console tool and streams its output line by line.</summary>
public static class ProcessRunner
{
    public static async Task<int> RunAsync(
        string exe,
        IEnumerable<string> args,
        string? workingDirectory,
        IReadOnlyDictionary<string, string>? extraEnvironment,
        Action<string> onLine,
        CancellationToken ct)
    {
        var psi = new ProcessStartInfo(exe)
        {
            WorkingDirectory = workingDirectory ?? Path.GetDirectoryName(exe) ?? ".",
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        if (extraEnvironment is not null)
            foreach (var (k, v) in extraEnvironment) psi.Environment[k] = v;

        using var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
        p.OutputDataReceived += (_, e) => { if (e.Data is not null) onLine(e.Data); };
        p.ErrorDataReceived  += (_, e) => { if (e.Data is not null) onLine(e.Data); };

        if (!p.Start()) throw new InvalidOperationException($"Could not start {exe}");
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();

        try
        {
            await p.WaitForExitAsync(ct).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            try { if (!p.HasExited) p.Kill(entireProcessTree: true); } catch { /* already gone */ }
            throw;
        }
        return p.ExitCode;
    }

    /// <summary>Runs a tool and returns its combined output as one string.</summary>
    public static async Task<(int ExitCode, string Output)> CaptureAsync(
        string exe, IEnumerable<string> args, CancellationToken ct)
    {
        var sb = new System.Text.StringBuilder();
        var code = await RunAsync(exe, args, null, null,
            line => { lock (sb) sb.AppendLine(line); }, ct).ConfigureAwait(false);
        return (code, sb.ToString());
    }
}
