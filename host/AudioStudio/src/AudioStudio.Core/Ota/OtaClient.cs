// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using System.Diagnostics;
using System.Net.Sockets;
using System.Text;

namespace AudioStudio.Core.Ota;

/// <summary>The board refused or failed the update ("ERR ..."), or answered something unexpected.</summary>
public sealed class OtaException(string message) : Exception(message);

/// <summary>How long each step may take. The defaults are those of <c>script/ota.py</c>.</summary>
public sealed record OtaTimings
{
    /// <summary>The answer to the header (the board may stop a USB audio stream first).</summary>
    public TimeSpan Header { get; init; } = TimeSpan.FromSeconds(15);
    /// <summary>One block of the image (a sector erase and the network).</summary>
    public TimeSpan Send { get; init; } = TimeSpan.FromSeconds(20);
    /// <summary>The check after the image (read back twice, with its SHA-256).</summary>
    public TimeSpan Result { get; init; } = TimeSpan.FromSeconds(60);
    public TimeSpan Connect { get; init; } = TimeSpan.FromSeconds(3);
    /// <summary>After the restart, before the first attempt to confirm.</summary>
    public TimeSpan RestartDelay { get; init; } = TimeSpan.FromSeconds(2);
    public TimeSpan RetryDelay { get; init; } = TimeSpan.FromSeconds(1);
    /// <summary>How long the new image may take to answer after the restart.</summary>
    public TimeSpan Wait { get; init; } = TimeSpan.FromSeconds(60);
}

public enum OtaOutcome
{
    /// <summary>The new image runs and is confirmed.</summary>
    Confirmed,
    /// <summary>Another image runs after the restart: the new one did not start and the previous one is back.</summary>
    OtherImage,
}

/// <param name="Slot">The slot written.</param>
/// <param name="Answer">The board's last line, without "OK" or "ERR other".</param>
public sealed record OtaResult(OtaOutcome Outcome, string Slot, string Answer);

/// <summary>
/// The client of the board's OTA service (TCP port 3232,
/// <c>shell/port/ota_service.c</c>): the same sequence as <c>script/ota.py</c>.
/// A header of 16 bytes ("XOTA", version 1, the command, flags, then the
/// image's size or the expected ELF SHA-256's first 8 bytes), the image, and
/// one line of text from the board after the header and after the image.
/// </summary>
public sealed class OtaClient(string host, int port = OtaClient.DefaultPort, OtaTimings? timings = null)
{
    public const int DefaultPort = 3232;
    private const byte Version = 1;
    private const byte CommandWrite = 1;
    private const byte CommandConfirm = 2;
    private const byte FlagRestart = 0x01;
    private const int Chunk = 4096;

    private readonly OtaTimings _t = timings ?? new OtaTimings();

    /// <summary>
    /// Writes the image, restarts the board and confirms the new image once it
    /// runs. <paramref name="log"/> gets a line per step, <paramref name="sent"/>
    /// the fraction of the image sent.
    /// </summary>
    /// <exception cref="OtaException">The board refused or failed the update.</exception>
    /// <exception cref="IOException">The connection failed, or the board did not come back.</exception>
    public async Task<OtaResult> UpdateAsync(AppImage image, IProgress<string>? log, IProgress<double>? sent,
                                             CancellationToken ct = default)
    {
        string slot = await WriteAsync(image, restart: true, log, sent, ct).ConfigureAwait(false);
        log?.Report("Restarting; waiting for the new image");
        string line = await ConfirmAsync(image.ElfShaPrefix, ct).ConfigureAwait(false);
        if (line.StartsWith("ERR other", StringComparison.Ordinal))
        {
            string other = line["ERR other".Length..].Trim();
            log?.Report($"Another image runs ({other}): the new image did not start and the previous one is back");
            return new OtaResult(OtaOutcome.OtherImage, slot, other);
        }
        string answer = ExpectOk(line, "to the confirmation");
        log?.Report("Confirmed: " + answer);
        return new OtaResult(OtaOutcome.Confirmed, slot, answer);
    }

    /// <summary>Sends the image; returns the slot it went to.</summary>
    public async Task<string> WriteAsync(AppImage image, bool restart, IProgress<string>? log,
                                         IProgress<double>? sent, CancellationToken ct = default)
    {
        using TcpClient client = await ConnectAsync(_t.Connect * 3, ct).ConfigureAwait(false);
        NetworkStream stream = client.GetStream();
        byte[] tail = new byte[8];
        BinaryPrimitives.WriteUInt32LittleEndian(tail, (uint)image.Bytes.Length);
        await stream.WriteAsync(Header(CommandWrite, restart ? FlagRestart : (byte)0, tail), ct).ConfigureAwait(false);
        LineReader reader = new(stream);
        string slot = ExpectOk(await reader.ReadLineAsync(_t.Header, ct).ConfigureAwait(false), "to the header");
        log?.Report($"Writing {image.Bytes.Length} bytes to {slot}");
        Stopwatch watch = Stopwatch.StartNew();

        // The next line from the board ends the transfer: an "ERR" while the
        // image goes, or the result once it is in.
        using CancellationTokenSource readCancel = CancellationTokenSource.CreateLinkedTokenSource(ct);
        Task<string> answer = reader.ReadLineAsync(Timeout.InfiniteTimeSpan, readCancel.Token);
        try
        {
            for (int pos = 0; pos < image.Bytes.Length; pos += Chunk)
            {
                if (answer.IsCompleted)
                {
                    throw new OtaException($"the board answered '{await answer.ConfigureAwait(false)}' after {pos} bytes");
                }
                int n = Math.Min(Chunk, image.Bytes.Length - pos);
                using CancellationTokenSource sendTimeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
                sendTimeout.CancelAfter(_t.Send);
                try
                {
                    await stream.WriteAsync(image.Bytes.AsMemory(pos, n), sendTimeout.Token).ConfigureAwait(false);
                }
                catch (Exception e) when (e is IOException or SocketException ||
                                          (e is OperationCanceledException && !ct.IsCancellationRequested))
                {
                    string line = await Task.WhenAny(answer, Task.Delay(2000, ct)).ConfigureAwait(false) == answer &&
                                  answer.IsCompletedSuccessfully ? answer.Result : "";
                    throw new OtaException(line.Length > 0
                        ? $"the board answered '{line}' after {pos} bytes"
                        : $"sending failed after {pos} bytes: {(e is OperationCanceledException ? "timed out" : e.Message)}");
                }
                sent?.Report((double)(pos + n) / image.Bytes.Length);
            }
            log?.Report("Sent; the board checks the image");
            if (await Task.WhenAny(answer, Task.Delay(_t.Result, ct)).ConfigureAwait(false) != answer)
            {
                ct.ThrowIfCancellationRequested();
                throw new IOException($"no answer within {_t.Result.TotalSeconds:0} s after the image");
            }
            ExpectOk(await answer.ConfigureAwait(false), "after the image");
        }
        finally
        {
            readCancel.Cancel();
            try
            {
                await answer.ConfigureAwait(false);
            }
            catch (Exception e) when (e is OperationCanceledException or IOException or SocketException or ObjectDisposedException)
            {
            }
        }
        log?.Report($"Written and checked in {watch.Elapsed.TotalSeconds:F1} s; {slot} boots next");
        return slot;
    }

