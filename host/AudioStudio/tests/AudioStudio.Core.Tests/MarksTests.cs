// SPDX-License-Identifier: MIT
using System.Text;
using AudioStudio.Core.Marks;
using AudioStudio.Core.Model;
using FluentAssertions;
using Xunit;

namespace AudioStudio.Core.Tests;

public sealed class MarksTests : IDisposable
{
    private const int Rate = 16000;
    private readonly string _dir = Directory.CreateTempSubdirectory("audiostudio-marks-").FullName;

    public void Dispose() => Directory.Delete(_dir, recursive: true);

    private string LogPath => Path.Combine(_dir, "20261127-121827.marks.csv");

    [Fact]
    public void The_marks_file_is_named_after_the_wav()
    {
        MarkFile.PathFor("/r/20261127-121827.wav").Should().Be("/r/20261127-121827.marks.csv");
        MarkFile.PathFor("/r/20261127-121827_meal-2.wav").Should().Be("/r/20261127-121827_meal-2.marks.csv");
    }

    [Fact]
    public void Round_trip_keeps_the_marks_and_quotes_the_text()
    {
        List<Mark> marks =
        [
            new(16000, MarkKind.Swallow),
            new(24008, MarkKind.Indication, 0.97315),
            new(40000, MarkKind.False, null, "too early, I only coughed"),
            new(48000, MarkKind.Other, null, "said \"ah\"\nthen a second line"),
            new(56000, MarkKind.Other, null, "plain"),
            new(60000, MarkKind.Other, null, "carriage\r\nreturn"),
        ];
        MarkFile.Write(LogPath, marks, Rate);
        string text = File.ReadAllText(LogPath, Encoding.UTF8);
        text.Should().StartWith("sample,seconds,kind,probability,text\n16000,1.000,swallow,,\n24008,1.501,indication,0.9732,\n");
        text.Should().Contain("40000,2.500,false,,\"too early, I only coughed\"\n");
        text.Should().Contain("48000,3.000,other,,\"said \"\"ah\"\"\nthen a second line\"\n");
        text.Should().Contain("56000,3.500,other,,plain\n");
        File.ReadAllBytes(LogPath)[0].Should().Be((byte)'s', "UTF-8 without a byte order mark");
        List<Mark> read = MarkFile.Read(LogPath);
        read.Should().HaveCount(marks.Count);
        read[1].Should().Be(new Mark(24008, MarkKind.Indication, 0.9732));
        read.Where((m, i) => i != 1).Should().Equal(marks.Where((m, i) => i != 1));
        File.Exists(LogPath + ".tmp").Should().BeFalse();
    }

    [Fact]
    public void Non_ascii_text_and_crlf_lines_are_read()
    {
        string text = "sample,seconds,kind,probability,text\r\n8000,0.500,other,,むせた\r\n";
        MarkFile.Parse(text).Should().Equal(new Mark(8000, MarkKind.Other, null, "むせた"));
    }

    [Fact]
    public void Rows_are_sorted_by_sample_index_and_ties_keep_their_order()
    {
        List<Mark> marks =
        [
            new(32000, MarkKind.Swallow),
            new(8000, MarkKind.Other, null, "first"),
            new(8000, MarkKind.Other, null, "second"),
            new(100, MarkKind.Indication, 0.9),
        ];
        string text = MarkFile.Format(marks, Rate);
        MarkFile.Parse(text).Select(m => m.Sample).Should().Equal(100, 8000, 8000, 32000);
        MarkFile.Parse(text).Where(m => m.Sample == 8000).Select(m => m.Text).Should().Equal("first", "second");
        // A file in another order is read sorted.
        MarkFile.Parse("sample,seconds,kind,probability,text\n9,0,swallow,,\n3,0,swallow,,\n")
            .Select(m => m.Sample).Should().Equal(3, 9);
    }

