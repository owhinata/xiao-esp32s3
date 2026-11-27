// SPDX-License-Identifier: MIT
using AudioStudio.Core.Audio;

namespace AudioStudio.Core.Dsp;

/// <summary>One column of the spectrogram, reduced to the rows of the picture.</summary>
/// <param name="Rows">Level of each row in dBFS, the lowest frequency first (the strongest bin in the row).</param>
/// <param name="PeakHz">Frequency of the strongest bin above DC.</param>
/// <param name="PeakDb">Its level in dBFS.</param>
public sealed record SpectrumColumn(float[] Rows, float PeakHz, float PeakDb);

/// <summary>Peak and RMS level in dBFS: relative to full scale, as the board's <c>uac level</c> prints them.</summary>
public static class Level
{
    public static (double PeakDb, double RmsDb) Measure(ReadOnlySpan<float> samples)
    {
        if (samples.Length == 0)
        {
            return (double.NegativeInfinity, double.NegativeInfinity);
        }
        double peak = 0, sum = 0;
        foreach (float s in samples)
        {
            peak = Math.Max(peak, Math.Abs(s));
            sum += (double)s * s;
        }
        return (20 * Math.Log10(peak), 10 * Math.Log10(sum / samples.Length));
    }
}

/// <summary>
/// The spectrogram of a sample source, column by column. Column k is the
/// amplitude spectrum of the <see cref="FftSize"/> samples that end at
/// position k × <see cref="Hop"/>, with a Hann window after removing their
/// mean, in dBFS: a sine of amplitude A (full scale 1) reads 20·log10(A) at
/// its frequency. Each column is reduced to <see cref="Rows"/> rows along a
/// linear or logarithmic frequency axis, and kept, so that a picture that
/// moves on computes only its new columns.
/// </summary>
public sealed class Spectrogram
{
    public static readonly int[] Sizes = [512, 1024, 2048, 4096, 8192];
    public const double LogFloorHz = 10;

    private readonly Fft _fft;
    private readonly float[] _window, _samples, _re, _im, _db;
    private readonly float _windowSum;
    private readonly int[] _rowFirstBin, _rowEndBin;
    private readonly long[] _cacheKey;
    private readonly SpectrumColumn?[] _cache;

    public int FftSize { get; }
    public int Hop { get; }
    public int Rows { get; }
    public int SampleRate { get; }
    public bool LogFrequency { get; }
    public double BinHz { get; }
    /// <summary>The top of the frequency axis.</summary>
    public double MaxHz { get; }

    /// <param name="maxHz">Top of the axis; at most half the sample rate.</param>
    /// <param name="cacheColumns">Columns kept; at least those of one picture.</param>
    public Spectrogram(int sampleRate, int fftSize, int hop, int rows, double maxHz, bool logFrequency,
                       int cacheColumns = 4096)
    {
        if (hop < 1 || rows < 1)
        {
            throw new ArgumentOutOfRangeException(nameof(hop));
        }
        _fft = new Fft(fftSize);
        FftSize = fftSize;
        Hop = hop;
        Rows = rows;
        SampleRate = sampleRate;
        LogFrequency = logFrequency;
        BinHz = (double)sampleRate / fftSize;
        MaxHz = Math.Min(maxHz, sampleRate / 2.0);
        _window = new float[fftSize];
        for (int i = 0; i < fftSize; i++)
        {
            _window[i] = 0.5f - 0.5f * MathF.Cos(2 * MathF.PI * i / fftSize);
            _windowSum += _window[i];
        }
        _samples = new float[fftSize];
        _re = new float[fftSize];
        _im = new float[fftSize];
        _db = new float[fftSize / 2];
        _rowFirstBin = new int[rows];
        _rowEndBin = new int[rows];
        int bins = fftSize / 2;
        for (int row = 0; row < rows; row++)
        {
            int first = (int)Math.Floor(Frequency(row / (double)rows) / BinHz);
            int end = (int)Math.Ceiling(Frequency((row + 1) / (double)rows) / BinHz);
            first = Math.Clamp(first, 0, bins - 1);
            _rowFirstBin[row] = first;
            _rowEndBin[row] = Math.Clamp(end, first + 1, bins);
        }
        _cacheKey = new long[cacheColumns];
        Array.Fill(_cacheKey, long.MinValue);
        _cache = new SpectrumColumn?[cacheColumns];
    }

