// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using System.Text;

namespace AudioStudio.Core.Audio;

/// <summary>A file that is not a WAV file this application reads.</summary>
public sealed class WavFormatException(string message) : Exception(message);

public sealed record WavData(int SampleRate, short[] Samples);

/// <summary>Reading of 16-bit PCM mono WAV files (RIFF, "fmt " and "data" chunks).</summary>
public static class WavFile
{
    private const ushort FormatPcm = 1;
    private const ushort FormatExtensible = 0xFFFE;
    private const long MaxSamples = int.MaxValue;

    public static WavData Read(string path)
    {
        byte[] file;
        try
        {
            file = File.ReadAllBytes(path);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            throw new WavFormatException($"Cannot read {path}: {e.Message}");
        }
        return Parse(file, Path.GetFileName(path));
    }

    public static WavData Parse(ReadOnlySpan<byte> file, string name = "the file")
    {
        if (file.Length < 12 || !file[..4].SequenceEqual("RIFF"u8) || !file[8..12].SequenceEqual("WAVE"u8))
        {
            throw new WavFormatException($"{name} is not a WAV file (no RIFF/WAVE header).");
        }
        int pos = 12;
        int? rate = null;
        while (pos + 8 <= file.Length)
        {
            ReadOnlySpan<byte> id = file.Slice(pos, 4);
            uint size = BinaryPrimitives.ReadUInt32LittleEndian(file[(pos + 4)..]);
            int body = pos + 8;
            if (id.SequenceEqual("fmt "u8))
            {
                if (size < 16 || body + 16 > file.Length)
                {
                    throw new WavFormatException($"{name}: the format chunk is too short.");
                }
                ReadOnlySpan<byte> f = file.Slice(body, 16);
                ushort format = BinaryPrimitives.ReadUInt16LittleEndian(f);
                ushort channels = BinaryPrimitives.ReadUInt16LittleEndian(f[2..]);
                int sampleRate = (int)BinaryPrimitives.ReadUInt32LittleEndian(f[4..]);
                ushort bits = BinaryPrimitives.ReadUInt16LittleEndian(f[14..]);
                if (format == FormatExtensible && size >= 40 && body + 40 <= file.Length)
                {
                    // The sub-format GUID starts with the format code.
                    format = BinaryPrimitives.ReadUInt16LittleEndian(file[(body + 24)..]);
                }
                if (format != FormatPcm)
                {
                    throw new WavFormatException($"{name}: format code {format}; only PCM (1) is read.");
                }
                if (bits != 16)
                {
                    throw new WavFormatException($"{name}: {bits} bits per sample; only 16-bit PCM is read.");
                }
                if (channels != 1)
                {
                    throw new WavFormatException($"{name}: {channels} channels; only mono is read.");
                }
                if (sampleRate is < 1000 or > 384000)
                {
                    throw new WavFormatException($"{name}: a sample rate of {sampleRate} Hz.");
                }
                rate = sampleRate;
            }
            else if (id.SequenceEqual("data"u8))
            {
                if (rate is not int r)
                {
                    throw new WavFormatException($"{name}: the data chunk comes before the format chunk.");
                }
                // An unfinished file has a size of 0 or more than there is: take what there is.
                long available = file.Length - body;
                long bytes = size == 0 || size > available ? available : size;
                long count = bytes / 2;
                if (count > MaxSamples)
                {
                    throw new WavFormatException($"{name} is too long.");
                }
                short[] samples = new short[count];
                for (int i = 0; i < count; i++)
                {
                    samples[i] = BinaryPrimitives.ReadInt16LittleEndian(file[(body + i * 2)..]);
                }
                return new WavData(r, samples);
            }
            pos = body + (int)Math.Min(size + (size & 1), int.MaxValue - body);
        }
        throw new WavFormatException(rate is null ? $"{name} has no format chunk." : $"{name} has no data chunk.");
    }
}

/// <summary>
/// Writes a 16-bit PCM mono WAV file as the samples come. The sizes in the
/// header are brought up to date by <see cref="Flush"/> and at the end, so a
/// file whose writer did not finish still reads up to the last flush.
/// </summary>
public sealed class WavWriter : IDisposable
{
    private const int HeaderSize = 44;
    private readonly FileStream _file;
    private readonly byte[] _buffer = new byte[8192];
    private bool _closed;

    public string Path { get; }
    public int SampleRate { get; }
    /// <summary>Samples written, zeros for gaps included.</summary>
    public long Samples { get; private set; }
    /// <summary>Of those, zeros that stand for lost samples.</summary>
    public long GapSamples { get; private set; }
    public long Gaps { get; private set; }

    public WavWriter(string path, int sampleRate)
    {
        Path = path;
        SampleRate = sampleRate;
        _file = new FileStream(path, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.Read);
        _file.Write(Header(0));
    }

    private byte[] Header(long samples)
    {
        uint dataBytes = (uint)Math.Min(samples * 2, uint.MaxValue - HeaderSize);
        byte[] h = new byte[HeaderSize];
        Encoding.ASCII.GetBytes("RIFF").CopyTo(h, 0);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(4), dataBytes + HeaderSize - 8);
        Encoding.ASCII.GetBytes("WAVEfmt ").CopyTo(h, 8);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(16), 16);
        BinaryPrimitives.WriteUInt16LittleEndian(h.AsSpan(20), 1);           // PCM
        BinaryPrimitives.WriteUInt16LittleEndian(h.AsSpan(22), 1);           // mono
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(24), (uint)SampleRate);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(28), (uint)SampleRate * 2);
        BinaryPrimitives.WriteUInt16LittleEndian(h.AsSpan(32), 2);           // bytes per frame
        BinaryPrimitives.WriteUInt16LittleEndian(h.AsSpan(34), 16);          // bits
        Encoding.ASCII.GetBytes("data").CopyTo(h, 36);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(40), dataBytes);
        return h;
    }

    public void Write(ReadOnlySpan<short> samples)
    {
        ObjectDisposedException.ThrowIf(_closed, this);
        while (samples.Length > 0)
        {
            int n = Math.Min(samples.Length, _buffer.Length / 2);
            for (int i = 0; i < n; i++)
            {
                BinaryPrimitives.WriteInt16LittleEndian(_buffer.AsSpan(i * 2), samples[i]);
            }
            _file.Write(_buffer, 0, n * 2);
            Samples += n;
            samples = samples[n..];
        }
    }

    /// <summary>Lost samples, written as zeros.</summary>
    public void WriteGap(long count)
    {
        ObjectDisposedException.ThrowIf(_closed, this);
        Array.Clear(_buffer);
        for (long left = count; left > 0;)
        {
            int n = (int)Math.Min(left, _buffer.Length / 2);
            _file.Write(_buffer, 0, n * 2);
            left -= n;
        }
        Samples += count;
        GapSamples += count;
        Gaps++;
    }

    /// <summary>Brings the header's sizes up to date and writes everything to the disk.</summary>
    public void Flush()
    {
        ObjectDisposedException.ThrowIf(_closed, this);
        long end = _file.Position;
        _file.Position = 0;
        _file.Write(Header(Samples));
        _file.Position = end;
        _file.Flush(flushToDisk: false);
    }

    public void Dispose()
    {
        if (_closed)
        {
            return;
        }
        Flush();
        _closed = true;
        _file.Dispose();
    }
}
