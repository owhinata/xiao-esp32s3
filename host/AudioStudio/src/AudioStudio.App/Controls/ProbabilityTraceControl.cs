// SPDX-License-Identifier: MIT
using System.Globalization;
using AudioStudio.Core.Model;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace AudioStudio.App.Controls;

/// <summary>
/// The M4 detector's probability along the timeline (0 at the bottom, 1 at
/// the top). Each window is drawn over the 0.25 s around its centre, so the
/// windows tile the axis and line up with the sound they hold. The windows
/// of a swallow indication (positive after the post-processing) are green
/// bands, windows that were not valid (lost samples) red bands without a
/// probability; dashed lines mark the configuration's enter and exit. The
/// margins are those of <see cref="SpectrogramControl"/>.
/// </summary>
public sealed class ProbabilityTraceControl : Control
{
    private static readonly IBrush s_background = new SolidColorBrush(Color.FromRgb(0x12, 0x12, 0x16));
    private static readonly IBrush s_dim = new SolidColorBrush(Color.FromArgb(0x90, 0xE0, 0xE0, 0xE0));
    private static readonly IBrush s_positive = new SolidColorBrush(Color.FromArgb(0x70, 0x3F, 0xB9, 0x50));
    private static readonly IBrush s_invalid = new SolidColorBrush(Color.FromArgb(0x60, 0xEB, 0x4D, 0x4B));
    private static readonly IBrush s_enterBrush = new SolidColorBrush(Color.FromRgb(0x6C, 0xD6, 0x7C));
    private static readonly IBrush s_exitBrush = new SolidColorBrush(Color.FromRgb(0xF2, 0xC1, 0x4E));
    private static readonly IPen s_enter = new Pen(s_enterBrush, 1, new DashStyle([4, 3], 0));
    private static readonly IPen s_exit = new Pen(s_exitBrush, 1, new DashStyle([4, 3], 0));
    private static readonly IPen s_grid = new Pen(new SolidColorBrush(Color.FromArgb(0x30, 0xFF, 0xFF, 0xFF)), 1);
    private static readonly IPen s_trace = new Pen(new SolidColorBrush(Color.FromRgb(0x5A, 0xC8, 0xFA)), 1.5);
    private static readonly Typeface s_typeface = new("monospace");

    private List<M4Window> _windows = [];
    private long _from, _to;
    private double _enter = double.NaN, _exit = double.NaN;
    private string _note = "";

    public ProbabilityTraceControl()
    {
        ClipToBounds = true;
    }

    /// <param name="enter">The configuration's enter level (NaN: none).</param>
    /// <param name="exit">The configuration's exit level (NaN: none).</param>
    public void Show(List<M4Window> windows, long from, long to, double enter, double exit)
    {
        _windows = windows;
        _from = from;
        _to = to;
        _enter = enter;
        _exit = exit;
        _note = "";
        InvalidateVisual();
    }

    /// <summary>No windows to show; <paramref name="note"/> says why.</summary>
    public void Clear(string note)
    {
        _windows = [];
        _note = note;
        InvalidateVisual();
    }

    public override void Render(DrawingContext context)
    {
        Rect bounds = new(Bounds.Size);
        context.FillRectangle(s_background, bounds);
        Rect plot = new(SpectrogramControl.AxisWidth, 6,
                        Math.Max(1, bounds.Width - SpectrogramControl.AxisWidth - SpectrogramControl.CurveWidth - 4),
                        Math.Max(1, bounds.Height - 12));
        double Y(double p) => plot.Bottom - Math.Clamp(p, 0, 1) * plot.Height;
        Label("1", s_dim, Y(1));
        Label("0", s_dim, Y(0));
        context.DrawLine(s_grid, new Point(plot.X, Y(1)), new Point(plot.Right, Y(1)));
        context.DrawLine(s_grid, new Point(plot.X, Y(0)), new Point(plot.Right, Y(0)));
        context.DrawText(Text("prob", s_dim), new Point(plot.Right + 6, plot.Y + plot.Height / 2 - 7));
        if (!double.IsNaN(_enter))
        {
            Label(_enter.ToString("0.00", CultureInfo.InvariantCulture), s_enterBrush, Y(_enter));
        }
        if (!double.IsNaN(_exit) && _exit != _enter)
        {
            Label(_exit.ToString("0.00", CultureInfo.InvariantCulture), s_exitBrush, Y(_exit));
        }
        if (_note.Length > 0)
        {
            context.DrawText(Text(_note, s_dim), new Point(plot.X + 6, plot.Y + plot.Height / 2 - 7));
            return;
        }
        double span = Math.Max(1, _to - _from);
        double X(long position) => plot.X + (position - _from) / span * plot.Width;
        using (context.PushClip(plot.Inflate(new Thickness(0, 6))))
        {
            Bands(context, w => w.Step.Positive, s_positive, X, plot);
            Bands(context, w => !w.Step.Valid, s_invalid, X, plot);
            if (!double.IsNaN(_enter))
            {
                context.DrawLine(s_enter, new Point(plot.X, Y(_enter)), new Point(plot.Right, Y(_enter)));
            }
            if (!double.IsNaN(_exit) && _exit != _enter)
            {
                context.DrawLine(s_exit, new Point(plot.X, Y(_exit)), new Point(plot.Right, Y(_exit)));
            }
            StreamGeometry trace = new();
            using (StreamGeometryContext g = trace.Open())
            {
                M4Window? previous = null;
                foreach (M4Window w in _windows)
                {
                    if (!w.Step.Valid)
                    {
                        previous = null;
                        continue;
                    }
                    double y = Y(w.Step.Probability);
                    // A step: across the window's own 0.25 s, joined to the window before it.
                    if (previous is null || previous.DrawTo != w.DrawFrom)
                    {
                        if (previous is not null)
                        {
                            g.EndFigure(false);
                        }
                        g.BeginFigure(new Point(X(w.DrawFrom), y), false);
                    }
                    else
                    {
                        g.LineTo(new Point(X(w.DrawFrom), y));
                    }
                    g.LineTo(new Point(X(w.DrawTo), y));
                    previous = w;
                }
                if (previous is not null)
                {
                    g.EndFigure(false);
                }
            }
            context.DrawGeometry(null, s_trace, trace);
        }

        void Label(string text, IBrush brush, double y)
        {
            FormattedText t = Text(text, brush);
            context.DrawText(t, new Point(SpectrogramControl.AxisWidth - 4 - t.Width, y - t.Height / 2));
        }
    }

    /// <summary>Fills the drawn spans of the windows that <paramref name="which"/> picks, joining neighbours.</summary>
    private void Bands(DrawingContext context, Func<M4Window, bool> which, IBrush brush, Func<long, double> x, Rect plot)
    {
        long? from = null, to = null;
        foreach (M4Window w in _windows.Where(which))
        {
            if (to == w.DrawFrom)
            {
                to = w.DrawTo;
                continue;
            }
            Fill();
            from = w.DrawFrom;
            to = w.DrawTo;
        }
        Fill();

        void Fill()
        {
            if (from is long a && to is long b)
            {
                double x0 = x(a), x1 = x(b);
                context.FillRectangle(brush, new Rect(x0, plot.Y, Math.Max(1, x1 - x0), plot.Height));
            }
        }
    }

    private static FormattedText Text(string text, IBrush brush) =>
        new(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, s_typeface, 11, brush);
}