    [Theory]
    [InlineData("sample,kind\n")]
    [InlineData("sample,seconds,kind,probability,text\nx,0,swallow,,\n")]
    [InlineData("sample,seconds,kind,probability,text\n1,0,cough,,\n")]
    [InlineData("sample,seconds,kind,probability,text\n1,0,swallow,,\"open\n")]
    [InlineData("sample,seconds,kind,probability,text\n1,0,swallow\n")]
    public void A_broken_file_is_refused(string text)
    {
        FluentActions.Invoking(() => MarkFile.Parse(text)).Should().Throw<MarkFileException>();
    }

    [Fact]
    public void Seconds_are_the_sample_index_over_the_rate()
    {
        new Mark(12345, MarkKind.Swallow).Seconds(Rate).Should().BeApproximately(0.7715625, 1e-12);
        MarkFile.Format([new Mark(12345, MarkKind.Swallow)], Rate).Should().Contain("12345,0.772,swallow");
        MarkFile.Format([new Mark(12345, MarkKind.Swallow)], 48000).Should().Contain("12345,0.257,swallow");
    }

    [Fact]
    public void The_timeline_follows_gaps_longer_than_their_zeros()
    {
        RecordingTimeline t = new();
        t.ToWav(0).Should().BeNull();
        t.HasAnchor.Should().BeFalse();
        // The recording starts at live position 1000; blocks of 100 follow on.
        t.Anchor(1000, 0);
        t.Anchor(1100, 100);
        t.Anchor(1200, 200);
        t.ToWav(1150).Should().Be(150);
        t.ToWav(900).Should().Be(-100);
        // A gap of 2000 lost samples, of which 500 are written as zeros.
        t.Anchor(3300, 800);
        t.ToWav(3350).Should().Be(850);
        t.ToWav(1250).Should().Be(250);
        t.ToLive(850).Should().Be(3350);
        t.ToLive(250).Should().Be(1250);
    }

    [Fact]
    public void A_mark_is_on_disk_at_once_without_its_memo()
    {
        MarkLog log = new(LogPath, Rate);
        File.Exists(LogPath).Should().BeFalse("no mark, no file");
        log.Add(16000, MarkKind.Other);
        log.MemoMark.Should().Be(0);
        // As after a crash while the memo is typed: the mark is there, without text.
        MarkFile.Read(LogPath).Should().Equal(new Mark(16000, MarkKind.Other));
        log.StoreMemo("it was a cough");
        MarkFile.Read(LogPath).Should().Equal(new Mark(16000, MarkKind.Other, null, "it was a cough"));
        log.MemoMark.Should().BeNull();
    }

    [Fact]
    public void A_false_mark_is_stored_at_once_without_a_memo()
    {
        MarkLog log = new(LogPath, Rate);
        log.Add(16000, MarkKind.False);
        log.MemoMark.Should().BeNull("a false mark takes no memo");
        log.StoreMemo("not for it");
        MarkFile.Read(LogPath).Should().Equal(new Mark(16000, MarkKind.False));
    }

    [Fact]
    public void A_mark_during_memo_entry_keeps_the_earlier_mark_with_what_was_typed()
    {
        MarkLog log = new(LogPath, Rate);
        log.Add(32000, MarkKind.Other);
        log.Add(40000, MarkKind.Swallow, typedSoFar: "half a sen");
        log.MemoMark.Should().BeNull("a swallow mark takes no memo");
        log.Add(44000, MarkKind.Other);
        log.Add(48000, MarkKind.False, typedSoFar: "a cou");
        log.MemoMark.Should().BeNull("a false mark takes no memo");
        log.Add(56000, MarkKind.Other, typedSoFar: "");
        log.MemoMark.Should().Be(4);
        log.CancelMemo();
        MarkFile.Read(LogPath).Should().Equal(
            new Mark(32000, MarkKind.Other, null, "half a sen"),
            new Mark(40000, MarkKind.Swallow),
            new Mark(44000, MarkKind.Other, null, "a cou"),
            new Mark(48000, MarkKind.False),
            new Mark(56000, MarkKind.Other));
    }

