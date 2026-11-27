// SPDX-License-Identifier: MIT
using AudioStudio.Core.Audio;

namespace AudioStudio.Core.Model;

/// <summary>
/// The mains-synchronous spike filter: the front-end <c>MainsSpike</c> of
/// <c>model/analysis/mains_spike.py</c>, one sample at a time, with the same
/// constants and the same arithmetic (double precision throughout, as the
/// Python code's float64; the output rounded to float as its float32).
/// It limits short bursts above about 450 Hz that come back at the same
/// place of every mains period (an electric heater the wearer touches) and
/// leaves sound that is not synchronous with the mains alone. It is causal;
/// the output is the input delayed by <see cref="Delay"/> samples wherever
/// it does not act. The Python module's docstring gives the algorithm.
/// </summary>
public sealed class MainsSpike
{
    public const int SampleRate = 16000;
    public const double DefaultMainsHz = 50.0;
    /// <summary>Cutoff of the linear-phase low-pass of the band split.</summary>
    public const double LowPassHz = 300.0;
    public const int Taps = 161;
    /// <summary>The delay of the output: (Taps - 1) / 2 samples, 5 ms.</summary>
    public const int Delay = (Taps - 1) / 2;
    public const double EnvelopeTau = 4.0;
    public const double HumQ = 2.0;
    public const double Refractory = 0.75;
    public const double PeriodTolerance = 0.03;
    public const double PeriodRate = 1.0 / 256.0;
    public const double Alpha = 1.0 / 8.0;
    public const double QuantStep = 1.0 / 512.0;
    public const double Margin = 3.0;
    public const double Limit = 2.0;
    public const double FloorGain2 = 0.01;
    public const double FInit = 1e-12;
    public const int Warmup = 32;

    private readonly double[] _taps;
    private readonly double[] _history = new double[2 * Taps]; // each input twice: a contiguous window
    private int _pos;
    private readonly double _b0, _b1, _b2, _a1, _a2;
    private double _x1, _x2, _y1, _y2;
    private readonly double _a;
    private double _e, _zPrev;
    private double? _last;
    private double _p, _pBlock;
    private readonly double[] _template;
    private double _f = FInit, _r, _rLast, _acc;
    private int _j, _length, _up, _blocks;
    private long _n;

    public MainsSpike(double mainsHz = DefaultMainsHz)
    {
        if (!(mainsHz > 0))
        {
            throw new ArgumentOutOfRangeException(nameof(mainsHz));
        }
        MainsHz = mainsHz;
        _taps = LowPassTaps();
        (_b0, _b1, _b2, _a1, _a2) = BandPass(mainsHz);
        _a = Math.Exp(-1.0 / EnvelopeTau);
        _p = _pBlock = SampleRate / mainsHz;
        _template = new double[Bins(mainsHz)];
        _length = (int)Math.Ceiling(_pBlock - _r);
    }

    public double MainsHz { get; }

    /// <summary>The template's length: ceil((1 + PeriodTolerance) x the nominal period) + 1.</summary>
    public static int Bins(double mainsHz) => (int)Math.Ceiling((1.0 + PeriodTolerance) * SampleRate / mainsHz) + 1;

    /// <summary>
    /// The symmetric FIR low-pass of the band split (lp_taps): a
    /// Hamming-windowed sinc at <see cref="LowPassHz"/>, scaled to sum 1.
    /// </summary>
    public static double[] LowPassTaps()
    {
        double[] h = new double[Taps];
        double c = 2.0 * LowPassHz / SampleRate, sum = 0;
        for (int k = 0; k < Taps; k++)
        {
            double w = 0.54 - 0.46 * Math.Cos(2.0 * Math.PI * k / (Taps - 1));
            double x = c * (k - Delay);
            double sinc = x == 0 ? 1.0 : Math.Sin(Math.PI * x) / (Math.PI * x);
            h[k] = w * c * sinc;
            sum += h[k];
        }
        for (int k = 0; k < Taps; k++)
        {
            h[k] /= sum;
        }
        return h;
    }

    /// <summary>(b0, b1, b2, a1, a2) of the RBJ band-pass with 0 dB peak gain at the mains frequency (bp_coeffs).</summary>
    public static (double, double, double, double, double) BandPass(double mainsHz)
    {
        double w = 2.0 * Math.PI * mainsHz / SampleRate;
        double al = Math.Sin(w) / (2.0 * HumQ);
        double a0 = 1.0 + al;
        return (al / a0, 0.0, -al / a0, -2.0 * Math.Cos(w) / a0, (1.0 - al) / a0);
    }

