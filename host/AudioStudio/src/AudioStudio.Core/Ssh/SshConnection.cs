// SPDX-License-Identifier: MIT
using System.Text;

namespace AudioStudio.Core.Ssh;

/// <summary>What a command run over ssh printed, and how it ended.</summary>
/// <param name="ExitCode">Null when the command did not end within the wait (it was stopped).</param>
public sealed record SshResult(string Output, int? ExitCode, IReadOnlyList<string> Errors);

/// <summary>
/// One connection to a target with the system's ssh program, shared by the
/// shell, the audio stream and the queries: a master ssh process
/// (<c>ControlMaster=auto</c>, its control socket in a private temporary
/// directory) that the others use as separate channels. ssh runs with
/// <c>BatchMode=yes</c>: it never asks for a password, and a target that
/// wants one fails with a message that says so. The master is started by the
/// first <see cref="AcquireAsync"/> and stopped when the last lease is
/// disposed; the directory is removed by <see cref="Dispose"/>.
/// </summary>
public sealed class SshConnection : IDisposable
{
    /// <summary>The environment variable that names the ssh program (it overrides the setting).</summary>
    public const string ExecutableVariable = "AUDIOSTUDIO_SSH";
    /// <summary>
    /// The master's own session: it reads its input until this application's
    /// end of it closes, also when the application ends without cleaning up,
    /// and the master then ends after its last channel.
    /// </summary>
    public const string MasterCommand = "cat >/dev/null";
    public static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(20);
    private static readonly TimeSpan StopWait = TimeSpan.FromSeconds(2);

    private readonly string _directory;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private SshProcess? _master;
    private int _users;
    private bool _disposed;

    /// <summary>The target as ssh takes it: <c>user@host</c>, <c>host</c>, or a name of the ssh configuration.</summary>
    public string Destination { get; }
    public string Executable { get; }
    public string ControlPath { get; }

    /// <exception cref="SshException">The destination is not one ssh would take as a destination.</exception>
    public SshConnection(string destination, string executable)
    {
        if (CheckDestination(destination) is string error)
        {
            throw new SshException(error);
        }
        Destination = destination;
        Executable = executable;
        _directory = Directory.CreateTempSubdirectory("audiostudio-ssh-").FullName;
        ControlPath = Path.Combine(_directory, "master");
    }

    /// <summary>The ssh program: <see cref="ExecutableVariable"/>, else the setting, else <c>ssh</c> on the PATH.</summary>
    public static string ResolveExecutable(string? configured)
    {
        string? variable = Environment.GetEnvironmentVariable(ExecutableVariable);
        return !string.IsNullOrWhiteSpace(variable) ? variable
            : !string.IsNullOrWhiteSpace(configured) ? configured
            : "ssh";
    }

    /// <summary>Null when ssh takes the text as its destination; otherwise why not.</summary>
    public static string? CheckDestination(string destination) =>
        destination.Length == 0 ? "Enter the target as user@host"
        : destination.StartsWith('-') || destination.Any(char.IsWhiteSpace)
            ? $"Not an ssh destination (user@host): {destination}"
            : null;

    /// <summary>The options every run of ssh gets.</summary>
    private List<string> Options() =>
    [
        "-o", "BatchMode=yes",
        "-o", "ControlMaster=auto",
        "-o", "ControlPath=" + ControlPath,
        "-o", "ControlPersist=no",
        "-o", "ConnectTimeout=10",
        "-o", "ServerAliveInterval=5",
        "-o", "ServerAliveCountMax=3",
    ];

