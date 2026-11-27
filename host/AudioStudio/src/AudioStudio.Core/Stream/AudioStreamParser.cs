// SPDX-License-Identifier: MIT
using System.Buffers.Binary;

namespace AudioStudio.Core.Stream;

/// <summary>A stream that does not follow the protocol; the connection is of no further use.</summary>
public sealed class AudioStreamException(string message) : Exception(message);

/// <summary>The stream header the board sends once per connection.</summary>
public sealed record AudioStreamHeader(int Version, int Format, int SampleRate);

/// <summary>
/// One block of the stream, with what the parser worked out from the
/// indices: <see cref="Lost"/> samples are missing right before it.
/// </summary>
/// <param name="Index">Index of the block's first sample (wraps at 2^32).</param>
/// <param name="Lost">Samples the board passed without sending, before this block.</param>
/// <param name="Idle">No stream runs on the board (a keep-alive; no samples).</param>
/// <param name="NewStream">The first block of a new stream: the signal does not continue.</param>
/// <param name="Samples">The samples, signed 16-bit.</param>
public sealed record AudioBlock(uint Index, long Lost, bool Idle, bool NewStream, short[] Samples);

/// <summary>
/// Turns the bytes of the board's audio stream (TCP port 3233, protocol
/// version 1) into the header and blocks. Data may arrive in pieces of any
/// size; the parser keeps what it has of an unfinished header or block.
/// One parser serves one connection.
/// </summary>
public sealed class AudioStreamParser
{
    public const int HeaderSize = 16;
    public const int BlockHeaderSize = 8;
    public const int MaxBlockSamples = 1024;
    public const ushort FlagIdle = 0x0001;
    public const ushort FlagNewStream = 0x0002;
    private static ReadOnlySpan<byte> Magic => "XAUD"u8;

    private readonly byte[] _pending = new byte[BlockHeaderSize + MaxBlockSamples * 2];
    private int _have, _need = HeaderSize;
    private bool _inBlock;
    private uint _expected;

    public AudioStreamHeader? Header { get; private set; }

    /// <summary>Takes the next bytes; calls <paramref name="block"/> for every block completed by them.</summary>
    /// <exception cref="AudioStreamException">The bytes do not follow the protocol.</exception>
    public void Feed(ReadOnlySpan<byte> data, Action<AudioBlock> block)
    {
        while (data.Length > 0)
        {
            int take = Math.Min(_need - _have, data.Length);
            data[..take].CopyTo(_pending.AsSpan(_have));
            _have += take;
            data = data[take..];
            if (_have < _need)
            {
                return;
            }
            if (Header is null)
            {
                Header = ParseHeader(_pending.AsSpan(0, HeaderSize));
                _have = 0;
                _need = BlockHeaderSize;
            }
            else if (!_inBlock)
            {
                int count = BinaryPrimitives.ReadUInt16LittleEndian(_pending.AsSpan(4));
                if (count > MaxBlockSamples)
                {
                    throw new AudioStreamException($"A block of {count} samples (at most {MaxBlockSamples}).");
                }
                _inBlock = true;
                _need = BlockHeaderSize + count * 2;
                if (count == 0)
                {
                    block(FinishBlock());
                }
            }
            else
            {
                block(FinishBlock());
            }
        }
    }

    private static AudioStreamHeader ParseHeader(ReadOnlySpan<byte> h)
    {
        if (!h[..4].SequenceEqual(Magic))
        {
            throw new AudioStreamException("Not the board's audio stream (no \"XAUD\" header).");
        }
        if (h[4] != 1)
        {
            throw new AudioStreamException($"Protocol version {h[4]}; this application reads version 1.");
        }
        if (h[5] != 1)
        {
            throw new AudioStreamException($"Sample format {h[5]}; this application reads 1 (16-bit mono).");
        }
        int rate = (int)BinaryPrimitives.ReadUInt32LittleEndian(h[8..]);
        if (rate is < 1000 or > 384000)
        {
            throw new AudioStreamException($"A sample rate of {rate} Hz.");
        }
        return new AudioStreamHeader(h[4], h[5], rate);
    }

    private AudioBlock FinishBlock()
    {
        ReadOnlySpan<byte> b = _pending.AsSpan(0, _need);
        uint index = BinaryPrimitives.ReadUInt32LittleEndian(b);
        int count = BinaryPrimitives.ReadUInt16LittleEndian(b[4..]);
        ushort flags = BinaryPrimitives.ReadUInt16LittleEndian(b[6..]);
        short[] samples = new short[count];
        for (int i = 0; i < count; i++)
        {
            samples[i] = BinaryPrimitives.ReadInt16LittleEndian(b[(BlockHeaderSize + i * 2)..]);
        }
        // Unsigned difference: correct across the wrap at 2^32.
        uint lost = unchecked(index - _expected);
        if (lost >= 1u << 31)
        {
            throw new AudioStreamException($"The index went back from {_expected} to {index}.");
        }
        _expected = unchecked(index + (uint)count);
        _have = 0;
        _need = BlockHeaderSize;
        _inBlock = false;
        return new AudioBlock(index, lost, (flags & FlagIdle) != 0, (flags & FlagNewStream) != 0, samples);
    }
}
