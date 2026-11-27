// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using AudioStudio.Core.Audio;
using FluentAssertions;
using Xunit;

namespace AudioStudio.Core.Tests;

public sealed class AudioTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("audiostudio-test-").FullName;

    public void Dispose() => Directory.Delete(_dir, recursive: true);

    [Fact]
    public void Wav_round_trip_keeps_the_samples_and_fills_gaps_with_zeros()
    {
        string path = Path.Combine(_dir, "a.wav");
        short[] first = [1, -2, 32767, -32768];
        using (WavWriter writer = new(path, 16000))
        {
            writer.Write(first);
            writer.WriteGap(5000);
            writer.Write([7]);
            writer.Samples.Should().Be(5005);
            writer.GapSamples.Should().Be(5000);
            writer.Gaps.Should().Be(1);
        }
        byte[] file = File.ReadAllBytes(path);
        file.Length.Should().Be(44 + 5005 * 2);
        BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(4)).Should().Be((uint)(file.Length - 8));
        BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(40)).Should().Be(5005 * 2);
        WavData wav = WavFile.Read(path);
        wav.SampleRate.Should().Be(16000);
        wav.Samples.Should().HaveCount(5005);
        wav.Samples[..4].Should().Equal(first);
        wav.Samples[4..5004].Should().OnlyContain(s => s == 0);
        wav.Samples[^1].Should().Be(7);
    }

    [Fact]
    public void A_file_whose_writer_did_not_finish_reads_what_is_there()
    {
        string path = Path.Combine(_dir, "b.wav");
        WavWriter writer = new(path, 16000);
        writer.Write([1, 2, 3]);
        writer.Flush();
        writer.Write([4, 5]);
        // Read while the header still says three samples, as after a crash.
        using (FileStream f = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        {
            byte[] bytes = new byte[f.Length];
            f.ReadExactly(bytes);
            WavFile.Parse(bytes).Samples.Should().Equal(1, 2, 3);
        }
        writer.Dispose();
        WavFile.Read(path).Samples.Should().Equal(1, 2, 3, 4, 5);
    }

    private static byte[] Wav(ushort format, ushort channels, ushort bits, int rate = 16000, bool extraChunk = false)
    {
        List<byte> b = new();
        b.AddRange("RIFF"u8.ToArray());
        b.AddRange(new byte[4]);
        b.AddRange("WAVE"u8.ToArray());
        if (extraChunk)
        {
            b.AddRange("LIST"u8.ToArray());
            b.AddRange(BitConverter.GetBytes(3u));
            b.AddRange(new byte[4]); // three bytes and the pad byte
        }
        b.AddRange("fmt "u8.ToArray());
        b.AddRange(BitConverter.GetBytes(16u));
        b.AddRange(BitConverter.GetBytes(format));
        b.AddRange(BitConverter.GetBytes(channels));
        b.AddRange(BitConverter.GetBytes((uint)rate));
        b.AddRange(BitConverter.GetBytes((uint)(rate * channels * bits / 8)));
        b.AddRange(BitConverter.GetBytes((ushort)(channels * bits / 8)));
        b.AddRange(BitConverter.GetBytes(bits));
        b.AddRange("data"u8.ToArray());
        b.AddRange(BitConverter.GetBytes(4u));
        b.AddRange(new byte[] { 0x10, 0x00, 0xF0, 0xFF });
        return b.ToArray();
    }

    [Fact]
    public void Reads_other_rates_and_skips_unknown_chunks()
    {
        WavData wav = WavFile.Parse(Wav(1, 1, 16, 44100, extraChunk: true));
        wav.SampleRate.Should().Be(44100);
        wav.Samples.Should().Equal(16, -16);
    }

    [Theory]
    [InlineData(1, 2, 16, "*2 channels*")]
    [InlineData(1, 1, 24, "*24 bits*")]
    [InlineData(3, 1, 32, "*format code 3*")]
    public void Says_clearly_what_it_does_not_read(int format, int channels, int bits, string message)
    {
        Action parse = () => WavFile.Parse(Wav((ushort)format, (ushort)channels, (ushort)bits));
        parse.Should().Throw<WavFormatException>().WithMessage(message);
        Action notWav = () => WavFile.Parse("not a wav file at all"u8);
        notWav.Should().Throw<WavFormatException>().WithMessage("*not a WAV file*");
    }

    [Fact]
    public void Live_source_keeps_the_latest_samples_and_marks_gaps()
    {
        LiveSource live = new(16000, capacity: 1000);
        live.Append(Enumerable.Range(0, 600).Select(i => (short)i).ToArray());
        live.AppendGap(100);
        live.MarkNewStream();
        live.Append(Enumerable.Range(0, 600).Select(i => (short)(i + 1)).ToArray());
        live.End.Should().Be(1300);
        live.Start.Should().Be(300);
        float[] read = new float[4];
        live.Read(298, read);
        read[0].Should().Be(0f, "position 298 is no longer held");
        read[2].Should().Be(300 / 32768f);
        live.Read(599, read);
        read.Should().Equal(599 / 32768f, 0f, 0f, 0f);
        live.Read(700, read);
        read[0].Should().Be(1 / 32768f);
        List<Marker> markers = live.Markers(0, 2000);
        markers.Should().Equal(new Marker(600, 100, MarkerKind.Gap), new Marker(700, 0, MarkerKind.NewStream));
        live.Markers(0, 500).Should().BeEmpty();
    }
}
