// SPDX-License-Identifier: MIT
using System.Diagnostics;
using AudioStudio.Core.Audio;

namespace AudioStudio.Core.Model;

/// <summary>One window of the M4 detector on a sample source.</summary>
/// <param name="Start">The source position of the window's first sample.</param>
/// <param name="Rise">The first positive window of a run: one swallow indication.</param>
/// <param name="Lagged">Not valid because its samples had left the live ring before the detector reached them.</param>
public sealed record M4Window(long Start, M4Step Step, bool Rise, bool Lagged)
{
    public long End => Start + M4Model.WindowSamples;
    /// <summary>The 0.25 s around the window's centre, where the views draw the window.</summary>
    public long DrawFrom => Start + (M4Model.WindowSamples - M4Model.HopSamples) / 2;
    public long DrawTo => DrawFrom + M4Model.HopSamples;
}

/// <summary>The windows of an M4 runner in the order of their positions, read by any thread.</summary>
public sealed class M4History
{
    private readonly object _lock = new();
    private readonly List<M4Window> _windows = new();
    private long _rises;
    private M4Window? _lastRise;

    public int Count
    {
        get { lock (_lock) { return _windows.Count; } }
    }

    public M4Window? Latest
    {
        get { lock (_lock) { return _windows.Count > 0 ? _windows[^1] : null; } }
    }

    /// <summary>Swallow indications since the runner started (trimmed windows included).</summary>
    public long Indications
    {
        get { lock (_lock) { return _rises; } }
    }

    /// <summary>The first window of the last swallow indication, trimmed or not.</summary>
    public M4Window? LastRise
    {
        get { lock (_lock) { return _lastRise; } }
    }

    public void Add(M4Window window)
    {
        lock (_lock)
        {
            _windows.Add(window);
            if (window.Rise)
            {
                _rises++;
                _lastRise = window;
            }
        }
    }

    /// <summary>Drops the windows that start before <paramref name="position"/>.</summary>
    public void TrimBefore(long position)
    {
        lock (_lock)
        {
            int n = FirstDrawnAfter(position - M4Model.WindowSamples);
            while (n < _windows.Count && _windows[n].Start < position)
            {
                n++;
            }
            _windows.RemoveRange(0, Math.Min(n, _windows.Count));
        }
    }

    /// <summary>The windows drawn in <paramref name="from"/>..<paramref name="to"/>.</summary>
    public List<M4Window> Range(long from, long to)
    {
        lock (_lock)
        {
            int first = FirstDrawnAfter(from);
            int end = first;
            while (end < _windows.Count && _windows[end].DrawFrom < to)
            {
                end++;
            }
            return _windows.GetRange(first, end - first);
        }
    }

    /// <summary>The first windows of the runs (the swallow indications) drawn in <paramref name="from"/>..<paramref name="to"/>.</summary>
    public List<M4Window> Rises(long from, long to) => Range(from, to).Where(w => w.Rise).ToList();

    /// <summary>Index of the first window whose drawn span ends after <paramref name="position"/>.</summary>
    private int FirstDrawnAfter(long position)
    {
        int lo = 0, hi = _windows.Count;
        while (lo < hi)
        {
            int mid = (lo + hi) / 2;
            if (_windows[mid].DrawTo <= position)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        return lo;
    }
}

/// <summary>
/// Runs the M4 detector over a sample source on a thread of its own, every
/// window in order (the post-processing needs each one): window k of a
/// stretch starts k x 4000 samples after the stretch's first sample. A
/// stretch is a file from its start, or the live stream from the start of
/// a new stream (a <see cref="MarkerKind.NewStream"/> marker: a new
/// connection, or a new stream on the board), where the detector starts
/// afresh; the windows of the stretch before that do not reach past it are
/// left out. A window that overlaps lost samples (a
/// <see cref="MarkerKind.Gap"/> marker) is not valid: it is not computed and
/// is negative. On the live stream the runner starts at the newest complete
/// window, and when it falls behind it catches up rather than dropping
/// windows; <see cref="Backlog"/> tells how far behind it is.
/// <para>
/// With the spike filter (<see cref="MainsSpike"/>, a mains frequency given
/// to the constructor) the detector's windows are the filter's output, read
/// in the source's time (<see cref="SpikeFilteredInput"/>), so the window
/// positions and the indications refer to the source's samples as without
/// it. The filter runs over a stretch's samples in order: a file from its
/// first sample, the live stream from <see cref="LivePrimeSamples"/> before
/// the first window run (where the ring still holds them), and afresh after
/// a new stream or after the runner fell behind the ring. A window then
/// needs <see cref="MainsSpike.Delay"/> samples after its end, and lost
/// samples within that many samples of it make it not valid. The source is
/// only read: what is shown and recorded stays the source's samples.
/// </para>
/// </summary>
public sealed class M4Runner
{
    /// <summary>How far before its first window the filter starts on the live stream: 2 s.</summary>
    public const int LivePrimeSamples = 2 * M4Model.SampleRate;

