// SPDX-License-Identifier: MIT
using System.Net.Sockets;
using AudioStudio.Core.Ssh;

namespace AudioStudio.Core.Stream;

/// <summary>A connection to the stream that cannot be made, with the reason for the status line.</summary>
public sealed class AudioStreamOpenException(string message) : Exception(message);

/// <summary>Where the bytes of the XAUD stream come from: one connection per <see cref="OpenAsync"/>.</summary>
public interface IAudioStreamSource
{
    /// <summary>The source as the status line names it.</summary>
    string Name { get; }

    /// <exception cref="AudioStreamOpenException">The connection cannot be made.</exception>
    Task<AudioStreamConnection> OpenAsync(CancellationToken ct);
}

/// <summary>One connection: its bytes, and why they ended.</summary>
public abstract class AudioStreamConnection : IDisposable
{
    public abstract System.IO.Stream Data { get; }

    /// <summary>Why the bytes ended; <paramref name="announced"/>: the stream header had come.</summary>
    public abstract Task<string> EndReasonAsync(bool announced);

    public abstract void Dispose();
}

/// <summary>The XIAO's audio stream service on a TCP port. It serves one client at a time and closes a second one at once.</summary>
public sealed class TcpAudioSource(string host, int port) : IAudioStreamSource
{
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(3);

    public string Host { get; } = host;
    public int Port { get; } = port;
    public string Name => $"{Host}:{Port}";

    public async Task<AudioStreamConnection> OpenAsync(CancellationToken ct)
    {
        TcpClient client = new() { NoDelay = true };
        using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(ConnectTimeout);
        try
        {
            await client.ConnectAsync(Host, Port, timeout.Token).ConfigureAwait(false);
        }
        catch (SocketException e)
        {
            client.Dispose();
            throw new AudioStreamOpenException($"cannot connect: {e.Message}");
        }
        catch (OperationCanceledException)
        {
            client.Dispose();
            ct.ThrowIfCancellationRequested();
            throw new AudioStreamOpenException("cannot connect: timed out");
        }
        return new Connection(client);
    }

    private sealed class Connection(TcpClient client) : AudioStreamConnection
    {
        public override System.IO.Stream Data { get; } = client.GetStream();

        public override Task<string> EndReasonAsync(bool announced) => Task.FromResult(
            announced ? "closed by the board" : "closed before the header (is another client connected?)");

        public override void Dispose() => client.Dispose();
    }
}

/// <summary>
/// A command run over ssh on the target whose standard output is the XAUD
/// stream (the UNO Q's <c>audio_stream.py</c>). Each connection runs the
/// command as a channel of <see cref="Ssh"/>; ending the connection ends the
/// channel's ssh process, and the command on the target then ends at its
/// next write. The command's standard error lines are raised as
/// <see cref="ErrorLine"/>.
/// </summary>
public sealed class SshAudioSource(SshConnection ssh, string command) : IAudioStreamSource
{
    /// <summary>The exit code of <c>flock -n -E 75</c> when another process holds the target's lock.</summary>
    public const int InUseExitCode = 75;
    public const string InUseReason = "the adapter is in use on the target (detector or a recording)";

    public SshConnection Ssh { get; } = ssh;
    public string Command { get; } = command;
    public string Name => "ssh " + Ssh.Destination;

    /// <summary>A line the command (or ssh) wrote to its standard error; raised on a thread-pool thread.</summary>
    public event Action<string>? ErrorLine;

    public async Task<AudioStreamConnection> OpenAsync(CancellationToken ct)
    {
        IDisposable lease;
        try
        {
            lease = await Ssh.AcquireAsync(ct).ConfigureAwait(false);
        }
        catch (SshException e)
        {
            throw new AudioStreamOpenException("cannot connect: " + e.Message);
        }
        SshProcess process;
        try
        {
            process = Ssh.Start(Command);
        }
        catch (SshException e)
        {
            lease.Dispose();
            throw new AudioStreamOpenException(e.Message);
        }
        process.CloseInput();
        process.ErrorLine += line => ErrorLine?.Invoke(line);
        return new Connection(this, process, lease);
    }

    /// <summary>Why the command ended with <paramref name="exitCode"/>, for the status line.</summary>
    public string Reason(int? exitCode, IReadOnlyList<string> errors, bool announced) => exitCode switch
    {
        InUseExitCode => InUseReason,
        0 => announced ? "the stream command ended" : "the stream command ended before the header",
        255 or null => Ssh.Explain(exitCode, errors),
        _ => $"the stream command ended with exit code {exitCode}" + (errors.Count > 0 ? ": " + errors[^1] : ""),
    };

    private sealed class Connection(SshAudioSource source, SshProcess process, IDisposable lease) : AudioStreamConnection
    {
        public override System.IO.Stream Data => process.Output;

        public override async Task<string> EndReasonAsync(bool announced)
        {
            await process.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
            return source.Reason(process.ExitCode, process.ErrorTail(), announced);
        }

        public override void Dispose()
        {
            process.Dispose();
            lease.Dispose();
        }
    }
}
