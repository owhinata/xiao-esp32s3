// SPDX-License-Identifier: MIT
using System.Globalization;
using AudioStudio.Core.Audio;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace AudioStudio.App.Controls;

/// <summary>
/// The waveform of a stretch of a sample source: per pixel column the
/// smallest and the largest sample, a time axis below, lost samples
/// shaded and the start of a new stream as a line. The margins are those
/// of <see cref="SpectrogramControl"/>, so the two line up in time.
/// </summary>
public sealed class WaveformControl : Control
{
    private static readonly IBrush s_background = new SolidColorBrush(Color.FromRgb(0x12, 0x12, 0x16));
    private static readonly IBrush s_text = new SolidColorBrush(Color.FromArgb(0xD0, 0xE0, 0xE0, 0xE0));
    private static readonly IPen s_grid = new Pen(new SolidColorBrush(Color.FromArgb(0x30, 0xFF, 0xFF, 0xFF)), 1);
    private static readonly IPen s_zero = new Pen(new SolidColorBrush(Color.FromArgb(0x60, 0xFF, 0xFF, 0xFF)), 1);
    private static readonly IPen s_wave = new Pen(new SolidColorBrush(Color.FromRgb(0x5A, 0xC8, 0xFA)), 1);
    private static readonly IBrush s_gap = new SolidColorBrush(Color.FromArgb(0x60, 0xEB, 0x4D, 0x4B));
    private static readonly IPen s_newStream = new Pen(new SolidColorBrush(Color.FromRgb(0xF2, 0xC1, 0x4E)), 1.5);
    private static readonly Typeface s_typeface = new("monospace");
    private const double TimeAxisHeight = 16;

    private float[] _samples = [];
    private float[] _min = [], _max = [];
    private List<Marker> _markers = [];
    private long _from, _to;
    private int _rate = 16000;
    private double _zoom = 1;
    private bool _relative;

    public WaveformControl()
    {
        ClipToBounds = true;
    }

    /// <param name="relative">Label the time relative to the right end (the live stream) instead of from the start.</param>
    public void Show(ISampleSource source, long from, long to, double zoom, bool relative)
    {
        _from = from;
        _to = to;
        _rate = source.SampleRate;
        _zoom = zoom;
        _relative = relative;
        int columns = Math.Max(1, (int)(Bounds.Width - SpectrogramControl.AxisWidth - SpectrogramControl.CurveWidth - 4));
        int length = (int)Math.Max(0, to - from);
        if (_samples.Length < length)
        {
            _samples = new float[length];
        }
        source.Read(from, _samples.AsSpan(0, length));
        if (_min.Length != columns)
        {
            _min = new float[columns];
            _max = new float[columns];
        }
        for (int x = 0; x < columns; x++)
        {
            int a = (int)((long)length * x / columns);
            int b = Math.Max(a + 1, (int)((long)length * (x + 1) / columns));
            float lo = float.PositiveInfinity, hi = float.NegativeInfinity;
            for (int i = a; i < b && i < length; i++)
            {
                lo = MathF.Min(lo, _samples[i]);
                hi = MathF.Max(hi, _samples[i]);
            }
            _min[x] = lo;
            _max[x] = hi;
        }
        _markers = source.Markers(from, to);
        InvalidateVisual();
    }

    public void Clear()
    {
        _min = [];
        _max = [];
        _markers = [];
        InvalidateVisual();
    }

