// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using System.Text;

namespace AudioStudio.Core.Ota;

/// <summary>A file that is not an ESP application image the board takes.</summary>
public sealed class AppImageException(string message) : Exception(message);

/// <summary>
/// An ESP application image (<c>xiao_esp32s3_threadx.bin</c>) with what its
/// application description says: the image header, its first segment's
/// header, then <c>esp_app_desc_t</c> (esp_app_format.h, esp_app_desc.h).
/// </summary>
public sealed class AppImage
{
    private const byte ImageMagic = 0xE9;
    private const int HashAppendedOffset = 23;
    private const int DescOffset = 0x20;
    private const uint DescMagic = 0xABCD5432;
    private const int DescVersion = DescOffset + 16;
    private const int DescProject = DescOffset + 48;
    private const int DescTime = DescOffset + 80;
    private const int DescDate = DescOffset + 96;
    private const int DescElfSha256 = DescOffset + 144;
    public const int ShaPrefixBytes = 8;

    public string Path { get; }
    public byte[] Bytes { get; }
    public string Project { get; }
    public string Version { get; }
    /// <summary>Build date and time as the description holds them.</summary>
    public string Built { get; }
    /// <summary>The first <see cref="ShaPrefixBytes"/> bytes of the ELF SHA-256: what the confirmation names.</summary>
    public byte[] ElfShaPrefix { get; }
    public string ElfShaHex => Convert.ToHexStringLower(ElfShaPrefix);

    private AppImage(string path, byte[] bytes)
    {
        Path = path;
        Bytes = bytes;
        string name = System.IO.Path.GetFileName(path);
        if (bytes.Length < DescElfSha256 + 32 || bytes[0] != ImageMagic)
        {
            throw new AppImageException($"{name}: not an ESP application image");
        }
        if (BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(DescOffset)) != DescMagic)
        {
            throw new AppImageException($"{name}: no application description");
        }
        if (bytes[HashAppendedOffset] != 1)
        {
            throw new AppImageException($"{name}: no SHA-256 appended");
        }
        Project = Text(bytes, DescProject, 32);
        Version = Text(bytes, DescVersion, 32);
        Built = $"{Text(bytes, DescDate, 16)} {Text(bytes, DescTime, 16)}";
        ElfShaPrefix = bytes.AsSpan(DescElfSha256, ShaPrefixBytes).ToArray();
    }

    public static AppImage Read(string path)
    {
        try
        {
            return new AppImage(path, File.ReadAllBytes(path));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            throw new AppImageException($"Cannot read {path}: {e.Message}");
        }
    }

    public static AppImage FromBytes(string path, byte[] bytes) => new(path, bytes);

    private static string Text(byte[] bytes, int offset, int size)
    {
        ReadOnlySpan<byte> field = bytes.AsSpan(offset, size);
        int end = field.IndexOf((byte)0);
        return Encoding.ASCII.GetString(end < 0 ? field : field[..end]);
    }
}
