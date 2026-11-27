// SPDX-License-Identifier: MIT
using AudioStudio.Core.Marks;
using FluentAssertions;
using Xunit;

namespace AudioStudio.Core.Tests;

public sealed class MarkEditorTests : IDisposable
{
    private const int Rate = 16000;
    private readonly string _dir = Directory.CreateTempSubdirectory("audiostudio-edit-").FullName;

    public void Dispose() => Directory.Delete(_dir, recursive: true);

    private string FilePath => Path.Combine(_dir, "20261127-121827.marks.csv");

    private static readonly Mark[] s_recorded =
    [
        new(16000, MarkKind.Indication, 0.9),
        new(48000, MarkKind.Swallow),
        new(80000, MarkKind.Indication, 0.8),
    ];

    private MarkEditor Recorded()
    {
        MarkFile.Write(FilePath, s_recorded, Rate);
        return new MarkEditor(FilePath, Rate, MarkFile.Read(FilePath));
    }

    private List<Mark> OnDisk() => MarkFile.Read(FilePath);

    [Fact]
    public void A_file_without_marks_gets_its_file_with_the_first_mark()
    {
        MarkEditor editor = new(FilePath, Rate, []);
        editor.Add(32000, MarkKind.Other, "first").Should().Be(0);
        OnDisk().Should().Equal(new Mark(32000, MarkKind.Other, null, "first"));
        editor.Error.Should().BeNull();
        File.Exists(FilePath + ".tmp").Should().BeFalse();
    }

    [Fact]
    public void An_added_mark_is_sorted_in_and_its_index_given()
    {
        MarkEditor editor = Recorded();
        editor.Add(64000, MarkKind.Swallow).Should().Be(2);
        editor.Marks.Select(m => m.Sample).Should().Equal(16000, 48000, 64000, 80000);
        OnDisk().Should().Equal(editor.Marks);
    }

    [Fact]
    public void A_mark_added_at_the_sample_of_another_comes_after_it()
    {
        MarkEditor editor = Recorded();
        editor.Add(48000, MarkKind.Other, "second").Should().Be(2);
        editor.Marks[1].Kind.Should().Be(MarkKind.Swallow);
        OnDisk().Should().Equal(editor.Marks);
    }

    [Fact]
    public void Edit_changes_the_kind_and_the_text_on_disk()
    {
        MarkEditor editor = Recorded();
        editor.Edit(1, MarkKind.False, "coughed").Should().BeTrue();
        OnDisk()[1].Should().Be(new Mark(48000, MarkKind.False, null, "coughed"));
        editor.Marks.Should().Equal(OnDisk());
    }

    [Fact]
    public void Text_with_commas_quotes_and_line_breaks_survives_the_file()
    {
        MarkEditor editor = Recorded();
        const string text = "said \"ah\", then\na second line\r\nand a third";
        editor.Edit(1, MarkKind.Other, text).Should().BeTrue();
        OnDisk()[1].Text.Should().Be(text);
        new MarkEditor(FilePath, Rate, OnDisk()).Marks.Should().Equal(editor.Marks);
    }

    [Fact]
    public void Move_sorts_the_mark_again_and_gives_its_new_index()
    {
        MarkEditor editor = Recorded();
        editor.Move(1, 96000).Should().Be(2);
        editor.Marks.Select(m => (m.Sample, m.Kind)).Should().Equal(
            (16000L, MarkKind.Indication), (80000L, MarkKind.Indication), (96000L, MarkKind.Swallow));
        OnDisk().Should().Equal(editor.Marks);
        editor.Move(2, 0).Should().Be(0);
        OnDisk()[0].Should().Be(new Mark(0, MarkKind.Swallow));
    }

    [Fact]
    public void Delete_removes_the_mark_from_the_file()
    {
        MarkEditor editor = Recorded();
        editor.Delete(1).Should().BeTrue();
        OnDisk().Should().Equal(s_recorded[0], s_recorded[2]);
    }

