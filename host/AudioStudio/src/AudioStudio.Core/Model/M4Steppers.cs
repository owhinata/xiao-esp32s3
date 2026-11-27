// SPDX-License-Identifier: MIT
using System.Globalization;

namespace AudioStudio.Core.Model;

/// <summary>
/// The post-processing of the M4 detector, one window at a time: the same
/// rules and arithmetic as <c>PostStepper</c> of zaitaku-mealpace's
/// <c>analysis/postprocess.py</c>.
/// <list type="number">
/// <item>Hysteresis: the state h starts false; when false it becomes true for
/// a valid window with p &gt;= enter, when true it stays true only for a valid
/// window with p &gt;= exit.</item>
/// <item>A window where h turns true starts a new run.</item>
/// <item>Refractory period: a run whose first window starts at most
/// <c>refractory_s</c> after the last window of the run before is suppressed
/// as a whole (counted from that run's last window even when it was
/// suppressed or capped itself).</item>
/// <item>Cap: of a run that is not suppressed, the first
/// round(cap_s / hop) windows are positive and the rest negative.</item>
/// </list>
/// A window is positive when h is true, its run is not suppressed and it is
/// within the cap. <c>Step</c> takes the window's start time in seconds,
/// which must advance by <see cref="HopSeconds"/>.
/// </summary>
public sealed class PostStepper
{
    /// <summary>Window start to window start (evaluate.HOP_S).</summary>
    public const double HopSeconds = 0.25;
    /// <summary>The tolerance on the grid of window times (postprocess.GRID_TOL_S).</summary>
    public const double GridTolerance = 1e-6;

    private readonly double _enter, _exit;
    private readonly double? _refractory;
    private readonly int? _capWindows;
    private bool _h, _suppressed;
    private int _count;
    private double? _lastHTime, _previousTime;

    public PostStepper(double enter, double exit, double? capSeconds = null, double? refractorySeconds = null)
    {
        if (!(0.0 < exit && exit <= enter && enter <= 1.0))
        {
            throw new ArgumentException(string.Create(CultureInfo.InvariantCulture,
                $"Needs 0 < exit <= enter <= 1 (enter {enter}, exit {exit})."));
        }
        if (capSeconds is double cap)
        {
            double n = cap / HopSeconds;
            if (!(cap > 0) || Math.Abs(n - Math.Round(n, MidpointRounding.ToEven)) > 1e-9)
            {
                throw new ArgumentException(string.Create(CultureInfo.InvariantCulture,
                    $"The cap of a run is none or a positive multiple of {HopSeconds} s: {cap}."));
            }
            _capWindows = (int)Math.Round(n, MidpointRounding.ToEven);
        }
        if (refractorySeconds is double r && r != M4Config.RefractoryChoice)
        {
            throw new ArgumentException(string.Create(CultureInfo.InvariantCulture,
                $"The refractory period is none or {M4Config.RefractoryChoice} s: {r}."));
        }
        _enter = enter;
        _exit = exit;
        _refractory = refractorySeconds;
    }

    /// <summary>Whether the window that starts at <paramref name="t"/> seconds is positive.</summary>
    public bool Step(double p, bool valid, double t)
    {
        if (_previousTime is double previous && Math.Abs(t - previous - HopSeconds) > GridTolerance)
        {
            throw new ArgumentException(string.Create(CultureInfo.InvariantCulture,
                $"Window times are not {HopSeconds} s apart ({previous} -> {t})."));
        }
        _previousTime = t;
        bool h = _h ? valid && p >= _exit : valid && p >= _enter;
        if (h && !_h)
        {
            _count = 0;
            _suppressed = _refractory is double r && _lastHTime is double last && t - last <= r + GridTolerance;
        }
        _h = h;
        if (!h)
        {
            return false;
        }
        _lastHTime = t;
        _count++;
        if (_suppressed)
        {
            return false;
        }
        return _capWindows is not int capWindows || _count <= capWindows;
    }
}

/// <summary>
/// The LED of the M4 device, one window at a time: the same rule as
/// <c>LedStepper</c> of zaitaku-mealpace's <c>analysis/led_rule.py</c>.
/// Green (2, a swallow indication) while the window starts at most
/// <see cref="GreenHoldMs"/> after the start of the last positive window,
/// otherwise yellow (1, running). The device's off (0) is not a window
/// result: it is shown when no window comes for a second.
/// </summary>
public sealed class LedStepper
{
    public const int Off = 0;
    public const int Yellow = 1;
    public const int Green = 2;
    public const int GreenHoldMs = 1750;

    private long? _lastPositive, _previous;

    /// <summary>The LED for the window that starts at <paramref name="windowMs"/>; the times must increase.</summary>
    public int Step(long windowMs, bool positive)
    {
        if (_previous is long previous && windowMs <= previous)
        {
            throw new ArgumentException(string.Create(CultureInfo.InvariantCulture,
                $"Window times do not increase: {previous} -> {windowMs}."));
        }
        _previous = windowMs;
        if (positive)
        {
            _lastPositive = windowMs;
        }
        return _lastPositive is long last && windowMs - last <= GreenHoldMs ? Green : Yellow;
    }
}
