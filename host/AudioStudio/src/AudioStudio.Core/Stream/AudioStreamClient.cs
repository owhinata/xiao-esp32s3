// SPDX-License-Identifier: MIT
using System.Net.Sockets;

namespace AudioStudio.Core.Stream;

public enum AudioStreamState
{
    /// <summary>Not started, or stopped.</summary>
    Stopped,
    Connecting,
    /// <summary>The header came; no block yet.</summary>
    Connected,
    /// <summary>The board sends keep-alives: no stream runs on it.</summary>
    Idle,
    Streaming,
    /// <summary>The connection ended or could not be made; another attempt follows.</summary>
    Waiting,
}

/// <summary>What the client has seen, summed over all its connections.</summary>
/// <param name="Reason">Why the last connection ended or failed; null while none did.</param>
/// <param name="SamplesPerSecond">Samples received per second over the last few seconds; 0 if unknown.</param>
public sealed record AudioStreamStatus(
    AudioStreamState State, string? Reason, AudioStreamHeader? Header, int Connections, long Blocks,
    long Samples, long LostSamples, long Gaps, long NewStreams, double SamplesPerSecond);

/// <summary>
/// Receives the target's audio stream from an <see cref="IAudioStreamSource"/>
/// (the XIAO's TCP port 3233, or a command over ssh) and keeps it up: a
/// connection that ends, breaks the protocol or stays silent for
/// <see cref="SilenceTimeout"/> (the target sends at least one block a
/// second) is closed and made again after <see cref="RetryDelay"/>, until
/// the client is disposed.
/// </summary>
public sealed class AudioStreamClient : IDisposable
{
    public const int DefaultPort = 3233;
    public static readonly TimeSpan SilenceTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan RetryDelay = TimeSpan.FromSeconds(2);
    private const long RateWindowMs = 5000;

    private readonly CancellationTokenSource _stop = new();
    private readonly object _lock = new();
    private readonly Queue<(long Ms, long Samples)> _rate = new();
    private Task? _loop;
    private AudioStreamState _state = AudioStreamState.Stopped;
    private string? _reason;
    private AudioStreamHeader? _header;
    private int _connections;
    private long _blocks, _samples, _lost, _gaps, _newStreams;

    public IAudioStreamSource Source { get; }

    /// <summary>A connection got its header; raised on the receiving thread.</summary>
    public event Action<AudioStreamHeader>? Connected;
    /// <summary>A block arrived; raised on the receiving thread, in order.</summary>
    public event Action<AudioBlock>? BlockReceived;
    /// <summary>A connection ended, with the reason; raised on the receiving thread.</summary>
    public event Action<string>? Disconnected;

    public AudioStreamClient(IAudioStreamSource source)
    {
        Source = source;
    }

    /// <summary>The XIAO's audio stream service.</summary>
    public AudioStreamClient(string host, int port = DefaultPort) : this(new TcpAudioSource(host, port))
    {
    }

    public void Start()
    {
        _loop ??= Task.Run(() => RunAsync(_stop.Token));
    }

    public AudioStreamStatus Status()
    {
        lock (_lock)
        {
            double rate = 0;
            if (_rate.Count >= 2)
            {
                (long firstMs, long firstSamples) = _rate.Peek();
                long spanMs = Environment.TickCount64 - firstMs;
                if (spanMs >= 1000)
                {
                    rate = (_samples - firstSamples) * 1000.0 / spanMs;
                }
            }
            return new AudioStreamStatus(_state, _reason, _header, _connections, _blocks, _samples, _lost, _gaps,
                                         _newStreams, rate);
        }
    }

    private void SetState(AudioStreamState state, string? reason = null)
    {
        lock (_lock)
        {
            _state = state;
            if (reason is not null)
            {
                _reason = reason;
            }
            if (state is not (AudioStreamState.Streaming or AudioStreamState.Idle or AudioStreamState.Connected))
            {
                _rate.Clear();
            }
        }
    }

    private async Task RunAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            string reason = await ConnectOnceAsync(ct).ConfigureAwait(false);
            if (ct.IsCancellationRequested)
            {
                break;
            }
            SetState(AudioStreamState.Waiting, reason);
            Disconnected?.Invoke(reason);
            try
            {
                await Task.Delay(RetryDelay, ct).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }
        }
        SetState(AudioStreamState.Stopped);
    }

    /// <summary>One connection, from the attempt to its end. Returns why it ended.</summary>
    private async Task<string> ConnectOnceAsync(CancellationToken ct)
    {
        SetState(AudioStreamState.Connecting);
        AudioStreamConnection connection;
        try
        {
            connection = await Source.OpenAsync(ct).ConfigureAwait(false);
        }
        catch (AudioStreamOpenException e)
        {
            return e.Message;
        }
        catch (OperationCanceledException)
        {
            return "stopped";
        }
        using AudioStreamConnection owned = connection;
        System.IO.Stream stream = connection.Data;
        AudioStreamParser parser = new();
        byte[] buffer = new byte[16384];
        bool announced = false;
        // The parser completes the header before any block; it is announced
        // before the first block that follows it.
        void Announce()
        {
            if (!announced && parser.Header is AudioStreamHeader header)
            {
                announced = true;
                lock (_lock)
                {
                    _header = header;
                    _connections++;
                }
                SetState(AudioStreamState.Connected);
                Connected?.Invoke(header);
            }
        }
        try
        {
            while (true)
            {
                int n;
                using (CancellationTokenSource silence = CancellationTokenSource.CreateLinkedTokenSource(ct))
                {
                    silence.CancelAfter(SilenceTimeout);
                    try
                    {
                        n = await stream.ReadAsync(buffer, silence.Token).ConfigureAwait(false);
                    }
                    catch (OperationCanceledException) when (!ct.IsCancellationRequested)
                    {
                        return $"nothing received for {SilenceTimeout.TotalSeconds:0} s";
                    }
                }
                if (n == 0)
                {
                    return await connection.EndReasonAsync(announced).ConfigureAwait(false);
                }
                parser.Feed(buffer.AsSpan(0, n), block =>
                {
                    Announce();
                    Deliver(block);
                });
                Announce();
            }
        }
        catch (OperationCanceledException)
        {
            return "stopped";
        }
        catch (AudioStreamException e)
        {
            return "protocol error: " + e.Message;
        }
        catch (Exception e) when (e is IOException or SocketException or ObjectDisposedException)
        {
            return e.Message;
        }
    }

    private void Deliver(AudioBlock block)
    {
        lock (_lock)
        {
            _blocks++;
            _samples += block.Samples.Length;
            if (block.Lost > 0)
            {
                _lost += block.Lost;
                _gaps++;
            }
            if (block.NewStream)
            {
                _newStreams++;
            }
            _state = block.Idle ? AudioStreamState.Idle : AudioStreamState.Streaming;
            long now = Environment.TickCount64;
            _rate.Enqueue((now, _samples));
            while (_rate.Count > 2 && now - _rate.Peek().Ms > RateWindowMs)
            {
                _rate.Dequeue();
            }
        }
        BlockReceived?.Invoke(block);
    }

    public void Dispose()
    {
        _stop.Cancel();
        try
        {
            _loop?.Wait(TimeSpan.FromSeconds(2));
        }
        catch (AggregateException)
        {
        }
        _stop.Dispose();
    }
}