    private readonly M4Model _model;
    private readonly SpikeFilteredInput? _spike;
    private readonly object _stateLock = new();
    private M4Detector? _detector;
    private long _origin = -1;
    private bool _lastPositive;
    private Task _running = Task.CompletedTask;
    private int _busy;
    private long _runs, _backlog, _lagged, _computed, _end;
    private double _milliseconds, _totalMilliseconds;
    private long _lastWindowTicks;
    private string? _error;

    /// <param name="spikeFilterMainsHz">The mains frequency of the spike filter before the detector; null: no filter.</param>
    public M4Runner(M4Model model, double? spikeFilterMainsHz = null)
    {
        _model = model;
        _spike = spikeFilterMainsHz is double hz ? new SpikeFilteredInput(hz) : null;
    }

    public M4Model Model => _model;
    /// <summary>The spike filter's mains frequency; null when the runner has no filter.</summary>
    public double? SpikeFilterMainsHz => _spike?.MainsHz;
    /// <summary>Samples a window needs after its end: the filter's delay, or none.</summary>
    private int Lookahead => _spike is null ? 0 : MainsSpike.Delay;
    public M4History History { get; } = new();
    /// <summary>Windows run, valid or not.</summary>
    public long Runs => Interlocked.Read(ref _runs);
    /// <summary>Complete windows of the live stream not run yet.</summary>
    public long Backlog => Interlocked.Read(ref _backlog);
    /// <summary>Windows that were not valid because the runner fell behind the live ring.</summary>
    public long Lagged => Interlocked.Read(ref _lagged);
    /// <summary>Time of the last computed window.</summary>
    public double Milliseconds => Volatile.Read(ref _milliseconds);
    /// <summary>Mean time of the computed windows.</summary>
    public double MeanMilliseconds
    {
        get { lock (_stateLock) { return _computed == 0 ? 0 : _totalMilliseconds / _computed; } }
    }
    public string? Error => Volatile.Read(ref _error);

    /// <summary>Time since the last window was run (live); null before the first.</summary>
    public TimeSpan? SinceLastWindow
    {
        get
        {
            long t = Interlocked.Read(ref _lastWindowTicks);
            return t == 0 ? null : Stopwatch.GetElapsedTime(t);
        }
    }

    /// <summary>Completes when no run of this runner is in progress any more.</summary>
    public Task WhenIdle() => _running;

    /// <summary>
    /// Runs, in the background, the windows of a live source that have
    /// become complete. A run that is still going takes the new samples on
    /// as well. Returns at once.
    /// </summary>
    public void Offer(ISampleSource source)
    {
        if (source.SampleRate != M4Model.SampleRate)
        {
            Volatile.Write(ref _error, $"The model takes {M4Model.SampleRate} Hz; the source has {source.SampleRate} Hz.");
            return;
        }
        Interlocked.Exchange(ref _end, source.End);
        if (Interlocked.CompareExchange(ref _busy, 1, 0) != 0)
        {
            return;
        }
        _running = Task.Run(() =>
        {
            try
            {
                RunLive(source);
            }
            finally
            {
                Volatile.Write(ref _busy, 0);
            }
        });
    }

    private void RunLive(ISampleSource source)
    {
        float[] window = new float[M4Model.WindowSamples];
        long end = Interlocked.Read(ref _end);
        if (_origin < 0)
        {
            // The newest stretch, from its newest complete window.
            _origin = LastNewStream(source, source.Start, end) ?? 0;
            long k0 = Math.Max(0, (end - _origin - M4Model.WindowSamples - Lookahead) / M4Model.HopSamples);
            StartStretch(_origin, k0, source);
        }
        while (true)
        {
            end = Interlocked.Read(ref _end);
            long k = _detector!.Next;
            long start = _origin + k * M4Model.HopSamples;
            long stop = start + M4Model.WindowSamples;
            // A new stream that begins inside this window starts a stretch of its own.
            if (LastNewStream(source, start + 1, stop - 1) is long restart && restart > _origin)
            {
                _origin = restart;
                StartStretch(restart, 0, source);
                continue;
            }
            if (stop + Lookahead > end)
            {
                Interlocked.Exchange(ref _backlog, 0);
                break;
            }
            bool lagged;
            if (_spike is null)
            {
                lagged = start < source.Start;
            }
            else
            {
                if (_spike.Next < source.Start)
                {
                    // Samples the filter had not taken left the ring: it starts afresh.
                    _spike.Restart(source.Start);
                }
                lagged = start < _spike.From;
            }
            bool valid = !lagged && !OverlapsGap(source, start - Lookahead, stop + Lookahead);
            if (lagged)
            {
                Interlocked.Increment(ref _lagged);
            }
            if (_spike is not null)
            {
                _spike.Advance(source, stop + Lookahead);
            }
            if (valid)
            {
                ReadWindow(source, start, window);
            }
            Record(start, Compute(valid ? window : null), lagged);
            Interlocked.Exchange(ref _lastWindowTicks, Stopwatch.GetTimestamp());
            Interlocked.Exchange(ref _backlog, Math.Max(0, (end - stop - Lookahead) / M4Model.HopSamples));
        }
    }

