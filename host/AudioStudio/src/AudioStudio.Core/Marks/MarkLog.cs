// SPDX-License-Identifier: MIT
namespace AudioStudio.Core.Marks;

/// <summary>
/// The marks of a recording as they are made, and their file. Every change
/// that matters is written at once: the whole file is written again (see
/// <see cref="MarkFile.Write"/>), sorted, so the file on disk is complete
/// after each step. A mark is written the moment it is made, without its
/// text; the memo typed for it afterwards is written when it is stored.
/// Without any mark no file is written. Not thread-safe: one thread uses it.
/// </summary>
public sealed class MarkLog
{
    private readonly List<Mark> _marks = new();

    public MarkLog(string path, int sampleRate)
    {
        Path = path;
        SampleRate = sampleRate;
    }

    public string Path { get; }
    public int SampleRate { get; }
    /// <summary>Changes on every change of the marks, for the views to redraw.</summary>
    public int Version { get; private set; }
    /// <summary>The reason the last write failed; null after a write that worked.</summary>
    public string? Error { get; private set; }

    /// <summary>The mark whose memo is being typed; null when no memo is open.</summary>
    public int? MemoMark { get; private set; }

    /// <summary>The marks in the order they were made; the handles of the other methods index this.</summary>
    public IReadOnlyList<Mark> Marks => _marks;

    /// <summary>
    /// A mark made by the user at <paramref name="sample"/>. A memo still
    /// open keeps <paramref name="typedSoFar"/> as its text first. For
    /// <see cref="MarkKind.False"/> and <see cref="MarkKind.Other"/> a memo
    /// is then open on the new mark. Returns the new mark's handle.
    /// </summary>
    public int Add(long sample, MarkKind kind, string typedSoFar = "")
    {
        if (kind == MarkKind.Indication)
        {
            throw new ArgumentException("An indication is added with AddIndication.", nameof(kind));
        }
        if (MemoMark is int open)
        {
            Replace(open, _marks[open] with { Text = typedSoFar });
            MemoMark = null;
        }
        _marks.Add(new Mark(sample, kind));
        int index = _marks.Count - 1;
        if (kind is MarkKind.False or MarkKind.Other)
        {
            MemoMark = index;
        }
        Changed(save: true);
        return index;
    }

    /// <summary>Enter in the memo box: the text becomes the open mark's.</summary>
    public void StoreMemo(string text)
    {
        if (MemoMark is int open)
        {
            MemoMark = null;
            Replace(open, _marks[open] with { Text = text });
            Changed(save: true);
        }
    }

    /// <summary>Escape in the memo box: the mark stays, with no text.</summary>
    public void CancelMemo()
    {
        if (MemoMark is int open)
        {
            MemoMark = null;
            Replace(open, _marks[open] with { Text = "" });
            Changed(save: true);
        }
    }

    /// <summary>An indication of the detector; written at once.</summary>
    public int AddIndication(long sample, double probability)
    {
        _marks.Add(new Mark(sample, MarkKind.Indication, probability));
        Changed(save: true);
        return _marks.Count - 1;
    }

    /// <summary>A higher probability of an indication; written when <paramref name="save"/> is set.</summary>
    public void SetProbability(int index, double probability, bool save)
    {
        Replace(index, _marks[index] with { Probability = probability });
        Changed(save);
    }

    /// <summary>The end of the recording: an open memo keeps what was typed, and the file is written once more.</summary>
    public void Finish(string typedSoFar)
    {
        if (MemoMark is int open)
        {
            MemoMark = null;
            Replace(open, _marks[open] with { Text = typedSoFar });
        }
        Changed(save: true);
    }

    private void Replace(int index, Mark mark) => _marks[index] = mark;

    private void Changed(bool save)
    {
        Version++;
        if (!save || _marks.Count == 0)
        {
            return;
        }
        try
        {
            MarkFile.Write(Path, _marks, SampleRate);
            Error = null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Error = e.Message;
        }
    }
}
