// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using System.Globalization;
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;

namespace AudioStudio.Core.Model;

/// <summary>A file that is not a .npy / .npz this reader takes.</summary>
public sealed class NpyFormatException(string message) : Exception(message);

/// <summary>
/// One array of a .npy file, in C order: its dtype as numpy writes it
/// (<c>descr</c>, for example <c>&lt;f4</c>), its shape, and its bytes.
/// </summary>
public sealed class NpyArray
{
    private readonly byte[] _data;

    internal NpyArray(string descr, int[] shape, byte[] data)
    {
        Descr = descr;
        Shape = shape;
        _data = data;
    }

    /// <summary>The dtype: <c>&lt;f4</c>, <c>&lt;f8</c>, <c>&lt;i8</c>, <c>|b1</c>, <c>&lt;U126</c>, ...</summary>
    public string Descr { get; }
    public int[] Shape { get; }
    /// <summary>Number of elements (1 for a scalar of shape ()).</summary>
    public long Count => Shape.Aggregate(1L, (n, d) => n * d);
    /// <summary>numpy's dtype kind: 'f' float, 'i' signed integer, 'u' unsigned, 'b' bool, 'U' unicode string.</summary>
    public char Kind => Descr[1];
    /// <summary>Bytes per element (for 'U', 4 per character).</summary>
    public int ItemSize => Kind == 'U' ? 4 * int.Parse(Descr[2..], CultureInfo.InvariantCulture)
                                       : int.Parse(Descr[2..], CultureInfo.InvariantCulture);

    public bool HasShape(params int[] shape) => Shape.SequenceEqual(shape);

    public float[] ToFloat32()
    {
        Require("<f4");
        float[] v = new float[Count];
        for (int i = 0; i < v.Length; i++)
        {
            v[i] = BinaryPrimitives.ReadSingleLittleEndian(_data.AsSpan(4 * i));
        }
        return v;
    }

    public double[] ToFloat64()
    {
        Require("<f8");
        double[] v = new double[Count];
        for (int i = 0; i < v.Length; i++)
        {
            v[i] = BinaryPrimitives.ReadDoubleLittleEndian(_data.AsSpan(8 * i));
        }
        return v;
    }

    /// <summary>Any signed or unsigned integer dtype, as long.</summary>
    public long[] ToInt64()
    {
        if (Kind is not ('i' or 'u') || (ItemSize > 1 && Descr[0] != '<'))
        {
            throw new NpyFormatException($"An array of {Descr}, not of little-endian integers.");
        }
        int size = ItemSize;
        long[] v = new long[Count];
        for (int i = 0; i < v.Length; i++)
        {
            ReadOnlySpan<byte> b = _data.AsSpan(size * i, size);
            v[i] = (Kind, size) switch
            {
                ('i', 1) => (sbyte)b[0],
                ('i', 2) => BinaryPrimitives.ReadInt16LittleEndian(b),
                ('i', 4) => BinaryPrimitives.ReadInt32LittleEndian(b),
                ('i', 8) => BinaryPrimitives.ReadInt64LittleEndian(b),
                ('u', 1) => b[0],
                ('u', 2) => BinaryPrimitives.ReadUInt16LittleEndian(b),
                ('u', 4) => BinaryPrimitives.ReadUInt32LittleEndian(b),
                _ => throw new NpyFormatException($"Integers of {Descr} are not supported."),
            };
        }
        return v;
    }

    public short[] ToInt16()
    {
        Require("<i2");
        short[] v = new short[Count];
        for (int i = 0; i < v.Length; i++)
        {
            v[i] = BinaryPrimitives.ReadInt16LittleEndian(_data.AsSpan(2 * i));
        }
        return v;
    }

    public bool[] ToBool()
    {
        Require("|b1");
        return _data.Take((int)Count).Select(b => b != 0).ToArray();
    }

    /// <summary>The strings of a unicode array; numpy drops the trailing NULs of each.</summary>
    public string[] ToStrings()
    {
        if (Kind != 'U' || Descr[0] != '<')
        {
            throw new NpyFormatException($"An array of {Descr}, not of little-endian unicode strings.");
        }
        int size = ItemSize;
        string[] v = new string[Count];
        for (int i = 0; i < v.Length; i++)
        {
            v[i] = Encoding.UTF32.GetString(_data, size * i, size).TrimEnd('\0');
        }
        return v;
    }

    private void Require(string descr)
    {
        if (Descr != descr)
        {
            throw new NpyFormatException($"An array of {Descr}, not of {descr}.");
        }
    }
}