    /// <summary>One input sample in, one output sample out.</summary>
    public double Step(double x)
    {
        long n = _n;
        if (_j == 0)
        {
            // Block start: the period known from the crossings at or before n.
            _pBlock = _p;
            _length = (int)Math.Ceiling(_pBlock - _r);
            _up = 0;
        }
        _history[_pos] = x;
        _history[_pos + Taps] = x;
        // lp = sum_i taps[i] x[n - i]; x[n - i] is _history[_pos + Taps - i].
        double lp = 0;
        int newest = _pos + Taps;
        for (int i = 0; i < Taps; i++)
        {
            lp += _taps[i] * _history[newest - i];
        }
        double xd = _history[newest - Delay];
        _pos = _pos + 1 == Taps ? 0 : _pos + 1;
        double h = xd - lp;
        _e = _a * _e + (1.0 - _a) * h * h;

        double z = _b0 * x + _b1 * _x1 + _b2 * _x2 - _a1 * _y1 - _a2 * _y2;
        _x2 = _x1;
        _x1 = x;
        _y2 = _y1;
        _y1 = z;
        if (_zPrev < 0.0 && 0.0 <= z)
        {
            double t = (n - 1) + (-_zPrev) / (z - _zPrev);
            if (_last is not double last || t - last >= Refractory * _p)
            {
                if (_last is double previous)
                {
                    double d = t - previous;
                    if (Math.Abs(d - _p) <= PeriodTolerance * _p)
                    {
                        _p += PeriodRate * (d - _p);
                    }
                }
                _last = t;
            }
        }
        _zPrev = z;

        int j = _j;
        double tj = _template[j];
        double reference = Math.Max(_f, _rLast);
        double g2 = 1.0;
        if (_blocks >= Warmup && tj > Margin * reference && _e > 0.0)
        {
            g2 = Math.Min(1.0, Math.Max(FloorGain2, Limit * reference / _e));
        }
        double g = Math.Sqrt(g2);
        _acc += g2 * _e;
        if (tj > _f)
        {
            _up++;
        }
        _template[j] = tj + Alpha * (_e - tj);
        _j++;
        _n++;
        if (_j == _length)
        {
            _rLast = _acc / _length;
            _acc = 0.0;
            _f *= Math.Exp(QuantStep * (_up - (_length - _up)));
            _r = _r + _length - _pBlock;
            _j = 0;
            _blocks++;
        }
        return xd - (1.0 - g) * h;
    }

    /// <summary>The samples of <paramref name="input"/> in order; output[i] is the output for input[i].</summary>
    public void Process(ReadOnlySpan<float> input, Span<float> output)
    {
        if (output.Length < input.Length)
        {
            throw new ArgumentException("The output is shorter than the input.", nameof(output));
        }
        for (int i = 0; i < input.Length; i++)
        {
            output[i] = (float)Step(input[i]);
        }
    }
}

/// <summary>
/// The spike filter's output for a sample source, in the source's time: the
/// filter runs over the source's samples from <see cref="From"/> in order,
/// and <see cref="Read"/> gives the output <see cref="MainsSpike.Delay"/>
/// samples later, so that window samples line up with the source's (the
/// filter's 5 ms delay compensated). Positions past the source's end read as
/// 0 (<see cref="ISampleSource.Read"/>), which lets a file's last windows be
/// read. The source itself is only read, never changed.
/// </summary>
public sealed class SpikeFilteredInput(double mainsHz)
{
    /// <summary>Output samples kept: more than a batch of 64 windows of a file (268000 samples).</summary>
    public const int Capacity = 1 << 19;
    private const int Chunk = 4096;

    private readonly float[] _output = new float[Capacity];
    private readonly float[] _in = new float[Chunk], _out = new float[Chunk];
    private MainsSpike _filter = new(mainsHz);
    private long _from, _next;

    public double MainsHz => mainsHz;
    /// <summary>The source position where the filter last started afresh.</summary>
    public long From => _from;
    /// <summary>The next source position the filter takes.</summary>
    public long Next => _next;

    /// <summary>Starts the filter afresh at <paramref name="position"/>.</summary>
    public void Restart(long position)
    {
        _filter = new MainsSpike(mainsHz);
        _from = _next = position;
    }

    /// <summary>Runs the filter over the source's samples up to <paramref name="to"/> (exclusive).</summary>
    public void Advance(ISampleSource source, long to)
    {
        while (_next < to)
        {
            int n = (int)Math.Min(Chunk, to - _next);
            source.Read(_next, _in.AsSpan(0, n));
            _filter.Process(_in.AsSpan(0, n), _out);
            for (int i = 0; i < n; i++)
            {
                _output[(int)((_next + i) & (Capacity - 1))] = _out[i];
            }
            _next += n;
        }
    }

    /// <summary>
    /// The filtered samples of source positions <paramref name="start"/> on:
    /// destination[i] is the output for input start + i + Delay. Outputs not
    /// computed (before the start, not reached yet, or no longer kept) read as 0.
    /// </summary>
    public void Read(long start, Span<float> destination)
    {
        for (int i = 0; i < destination.Length; i++)
        {
            long p = start + MainsSpike.Delay + i;
            destination[i] = p >= _from && p < _next && p >= _next - Capacity
                ? _output[(int)(p & (Capacity - 1))] : 0f;
        }
    }
}
