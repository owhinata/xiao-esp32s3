// SPDX-License-Identifier: MIT
using System.Globalization;
using System.Runtime.InteropServices;
using AudioStudio.Core.Audio;
using AudioStudio.Core.Dsp;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace AudioStudio.App.Controls;

/// <summary>
/// The spectrogram of a stretch of a sample source: time runs to the right,
/// frequency upwards, the colour is the level. A strip on the right shows
/// the newest spectrum in view as a curve. The columns come from a
/// <see cref="Spectrogram"/>, which keeps them, so a picture that moves on
/// computes only its new columns.
/// </summary>
public sealed class SpectrogramControl : Control
{
    public const int Rows = 256;
    public const int MaxColumns = 1024;
    public const double AxisWidth = 46;
    public const double CurveWidth = 72;

    private static readonly uint[] s_palette = MakePalette();
    private static readonly IBrush s_background = new SolidColorBrush(Color.FromRgb(0x12, 0x12, 0x16));
    private static readonly IBrush s_text = new SolidColorBrush(Color.FromArgb(0xD0, 0xE0, 0xE0, 0xE0));
    private static readonly IPen s_grid = new Pen(new SolidColorBrush(Color.FromArgb(0x38, 0xFF, 0xFF, 0xFF)), 1);
    private static readonly IPen s_curve = new Pen(new SolidColorBrush(Color.FromRgb(0xF2, 0xC1, 0x4E)), 1.2);
    private static readonly Typeface s_typeface = new("monospace");

    private readonly WriteableBitmap _bitmap = new(new PixelSize(MaxColumns, Rows), new Vector(96, 96),
                                                   PixelFormat.Bgra8888, AlphaFormat.Opaque);
    private readonly int[] _pixels = new int[MaxColumns * Rows];
    private Spectrogram? _analysis;
    private SpectrumColumn? _latest;
    private int _columns;
    private double _firstCentre, _lastCentre, _from, _to;
    private float _minDb = -110, _maxDb = -20;
    private string _peak = "";

    public string Title { get; set; } = "Spectrogram";

    public SpectrogramControl()
    {
        ClipToBounds = true;
    }

    /// <summary>The levels shown as the darkest and the brightest colour.</summary>
    public void SetRange(float minDb, float maxDb)
    {
        _minDb = minDb;
        _maxDb = Math.Max(minDb + 1, maxDb);
    }

    public void Clear()
    {
        _analysis = null;
        _latest = null;
        _columns = 0;
        _peak = "";
        InvalidateVisual();
    }

    /// <summary>
    /// Shows the columns centred in <paramref name="from"/>..<paramref name="to"/>,
    /// computing at most <paramref name="budget"/> missing ones, the newest
    /// first. Returns whether all columns that can be had are in the picture.
    /// </summary>
    public bool Show(Spectrogram analysis, ISampleSource source, long from, long to, int budget)
    {
        _analysis = analysis;
        _from = from;
        _to = to;
        (long first, long last) = analysis.ColumnsCentredIn(from, to);
        if (last - first + 1 > MaxColumns)
        {
            first = last - MaxColumns + 1;
        }
        _columns = (int)Math.Max(0, last - first + 1);
        _firstCentre = analysis.CentreOf(first);
        _lastCentre = analysis.CentreOf(last);
        _latest = null;
        bool complete = true;
        int background = unchecked((int)s_palette[0]);
        float scale = (s_palette.Length - 1) / (_maxDb - _minDb);
        long end = source.End;
        for (long k = last; k >= first; k--)
        {
            int x = (int)(k - first);
            SpectrumColumn? column = analysis.Cached(k);
            if (column is null && k * analysis.Hop <= end && budget > 0)
            {
                budget--;
                column = analysis.Column(source, k);
            }
            else if (column is null && k * analysis.Hop <= end && k * analysis.Hop > source.Start)
            {
                complete = false;
            }
            _latest ??= column;
            for (int row = 0; row < Rows; row++)
            {
                int pixel = background;
                if (column is not null)
                {
                    int index = (int)Math.Clamp((column.Rows[row] - _minDb) * scale, 0, s_palette.Length - 1);
                    pixel = unchecked((int)s_palette[index]);
                }
                // Row 0 is the lowest frequency; the bitmap's first line is the top.
                _pixels[(Rows - 1 - row) * MaxColumns + x] = pixel;
            }
        }
        if (_columns > 0)
        {
            using ILockedFramebuffer frame = _bitmap.Lock();
            for (int y = 0; y < Rows; y++)
            {
                Marshal.Copy(_pixels, y * MaxColumns, frame.Address + y * frame.RowBytes, _columns);
            }
        }
        _peak = _latest is null ? "" :
            string.Create(CultureInfo.InvariantCulture, $"peak {_latest.PeakHz,7:F1} Hz {_latest.PeakDb,6:F1} dBFS");
        InvalidateVisual();
        return complete;
    }

