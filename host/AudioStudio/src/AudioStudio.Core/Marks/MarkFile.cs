// SPDX-License-Identifier: MIT
using System.Globalization;
using System.Text;

namespace AudioStudio.Core.Marks;

public enum MarkKind
{
    /// <summary>The user swallowed now.</summary>
    Swallow,
    /// <summary>The user's note that the indication just shown was wrong.</summary>
    False,
    /// <summary>Anything else the user notes.</summary>
    Other,
    /// <summary>A swallow indication of the detector, at its start, with its highest probability.</summary>
    Indication,
    /// <summary>The detector setup from here on, as <see cref="DetectorSetup"/> writes it in the text.</summary>
    Setup,
}

/// <summary>One timed note of a recording.</summary>
/// <param name="Sample">The sample index in the WAV file.</param>
/// <param name="Probability">The indication's highest probability; null for the other kinds.</param>
/// <param name="Text">The user's text; for a setup row the setup.</param>
public sealed record Mark(long Sample, MarkKind Kind, double? Probability = null, string Text = "")
{
    public double Seconds(int sampleRate) => Sample / (double)sampleRate;
}

/// <summary>A marks file that cannot be read.</summary>
public sealed class MarkFileException(string message) : Exception(message);

/// <summary>
/// The marks file of a recording: <c>name.marks.csv</c> beside <c>name.wav</c>,
/// UTF-8, the header line <see cref="Header"/> and one row per mark sorted by
/// the sample index: sample, seconds (3 decimals), kind, probability (empty
/// unless an indication) and text, quoted as CSV when it holds a comma, a
/// quote or a line break. Rows at the same sample index keep their order.
/// </summary>
public static class MarkFile
{
    public const string Suffix = ".marks.csv";
    public const string Header = "sample,seconds,kind,probability,text";

    /// <summary>The marks file that belongs to a WAV file.</summary>
    public static string PathFor(string wavPath) => Path.ChangeExtension(wavPath, null) + Suffix;

    public static string KindName(MarkKind kind) => kind switch
    {
        MarkKind.Swallow => "swallow",
        MarkKind.False => "false",
        MarkKind.Other => "other",
        MarkKind.Indication => "indication",
        _ => "setup",
    };

    /// <summary>
    /// The kinds the user makes and edits: <c>swallow</c>, <c>false</c> and
    /// <c>other</c>. Indications and setup rows are written by the application.
    /// </summary>
    public static bool IsUserKind(MarkKind kind) => kind is MarkKind.Swallow or MarkKind.False or MarkKind.Other;

    public static MarkKind? ParseKind(string name) => name switch
    {
        "swallow" => MarkKind.Swallow,
        "false" => MarkKind.False,
        "other" => MarkKind.Other,
        "indication" => MarkKind.Indication,
        "setup" => MarkKind.Setup,
        _ => null,
    };

    /// <summary>The marks sorted by sample index; marks at the same index keep their order.</summary>
    public static List<Mark> Sorted(IEnumerable<Mark> marks) => marks.OrderBy(m => m.Sample).ToList();

    public static string Format(IEnumerable<Mark> marks, int sampleRate)
    {
        StringBuilder text = new();
        text.Append(Header).Append('\n');
        foreach (Mark m in Sorted(marks))
        {
            text.Append(m.Sample.ToString(CultureInfo.InvariantCulture)).Append(',')
                .Append(m.Seconds(sampleRate).ToString("0.000", CultureInfo.InvariantCulture)).Append(',')
                .Append(KindName(m.Kind)).Append(',')
                .Append(m.Probability is double p ? p.ToString("0.0000", CultureInfo.InvariantCulture) : "").Append(',')
                .Append(Quote(m.Text)).Append('\n');
        }
        return text.ToString();
    }

    private static string Quote(string field) =>
        field.AsSpan().IndexOfAny(",\"\r\n") < 0 ? field : "\"" + field.Replace("\"", "\"\"") + "\"";

