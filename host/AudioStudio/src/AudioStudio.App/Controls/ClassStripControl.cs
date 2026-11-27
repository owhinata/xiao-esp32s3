// SPDX-License-Identifier: MIT
using System.Globalization;
using AudioStudio.Core.Model;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace AudioStudio.App.Controls;

/// <summary>
/// The model's top class along the timeline: each result colours the last
/// hop of its window, with the class name where there is room. The
/// margins are those of <see cref="SpectrogramControl"/>.
/// </summary>
public sealed class ClassStripControl : Control
{
    private static readonly IBrush s_background = new SolidColorBrush(Color.FromRgb(0x12, 0x12, 0x16));
    private static readonly IBrush s_text = new SolidColorBrush(Color.FromArgb(0xE0, 0xF0, 0xF0, 0xF0));
    private static readonly IBrush s_dim = new SolidColorBrush(Color.FromArgb(0x90, 0xE0, 0xE0, 0xE0));
    private static readonly Typeface s_typeface = new("monospace");

    public static readonly Color[] Colors =
    [
        Color.FromRgb(0x4E, 0x79, 0xA7), Color.FromRgb(0xF2, 0x8E, 0x2B), Color.FromRgb(0x59, 0xA1, 0x4F),
        Color.FromRgb(0xE1, 0x57, 0x59), Color.FromRgb(0x76, 0xB7, 0xB2), Color.FromRgb(0xED, 0xC9, 0x48),
        Color.FromRgb(0xB0, 0x7A, 0xA1), Color.FromRgb(0xFF, 0x9D, 0xA7), Color.FromRgb(0x9C, 0x75, 0x5F),
        Color.FromRgb(0xBA, 0xB0, 0xAC),
    ];
    private static readonly IBrush[] s_brushes = Colors.Select(c => (IBrush)new SolidColorBrush(c)).ToArray();

    private List<ClassResult> _results = [];
    private IReadOnlyList<string> _labels = [];
    private long _from, _to;
    private int _hop = 1;
    private string _note = "";

    public ClassStripControl()
    {
        ClipToBounds = true;
    }

    public static IBrush BrushOf(int index) => s_brushes[index % s_brushes.Length];

    public void Show(List<ClassResult> results, IReadOnlyList<string> labels, long from, long to, int hop)
    {
        _results = results;
        _labels = labels;
        _from = from;
        _to = to;
        _hop = Math.Max(1, hop);
        _note = "";
        InvalidateVisual();
    }

    /// <summary>No results to show; <paramref name="note"/> says why.</summary>
    public void Clear(string note)
    {
        _results = [];
        _note = note;
        InvalidateVisual();
    }

    public override void Render(DrawingContext context)
    {
        Rect bounds = new(Bounds.Size);
        context.FillRectangle(s_background, bounds);
        Rect strip = new(SpectrogramControl.AxisWidth, 2,
                         Math.Max(1, bounds.Width - SpectrogramControl.AxisWidth - SpectrogramControl.CurveWidth - 4),
                         Math.Max(1, bounds.Height - 4));
        context.DrawText(Text("class", s_dim), new Point(4, strip.Y + strip.Height / 2 - 7));
        if (_note.Length > 0)
        {
            context.DrawText(Text(_note, s_dim), new Point(strip.X + 6, strip.Y + strip.Height / 2 - 7));
            return;
        }
        double span = Math.Max(1, _to - _from);
        // Runs of one class: coloured first, then named where there is room.
        List<(int Top, double X0, double X1)> runs = new();
        foreach (ClassResult r in _results)
        {
            double x0 = strip.X + (r.End - _hop - _from) / span * strip.Width;
            double x1 = strip.X + (r.End - _from) / span * strip.Width;
            if (runs.Count > 0 && runs[^1].Top == r.Top && x0 <= runs[^1].X1 + 0.5)
            {
                runs[^1] = (r.Top, runs[^1].X0, x1);
            }
            else
            {
                runs.Add((r.Top, x0, x1));
            }
        }
        using (context.PushClip(strip))
        {
            foreach ((int top, double x0, double x1) in runs)
            {
                context.FillRectangle(BrushOf(top), new Rect(x0, strip.Y, Math.Max(1, x1 - x0), strip.Height));
            }
            foreach ((int top, double x0, double x1) in runs)
            {
                double left = Math.Max(x0, strip.X), right = Math.Min(x1, strip.Right);
                FormattedText label = Text(top < _labels.Count ? _labels[top] : $"class {top}", s_text);
                if (right - left >= label.Width + 6)
                {
                    context.DrawText(label, new Point(left + 3, strip.Y + strip.Height / 2 - label.Height / 2));
                }
            }
        }
    }

    private static FormattedText Text(string text, IBrush brush) =>
        new(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, s_typeface, 11, brush);
}