    private void StartStretch(long origin, long firstWindow, ISampleSource source)
    {
        _detector = new M4Detector(_model, firstWindow);
        _origin = origin;
        _lastPositive = false;
        long first = origin + firstWindow * M4Model.HopSamples;
        _spike?.Restart(Math.Max(Math.Max(origin, source.Start), first - LivePrimeSamples));
    }

    /// <summary>The window's samples: the source's, or the spike filter's output for them.</summary>
    private void ReadWindow(ISampleSource source, long start, Span<float> window)
    {
        if (_spike is null)
        {
            source.Read(start, window);
        }
        else
        {
            _spike.Read(start, window);
        }
    }

    /// <summary>Runs every window of a file in order; reports the fraction done.</summary>
    public Task AnalyzeAsync(ISampleSource source, IProgress<double>? progress, CancellationToken ct)
    {
        if (source.SampleRate != M4Model.SampleRate)
        {
            Volatile.Write(ref _error, $"The model takes {M4Model.SampleRate} Hz; the file has {source.SampleRate} Hz.");
            return Task.CompletedTask;
        }
        return _running = Task.Run(() =>
        {
            long count = Math.Max(0, (source.End - source.Start - M4Model.WindowSamples) / M4Model.HopSamples + 1);
            StartStretch(source.Start, 0, source);
            // The probabilities of a batch in parallel, then the post-processing in order.
            const int Batch = 64;
            double[] probabilities = new double[Batch];
            bool[] valid = new bool[Batch];
            for (long first = 0; first < count; first += Batch)
            {
                ct.ThrowIfCancellationRequested();
                int n = (int)Math.Min(Batch, count - first);
                // The filter in order, then the windows of the batch read its output in parallel.
                _spike?.Advance(source, source.Start + (first + n - 1) * M4Model.HopSamples + M4Model.WindowSamples + Lookahead);
                Parallel.For(0, n, new ParallelOptions { CancellationToken = ct }, i =>
                {
                    long start = source.Start + (first + i) * M4Model.HopSamples;
                    valid[i] = !OverlapsGap(source, start - Lookahead, start + M4Model.WindowSamples + Lookahead);
                    float[] window = new float[M4Model.WindowSamples];
                    ReadWindow(source, start, window);
                    probabilities[i] = valid[i] ? Probability(window) : double.NaN;
                });
                for (int i = 0; i < n; i++)
                {
                    long start = source.Start + (first + i) * M4Model.HopSamples;
                    Record(start, _detector!.Advance(probabilities[i], valid[i]), false);
                }
                progress?.Report((double)(first + n) / count);
            }
            progress?.Report(1);
        }, ct);
    }

    private M4Step Compute(float[]? window)
    {
        if (window is null)
        {
            return _detector!.StepInvalid();
        }
        return _detector!.Advance(Probability(window), true);
    }

    private double Probability(float[] window)
    {
        long t0 = Stopwatch.GetTimestamp();
        double p = _model.Probability(_model.Mfe40(window));
        double ms = Stopwatch.GetElapsedTime(t0).TotalMilliseconds;
        Volatile.Write(ref _milliseconds, ms);
        lock (_stateLock)
        {
            _totalMilliseconds += ms;
            _computed++;
        }
        return p;
    }

    private void Record(long start, M4Step step, bool lagged)
    {
        bool rise = step.Positive && !_lastPositive;
        _lastPositive = step.Positive;
        History.Add(new M4Window(start, step, rise, lagged));
        Interlocked.Increment(ref _runs);
        Volatile.Write(ref _error, null);
    }

    /// <summary>The position of the last new stream in <paramref name="from"/>..<paramref name="to"/>.</summary>
    private static long? LastNewStream(ISampleSource source, long from, long to)
    {
        long? last = null;
        foreach (Marker m in source.Markers(from, to))
        {
            if (m.Kind == MarkerKind.NewStream && m.Position >= from && m.Position <= to && (last is null || m.Position > last))
            {
                last = m.Position;
            }
        }
        return last;
    }

    private static bool OverlapsGap(ISampleSource source, long start, long stop) =>
        source.Markers(start, stop).Any(m => m.Kind == MarkerKind.Gap && m.Length > 0 && m.Position < stop && m.Position + m.Length > start);
}
