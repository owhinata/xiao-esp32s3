"""studio.py on synthetic recordings in a temporary directory (no data/)."""

from __future__ import annotations
import tempfile, unittest, wave
from pathlib import Path

import numpy as np

import features_v2, studio, train_eval


def write_rec(d: Path, seconds=12.0, rows=(), seed=0, name="20260101-000000_x"):
    d.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(seed)
    x = (300 * rng.standard_normal(int(seconds * 16000))).astype("<i2")
    with wave.open(str(d / f"{name}.wav"), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(16000)
        f.writeframes(x.tobytes())
    if rows is not None:
        lines = ["sample,seconds,kind,probability,text"]
        for s, k in rows:
            lines.append(f"{s},{s / 16000:.3f},{k},{'0.9' if k == 'indication' else ''},")
        (d / f"{name}.marks.csv").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return x


class ReviewTest(unittest.TestCase):
    def test_rules(self):
        marks = [
            (1.0, "indication"),  # hit, taken by the swallow mark at 1.5
            (1.5, "swallow"),
            (4.0, "indication"),  # marked false
            (4.0, "false"),
            (6.0, "indication"),  # marked false by the mark at 9.5
            (9.0, "swallow"),  # no hit near it: a missed swallow
            (9.5, "false"),  # the latest indication at or before it: 6.0
            (11.0, "other"),
        ]
        r = studio.review(marks)
        self.assertEqual(r["false"], [4.0, 6.0])
        self.assertEqual(r["hits"], [1.0])
        self.assertEqual(r["swallows"], [1.5, 9.0])

    def test_false_skips_taken(self):
        r = studio.review([(1.0, "indication"), (2.0, "indication"),
                           (3.0, "false"), (3.5, "false"), (4.0, "false")])
        self.assertEqual(r["false"], [1.0, 2.0])
        self.assertEqual(r["swallows"], [])

    def test_hit_without_mark_is_swallow(self):
        r = studio.review([(2.0, "indication")])
        self.assertEqual(r["swallows"], [2.0])


class SetupRowTest(unittest.TestCase):
    def test_setup_rows_are_read_and_give_no_label(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            plain = d / "plain.marks.csv"
            plain.write_text(
                "sample,seconds,kind,probability,text\n"
                "16000,1.000,indication,0.9000,\n"
                "32000,2.000,false,,\n"
                "64000,4.000,swallow,,\n",
                encoding="utf-8",
            )
            setup = d / "setup.marks.csv"
            setup.write_text(
                "sample,seconds,kind,probability,text\n"
                "0,0.000,setup,,model=m4-0038.npz spike_filter=off mains=50\n"
                "16000,1.000,indication,0.9000,\n"
                "24000,1.500,setup,,model=- spike_filter=off mains=50\n"
                "24000,1.500,setup,,\"model=m4,x.npz spike_filter=on mains=50\"\n"
                "32000,2.000,false,,\n"
                "64000,4.000,swallow,,\n",
                encoding="utf-8",
            )
            marks = studio.read_marks(setup)
            self.assertEqual(
                [k for _, k in marks],
                ["setup", "indication", "setup", "setup", "false", "swallow"],
            )
            self.assertEqual(studio.review(marks), studio.review(studio.read_marks(plain)))
            self.assertEqual(studio.review(marks)["false"], [1.0])
            self.assertEqual(studio.review(marks)["swallows"], [4.0])


class LoadTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name) / "rec"
        self.x = write_rec(
            self.dir,
            rows=[(32000, "indication"), (32000, "false"), (128000, "swallow")],
        )

    def tearDown(self):
        self._tmp.cleanup()

    def test_spec(self):
        s = studio.parse_spec(f"{self.dir}@2:7.5")
        self.assertEqual((s.a_s, s.b_s), (2.0, 7.5))
        self.assertEqual(s.name, "studio:rec@2:7.5")
        self.assertEqual(studio.parse_spec(str(self.dir)).name, "studio:rec")
        self.assertEqual(studio.parse_spec(f"{self.dir}@5:").b_s, None)
        for bad in (f"{self.dir}@5", f"{self.dir}@5:2"):
            with self.assertRaises(ValueError):
                studio.parse_spec(bad)

    def test_whole(self):
        r = studio.load(str(self.dir), "mfe40")
        d = r.data
        self.assertEqual(len(d.labels), 1 + (12 * 16000 - 16000) // 4000)
        np.testing.assert_allclose(d.feats.t_start_s, 0.25 * np.arange(len(d.labels)))
        self.assertTrue(d.feats.valid.all())
        self.assertEqual(d.swallows, [8.0])
        self.assertEqual(r.false_indications, [2.0])
        np.testing.assert_array_equal(
            d.labels, train_eval.window_labels(d.feats.t_start_s, [8.0])
        )
        x = self.x / 32768.0
        np.testing.assert_array_equal(
            d.feats.X[3],
            features_v2.mfe_one(x[12000:28000].astype(np.float32), "mfe40"),
        )

    def test_range_and_front_end(self):
        r = studio.load(f"{self.dir}@6:12", "mfe40ms")
        d = r.data
        self.assertEqual(d.t0_s, 6.0)
        self.assertEqual(d.duration_s, 6.0)
        self.assertEqual(d.feats.t_start_s[0], 6.0)
        self.assertLessEqual(d.feats.t_start_s[-1] + 1.0, 12.0)
        self.assertEqual(d.swallows, [8.0])
        # the front-end runs over the whole recording before the cut
        y, _ = features_v2.cnn_signal((self.x / 32768.0).astype(np.float32), "mfe40ms")
        np.testing.assert_array_equal(
            d.feats.X[0], features_v2.mfe_one(y[96000:112000], "mfe40")
        )
        a = studio.load(f"{self.dir}@0:6", "mfe40").data
        self.assertEqual(a.swallows, [])
        self.assertLessEqual(a.feats.t_start_s[-1] + 1.0, 6.0)

    def test_no_marks_and_bad_wav(self):
        d2 = Path(self._tmp.name) / "nomarks"
        write_rec(d2, seconds=3.0, rows=None)
        r = studio.load(str(d2), "mfe40")
        self.assertEqual(r.data.swallows, [])
        self.assertTrue((r.data.labels == train_eval.NEGATIVE).all())
        d3 = Path(self._tmp.name) / "stereo"
        d3.mkdir()
        with wave.open(str(d3 / "a.wav"), "wb") as f:
            f.setnchannels(2)
            f.setsampwidth(2)
            f.setframerate(16000)
            f.writeframes(b"\0" * 64000)
        with self.assertRaises(SystemExit):
            studio.load(str(d3), "mfe40")


if __name__ == "__main__":
    unittest.main()
