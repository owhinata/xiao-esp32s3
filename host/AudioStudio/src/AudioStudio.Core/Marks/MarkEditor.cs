// SPDX-License-Identifier: MIT
namespace AudioStudio.Core.Marks;

/// <summary>
/// The marks of an opened WAV file as the user edits them, and their file.
/// The marks are kept sorted by sample index (ties in the order they came)
/// and the methods take and give indices into <see cref="Marks"/>. Every
/// change is written at once as the whole file (see
/// <see cref="MarkFile.Write"/>); when the last row goes, the file is
/// removed. A change whose write fails is not made: the marks stay as they
/// are on disk and <see cref="Error"/> says why.
/// <para>
/// Indications are the record of what the detector showed, and setup rows
/// of which detector setup showed it: their time, probability, kind and
/// text are not edited, they are not deleted and none is added (an attempt
/// throws <see cref="InvalidOperationException"/> or
/// <see cref="ArgumentException"/>); every rewrite keeps them as they are,
/// in their order. The user's note
/// that one was wrong is a <c>false</c> mark at its sample
/// (<see cref="MarkWrong"/>), which <see cref="MarkReview"/> attaches to it.
/// </para>
/// Not thread-safe: one thread uses it.
/// </summary>
public sealed class MarkEditor
{
    private List<Mark> _marks;

    public MarkEditor(string path, int sampleRate, IEnumerable<Mark> marks)
    {
        Path = path;
        SampleRate = sampleRate;
        _marks = MarkFile.Sorted(marks);
    }

    public string Path { get; }
    public int SampleRate { get; }
    /// <summary>The marks sorted by sample index.</summary>
    public IReadOnlyList<Mark> Marks => _marks;
    /// <summary>The reason the last change was not written (and not made); null after one that was.</summary>
    public string? Error { get; private set; }

    public MarkReviewResult Review() => MarkReview.Review(_marks, SampleRate);

    /// <summary>A mark of the user at <paramref name="sample"/>; its index, or null when it could not be written.</summary>
    public int? Add(long sample, MarkKind kind, string text = "")
    {
        RequireUserKind(kind);
        ArgumentOutOfRangeException.ThrowIfNegative(sample);
        Mark added = new(sample, kind, null, text);
        return Save([.. _marks, added]) ? IndexOf(added) : null;
    }

    /// <summary>Changes the kind and text of a user's mark; false when it could not be written.</summary>
    public bool Edit(int index, MarkKind kind, string text)
    {
        Mark mark = UserMark(index);
        RequireUserKind(kind);
        if (mark.Kind == kind && mark.Text == text)
        {
            Error = null;
            return true;
        }
        List<Mark> marks = [.. _marks];
        marks[index] = mark with { Kind = kind, Text = text };
        return Save(marks);
    }

    /// <summary>Moves a user's mark to <paramref name="sample"/>; its new index, or null when it could not be written.</summary>
    public int? Move(int index, long sample)
    {
        Mark mark = UserMark(index);
        ArgumentOutOfRangeException.ThrowIfNegative(sample);
        if (mark.Sample == sample)
        {
            Error = null;
            return index;
        }
        Mark moved = mark with { Sample = sample };
        List<Mark> marks = [.. _marks];
        marks.RemoveAt(index);
        marks.Add(moved);
        return Save(marks) ? IndexOf(moved) : null;
    }

    /// <summary>Deletes a user's mark; false when it could not be written.</summary>
    public bool Delete(int index)
    {
        UserMark(index);
        List<Mark> marks = [.. _marks];
        marks.RemoveAt(index);
        return Save(marks);
    }

    /// <summary>The index of the <c>false</c> mark attached to the indication at <paramref name="index"/>; null when it has none.</summary>
    public int? FalseMarkOf(int index)
    {
        Mark indication = IndicationAt(index);
        ReviewedIndication reviewed = Review().Indications.First(r => ReferenceEquals(r.Indication, indication));
        return reviewed.MarkedFalse is Mark f ? IndexOf(f) : null;
    }

    /// <summary>
    /// The user's note that the indication at <paramref name="index"/> was
    /// wrong: a <c>false</c> mark at the indication's sample, which the
    /// pairing of <see cref="MarkReview"/> attaches to it. When it already
    /// has one, that one's index is given and nothing changes. Null when the
    /// new mark could not be written.
    /// </summary>
    public int? MarkWrong(int index, string text = "")
    {
        if (FalseMarkOf(index) is int existing)
        {
            Error = null;
            return existing;
        }
        return Add(_marks[index].Sample, MarkKind.False, text);
    }

    private static void RequireUserKind(MarkKind kind)
    {
        if (!MarkFile.IsUserKind(kind))
        {
            throw new ArgumentException("Indications and setup rows come from the application; they are not made by hand.", nameof(kind));
        }
    }

    private Mark UserMark(int index)
    {
        Mark mark = _marks[index];
        if (!MarkFile.IsUserKind(mark.Kind))
        {
            throw new InvalidOperationException("An indication or a setup row is the record of what the detector showed; it is not edited or deleted.");
        }
        return mark;
    }

    private Mark IndicationAt(int index)
    {
        Mark mark = _marks[index];
        if (mark.Kind != MarkKind.Indication)
        {
            throw new InvalidOperationException("The mark is not an indication.");
        }
        return mark;
    }

    private int IndexOf(Mark mark) => _marks.FindIndex(m => ReferenceEquals(m, mark));

    /// <summary>Writes the marks (or removes the file for none) and takes them; on a failure keeps the old ones.</summary>
    private bool Save(List<Mark> marks)
    {
        List<Mark> sorted = MarkFile.Sorted(marks);
        try
        {
            if (sorted.Count == 0)
            {
                File.Delete(Path);
            }
            else
            {
                MarkFile.Write(Path, sorted, SampleRate);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Error = e.Message;
            return false;
        }
        _marks = sorted;
        Error = null;
        return true;
    }
}
