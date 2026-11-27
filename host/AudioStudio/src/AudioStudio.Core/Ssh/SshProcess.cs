// SPDX-License-Identifier: MIT
using System.ComponentModel;
using System.Diagnostics;

namespace AudioStudio.Core.Ssh;

/// <summary>An ssh connection or channel that cannot be made, or that failed.</summary>
public sealed class SshException(string message) : Exception(message);

/// <summary>
/// One run of the ssh program, a child process of this application: its
/// standard input and output as streams of bytes, its standard error as
/// lines (the last ones kept, each also raised as <see cref="ErrorLine"/>).
/// </summary>
public sealed class SshProcess : IDisposable
{
    private const int TailLines = 8;

    private readonly Process _process;
    private readonly Queue<string> _tail = new();
    private readonly TaskCompletionSource _errorDone = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private int _stopped;

    /// <summary>A line the program wrote to its standard error, raised on a thread-pool thread.</summary>
    public event Action<string>? ErrorLine;

    /// <summary>What the program writes to its standard output.</summary>
    public System.IO.Stream Output => _process.StandardOutput.BaseStream;

    /// <summary>What the program reads from its standard input.</summary>
    public System.IO.Stream Input => _process.StandardInput.BaseStream;

    public int Id { get; }

    private SshProcess(Process process)
    {
        _process = process;
        Id = process.Id;
    }

    /// <summary>Starts <paramref name="executable"/> with <paramref name="arguments"/>.</summary>
    /// <param name="terminal">For a session with a terminal on the target: TERM=dumb, so that the target's shell sends no colours or window titles.</param>
    /// <exception cref="SshException">The program cannot be started.</exception>
    public static SshProcess Start(string executable, IEnumerable<string> arguments, bool terminal = false)
    {
        ProcessStartInfo info = new(executable)
        {
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (string argument in arguments)
        {
            info.ArgumentList.Add(argument);
        }
        if (terminal)
        {
            info.Environment["TERM"] = "dumb";
        }
        Process process = new() { StartInfo = info };
        try
        {
            process.Start();
        }
        catch (Exception e) when (e is Win32Exception or InvalidOperationException)
        {
            process.Dispose();
            throw new SshException($"Cannot run {executable}: {e.Message}");
        }
        SshProcess ssh = new(process);
        process.ErrorDataReceived += (_, e) => ssh.OnError(e.Data);
        process.BeginErrorReadLine();
        return ssh;
    }

    private void OnError(string? line)
    {
        if (line is null)
        {
            _errorDone.TrySetResult();
            return;
        }
        line = line.TrimEnd();
        if (line.Length == 0)
        {
            return;
        }
        lock (_tail)
        {
            _tail.Enqueue(line);
            while (_tail.Count > TailLines)
            {
                _tail.Dequeue();
            }
        }
        ErrorLine?.Invoke(line);
    }

    /// <summary>The last lines of the standard error.</summary>
    public IReadOnlyList<string> ErrorTail()
    {
        lock (_tail)
        {
            return _tail.ToList();
        }
    }

    public bool HasExited => _process.HasExited;

    /// <summary>The exit code; null while the program runs or when it was stopped by this application.</summary>
    public int? ExitCode => _stopped == 0 && _process.HasExited ? _process.ExitCode : null;

    /// <summary>
    /// Waits for the program to end (at most <paramref name="timeout"/>) and
    /// for the rest of its standard error. False when it still runs.
    /// </summary>
    public async Task<bool> WaitAsync(TimeSpan timeout, CancellationToken ct = default)
    {
        using CancellationTokenSource wait = CancellationTokenSource.CreateLinkedTokenSource(ct);
        wait.CancelAfter(timeout);
        try
        {
            await _process.WaitForExitAsync(wait.Token).ConfigureAwait(false);
            await _errorDone.Task.WaitAsync(TimeSpan.FromMilliseconds(500), ct).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
        }
        catch (TimeoutException)
        {
        }
        return _process.HasExited;
    }

    /// <summary>Closes the program's standard input: it reads the end of its input.</summary>
    public void CloseInput()
    {
        try
        {
            _process.StandardInput.Close();
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or ObjectDisposedException)
        {
        }
    }

    /// <summary>Ends the program (SIGKILL) unless it has ended, and waits for it.</summary>
    public void Stop()
    {
        try
        {
            if (!_process.HasExited)
            {
                Interlocked.Exchange(ref _stopped, 1);
                _process.Kill();
            }
            _process.WaitForExit(2000);
        }
        catch (Exception e) when (e is InvalidOperationException or Win32Exception)
        {
        }
    }

    public void Dispose()
    {
        Stop();
        _process.Dispose();
    }
}
