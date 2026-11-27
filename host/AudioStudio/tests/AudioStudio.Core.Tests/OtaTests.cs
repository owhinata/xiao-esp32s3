// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;
using AudioStudio.Core.Ota;
using FluentAssertions;
using Xunit;

namespace AudioStudio.Core.Tests;

public class OtaTests
{
    private static readonly OtaTimings Fast = new()
    {
        Header = TimeSpan.FromSeconds(5), Send = TimeSpan.FromSeconds(5), Result = TimeSpan.FromSeconds(5),
        Connect = TimeSpan.FromSeconds(1), RestartDelay = TimeSpan.FromMilliseconds(50),
        RetryDelay = TimeSpan.FromMilliseconds(50), Wait = TimeSpan.FromSeconds(5),
    };

    internal static AppImage Image(int size = 20000, string version = "v1.2-3-gabc", string project = "xiao_esp32s3_threadx")
    {
        byte[] b = new byte[size];
        new Random(1).NextBytes(b);
        b[0] = 0xE9;
        b[23] = 1;
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(0x20), 0xABCD5432);
        void Text(int offset, int length, string text)
        {
            Array.Clear(b, offset, length);
            Encoding.ASCII.GetBytes(text).CopyTo(b, offset);
        }
        Text(0x20 + 16, 32, version);
        Text(0x20 + 48, 32, project);
        Text(0x20 + 80, 16, "12:34:56");
        Text(0x20 + 96, 16, "Nov 27 2026");
        for (int i = 0; i < 32; i++)
        {
            b[0xB0 + i] = (byte)(0xA0 + i);
        }
        return AppImage.FromBytes("test.bin", b);
    }

    /// <summary>
    /// The board's OTA service as shell/port/ota_service.c answers: one
    /// command per connection. <paramref name="confirm"/> gives the answer to
    /// each confirmation in turn (null: close without a word, as a board
    /// still restarting would refuse the connection).
    /// </summary>
    private sealed class FakeBoard : IDisposable
    {
        private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
        private readonly Task _server;
        public int Port { get; }
        public List<byte[]> Headers { get; } = new();
        public byte[]? Received { get; private set; }

        public FakeBoard(Func<int, string?> confirm, string afterHeader = "OK ota_1", string afterImage = "OK",
                         int failAfter = -1)
        {
            _listener.Start();
            Port = ((IPEndPoint)_listener.LocalEndpoint).Port;
            _server = Task.Run(async () =>
            {
                int confirmations = 0;
                while (true)
                {
                    TcpClient client;
                    try
                    {
                        client = await _listener.AcceptTcpClientAsync();
                    }
                    catch (Exception e) when (e is SocketException or ObjectDisposedException)
                    {
                        return;
                    }
                    using (client)
                    {
                        NetworkStream s = client.GetStream();
                        byte[] h = new byte[16];
                        await s.ReadExactlyAsync(h);
                        lock (Headers)
                        {
                            Headers.Add(h);
                        }
                        async Task Line(string text) => await s.WriteAsync(Encoding.ASCII.GetBytes(text + "\n"));
                        if (h[5] == 1)
                        {
                            await Line(afterHeader);
                            if (!afterHeader.StartsWith("OK", StringComparison.Ordinal))
                            {
                                continue;
                            }
                            int size = (int)BinaryPrimitives.ReadUInt32LittleEndian(h.AsSpan(8));
                            byte[] image = new byte[size];
                            int got = 0;
                            while (got < size)
                            {
                                if (failAfter >= 0 && got >= failAfter)
                                {
                                    await Line("ERR flash");
                                    // Drop what still comes, as the board does, then close.
                                    try
                                    {
                                        while (await s.ReadAsync(image).AsTask().WaitAsync(TimeSpan.FromMilliseconds(300)) > 0)
                                        {
                                        }
                                    }
                                    catch (Exception e) when (e is TimeoutException or IOException)
                                    {
                                    }
                                    break;
                                }
                                int n = await s.ReadAsync(image.AsMemory(got));
                                if (n == 0)
                                {
                                    break;
                                }
                                got += n;
                            }
                            if (got == size)
                            {
                                Received = image;
                                await Line(afterImage);
                            }
                        }
                        else if (h[5] == 2)
                        {
                            string? answer = confirm(confirmations++);
                            if (answer is not null)
                            {
                                await Line(answer);
                            }
                        }
                    }
                }
            });
        }

        public void Dispose()
        {
            _listener.Stop();
            _server.Wait(TimeSpan.FromSeconds(2));
        }
    }

    [Fact]
    public void Reads_the_application_description()
    {
        AppImage image = Image();
        image.Project.Should().Be("xiao_esp32s3_threadx");
        image.Version.Should().Be("v1.2-3-gabc");
        image.Built.Should().Be("Nov 27 2026 12:34:56");
        image.ElfShaHex.Should().Be("a0a1a2a3a4a5a6a7");
        byte[] bad = (byte[])image.Bytes.Clone();
        bad[23] = 0;
        Action noHash = () => AppImage.FromBytes("x.bin", bad);
        noHash.Should().Throw<AppImageException>().WithMessage("*no SHA-256 appended*");
        Action notImage = () => AppImage.FromBytes("x.bin", new byte[300]);
        notImage.Should().Throw<AppImageException>().WithMessage("*not an ESP application image*");
    }

    [Fact]
    public async Task Writes_restarts_and_confirms_as_ota_py_does()
    {
        AppImage image = Image();
        // Restarting: refused twice, then busy, then the new image answers.
        using FakeBoard board = new(n => n switch
        {
            0 or 1 => null,
            2 => "ERR busy",
            _ => "OK ota_1 valid a0a1a2a3a4a5a6a7",
        });
        List<string> log = new();
        List<double> sent = new();
        OtaClient client = new("127.0.0.1", board.Port, Fast);
        OtaResult result = await client.UpdateAsync(image, new SyncProgress<string>(log.Add),
                                                    new SyncProgress<double>(sent.Add));
        result.Should().Be(new OtaResult(OtaOutcome.Confirmed, "ota_1", "ota_1 valid a0a1a2a3a4a5a6a7"));
        board.Received.Should().Equal(image.Bytes);
        board.Headers.Should().HaveCount(5);
        byte[] write = board.Headers[0];
        Encoding.ASCII.GetString(write, 0, 4).Should().Be("XOTA");
        write[4..8].Should().Equal(1, 1, 1, 0);
        BinaryPrimitives.ReadUInt32LittleEndian(write.AsSpan(8)).Should().Be((uint)image.Bytes.Length);
        byte[] confirm = board.Headers[^1];
        confirm[4..8].Should().Equal(1, 2, 0, 0);
        confirm[8..].Should().Equal(image.ElfShaPrefix);
        sent[^1].Should().Be(1.0);
        log.Should().ContainMatch("Writing 20000 bytes to ota_1");
        log.Should().ContainMatch("Confirmed: ota_1 valid*");
    }

    [Fact]
    public async Task Reports_another_image_after_the_restart()
    {
        using FakeBoard board = new(_ => "ERR other ota_0 valid 0011223344556677");
        OtaResult result = await new OtaClient("127.0.0.1", board.Port, Fast).UpdateAsync(Image(), null, null);
        result.Outcome.Should().Be(OtaOutcome.OtherImage);
        result.Answer.Should().Be("ota_0 valid 0011223344556677");
    }

    [Fact]
    public async Task Stops_at_a_refusal_of_the_header()
    {
        using FakeBoard board = new(_ => null, afterHeader: "ERR pending");
        Func<Task> update = () => new OtaClient("127.0.0.1", board.Port, Fast).UpdateAsync(Image(), null, null);
        await update.Should().ThrowAsync<OtaException>().WithMessage("*'ERR pending' to the header*");
    }

    [Fact]
    public async Task Stops_at_an_error_during_the_image()
    {
        using FakeBoard board = new(_ => null, failAfter: 8192);
        Func<Task> update = () => new OtaClient("127.0.0.1", board.Port, Fast).UpdateAsync(Image(200_000), null, null);
        await update.Should().ThrowAsync<OtaException>().WithMessage("*'ERR flash' after*");
    }

    [Fact]
    public async Task Stops_at_a_failed_check()
    {
        using FakeBoard board = new(_ => null, afterImage: "ERR verify");
        Func<Task> update = () => new OtaClient("127.0.0.1", board.Port, Fast).UpdateAsync(Image(), null, null);
        await update.Should().ThrowAsync<OtaException>().WithMessage("*'ERR verify' after the image*");
    }

    [Fact]
    public async Task Gives_up_when_the_board_does_not_come_back()
    {
        using FakeBoard board = new(_ => null);
        OtaClient client = new("127.0.0.1", board.Port, Fast with { Wait = TimeSpan.FromSeconds(1) });
        Func<Task> update = () => client.UpdateAsync(Image(), null, null);
        await update.Should().ThrowAsync<IOException>().WithMessage("*did not answer*");
    }

    /// <summary>A progress that reports on the calling thread, so the list is complete when the call returns.</summary>
    private sealed class SyncProgress<T>(Action<T> report) : IProgress<T>
    {
        public void Report(T value)
        {
            lock (this)
            {
                report(value);
            }
        }
    }
}