    [Fact]
    public void The_end_of_the_recording_keeps_an_open_memo()
    {
        MarkLog log = new(LogPath, Rate);
        log.Add(8000, MarkKind.Other);
        log.Finish("typed");
        MarkFile.Read(LogPath).Should().Equal(new Mark(8000, MarkKind.Other, null, "typed"));
        new MarkLog(Path.Combine(_dir, "none.marks.csv"), Rate).Finish("");
        File.Exists(Path.Combine(_dir, "none.marks.csv")).Should().BeFalse();
    }

    [Fact]
    public void A_setup_row_is_written_at_once_and_only_when_the_setup_changes()
    {
        MarkLog log = new(LogPath, Rate);
        string a = DetectorSetup.Format("/x/models/m4-0038.npz", false, 50);
        log.AddSetup(0, a).Should().Be(0);
        MarkFile.Read(LogPath).Should().Equal(new Mark(0, MarkKind.Setup, null, "model=m4-0038.npz spike_filter=off mains=50"));
        log.AddSetup(16000, a).Should().BeNull("nothing changed");
        log.Add(20000, MarkKind.Other);
        log.AddSetup(24000, DetectorSetup.Format(null, true, 60)).Should().Be(2);
        log.MemoMark.Should().Be(1, "a setup row leaves the memo open");
        log.AddSetup(24000, a).Should().Be(3);
        log.Finish("typed");
        MarkFile.Read(LogPath).Should().Equal(
            new Mark(0, MarkKind.Setup, null, "model=m4-0038.npz spike_filter=off mains=50"),
            new Mark(20000, MarkKind.Other, null, "typed"),
            new Mark(24000, MarkKind.Setup, null, "model=- spike_filter=on mains=60"),
            new Mark(24000, MarkKind.Setup, null, "model=m4-0038.npz spike_filter=off mains=50"));
        File.ReadAllText(LogPath).Should().Contain("\n0,0.000,setup,,model=m4-0038.npz spike_filter=off mains=50\n");
        log.Invoking(l => l.Add(1, MarkKind.Setup)).Should().Throw<ArgumentException>();
    }

    [Fact]
    public void The_setup_text_escapes_what_would_split_a_field()
    {
        DetectorSetup.Format("/a b/my model=2%.npz", true, 50)
            .Should().Be("model=my%20model%3D2%25.npz spike_filter=on mains=50");
        DetectorSetup.Format("m4,ä\tx.onnx", false, 50).Should().Be("model=m4,ä%09x.onnx spike_filter=off mains=50");
        // A comma in the file name is quoted by the CSV and read back.
        string setup = DetectorSetup.Format("m4,x.npz", false, 50);
        MarkFile.Parse(MarkFile.Format([new Mark(0, MarkKind.Setup, null, setup)], Rate)).Single().Text.Should().Be(setup);
    }

    [Fact]
    public void Setup_rows_are_not_counted()
    {
        Mark setup = new(Rate, MarkKind.Setup, null, "model=- spike_filter=off mains=50");
        MarkReviewResult r = MarkReview.Review([setup, I(1), setup, F(2), S(10), setup], Rate);
        r.Indications.Single().MarkedFalse.Should().Be(F(2));
        r.MissedSwallows.Should().Equal(S(10));
        r.UnattachedFalse.Should().BeEmpty();
    }

    private static M4Window W(long k, double p, bool positive, bool rise) =>
        new(k * M4Model.HopSamples, new M4Step(k, p, true, positive, positive ? LedStepper.Green : LedStepper.Yellow), rise, false);