/// <summary>
/// Reads numpy's .npy files (format versions 1 to 3) and .npz files (a zip
/// of .npy files, stored or deflated), as <c>np.load(..., allow_pickle=False)</c>
/// does: little-endian numbers, booleans and unicode strings in C order.
/// Object arrays (pickles), Fortran order and big-endian data are refused.
/// </summary>
public static partial class Npy
{
    private static readonly byte[] s_magic = [0x93, (byte)'N', (byte)'U', (byte)'M', (byte)'P', (byte)'Y'];

    [GeneratedRegex(@"^\{\s*'descr'\s*:\s*'(?<descr>[^']*)'\s*,\s*'fortran_order'\s*:\s*(?<fortran>True|False)\s*,\s*'shape'\s*:\s*\((?<shape>[0-9,\s]*)\)\s*,?\s*\}\s*$")]
    private static partial Regex HeaderPattern();

    [GeneratedRegex(@"^(<f4|<f8|<i[1248]|\|i1|\|u1|<u[24]|\|b1|<U[0-9]+)$")]
    private static partial Regex SupportedDescr();

    public static NpyArray Read(System.IO.Stream stream, string name = "array")
    {
        byte[] head = ReadExactly(stream, 8, name);
        if (!head.AsSpan(0, 6).SequenceEqual(s_magic))
        {
            throw new NpyFormatException($"{name}: not a .npy file.");
        }
        int major = head[6];
        int headerLength;
        if (major == 1)
        {
            headerLength = BinaryPrimitives.ReadUInt16LittleEndian(ReadExactly(stream, 2, name));
        }
        else if (major is 2 or 3)
        {
            headerLength = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(ReadExactly(stream, 4, name)));
        }
        else
        {
            throw new NpyFormatException($"{name}: .npy format version {major} is not supported.");
        }
        byte[] headerBytes = ReadExactly(stream, headerLength, name);
        string header = (major == 3 ? Encoding.UTF8 : Encoding.Latin1).GetString(headerBytes).Trim();
        Match m = HeaderPattern().Match(header);
        if (!m.Success)
        {
            throw new NpyFormatException($"{name}: the header is not of the form numpy writes: {header}");
        }
        string descr = m.Groups["descr"].Value;
        if (m.Groups["fortran"].Value == "True")
        {
            throw new NpyFormatException($"{name}: Fortran order is not supported.");
        }
        if (!SupportedDescr().IsMatch(descr))
        {
            throw new NpyFormatException($"{name}: the dtype {descr} is not supported.");
        }
        int[] shape = m.Groups["shape"].Value
            .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(s => int.Parse(s, CultureInfo.InvariantCulture)).ToArray();
        NpyArray probe = new(descr, shape, []);
        long bytes = checked(probe.Count * probe.ItemSize);
        if (bytes > int.MaxValue)
        {
            throw new NpyFormatException($"{name}: {bytes} bytes are too many.");
        }
        return new NpyArray(descr, shape, ReadExactly(stream, (int)bytes, name));
    }

    public static NpyArray ReadFile(string path)
    {
        using FileStream file = File.OpenRead(path);
        return Read(file, Path.GetFileName(path));
    }

    /// <summary>The arrays of a .npz file by name (the member's name without <c>.npy</c>), in the zip's order.</summary>
    public static Dictionary<string, NpyArray> ReadNpz(string path)
    {
        Dictionary<string, NpyArray> arrays = new();
        try
        {
            using ZipArchive zip = ZipFile.OpenRead(path);
            foreach (ZipArchiveEntry entry in zip.Entries)
            {
                if (!entry.FullName.EndsWith(".npy", StringComparison.Ordinal))
                {
                    throw new NpyFormatException($"{Path.GetFileName(path)}: {entry.FullName} is not a .npy member.");
                }
                string key = entry.FullName[..^4];
                using System.IO.Stream stream = entry.Open();
                arrays[key] = Read(stream, $"{Path.GetFileName(path)}: {entry.FullName}");
            }
        }
        catch (InvalidDataException e)
        {
            throw new NpyFormatException($"{Path.GetFileName(path)}: not a .npz (zip) file: {e.Message}");
        }
        return arrays;
    }

    private static byte[] ReadExactly(System.IO.Stream stream, int count, string name)
    {
        byte[] buffer = new byte[count];
        try
        {
            stream.ReadExactly(buffer);
        }
        catch (EndOfStreamException)
        {
            throw new NpyFormatException($"{name}: the file ends early.");
        }
        return buffer;
    }
}
