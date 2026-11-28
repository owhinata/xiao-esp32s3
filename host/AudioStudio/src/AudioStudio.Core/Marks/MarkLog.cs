// SPDX-License-Identifier: MIT
namespace AudioStudio.Core.Marks;

/// <summary>
/// The marks of a recording as they are made, and their file. Every change
/// that matters is written at once: the whole file is written again (see
/// <see cref="MarkFile.Write"/>), sorted, so the file on disk is complete
/// after each step. A mark is written the moment it is made, without its
/// text; the memo typed for it afterwards is written when it is stored.
/// Without any mark no file is written. Not thread-safe: one thread uses it.
/// The application's rows (indications, setup rows) are added with their
/// own methods.
/// </summary>
public sealed class MarkLog
{
    private readonly List<Mark> _marks = new();
    private string? _lastSetup;

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
    /// <see cref="MarkKind.Other"/> a memo is then open on the new mark;
    /// <see cref="MarkKind.Swallow"/> and <see cref="MarkKind.False"/> take
    /// none (a text is added later in the mark editor of the opened file).
    /// Returns the new mark's handle.
    /// </summary>
    public int Add(long sample, MarkKind kind, string typedSoFar = "")
    {
        if (!MarkFile.IsUserKind(kind))
        {
            throw new ArgumentException("Indications and setup rows are added with AddIndication and AddSetup.", nameof(kind));
        }
        if (MemoMark is int open)
        {
            Replace(open, _marks[open] with { Text = typedSoFar });
            MemoMark = null;
        }
        _marks.Add(new Mark(sample, kind));
        int index = _marks.Count - 1;
        if (kind == MarkKind.Other)
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

    /// <summary>
    /// The detector setup (<see cref="DetectorSetup"/>) from
    /// <paramref name="sample"/> on, written at once. Nothing is added when
    /// it is the text of the last setup row; returns the new row's handle,
    /// or null then.
    /// </summary>
    public int? AddSetup(long sample, string setup)
    {
        if (setup == _lastSetup)
        {
            return null;
        }
        _lastSetup = setup;
        _marks.Add(new Mark(sample, MarkKind.Setup, null, setup));
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
