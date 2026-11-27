// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using AudioStudio.Core.Stream;
using FluentAssertions;
using Xunit;

namespace AudioStudio.Core.Tests;

public class StreamTests
{
    internal static byte[] Header(int rate = 16000, byte version = 1, byte format = 1, string magic = "XAUD")
    {
        byte[] h = new byte[16];
        System.Text.Encoding.ASCII.GetBytes(magic).CopyTo(h, 0);
        h[4] = version;
        h[5] = format;
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(8), (uint)rate);
        return h;
    }

    internal static byte[] Block(uint index, short[] samples, ushort flags = 0)
    {
        byte[] b = new byte[8 + samples.Length * 2];
        BinaryPrimitives.WriteUInt32LittleEndian(b, index);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(4), (ushort)samples.Length);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(6), flags);
        for (int i = 0; i < samples.Length; i++)
        {
            BinaryPrimitives.WriteInt16LittleEndian(b.AsSpan(8 + i * 2), samples[i]);
        }
        return b;
    }

    private static short[] Ramp(int start, int count) => Enumerable.Range(start, count).Select(i => (short)i).ToArray();

    /// <summary>A stream with each case the parser must tell apart.</summary>
    private static byte[] Sample()
    {
        return
        [
            .. Header(),
            .. Block(0, [], AudioStreamParser.FlagIdle),                 // keep-alive
            .. Block(0, Ramp(0, 320), AudioStreamParser.FlagNewStream),   // a stream starts
            .. Block(320, Ramp(320, 320)),
            .. Block(1000, Ramp(1000, 5)),                                // 360 lost
            .. Block(1005, [], AudioStreamParser.FlagIdle),
        ];
    }

    private static void Check(List<AudioBlock> blocks, AudioStreamParser parser)
    {
        parser.Header.Should().Be(new AudioStreamHeader(1, 1, 16000));
        blocks.Should().HaveCount(5);
        blocks[0].Idle.Should().BeTrue();
        blocks[0].Samples.Should().BeEmpty();
        blocks[0].Lost.Should().Be(0);
        blocks[1].NewStream.Should().BeTrue();
        blocks[1].Idle.Should().BeFalse();
        blocks[1].Samples.Should().Equal(Ramp(0, 320));
        blocks[2].Lost.Should().Be(0);
        blocks[2].Samples[^1].Should().Be(639);
        blocks[3].Lost.Should().Be(360);
        blocks[3].Samples.Should().Equal(Ramp(1000, 5));
        blocks[4].Lost.Should().Be(0);
        blocks[4].Idle.Should().BeTrue();
    }

    [Fact]
    public void Parses_header_blocks_gaps_keep_alives_and_new_streams()
    {
        AudioStreamParser parser = new();
        List<AudioBlock> blocks = new();
        parser.Feed(Sample(), blocks.Add);
        Check(blocks, parser);
    }

    [Fact]
    public void Parses_the_same_when_split_at_every_byte_boundary()
    {
        byte[] data = Sample();
        for (int split = 0; split <= data.Length; split++)
        {
            AudioStreamParser parser = new();
            List<AudioBlock> blocks = new();
            parser.Feed(data.AsSpan(0, split), blocks.Add);
            parser.Feed(data.AsSpan(split), blocks.Add);
            Check(blocks, parser);
        }
    }

    [Fact]
    public void Parses_the_same_one_byte_at_a_time()
    {
        byte[] data = Sample();
        AudioStreamParser parser = new();
        List<AudioBlock> blocks = new();
        foreach (byte b in data)
        {
            parser.Feed([b], blocks.Add);
        }
        Check(blocks, parser);
    }

    [Fact]
    public void Counts_across_the_wrap_of_the_index()
    {
        AudioStreamParser parser = new();
        List<AudioBlock> blocks = new();
        parser.Feed(Header(), blocks.Add);
        // Up to just before 2^32 in jumps the parser takes (each below 2^31).
        parser.Feed(Block(0x7FFF_0000, []), blocks.Add);
        parser.Feed(Block(0xF000_0000, []), blocks.Add);
        parser.Feed(Block(uint.MaxValue - 99, Ramp(0, 100)), blocks.Add);
        parser.Feed(Block(0, Ramp(0, 50)), blocks.Add);    // 2^32 is 0: continuous
        parser.Feed(Block(60, Ramp(0, 10)), blocks.Add);   // 10 lost after the wrap
        blocks[0].Lost.Should().Be(0x7FFF_0000);
        blocks[^3].Samples.Should().HaveCount(100);
        blocks[^2].Lost.Should().Be(0);
        blocks[^1].Lost.Should().Be(10);
    }

    [Fact]
    public void Rejects_what_is_not_the_protocol()
    {
        Action badMagic = () => new AudioStreamParser().Feed(Header(magic: "XOTA"), _ => { });
        badMagic.Should().Throw<AudioStreamException>().WithMessage("*XAUD*");
        Action badVersion = () => new AudioStreamParser().Feed(Header(version: 2), _ => { });
        badVersion.Should().Throw<AudioStreamException>().WithMessage("*version 2*");
        Action badFormat = () => new AudioStreamParser().Feed(Header(format: 3), _ => { });
        badFormat.Should().Throw<AudioStreamException>();
        byte[] tooLong = Block(0, []);
        BinaryPrimitives.WriteUInt16LittleEndian(tooLong.AsSpan(4), 1025);
        Action bigBlock = () => new AudioStreamParser().Feed([.. Header(), .. tooLong], _ => { });
        bigBlock.Should().Throw<AudioStreamException>().WithMessage("*1025*");
        Action back = () => new AudioStreamParser().Feed([.. Header(), .. Block(100, [1]), .. Block(50, [1])], _ => { });
        back.Should().Throw<AudioStreamException>().WithMessage("*went back*");
    }

    [Fact]
    public async Task Client_receives_counts_and_connects_again_after_the_board_closes()
    {
        TcpListener listener = new(IPAddress.Loopback, 0);
        listener.Start();
        int port = ((IPEndPoint)listener.LocalEndpoint).Port;
        Task server = Task.Run(async () =>
        {
            for (int connection = 0; connection < 2; connection++)
            {
                using TcpClient client = await listener.AcceptTcpClientAsync();
                NetworkStream stream = client.GetStream();
                byte[] data = [.. Header(), .. Block(0, Ramp(0, 320), AudioStreamParser.FlagNewStream),
                               .. Block(640, Ramp(0, 320)), .. Block(960, [], AudioStreamParser.FlagIdle)];
                // In pieces that cut through headers and samples.
                for (int i = 0; i < data.Length; i += 37)
                {
                    await stream.WriteAsync(data.AsMemory(i, Math.Min(37, data.Length - i)));
                    await stream.FlushAsync();
                }
                await Task.Delay(100);
            }
        });
        try
        {
            using AudioStreamClient client = new("127.0.0.1", port);
            int headers = 0;
            List<AudioBlock> blocks = new();
            TaskCompletionSource done = new(TaskCreationOptions.RunContinuationsAsynchronously);
            client.Connected += _ => Interlocked.Increment(ref headers);
            client.BlockReceived += b =>
            {
                lock (blocks)
                {
                    blocks.Add(b);
                    if (blocks.Count == 6)
                    {
                        done.TrySetResult();
                    }
                }
            };
            client.Start();
            await done.Task.WaitAsync(TimeSpan.FromSeconds(10));
            headers.Should().Be(2);
            AudioStreamStatus status = client.Status();
            status.Connections.Should().Be(2);
            status.Samples.Should().Be(4 * 320);
            status.LostSamples.Should().Be(2 * 320);
            status.Gaps.Should().Be(2);
            status.NewStreams.Should().Be(2);
            status.Header!.SampleRate.Should().Be(16000);
        }
        finally
        {
            listener.Stop();
            await Task.WhenAny(server, Task.Delay(2000));
        }
    }

    [Fact]
    public async Task Client_says_why_a_connection_ended()
    {
        TcpListener listener = new(IPAddress.Loopback, 0);
        listener.Start();
        int port = ((IPEndPoint)listener.LocalEndpoint).Port;
        // The board closes a second client at once, before any header.
        Task server = Task.Run(async () =>
        {
            using TcpClient client = await listener.AcceptTcpClientAsync();
        });
        try
        {
            using AudioStreamClient client = new("127.0.0.1", port);
            TaskCompletionSource<string> ended = new(TaskCreationOptions.RunContinuationsAsynchronously);
            client.Disconnected += reason => ended.TrySetResult(reason);
            client.Start();
            (await ended.Task.WaitAsync(TimeSpan.FromSeconds(5))).Should().Contain("before the header");
            client.Status().State.Should().Be(AudioStreamState.Waiting);
        }
        finally
        {
            listener.Stop();
            await Task.WhenAny(server, Task.Delay(1000));
        }
    }
}