    [Fact]
    public void Indications_are_written_at_their_start_with_their_highest_probability()
    {
        MarkLog log = new(LogPath, Rate);
        RecordingTimeline timeline = new();
        // The recording starts at live position 20000.
        timeline.Anchor(20000, 0);
        IndicationTracker tracker = new(log, timeline);
        List<M4Window> windows =
        [
            // A run that started before the recording: left out.
            W(0, 0.90, true, true), W(1, 0.95, true, false), W(2, 0.10, false, false),
            W(10, 0.86, true, true), W(11, 0.97, true, false), W(12, 0.91, true, false),
        ];
        tracker.Feed(windows);
        // The run is open: the file has it at its first probability.
        MarkFile.Read(LogPath).Should().Equal(new Mark(10 * 4000 + 8000 - 20000, MarkKind.Indication, 0.86));
        log.Marks[0].Probability.Should().Be(0.97);
        // Windows already fed are not taken again.
        tracker.Feed(windows);
        tracker.Feed([W(13, 0.40, false, false), W(20, 0.88, true, true)]);
        tracker.Close();
        MarkFile.Read(LogPath).Should().Equal(
            new Mark(28000, MarkKind.Indication, 0.97),
            new Mark(20 * 4000 + 8000 - 20000, MarkKind.Indication, 0.88));
    }

    private static Mark I(double s) => new((long)(s * Rate), MarkKind.Indication, 0.9);
    private static Mark F(double s) => new((long)(s * Rate), MarkKind.False);
    private static Mark S(double s) => new((long)(s * Rate), MarkKind.Swallow);

    [Fact]
    public void An_indication_without_a_false_mark_is_a_hit()
    {
        MarkReviewResult r = MarkReview.Review([I(1), I(5), new Mark(3 * Rate, MarkKind.Other, null, "x")], Rate);
        r.Indications.Should().HaveCount(2);
        r.Hits.Should().Be(2);
        r.MarkedFalse.Should().Be(0);
        r.MissedSwallows.Should().BeEmpty();
    }

    [Fact]
    public void A_false_mark_takes_the_latest_indication_at_or_before_it()
    {
        MarkReviewResult r = MarkReview.Review([I(1), I(5), F(6), I(8), I(10), F(10)], Rate);
        r.Indications.Select(i => i.MarkedFalse).Should().Equal(null, F(6), null, F(10));
        r.Hits.Should().Be(2);
        r.MarkedFalse.Should().Be(2);
    }

    [Fact]
    public void Two_false_marks_in_a_row_take_the_two_latest_indications()
    {
        MarkReviewResult r = MarkReview.Review([I(1), I(5), I(8), F(9), F(9.5)], Rate);
        r.Indications.Select(i => i.MarkedFalse).Should().Equal(null, F(9.5), F(9));
        r.UnattachedFalse.Should().BeEmpty();
    }

    [Fact]
    public void A_false_mark_before_any_indication_is_an_unattached_note()
    {
        MarkReviewResult r = MarkReview.Review([F(1), I(5), F(6), F(7)], Rate);
        r.Indications.Single().MarkedFalse.Should().Be(F(6));
        r.UnattachedFalse.Should().Equal(F(1), F(7));
        r.Hits.Should().Be(0);
    }

    [Fact]
    public void A_swallow_mark_next_to_an_indication_is_the_same_swallow()
    {
        // 2 s before to 3 s after the mark: the indications at 8.5 and 13 are the same swallows.
        MarkReviewResult r = MarkReview.Review([I(8.5), S(10), S(20), I(23), S(30), I(33.1), S(40), I(37.9)], Rate);
        r.Indications.Select(i => i.Swallow).Should().Equal(S(10), S(20), null, null);
        r.MissedSwallows.Should().Equal(S(30), S(40));
        r.Hits.Should().Be(4);
    }

    [Fact]
    public void A_swallow_mark_is_not_the_same_swallow_as_an_indication_marked_false()
    {
        MarkReviewResult r = MarkReview.Review([I(10), F(10.5), S(11), S(11.5), I(12)], Rate);
        r.Indications.Select(i => i.Swallow).Should().Equal(null, S(11));
        r.MissedSwallows.Should().Equal(S(11.5));
        r.MarkedFalse.Should().Be(1);
        r.Hits.Should().Be(1);
    }
}