    /// <summary>
    /// Writes the marks to <paramref name="path"/> as a whole: to a temporary
    /// file beside it, to the disk, then renamed over the old one, so the
    /// file on disk is always complete.
    /// </summary>
    public static void Write(string path, IEnumerable<Mark> marks, int sampleRate)
    {
        string temporary = path + ".tmp";
        using (FileStream file = new(temporary, FileMode.Create, FileAccess.Write, FileShare.None))
        {
            file.Write(new UTF8Encoding(false).GetBytes(Format(marks, sampleRate)));
            file.Flush(flushToDisk: true);
        }
        File.Move(temporary, path, overwrite: true);
    }

    public static List<Mark> Read(string path)
    {
        string text;
        try
        {
            text = File.ReadAllText(path, Encoding.UTF8);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            throw new MarkFileException($"Cannot read {path}: {e.Message}");
        }
        return Parse(text, Path.GetFileName(path));
    }

    /// <summary>The marks of a file's text, sorted by sample index; the sample index is what counts, the seconds are not read.</summary>
    public static List<Mark> Parse(string text, string name = "the marks file")
    {
        List<List<string>> rows = Rows(text, name);
        if (rows.Count == 0 || string.Join(",", rows[0]) != Header)
        {
            throw new MarkFileException($"{name}: the first line is not \"{Header}\".");
        }
        List<Mark> marks = new();
        for (int i = 1; i < rows.Count; i++)
        {
            List<string> r = rows[i];
            if (r.Count == 1 && r[0].Length == 0)
            {
                continue;
            }
            if (r.Count != 5)
            {
                throw new MarkFileException($"{name}: row {i + 1} has {r.Count} fields, not 5.");
            }
            if (!long.TryParse(r[0], NumberStyles.Integer, CultureInfo.InvariantCulture, out long sample) || sample < 0)
            {
                throw new MarkFileException($"{name}: row {i + 1}: \"{r[0]}\" is not a sample index.");
            }
            if (ParseKind(r[2]) is not MarkKind kind)
            {
                throw new MarkFileException($"{name}: row {i + 1}: unknown kind \"{r[2]}\".");
            }
            double? probability = null;
            if (r[3].Length > 0)
            {
                if (!double.TryParse(r[3], NumberStyles.Float, CultureInfo.InvariantCulture, out double p))
                {
                    throw new MarkFileException($"{name}: row {i + 1}: \"{r[3]}\" is not a probability.");
                }
                probability = p;
            }
            marks.Add(new Mark(sample, kind, probability, r[4]));
        }
        return Sorted(marks);
    }

    /// <summary>The CSV rows of the text: quoted fields may hold commas, doubled quotes and line breaks.</summary>
    private static List<List<string>> Rows(string text, string name)
    {
        List<List<string>> rows = new();
        List<string> row = new();
        StringBuilder field = new();
        bool quoted = false, any = false;
        for (int i = 0; i < text.Length; i++)
        {
            char c = text[i];
            if (quoted)
            {
                if (c != '"')
                {
                    field.Append(c);
                }
                else if (i + 1 < text.Length && text[i + 1] == '"')
                {
                    field.Append('"');
                    i++;
                }
                else
                {
                    quoted = false;
                }
                continue;
            }
            any = true;
            switch (c)
            {
                case '"' when field.Length == 0:
                    quoted = true;
                    break;
                case ',':
                    row.Add(field.ToString());
                    field.Clear();
                    break;
                case '\r' when i + 1 < text.Length && text[i + 1] == '\n':
                    break;
                case '\n':
                case '\r':
                    row.Add(field.ToString());
                    field.Clear();
                    rows.Add(row);
                    row = new();
                    any = false;
                    break;
                default:
                    field.Append(c);
                    break;
            }
        }
        if (quoted)
        {
            throw new MarkFileException($"{name}: a quoted field is not closed.");
        }
        if (any || field.Length > 0 || row.Count > 0)
        {
            row.Add(field.ToString());
            rows.Add(row);
        }
        return rows;
    }
}