    /// <summary>Frequency at a height from 0 (bottom) to 1 (top).</summary>
    public double Frequency(double height) =>
        LogFrequency ? LogFloorHz * Math.Pow(MaxHz / LogFloorHz, height) : height * MaxHz;

    /// <summary>Height from 0 (bottom) to 1 (top) of a frequency.</summary>
    public double HeightOf(double hz) =>
        LogFrequency ? Math.Log(hz / LogFloorHz) / Math.Log(MaxHz / LogFloorHz) : hz / MaxHz;

    /// <summary>The time a column stands for: the centre of its window.</summary>
    public double CentreOf(long k) => k * (double)Hop - FftSize / 2.0;

    /// <summary>The columns whose centres lie in <paramref name="from"/>..<paramref name="to"/> (the last excluded).</summary>
    public (long First, long Last) ColumnsCentredIn(long from, long to)
    {
        long first = (long)Math.Ceiling((from + FftSize / 2.0) / Hop);
        long last = (long)Math.Ceiling((to + FftSize / 2.0) / Hop) - 1;
        return (first, last);
    }

    /// <summary>The column kept for k, or null if it has not been computed.</summary>
    public SpectrumColumn? Cached(long k)
    {
        int slot = (int)(((k % _cache.Length) + _cache.Length) % _cache.Length);
        return _cacheKey[slot] == k ? _cache[slot] : null;
    }

    /// <summary>Column k, computed now unless it is kept. Null if its window does not end inside the source.</summary>
    public SpectrumColumn? Column(ISampleSource source, long k)
    {
        if (Cached(k) is SpectrumColumn kept)
        {
            return kept;
        }
        long end = k * Hop;
        // Before the first sample the window reads zeros.
        if (end <= source.Start || end > source.End)
        {
            return null;
        }
        SpectrumColumn column = Compute(source, end);
        int slot = (int)(((k % _cache.Length) + _cache.Length) % _cache.Length);
        _cacheKey[slot] = k;
        _cache[slot] = column;
        return column;
    }

    /// <summary>The spectrum in dBFS per bin (bin i at i × <see cref="BinHz"/>) of the samples before <paramref name="end"/>.</summary>
    public float[] Bins(ISampleSource source, long end)
    {
        Transform(source, end);
        return (float[])_db.Clone();
    }

    private SpectrumColumn Compute(ISampleSource source, long end)
    {
        Transform(source, end);
        float[] rows = new float[Rows];
        for (int row = 0; row < Rows; row++)
        {
            float level = float.NegativeInfinity;
            for (int bin = _rowFirstBin[row]; bin < _rowEndBin[row]; bin++)
            {
                level = MathF.Max(level, _db[bin]);
            }
            rows[row] = level;
        }
        int peak = 2;
        for (int bin = 3; bin < _db.Length; bin++)
        {
            if (_db[bin] > _db[peak])
            {
                peak = bin;
            }
        }
        return new SpectrumColumn(rows, (float)(peak * BinHz), _db[peak]);
    }

    private void Transform(ISampleSource source, long end)
    {
        source.Read(end - FftSize, _samples);
        float mean = 0;
        foreach (float s in _samples)
        {
            mean += s;
        }
        mean /= FftSize;
        for (int i = 0; i < FftSize; i++)
        {
            _re[i] = (_samples[i] - mean) * _window[i];
            _im[i] = 0;
        }
        _fft.Transform(_re, _im);
        // A sine of amplitude A gives A × windowSum / 2 in its bin.
        float scale = 2f / _windowSum;
        for (int bin = 0; bin < _db.Length; bin++)
        {
            float magnitude = MathF.Sqrt(_re[bin] * _re[bin] + _im[bin] * _im[bin]) * scale;
            _db[bin] = 20f * MathF.Log10(MathF.Max(magnitude, 1e-12f));
        }
    }
}
