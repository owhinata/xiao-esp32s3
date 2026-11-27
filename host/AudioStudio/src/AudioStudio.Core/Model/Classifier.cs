// SPDX-License-Identifier: MIT
using System.Diagnostics;
using AudioStudio.Core.Audio;

namespace AudioStudio.Core.Model;

/// <summary>The model's answer for the window that ends at <see cref="End"/>.</summary>
public sealed record ClassResult(long End, float[] Scores, int Top);

/// <summary>Results in the order of their windows, read by any thread.</summary>
public sealed class ClassHistory
{
    private readonly object _lock = new();
    private readonly List<ClassResult> _results = new();

    public int Count
    {
        get { lock (_lock) { return _results.Count; } }
    }

    public ClassResult? Latest
    {
        get { lock (_lock) { return _results.Count > 0 ? _results[^1] : null; } }
    }

    public void Add(ClassResult result)
    {
        lock (_lock)
        {
            _results.Add(result);
        }
    }

    public void Clear()
    {
        lock (_lock)
        {
            _results.Clear();
        }
    }

    /// <summary>Drops the results of windows that end before <paramref name="position"/>.</summary>
    public void TrimBefore(long position)
    {
        lock (_lock)
        {
            int n = 0;
            while (n < _results.Count && _results[n].End < position)
            {
                n++;
            }
            _results.RemoveRange(0, n);
        }
    }

    /// <summary>The result of the last window that ends at or before <paramref name="position"/>.</summary>
    public ClassResult? At(long position)
    {
        lock (_lock)
        {
            int i = FirstAfter(position) - 1;
            return i >= 0 ? _results[i] : null;
        }
    }

    /// <summary>The results whose windows end in <paramref name="from"/>..<paramref name="to"/>, and the one before.</summary>
    public List<ClassResult> Range(long from, long to)
    {
        lock (_lock)
        {
            int first = Math.Max(0, FirstAfter(from) - 1);
            int end = FirstAfter(to);
            return first < end ? _results.GetRange(first, end - first) : [];
        }
    }

    private int FirstAfter(long position)
    {
        int lo = 0, hi = _results.Count;
        while (lo < hi)
        {
            int mid = (lo + hi) / 2;
            if (_results[mid].End <= position)
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
/// Runs the model over a sample source, on a thread of its own. Windows end
/// at multiples of <see cref="Hop"/> samples. On the live stream
/// (<see cref="Offer"/>) a window that comes while the model is still busy
/// is dropped: the next run takes the newest window, so the results never
/// fall behind the stream. On a file (<see cref="AnalyzeAsync"/>) every
/// window is run.
/// </summary>
public sealed class Classifier
{
    private readonly AudioModel _model;
    private readonly float[] _window;
    private long _lastWindow = long.MinValue;
    private Task _running = Task.CompletedTask;
    private int _busy;
    private long _runs, _dropped;
    private double _milliseconds;
    private string? _error;

    public Classifier(AudioModel model, int hop)
    {
        _model = model;
        Hop = Math.Max(1, hop);
        _window = new float[model.WindowSamples];
    }

    public AudioModel Model => _model;
    public int Hop { get; }
    public ClassHistory History { get; } = new();
    public long Runs => Interlocked.Read(ref _runs);
    public long Dropped => Interlocked.Read(ref _dropped);
    /// <summary>Duration of the last run.</summary>
    public double Milliseconds => Volatile.Read(ref _milliseconds);
    public string? Error => Volatile.Read(ref _error);
    public bool Busy => Volatile.Read(ref _busy) != 0;

    /// <summary>
    /// Starts a run on the newest complete window of a live source if one is
    /// due and the model is free. Returns at once.
    /// </summary>
    public void Offer(ISampleSource source)
    {
        if (source.SampleRate != AudioModel.SampleRate)
        {
            Volatile.Write(ref _error, $"The model takes {AudioModel.SampleRate} Hz; the source has {source.SampleRate} Hz.");
            return;
        }
        long k = source.End / Hop;
        if (k * Hop - _window.Length < source.Start || k <= _lastWindow)
        {
            return;
        }
        if (Interlocked.CompareExchange(ref _busy, 1, 0) != 0)
        {
            return;
        }
        if (_lastWindow != long.MinValue && k - _lastWindow > 1)
        {
            Interlocked.Add(ref _dropped, k - _lastWindow - 1);
        }
        _lastWindow = k;
        long end = k * Hop;
        source.Read(end - _window.Length, _window);
        _running = Task.Run(() =>
        {
            try
            {
                RunWindow(end);
            }
            finally
            {
                Volatile.Write(ref _busy, 0);
            }
        });
    }

    /// <summary>Completes when no run of this classifier is in progress any more.</summary>
    public Task WhenIdle() => _running;

    /// <summary>Runs every window of the source, in order; reports the fraction done.</summary>
    public Task AnalyzeAsync(ISampleSource source, IProgress<double>? progress, CancellationToken ct)
    {
        if (source.SampleRate != AudioModel.SampleRate)
        {
            Volatile.Write(ref _error, $"The model takes {AudioModel.SampleRate} Hz; the file has {source.SampleRate} Hz.");
            return Task.CompletedTask;
        }
        return _running = Task.Run(() =>
        {
            long first = (source.Start + _window.Length + Hop - 1) / Hop;
            long last = source.End / Hop;
            for (long k = first; k <= last; k++)
            {
                ct.ThrowIfCancellationRequested();
                source.Read(k * Hop - _window.Length, _window);
                if (!RunWindow(k * Hop))
                {
                    return;
                }
                if ((k - first) % 16 == 0)
                {
                    progress?.Report((double)(k - first + 1) / (last - first + 1));
                }
            }
            progress?.Report(1);
        }, ct);
    }

    private bool RunWindow(long end)
    {
        Stopwatch watch = Stopwatch.StartNew();
        try
        {
            float[] scores = _model.Run(_window);
            int top = 0;
            for (int i = 1; i < scores.Length; i++)
            {
                if (scores[i] > scores[top])
                {
                    top = i;
                }
            }
            History.Add(new ClassResult(end, scores, top));
            Interlocked.Increment(ref _runs);
            Volatile.Write(ref _milliseconds, watch.Elapsed.TotalMilliseconds);
            Volatile.Write(ref _error, null);
            return true;
        }
        catch (ModelContractException e)
        {
            Volatile.Write(ref _error, e.Message);
            return false;
        }
    }
}