    /// <summary>
    /// Makes sure the connection is up (the master runs and its control
    /// socket exists) and counts a user of it until the lease is disposed.
    /// </summary>
    /// <exception cref="SshException">The connection cannot be made; the message says why (ssh's own words).</exception>
    public async Task<IDisposable> AcquireAsync(CancellationToken ct = default)
    {
        await _gate.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_master is null || _master.HasExited)
            {
                _master?.Dispose();
                _master = null;
                _master = await StartMasterAsync(ct).ConfigureAwait(false);
            }
            _users++;
            return new Lease(this);
        }
        finally
        {
            _gate.Release();
        }
    }

    private sealed class Lease(SshConnection connection) : IDisposable
    {
        private int _done;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _done, 1) == 0)
            {
                connection.Release();
            }
        }
    }

    private void Release()
    {
        _gate.Wait();
        try
        {
            if (--_users == 0)
            {
                StopMaster();
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    private async Task<SshProcess> StartMasterAsync(CancellationToken ct)
    {
        // The socket of a master that was killed would be taken as the new one's.
        File.Delete(ControlPath);
        SshProcess master = SshProcess.Start(Executable, [.. Options(), "-T", Destination, MasterCommand]);
        DateTime end = DateTime.UtcNow + ConnectTimeout;
        try
        {
            while (true)
            {
                if (File.Exists(ControlPath))
                {
                    return master;
                }
                if (master.HasExited)
                {
                    await master.WaitAsync(StopWait, ct).ConfigureAwait(false);
                    throw new SshException(Explain(master.ExitCode, master.ErrorTail()));
                }
                if (DateTime.UtcNow > end)
                {
                    throw new SshException(
                        $"ssh {Destination}: no connection within {ConnectTimeout.TotalSeconds:0} s" + Last(master.ErrorTail()));
                }
                await Task.Delay(50, ct).ConfigureAwait(false);
            }
        }
        catch
        {
            master.Dispose();
            throw;
        }
    }

    private void StopMaster()
    {
        SshProcess? master = _master;
        _master = null;
        if (master is null)
        {
            return;
        }
        // Its session ends with its input; the master then ends by itself.
        master.CloseInput();
        if (!master.WaitAsync(StopWait).GetAwaiter().GetResult())
        {
            master.Stop();
        }
        master.Dispose();
    }

    /// <summary>
    /// Starts a channel: <paramref name="command"/> on the target without a
    /// terminal (its standard output is passed through unchanged), or, with
    /// <paramref name="command"/> null, an interactive login shell on a
    /// terminal (<c>-tt</c>). The caller holds a lease while it runs.
    /// </summary>
    public SshProcess Start(string? command)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        return command is null
            ? SshProcess.Start(Executable, [.. Options(), "-tt", "-e", "none", Destination], terminal: true)
            : SshProcess.Start(Executable, [.. Options(), "-T", Destination, command]);
    }

    /// <summary>
    /// Runs <paramref name="command"/> on the target as a channel of its own
    /// and returns what it printed; a command that does not end within
    /// <paramref name="timeout"/> is stopped. The caller holds a lease.
    /// </summary>
    public async Task<SshResult> RunAsync(string command, TimeSpan timeout, CancellationToken ct = default)
    {
        using SshProcess process = Start(command);
        process.CloseInput();
        using CancellationTokenSource wait = CancellationTokenSource.CreateLinkedTokenSource(ct);
        wait.CancelAfter(timeout);
        MemoryStream output = new();
        try
        {
            await process.Output.CopyToAsync(output, wait.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            process.Stop();
        }
        await process.WaitAsync(StopWait, ct).ConfigureAwait(false);
        return new SshResult(Encoding.UTF8.GetString(output.ToArray()), process.ExitCode, process.ErrorTail());
    }

    /// <summary>
    /// Why ssh, or the command it ran, ended with <paramref name="exitCode"/>:
    /// for ssh's own failures (255) its last message, with what to do about
    /// a missing key or an unknown host key.
    /// </summary>
    public string Explain(int? exitCode, IReadOnlyList<string> errors)
    {
        string? denied = errors.LastOrDefault(l => l.Contains("Permission denied", StringComparison.Ordinal));
        if (denied is not null)
        {
            return $"ssh {Destination}: {denied} (ssh runs with BatchMode=yes and asks for no password: " +
                   "set up key authentication for this user)";
        }
        if (errors.Any(l => l.Contains("Host key verification failed", StringComparison.Ordinal)))
        {
            return $"ssh {Destination}: host key verification failed (connect once with ssh in a terminal to check and accept the target's key)";
        }
        return exitCode switch
        {
            null => $"ssh {Destination}: stopped",
            255 => $"ssh {Destination}: failed" + Last(errors),
            _ => $"ssh {Destination}: ended with exit code {exitCode}" + Last(errors),
        };
    }

    private static string Last(IReadOnlyList<string> errors) => errors.Count > 0 ? ": " + errors[^1] : "";

    public void Dispose()
    {
        _gate.Wait();
        try
        {
            if (_disposed)
            {
                return;
            }
            _disposed = true;
            StopMaster();
        }
        finally
        {
            _gate.Release();
        }
        try
        {
            Directory.Delete(_directory, recursive: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }
}
