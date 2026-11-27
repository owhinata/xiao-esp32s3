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
/// the top (S swallow, F false, O other, I indication). It lies over the
/// other views and takes no input; the margins are those of
/// <see cref="SpectrogramControl"/>, so the lines meet the views' time axis.
/// </summary>
public sealed class MarksControl : Control
{
    private static readonly Typeface s_typeface = new("monospace");
    private static readonly IBrush s_swallow = new SolidColorBrush(Color.FromRgb(0xE0, 0x70, 0xF0));
    private static readonly IBrush s_false = new SolidColorBrush(Color.FromRgb(0xFF, 0x6B, 0x5B));
    private static readonly IBrush s_other = new SolidColorBrush(Color.FromRgb(0xE8, 0xE8, 0xE8));
    private static readonly IBrush s_indication = new SolidColorBrush(Color.FromRgb(0x6C, 0xD6, 0x7C));

    private static readonly IPen[] s_pens =
        [.. new[] { s_swallow, s_false, s_other, s_indication }.Select(b => (IPen)new Pen(b, 1, new DashStyle([3, 2], 0)))];

    private List<(long Position, MarkKind Kind)> _marks = [];
    private long _from, _to;

    public MarksControl()
    {
        ClipToBounds = true;
        IsHitTestVisible = false;
    }

    private static IBrush BrushOf(MarkKind kind) => kind switch
    {
        MarkKind.Swallow => s_swallow,
        MarkKind.False => s_false,
        MarkKind.Other => s_other,
        _ => s_indication,
    };

    private static string LetterOf(MarkKind kind) => kind switch
    {
        MarkKind.Swallow => "S",
        MarkKind.False => "F",
        MarkKind.Other => "O",
        _ => "I",
    };

    public void Show(List<(long Position, MarkKind Kind)> marks, long from, long to)
    {
        _marks = marks;
        _from = from;
        _to = to;
        InvalidateVisual();
    }

    public override void Render(DrawingContext context)
    {
        Rect bounds = new(Bounds.Size);
        Rect plot = new(SpectrogramControl.AxisWidth, 0,
                        Math.Max(1, bounds.Width - SpectrogramControl.AxisWidth - SpectrogramControl.CurveWidth - 4),
                        bounds.Height);
        double span = Math.Max(1, _to - _from);
        using (context.PushClip(plot.Inflate(new Thickness(6, 0))))
        {
            foreach ((long position, MarkKind kind) in _marks)
            {
                if (position < _from || position > _to)
                {
                    continue;
                }
                double x = Math.Round(plot.X + (position - _from) / span * plot.Width) + 0.5;
                IBrush brush = BrushOf(kind);
                context.DrawLine(s_pens[(int)kind], new Point(x, plot.Y + 14), new Point(x, plot.Bottom));
                FormattedText letter = new(LetterOf(kind), CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
                                           s_typeface, 11, brush);
                context.DrawText(letter, new Point(x - letter.Width / 2, plot.Y + 1));
            }
        }
    }
}