    [Fact]
    public void Deleting_the_last_mark_removes_the_file()
    {
        MarkEditor editor = new(FilePath, Rate, []);
        editor.Add(8000, MarkKind.Swallow);
        editor.Add(9000, MarkKind.Other);
        editor.Delete(0).Should().BeTrue();
        File.Exists(FilePath).Should().BeTrue();
        editor.Delete(0).Should().BeTrue();
        File.Exists(FilePath).Should().BeFalse();
        editor.Marks.Should().BeEmpty();
    }

    [Fact]
    public void An_indication_is_not_edited_moved_or_deleted()
    {
        MarkEditor editor = Recorded();
        string before = File.ReadAllText(FilePath);
        editor.Invoking(e => e.Edit(0, MarkKind.False, "x")).Should().Throw<InvalidOperationException>();
        editor.Invoking(e => e.Move(0, 20000)).Should().Throw<InvalidOperationException>();
        editor.Invoking(e => e.Delete(2)).Should().Throw<InvalidOperationException>();
        editor.Invoking(e => e.Add(1000, MarkKind.Indication)).Should().Throw<ArgumentException>();
        editor.Invoking(e => e.Edit(1, MarkKind.Indication, "")).Should().Throw<ArgumentException>();
        editor.Marks.Should().Equal(s_recorded);
        File.ReadAllText(FilePath).Should().Be(before);
    }

    [Fact]
    public void Mark_wrong_adds_a_false_mark_that_the_review_attaches_to_the_indication()
    {
        MarkEditor editor = Recorded();
        int? f = editor.MarkWrong(2, "a cough");
        f.Should().Be(3);
        editor.Marks[3].Should().Be(new Mark(80000, MarkKind.False, null, "a cough"));
        OnDisk().Should().Equal(editor.Marks);
        MarkReviewResult r = editor.Review();
        r.Indications[1].MarkedFalse.Should().BeSameAs(editor.Marks[3]);
        r.Indications[0].IsHit.Should().BeTrue();
        r.MarkedFalse.Should().Be(1);
        editor.FalseMarkOf(2).Should().Be(3);
        editor.FalseMarkOf(0).Should().BeNull();
    }

    [Fact]
    public void Mark_wrong_attaches_to_an_earlier_indication_whose_later_one_is_already_marked()
    {
        MarkEditor editor = Recorded();
        editor.MarkWrong(2);
        int? f = editor.MarkWrong(0);
        f.Should().Be(1);
        MarkReviewResult r = editor.Review();
        r.Indications[0].MarkedFalse.Should().BeSameAs(editor.Marks[1]);
        r.Indications[1].MarkedFalse.Should().BeSameAs(editor.Marks[4]);
        r.UnattachedFalse.Should().BeEmpty();
    }

    [Fact]
    public void Mark_wrong_on_an_indication_already_marked_gives_its_false_mark()
    {
        // The false mark was made during the recording, a little after the indication.
        MarkFile.Write(FilePath, [.. s_recorded, new Mark(88000, MarkKind.False, null, "live")], Rate);
        MarkEditor editor = new(FilePath, Rate, MarkFile.Read(FilePath));
        string before = File.ReadAllText(FilePath);
        editor.MarkWrong(2, "again").Should().Be(3);
        editor.Marks.Should().HaveCount(4);
        File.ReadAllText(FilePath).Should().Be(before);
    }

    [Fact]
    public void Mark_wrong_is_only_for_an_indication()
    {
        MarkEditor editor = Recorded();
        editor.Invoking(e => e.MarkWrong(1)).Should().Throw<InvalidOperationException>();
    }

    [Fact]
    public void A_change_that_cannot_be_written_is_not_made()
    {
        string path = Path.Combine(_dir, "missing", "x.marks.csv");
        MarkEditor editor = new(path, Rate, s_recorded);
        editor.Add(1000, MarkKind.Swallow).Should().BeNull();
        editor.Error.Should().NotBeNull();
        editor.Edit(1, MarkKind.Other, "x").Should().BeFalse();
        editor.Move(1, 1000).Should().BeNull();
        editor.Delete(1).Should().BeFalse();
        editor.MarkWrong(0).Should().BeNull();
        editor.Marks.Should().Equal(s_recorded);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        editor.Add(1000, MarkKind.Swallow).Should().Be(0);
        editor.Error.Should().BeNull();
        MarkFile.Read(path).Should().Equal(editor.Marks);
    }
}