    public override void Render(DrawingContext context)
    {
        Rect bounds = new(Bounds.Size);
        context.FillRectangle(s_background, bounds);
        Rect image = new(AxisWidth, 2, Math.Max(1, bounds.Width - AxisWidth - CurveWidth - 4), Math.Max(1, bounds.Height - 4));
        Rect curve = new(image.Right + 4, image.Y, CurveWidth, image.Height);
        Spectrogram? analysis = _analysis;

        if (analysis is not null && _columns > 0 && _to > _from)
        {
            // Each column covers one hop around its centre.
            double hop = analysis.Hop;
            double x0 = image.X + (_firstCentre - hop / 2 - _from) / (_to - _from) * image.Width;
            double x1 = image.X + (_lastCentre + hop / 2 - _from) / (_to - _from) * image.Width;
            using (context.PushClip(image))
            {
                context.DrawImage(_bitmap, new Rect(0, 0, _columns, Rows), new Rect(x0, image.Y, x1 - x0, image.Height));
            }
        }

        if (analysis is not null)
        {
            foreach (double hz in Ticks(analysis))
            {
                double y = image.Bottom - analysis.HeightOf(hz) * image.Height;
                if (y < image.Y - 0.5 || y > image.Bottom + 0.5)
                {
                    continue;
                }
                context.DrawLine(s_grid, new Point(image.X, y), new Point(curve.Right, y));
                FormattedText label = Text(hz >= 1000 ? $"{hz / 1000:0.#}k" : $"{hz:0}");
                context.DrawText(label, new Point(AxisWidth - 4 - label.Width,
                                                  Math.Clamp(y - label.Height / 2, 0, bounds.Height - label.Height)));
            }
        }

        if (_latest is SpectrumColumn latest)
        {
            StreamGeometry geometry = new();
            using (StreamGeometryContext g = geometry.Open())
            {
                for (int row = 0; row < Rows; row++)
                {
                    double level = Math.Clamp((latest.Rows[row] - _minDb) / (_maxDb - _minDb), 0, 1);
                    Point p = new(curve.X + level * curve.Width, curve.Bottom - (row + 0.5) / Rows * curve.Height);
                    if (row == 0)
                    {
                        g.BeginFigure(p, false);
                    }
                    else
                    {
                        g.LineTo(p);
                    }
                }
                g.EndFigure(false);
            }
            context.DrawGeometry(null, s_curve, geometry);
        }

        FormattedText title = Text(analysis is null ? Title : string.Create(CultureInfo.InvariantCulture,
            $"{Title} [dBFS]  FFT {analysis.FftSize} ({analysis.BinHz:F1} Hz bins), step {analysis.Hop}"));
        context.DrawText(title, new Point(image.X + 6, image.Y + 3));
        if (_peak.Length > 0)
        {
            context.DrawText(Text(_peak), new Point(image.X + 6, image.Y + 4 + title.Height));
        }
    }

    private static IEnumerable<double> Ticks(Spectrogram analysis)
    {
        double max = analysis.MaxHz;
        if (analysis.LogFrequency)
        {
            for (double decade = 10; decade <= max; decade *= 10)
            {
                foreach (double m in new[] { 1.0, 2.0, 5.0 })
                {
                    if (decade * m >= Spectrogram.LogFloorHz * 2 && decade * m <= max)
                    {
                        yield return decade * m;
                    }
                }
            }
            yield break;
        }
        // About five steps of 1, 2 or 5 times a power of ten.
        double raw = max / 5;
        double power = Math.Pow(10, Math.Floor(Math.Log10(raw)));
        double step = power * (raw / power < 1.5 ? 1 : raw / power < 3.5 ? 2 : raw / power < 7.5 ? 5 : 10);
        for (double hz = step; hz <= max + step * 0.01; hz += step)
        {
            yield return hz;
        }
    }

    private static FormattedText Text(string text) =>
        new(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, s_typeface, 11, s_text);

    /// <summary>Dark blue through purple and orange to pale yellow: brighter is stronger.</summary>
    private static uint[] MakePalette()
    {
        (double At, int R, int G, int B)[] stops =
        [
            (0.00, 0, 0, 4), (0.15, 30, 12, 80), (0.35, 100, 22, 120), (0.55, 180, 54, 90),
            (0.75, 240, 110, 30), (0.90, 250, 190, 50), (1.00, 252, 255, 180),
        ];
        uint[] palette = new uint[256];
        for (int i = 0; i < palette.Length; i++)
        {
            double t = i / (double)(palette.Length - 1);
            int s = 0;
            while (s < stops.Length - 2 && t > stops[s + 1].At)
            {
                s++;
            }
            double f = (t - stops[s].At) / (stops[s + 1].At - stops[s].At);
            uint r = (uint)(stops[s].R + f * (stops[s + 1].R - stops[s].R));
            uint g = (uint)(stops[s].G + f * (stops[s + 1].G - stops[s].G));
            uint b = (uint)(stops[s].B + f * (stops[s + 1].B - stops[s].B));
            palette[i] = 0xFF000000 | r << 16 | g << 8 | b;
        }
        return palette;
    }
}
