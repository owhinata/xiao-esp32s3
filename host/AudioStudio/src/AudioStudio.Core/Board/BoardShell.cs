// SPDX-License-Identifier: MIT
using System.Net.Sockets;
using System.Text;

namespace AudioStudio.Core.Board;

/// <summary>
/// One Telnet connection to the board's shell, kept for as long as the
/// application talks to the board. The board accepts a single Telnet client
/// and stops accepting when connections are opened and closed in quick
/// succession, so nothing here reconnects per command.
/// </summary>
public sealed class BoardShell : IDisposable
{
    private readonly TcpClient _client;
    private readonly NetworkStream _stream;
    private readonly TerminalFilter _filter = new();
    private readonly SemaphoreSlim _turn = new(1, 1);
    private readonly CancellationTokenSource _closing = new();
    private readonly object _lock = new();
    private Capture? _capture;
    /// <summary>A quiet command's prompt has come: the rest of the prompt (its trailing space) is not shown either.</summary>
    private bool _quietTail;
    private long _lastReceive = Environment.TickCount64;
    private int _closed;

    private sealed class Capture(string line, bool quiet)
    {
        public string Line { get; } = line;
        /// <summary>What the board prints for this line is not raised as <see cref="Output"/>.</summary>
        public bool Quiet { get; } = quiet;
        public StringBuilder Text { get; } = new();
        /// <summary>How much of the line the board has echoed so far; -1 once it is known not to echo.</summary>
        public int Echoed { get; set; }
        public TaskCompletionSource<string> Done { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    /// <summary>Text from the board, raised on a thread-pool thread.</summary>
    public event Action<string>? Output;
    /// <summary>Raised once when the connection ends, with the reason if it was not asked for.</summary>
    public event Action<string?>? Closed;

    public string Address { get; }

    private BoardShell(string address, TcpClient client)
    {
        Address = address;
        _client = client;
        _stream = client.GetStream();
    }

    public static async Task<BoardShell> ConnectAsync(string address, int port = BoardLocator.TelnetPort,
                                                      CancellationToken ct = default)
    {
        TcpClient client = new() { NoDelay = true };
        using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(TimeSpan.FromSeconds(5));
        try
        {
            await client.ConnectAsync(address, port, timeout.Token).ConfigureAwait(false);
        }
        catch (Exception e) when (e is SocketException or OperationCanceledException)
        {
            client.Dispose();
            ct.ThrowIfCancellationRequested();
            throw new BoardException($"Cannot connect to {address}:{port}: " +
                                     (e is SocketException ? e.Message : "timed out"));
        }
        BoardShell shell = new(address, client);
        _ = Task.Run(shell.ReadLoopAsync, CancellationToken.None);
        return shell;
    }

    /// <summary>
    /// Waits until the board has been quiet for half a second (at most three
    /// seconds). A line sent right after connecting can be dropped before the
    /// shell starts to listen.
    /// </summary>
    public async Task SettleAsync(CancellationToken ct = default)
    {
        long end = Environment.TickCount64 + 3000;
        Interlocked.Exchange(ref _lastReceive, Environment.TickCount64);
        while (Environment.TickCount64 < end &&
               Environment.TickCount64 - Interlocked.Read(ref _lastReceive) < 500)
        {
            await Task.Delay(50, ct).ConfigureAwait(false);
        }
    }

    /// <summary>
    /// Sends a line and returns what the board printed until its prompt came
    /// back, or until <paramref name="wait"/> passed. Calls are served one at
    /// a time.
    /// </summary>
    public Task<string> ExecuteAsync(string line, TimeSpan wait, CancellationToken ct = default) =>
        RunAsync(line, wait, quiet: false, ct);

    /// <summary>
    /// As <see cref="ExecuteAsync"/>, but what the board prints for the line
    /// (its echo, its output and the prompt that ends it) is not raised as
    /// <see cref="Output"/>: for the application's own queries of the
    /// board's state. Waits behind a command that is running.
    /// </summary>
    public Task<string> QueryAsync(string line, TimeSpan wait, CancellationToken ct = default) =>
        RunAsync(line, wait, quiet: true, ct);

    /// <summary>Whether a capture ends with the prompt: the command finished within the wait.</summary>
    public static bool Finished(string captured) => IsPrompt(captured);

    private async Task<string> RunAsync(string line, TimeSpan wait, bool quiet, CancellationToken ct)
    {
        await _turn.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(_closed != 0, this);
            Capture capture = new(line, quiet);
            lock (_lock)
            {
                _capture = capture;
            }
            try
            {
                await _stream.WriteAsync(Encoding.UTF8.GetBytes(line + "\r\n"), ct).ConfigureAwait(false);
                Task finished = await Task.WhenAny(capture.Done.Task, Task.Delay(wait, ct)).ConfigureAwait(false);
                ct.ThrowIfCancellationRequested();
                if (finished == capture.Done.Task)
                {
                    return await capture.Done.Task.ConfigureAwait(false);
                }
                lock (_lock)
                {
                    return capture.Text.ToString();
                }
            }
            catch (Exception e) when (e is IOException or SocketException)
            {
                throw new BoardException($"Cannot send to {Address}: {e.Message}");
            }
            finally
            {
                lock (_lock)
                {
                    _capture = null;
                }
            }
        }
        finally
        {
            _turn.Release();
        }
    }

