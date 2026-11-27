// SPDX-License-Identifier: MIT
namespace AudioStudio.Core.Audio;

public enum MarkerKind
{
    /// <summary>Samples that were lost and are zeros here.</summary>
    Gap,
    /// <summary>A new stream starts: the signal does not continue across it.</summary>
    NewStream,
}

/// <param name="Position">The first sample the marker concerns.</param>
/// <param name="Length">Samples it covers (0 for a new stream).</param>
public readonly record struct Marker(long Position, long Length, MarkerKind Kind);

/// <summary>
/// Samples along a timeline, addressed by their position from the start (of
/// the connection, or of the file). Both the live stream and an opened file
/// are shown and classified through this.
/// </summary>
public interface ISampleSource
{
    int SampleRate { get; }
    /// <summary>The first position still held.</summary>
    long Start { get; }
    /// <summary>One past the last position.</summary>
    long End { get; }
    /// <summary>
    /// Copies the samples from <paramref name="position"/> on, scaled to -1..1;
    /// positions outside <see cref="Start"/>..<see cref="End"/> read as 0.
    /// </summary>
    void Read(long position, Span<float> destination);
    /// <summary>The markers that overlap <paramref name="from"/>..<paramref name="to"/>.</summary>
    List<Marker> Markers(long from, long to);
}

/// <summary>The samples of a file, all in memory.</summary>
public sealed class FileSource(string path, int sampleRate, short[] samples) : ISampleSource
{
    public string Path { get; } = path;
    public int SampleRate { get; } = sampleRate;
    public long Start => 0;
    public long End => samples.Length;

    public void Read(long position, Span<float> destination)
    {
        for (int i = 0; i < destination.Length; i++)
        {
            long p = position + i;
            destination[i] = p >= 0 && p < samples.Length ? samples[p] / 32768f : 0f;
        }
    }

    public List<Marker> Markers(long from, long to) => [];

    public static FileSource Open(string path)
    {
        WavData wav = WavFile.Read(path);
        return new FileSource(path, wav.SampleRate, wav.Samples);
    }
}

/// <summary>
/// The latest samples of the live stream in a ring. One thread appends,
/// any thread reads.
/// </summary>
public sealed class LiveSource : ISampleSource
{
    private readonly object _lock = new();
    private readonly short[] _ring;
    private readonly List<Marker> _markers = new();
    private long _end;

    public LiveSource(int sampleRate, int capacity = 1 << 21)
    {
        SampleRate = sampleRate;
        _ring = new short[capacity];
    }

    public int SampleRate { get; }
    public long Capacity => _ring.Length;

    public long Start
    {
        get { lock (_lock) { return Math.Max(0, _end - _ring.Length); } }
    }

    public long End
    {
        get { lock (_lock) { return _end; } }
    }

    public void Append(ReadOnlySpan<short> samples)
    {
        lock (_lock)
        {
            foreach (short s in samples)
            {
                _ring[_end++ % _ring.Length] = s;
            }
        }
    }

    /// <summary>Lost samples: zeros, and a marker for them.</summary>
    public void AppendGap(long count)
    {
        lock (_lock)
        {
            _markers.Add(new Marker(_end, count, MarkerKind.Gap));
            long zeros = Math.Min(count, _ring.Length);
            // Positions older than the ring are skipped without writing.
            _end += count - zeros;
            for (long i = 0; i < zeros; i++)
            {
                _ring[_end++ % _ring.Length] = 0;
            }
            Trim();
        }
    }

    public void MarkNewStream()
    {
        lock (_lock)
        {
            _markers.Add(new Marker(_end, 0, MarkerKind.NewStream));
            Trim();
        }
    }

    private void Trim()
    {
        long start = Math.Max(0, _end - _ring.Length);
        _markers.RemoveAll(m => m.Position + m.Length < start);
    }

    public void Read(long position, Span<float> destination)
    {
        lock (_lock)
        {
            long start = Math.Max(0, _end - _ring.Length);
            for (int i = 0; i < destination.Length; i++)
            {
                long p = position + i;
                destination[i] = p >= start && p < _end ? _ring[p % _ring.Length] / 32768f : 0f;
            }
        }
    }

    public List<Marker> Markers(long from, long to)
    {
        lock (_lock)
        {
            return _markers.Where(m => m.Position <= to && m.Position + m.Length >= from).ToList();
        }
    }
}