    /// <summary>
    /// Asks the board, until it answers, to confirm the image with this ELF
    /// SHA-256 prefix; returns its answer ("OK ..." or "ERR ...").
    /// </summary>
    public async Task<string> ConfirmAsync(byte[] shaPrefix, CancellationToken ct = default)
    {
        Stopwatch watch = Stopwatch.StartNew();
        await Task.Delay(_t.RestartDelay, ct).ConfigureAwait(false);
        string last = "no connection";
        while (watch.Elapsed < _t.Wait)
        {
            string line;
            try
            {
                using TcpClient client = await ConnectAsync(_t.Connect, ct).ConfigureAwait(false);
                NetworkStream stream = client.GetStream();
                await stream.WriteAsync(Header(CommandConfirm, 0, shaPrefix), ct).ConfigureAwait(false);
                line = await new LineReader(stream).ReadLineAsync(_t.Header, ct).ConfigureAwait(false);
            }
            catch (Exception e) when (e is IOException or SocketException)
            {
                last = e.Message;
                await Task.Delay(_t.RetryDelay, ct).ConfigureAwait(false);
                continue;
            }
            if (line.Length == 0 || line.StartsWith("ERR busy", StringComparison.Ordinal))
            {
                last = line.Length > 0 ? line : "closed";
                await Task.Delay(_t.RetryDelay, ct).ConfigureAwait(false);
                continue;
            }
            return line;
        }
        throw new IOException($"the board did not answer within {_t.Wait.TotalSeconds:0} s ({last})");
    }

    private static byte[] Header(byte command, byte flags, ReadOnlySpan<byte> tail)
    {
        byte[] h = new byte[16];
        "XOTA"u8.CopyTo(h);
        h[4] = Version;
        h[5] = command;
        h[6] = flags;
        tail.CopyTo(h.AsSpan(8));
        return h;
    }

    private async Task<TcpClient> ConnectAsync(TimeSpan timeout, CancellationToken ct)
    {
        TcpClient client = new() { NoDelay = true };
        using CancellationTokenSource cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        cts.CancelAfter(timeout);
        try
        {
            await client.ConnectAsync(host, port, cts.Token).ConfigureAwait(false);
            return client;
        }
        catch (Exception e) when (e is SocketException || (e is OperationCanceledException && !ct.IsCancellationRequested))
        {
            client.Dispose();
            throw new IOException($"{host}:{port}: " + (e is SocketException ? e.Message : "connection timed out"));
        }
        catch
        {
            client.Dispose();
            throw;
        }
    }

    private static string ExpectOk(string line, string what)
    {
        if (line.StartsWith("OK", StringComparison.Ordinal))
        {
            return line[2..].Trim();
        }
        if (line.Length == 0)
        {
            throw new OtaException($"the board closed the connection {what}");
        }
        throw new OtaException($"the board answered '{line}' {what}");
    }

    /// <summary>Reads lines of ASCII text ending in "\n"; keeps what follows a line for the next one.</summary>
    private sealed class LineReader(NetworkStream stream)
    {
        private readonly byte[] _buffer = new byte[256];
        private readonly StringBuilder _pending = new();

        /// <summary>One line without its end; "" if the board closed first.</summary>
        public async Task<string> ReadLineAsync(TimeSpan timeout, CancellationToken ct)
        {
            using CancellationTokenSource cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            if (timeout != Timeout.InfiniteTimeSpan)
            {
                cts.CancelAfter(timeout);
            }
            while (true)
            {
                string text = _pending.ToString();
                int end = text.IndexOf('\n', StringComparison.Ordinal);
                if (end >= 0)
                {
                    _pending.Remove(0, end + 1);
                    return text[..end].Trim();
                }
                int n;
                try
                {
                    n = await stream.ReadAsync(_buffer, cts.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (!ct.IsCancellationRequested)
                {
                    throw new IOException($"no answer within {timeout.TotalSeconds:0} s");
                }
                if (n == 0)
                {
                    string rest = _pending.ToString().Trim();
                    _pending.Clear();
                    return rest;
                }
                _pending.Append(Encoding.ASCII.GetString(_buffer, 0, n));
            }
        }
    }
}