    /// <summary>
    /// The lines a command printed: its output without the echo of the
    /// command and without the prompt.
    /// </summary>
    public static List<string> OutputLines(string captured, string line)
    {
        List<string> lines = new();
        foreach (string raw in captured.Split('\n'))
        {
            string text = raw.TrimEnd();
            if (text.Length == 0 || text.Contains(line, StringComparison.Ordinal) || IsPrompt(text))
            {
                continue;
            }
            lines.Add(text);
        }
        return lines;
    }

    private static bool IsPrompt(string text) => text.TrimEnd().EndsWith('$');

    private async Task ReadLoopAsync()
    {
        byte[] buffer = new byte[4096];
        string? reason = null;
        try
        {
            while (true)
            {
                int n = await _stream.ReadAsync(buffer, _closing.Token).ConfigureAwait(false);
                if (n == 0)
                {
                    reason = "closed by the board";
                    break;
                }
                Interlocked.Exchange(ref _lastReceive, Environment.TickCount64);
                string text = _filter.Feed(buffer.AsSpan(0, n));
                lock (_lock)
                {
                    if (_quietTail)
                    {
                        text = text.TrimStart(' ');
                        _quietTail = text.Length == 0;
                    }
                    if (_capture is Capture capture && !capture.Done.Task.IsCompleted)
                    {
                        // The end of the last prompt can come after the line
                        // was sent: it is not the line's echo or output.
                        string before = "";
                        if (capture.Text.Length == 0 && capture.Echoed == 0)
                        {
                            string rest = text.TrimStart(' ');
                            before = text[..(text.Length - rest.Length)];
                            text = rest;
                        }
                        text = WithoutEcho(capture, text);
                        capture.Text.Append(text);
                        string all = capture.Text.ToString();
                        // Done when the prompt follows some output: a stale
                        // prompt alone must not end the wait.
                        if (IsPrompt(all) && OutputLines(all, capture.Line).Count > 0)
                        {
                            capture.Done.TrySetResult(all);
                            _quietTail = capture.Quiet && !all.EndsWith(' ');
                        }
                        text = capture.Quiet ? before : before + text;
                    }
                }
                if (text.Length > 0)
                {
                    Output?.Invoke(text);
                }
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception e) when (e is IOException or SocketException or ObjectDisposedException)
        {
            reason = e.Message;
        }
        if (Interlocked.Exchange(ref _closed, 1) == 0)
        {
            Closed?.Invoke(reason);
        }
    }

    /// <summary>
    /// Removes the board's echo of the line just sent, so that whoever shows
    /// the line itself does not show it twice. A board that does not echo
    /// loses nothing: the first character that differs ends the comparison.
    /// </summary>
    private static string WithoutEcho(Capture capture, string text)
    {
        int skip = 0;
        while (capture.Echoed >= 0 && skip < text.Length)
        {
            if (capture.Echoed < capture.Line.Length && text[skip] == capture.Line[capture.Echoed])
            {
                capture.Echoed++;
                skip++;
            }
            else
            {
                capture.Echoed = -1;
            }
        }
        return skip == 0 ? text : text[skip..];
    }

    public void Dispose()
    {
        bool first = Interlocked.Exchange(ref _closed, 1) == 0;
        _closing.Cancel();
        _client.Dispose();
        if (first)
        {
            Closed?.Invoke(null);
        }
    }
}
