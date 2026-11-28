// SPDX-License-Identifier: MIT
using System.Globalization;
using AudioStudio.Core.Marks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace AudioStudio.App.Controls;

/// <summary>
/// The marks of a recording over the views: a thin vertical line per mark
/// from the top to the bottom, coloured by kind, with the kind's letter at
/// the top (S swallow, F false, O other, I indication); setup rows are not
/// drawn and not hit (the list of marks shows them). The selected mark
/// is drawn solid and thicker with its letter in a box, the picked
/// position as a solid yellow line. It lies over the other views and takes
/// no input (the window turns clicks into marks and positions with
/// <see cref="MarkAt"/> and <see cref="PositionAt"/>); the margins are those
/// of <see cref="SpectrogramControl"/>, so the lines meet the views' time axis.
/// </summary>
public sealed class MarksControl : Control
{
    private static readonly Typeface s_typeface = new("monospace");
    private static readonly IBrush s_swallow = new SolidColorBrush(Color.FromRgb(0xE0, 0x70, 0xF0));
    private static readonly IBrush s_false = new SolidColorBrush(Color.FromRgb(0xFF, 0x6B, 0x5B));
    private static readonly IBrush s_other = new SolidColorBrush(Color.FromRgb(0xE8, 0xE8, 0xE8));
    private static readonly IBrush s_indication = new SolidColorBrush(Color.FromRgb(0x6C, 0xD6, 0x7C));

    private static readonly IBrush[] s_brushes = [s_swallow, s_false, s_other, s_indication];
    private static readonly IPen[] s_pens = [.. s_brushes.Select(b => (IPen)new Pen(b, 1, new DashStyle([3, 2], 0)))];
    private static readonly IPen[] s_selectedPens = [.. s_brushes.Select(b => (IPen)new Pen(b, 2))];
    private static readonly IPen s_pickPen = new Pen(new SolidColorBrush(Color.FromRgb(0xF0, 0xD0, 0x40)), 1);
    private static readonly IBrush s_letterBox = new SolidColorBrush(Color.FromArgb(0xC0, 0x30, 0x30, 0x38));

    private List<(long Position, MarkKind Kind)> _marks = [];
    private long _from, _to;
    private int? _selected;
    private long? _picked;

    public MarksControl()
    {
        ClipToBounds = true;
        IsHitTestVisible = false;
    }

    private static string LetterOf(MarkKind kind) => kind switch
    {
        MarkKind.Swallow => "S",
        MarkKind.False => "F",
        MarkKind.Other => "O",
        _ => "I",
    };

    /// <param name="selected">The index in <paramref name="marks"/> of the selected mark; null for none.</param>
    /// <param name="picked">The picked position; null for none.</param>
    public void Show(List<(long Position, MarkKind Kind)> marks, long from, long to, int? selected = null, long? picked = null)
    {
        _marks = marks;
        _from = from;
        _to = to;
        _selected = selected;
        _picked = picked;
        InvalidateVisual();
    }

    private Rect Plot => new(SpectrogramControl.AxisWidth, 0,
                             Math.Max(1, Bounds.Width - SpectrogramControl.AxisWidth - SpectrogramControl.CurveWidth - 4),
                             Bounds.Height);

    private double XOf(long position, Rect plot) =>
        Math.Round(plot.X + (position - _from) / (double)Math.Max(1, _to - _from) * plot.Width) + 0.5;

    /// <summary>The index of the mark in view whose line is nearest to <paramref name="x"/>, within <paramref name="tolerance"/> pixels; null for none.</summary>
    public int? MarkAt(double x, double tolerance)
    {
        Rect plot = Plot;
        int? best = null;
        double nearest = tolerance;
        for (int i = 0; i < _marks.Count; i++)
        {
            (long position, MarkKind kind) = _marks[i];
            if (kind == MarkKind.Setup || position < _from || position > _to)
            {
                continue;
            }
            double d = Math.Abs(XOf(position, plot) - x);
            if (d <= nearest)
            {
                nearest = d;
                best = i;
            }
        }
        return best;
    }

    /// <summary>The position on the views' time axis at <paramref name="x"/>; null outside the plot.</summary>
    public long? PositionAt(double x)
    {
        Rect plot = Plot;
        if (x < plot.X || x > plot.Right)
        {
            return null;
        }
        return _from + (long)Math.Round((x - plot.X) / plot.Width * (_to - _from));
    }

    public override void Render(DrawingContext context)
    {
        Rect plot = Plot;
        using (context.PushClip(plot.Inflate(new Thickness(6, 0))))
        {
            if (_picked is long picked && picked >= _from && picked <= _to)
            {
                double x = XOf(picked, plot);
                context.DrawLine(s_pickPen, new Point(x, plot.Y), new Point(x, plot.Bottom));
            }
            for (int i = 0; i < _marks.Count; i++)
            {
                (long position, MarkKind kind) = _marks[i];
                if (kind == MarkKind.Setup || position < _from || position > _to)
                {
                    continue;
                }
                double x = XOf(position, plot);
                bool selected = i == _selected;
                context.DrawLine((selected ? s_selectedPens : s_pens)[(int)kind], new Point(x, plot.Y + 14), new Point(x, plot.Bottom));
                FormattedText letter = new(LetterOf(kind), CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
                                           s_typeface, 11, s_brushes[(int)kind]);
                Point at = new(x - letter.Width / 2, plot.Y + 1);
                if (selected)
                {
                    context.DrawRectangle(s_letterBox, s_selectedPens[(int)kind],
                                          new Rect(at, new Size(letter.Width, letter.Height)).Inflate(2));
                }
                context.DrawText(letter, at);
            }
        }
    }
}