    public override void Render(DrawingContext context)
    {
        Rect bounds = new(Bounds.Size);
        context.FillRectangle(s_background, bounds);
        Rect image = new(SpectrogramControl.AxisWidth, 2,
                         Math.Max(1, bounds.Width - SpectrogramControl.AxisWidth - SpectrogramControl.CurveWidth - 4),
                         Math.Max(1, bounds.Height - 4 - TimeAxisHeight));
        double mid = image.Y + image.Height / 2;
        double half = image.Height / 2 * _zoom;
        double span = Math.Max(1, _to - _from);

        // Amplitude grid at full scale, half and a quarter of it, as far as the zoom shows them.
        foreach (double level in new[] { 1.0, 0.5, 0.25, 0.125, 0.0625, 1.0 / 32, 1.0 / 64, 1.0 / 128 })
        {
            double dy = level * half;
            if (dy > image.Height / 2 + 0.5 || dy < 12)
            {
                continue;
            }
            context.DrawLine(s_grid, new Point(image.X, mid - dy), new Point(image.Right, mid - dy));
            context.DrawLine(s_grid, new Point(image.X, mid + dy), new Point(image.Right, mid + dy));
            FormattedText label = Text(level.ToString("0.####", CultureInfo.InvariantCulture));
            context.DrawText(label, new Point(SpectrogramControl.AxisWidth - 4 - label.Width,
                                              Math.Max(0, mid - dy - label.Height / 2)));
        }
        context.DrawLine(s_zero, new Point(image.X, mid), new Point(image.Right, mid));

        using (context.PushClip(image))
        {
            foreach (Marker m in _markers)
            {
                double x0 = image.X + (m.Position - _from) / span * image.Width;
                if (m.Kind == MarkerKind.Gap)
                {
                    double x1 = image.X + (m.Position + m.Length - _from) / span * image.Width;
                    context.FillRectangle(s_gap, new Rect(x0, image.Y, Math.Max(1, x1 - x0), image.Height));
                }
                else
                {
                    context.DrawLine(s_newStream, new Point(x0, image.Y), new Point(x0, image.Bottom));
                }
            }
            if (_min.Length > 0)
            {
                StreamGeometry geometry = new();
                using (StreamGeometryContext g = geometry.Open())
                {
                    for (int x = 0; x < _min.Length; x++)
                    {
                        if (float.IsInfinity(_min[x]))
                        {
                            continue;
                        }
                        double px = image.X + x + 0.5;
                        double top = mid - _max[x] * half, bottom = mid - _min[x] * half;
                        if (bottom - top < 1)
                        {
                            bottom = top + 1;
                        }
                        g.BeginFigure(new Point(px, top), false);
                        g.LineTo(new Point(px, bottom));
                        g.EndFigure(false);
                    }
                }
                context.DrawGeometry(null, s_wave, geometry);
            }
        }

        // Time axis.
        double seconds = span / _rate;
        double step = Step(seconds / 6);
        double fromS = _from / (double)_rate, toS = _to / (double)_rate;
        double origin = _relative ? toS : 0;
        for (double t = Math.Ceiling((fromS - origin) / step) * step + origin; t <= toS + 1e-9; t += step)
        {
            double x = image.X + (t - fromS) / seconds * image.Width;
            context.DrawLine(s_grid, new Point(x, image.Y), new Point(x, image.Bottom + 3));
            FormattedText label = Text(TimeLabel(t - origin, step));
            context.DrawText(label, new Point(Math.Clamp(x - label.Width / 2, image.X, image.Right - label.Width),
                                              image.Bottom + 2));
        }
        context.DrawText(Text(string.Create(CultureInfo.InvariantCulture, $"x{_zoom:0}")),
                         new Point(image.Right + 8, image.Y + 2));
    }

    private static double Step(double raw)
    {
        double power = Math.Pow(10, Math.Floor(Math.Log10(raw)));
        return power * (raw / power < 1.5 ? 1 : raw / power < 3.5 ? 2 : raw / power < 7.5 ? 5 : 10);
    }

    internal static string TimeLabel(double seconds, double step)
    {
        string format = step < 0.1 ? "0.00" : step < 1 ? "0.0" : "0";
        if (Math.Abs(seconds) < step / 1000)
        {
            seconds = 0;
        }
        if (Math.Abs(seconds) >= 60)
        {
            double abs = Math.Abs(seconds);
            int minutes = (int)(abs / 60);
            return (seconds < 0 ? "-" : "") + minutes.ToString(CultureInfo.InvariantCulture) + ":" +
                   (abs - minutes * 60).ToString(step < 1 ? "00.0" : "00", CultureInfo.InvariantCulture);
        }
        return seconds.ToString(format, CultureInfo.InvariantCulture) + " s";
    }

    private static FormattedText Text(string text) =>
        new(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, s_typeface, 11, s_text);
}
