// SPDX-License-Identifier: MIT
using System.Net.Sockets;
using System.Text;
using AudioStudio.Core.Ssh;

namespace AudioStudio.Core.Board;

/// <summary>
/// One shell session on the target, kept for as long as the application
/// talks to it: a Telnet connection (the XIAO accepts a single Telnet client
/// and stops accepting when connections are opened and closed in quick
/// succession, so nothing here reconnects per command), or an interactive
/// ssh session on a terminal, whose quiet queries and button commands run as
/// channels of their own on the same ssh connection.
/// </summary>
public sealed class BoardShell : IDisposable
{
    private readonly System.IO.Stream _from;
    private readonly System.IO.Stream _to;
    private readonly string _newline;
    private readonly Action _close;
    private readonly Func<string> _endReason;
    private readonly SshConnection? _ssh;
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
    public ShellPrompt Prompt { get; }

    private BoardShell(string address, ShellPrompt prompt, System.IO.Stream from, System.IO.Stream to, string newline,
                       Action close, Func<string> endReason, SshConnection? ssh)
    {
        Address = address;
        Prompt = prompt;
        _from = from;
        _to = to;
        _newline = newline;
        _close = close;
        _endReason = endReason;
        _ssh = ssh;
    }

    /// <summary>Connects to a Telnet shell.</summary>
    public static async Task<BoardShell> ConnectAsync(string address, int port, ShellPrompt prompt,
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
        NetworkStream stream = client.GetStream();
        BoardShell shell = new(address, prompt, stream, stream, "\r\n", client.Dispose, () => "closed by the board", null);
        _ = Task.Run(shell.ReadLoopAsync, CancellationToken.None);
        return shell;
    }

    /// <summary>
    /// Opens an interactive login shell over <paramref name="ssh"/> (which it
    /// holds a lease of until it is closed).
    /// </summary>
    public static async Task<BoardShell> OpenSshAsync(SshConnection ssh, ShellPrompt prompt, CancellationToken ct = default)
    {
        IDisposable lease;
        try
        {
            lease = await ssh.AcquireAsync(ct).ConfigureAwait(false);
        }
        catch (SshException e)
        {
            throw new BoardException(e.Message);
        }
        SshProcess process;
        try
        {
            process = ssh.Start(null);
        }
        catch (SshException e)
        {
            lease.Dispose();
            throw new BoardException(e.Message);
        }
        void Close()
        {
            process.Dispose();
            lease.Dispose();
        }
        string EndReason()
        {
            try
            {
                process.WaitAsync(TimeSpan.FromSeconds(2)).GetAwaiter().GetResult();
            }
            catch (InvalidOperationException)
            {
            }
            return process.ExitCode == 0 ? "the session ended" : ssh.Explain(process.ExitCode, process.ErrorTail());
        }
        // A terminal takes a line ending with the Enter key's character.
        BoardShell shell = new(ssh.Destination, prompt, process.Output, process.Input, "\n", Close, EndReason, ssh);
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
    /// Sends a line to the session and returns what the board printed until
    /// its prompt came back, or until <paramref name="wait"/> passed. Calls
    /// are served one at a time.
    /// </summary>
    public Task<string> ExecuteAsync(string line, TimeSpan wait, CancellationToken ct = default) =>
        RunAsync(line, wait, quiet: false, ct);

    /// <summary>
    /// Runs a line for the application's own reading of the board's state
    /// and returns the lines it printed (<see cref="ShellPrompt.OutputLines"/>);
    /// nothing of it is raised as <see cref="Output"/>. Over Telnet it is sent
    /// in the session (and waits behind a command that is running), over ssh
    /// it runs as a channel of its own.
    /// </summary>
    public async Task<IReadOnlyList<string>> QueryAsync(string line, TimeSpan wait, CancellationToken ct = default) =>
        _ssh is null
            ? Prompt.OutputLines(await RunAsync(line, wait, quiet: true, ct).ConfigureAwait(false), line)
            : Lines(await ChannelAsync(line, wait, ct).ConfigureAwait(false));

    /// <summary>
    /// Runs a button's command and returns the lines it printed. Over Telnet
    /// it is sent in the session, as <see cref="ExecuteAsync"/>; over ssh it
    /// runs as a channel of its own, and what it printed is raised as
    /// <see cref="Output"/>.
    /// </summary>
    public async Task<IReadOnlyList<string>> CommandAsync(string line, TimeSpan wait, CancellationToken ct = default)
    {
        if (_ssh is null)
        {
            return Prompt.OutputLines(await RunAsync(line, wait, quiet: false, ct).ConfigureAwait(false), line);
        }
        string output = await ChannelAsync(line, wait, ct).ConfigureAwait(false);
        Output?.Invoke("\n" + output.Replace("\r", "", StringComparison.Ordinal));
        return Lines(output);
    }

    private async Task<string> ChannelAsync(string line, TimeSpan wait, CancellationToken ct)
    {
        ObjectDisposedException.ThrowIf(_closed != 0, this);
        try
        {
            SshResult result = await _ssh!.RunAsync(line, wait, ct).ConfigureAwait(false);
            if (result.ExitCode == 255)
            {
                throw new BoardException(_ssh.Explain(result.ExitCode, result.Errors));
            }
            return result.Output;
        }
        catch (SshException e)
        {
            throw new BoardException(e.Message);
        }
    }

    private static List<string> Lines(string output) =>
        output.Split('\n').Select(l => l.TrimEnd()).Where(l => l.Length > 0).ToList();

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
                await _to.WriteAsync(Encoding.UTF8.GetBytes(line + _newline), ct).ConfigureAwait(false);
                await _to.FlushAsync(ct).ConfigureAwait(false);
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

    private async Task ReadLoopAsync()
    {
        byte[] buffer = new byte[4096];
        string? reason = null;
        try
        {
            while (true)
            {
                int n = await _from.ReadAsync(buffer, _closing.Token).ConfigureAwait(false);
                if (n == 0)
                {
                    reason = _closing.IsCancellationRequested ? null : _endReason();
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
                        if (Prompt.IsPrompt(all) && Prompt.OutputLines(all, capture.Line).Count > 0)
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
        _close();
        if (first)
        {
            Closed?.Invoke(null);
        }
    }
}
