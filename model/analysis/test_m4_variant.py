"""m4_variant.py on synthetic data (the helpers of test_m4_compare; no data/)."""

from __future__ import annotations
import contextlib, io, tempfile, unittest
from pathlib import Path
from unittest import mock

import numpy as np

import compare, features, features_np, fixed_eval, m4_compare as mc, m4_infer
import m4_variant as mv
import studio, train_eval
from test_m4_compare import (
    build_raw,
    cnn_fit_random,
    fake_fit,
    fake_score,
    make_loader,
    production_ok,
)


class ConfigTest(unittest.TestCase):
    def test_only_the_set_changes(self):
        for s in mv.VARIANT_SETS:
            c = mv.variant_config(s)
            self.assertTrue(c.startswith(f"set={s} "))
            self.assertEqual(
                c.split(" ")[1:], fixed_eval.M4_CONFIG.split(" ")[1:]
            )
            self.assertEqual(m4_infer.parse_config(c)["set"], s)
        self.assertEqual(mv.variant_config("mfe40"), fixed_eval.M4_CONFIG)
        with self.assertRaises(SystemExit):
            mv.variant_config("mfe26")
        self.assertEqual(mv.VARIANT_SETS, ("mfe40f200", "mfe40f500"))

    def test_recorded_guards_unchanged(self):
        self.assertTrue(fixed_eval.M4_CONFIG.startswith("set=mfe40 "))
        self.assertTrue(fixed_eval.m4_config_file_matches())

    def test_check_inputs_refuses_dirty(self):
        with (
            mock.patch.object(train_eval, "git_dirty", return_value=True),
            mock.patch.object(train_eval, "_git", return_value="abc\n"),
        ):
            with self.assertRaises(SystemExit) as cm:
                mv.check_inputs(False)
            self.assertIn("--allow-dirty", str(cm.exception))
            notes = mv.check_inputs(True)
        self.assertTrue(any("--allow-dirty" in n for n in notes))
        with mock.patch.object(fixed_eval, "m4_config_file_matches", return_value=False):
            with self.assertRaises(SystemExit):
                mv.check_inputs(True)

    def test_main_refuses_before_reading(self):
        err = io.StringIO()
        for argv in (
            ["x", "--set", "mfe40f200"],
            ["x", "--cv"],
            ["x", "--set", "mfe40", "--final-model", "m.npz"],
            ["x", "--hum", "m.npz", "--set", "mfe40f200"],
            ["x", "--models", "m.npz"],
            ["x", "--set", "mfe40", "--cv", "--extra-weight", "3"],
            ["x", "--set", "mfe40", "--cv", "--extra", "d", "--extra-weight", "0"],
        ):
            with self.subTest(argv), contextlib.redirect_stderr(err):
                with self.assertRaises(SystemExit):
                    mv.main(argv)


class HumEditTest(unittest.TestCase):
    def test_edits(self):
        sr = features_np.SR
        t = np.arange(10 * sr) / sr
        x = (
            0.1 * np.sin(2 * np.pi * 50 * t)
            + 0.05 * np.sin(2 * np.pi * 150 * t)
            + 0.02 * np.sin(2 * np.pi * 1000 * t)
        ).astype(np.float32)

        def amp(y, f):
            Y = np.abs(np.fft.rfft(y)) * 2 / len(y)
            return Y[int(round(f * len(y) / sr))]

        np.testing.assert_array_equal(mv.edit_hum(x, "recorded"), x)
        y = mv.edit_hum(x, "50hz_m7db")
        self.assertEqual(y.dtype, np.float32)
        self.assertAlmostEqual(
            20 * np.log10(amp(y, 50) / amp(x, 50)), -7.0, places=3
        )
        self.assertAlmostEqual(amp(y, 150), amp(x, 150), places=5)
        self.assertAlmostEqual(amp(y, 1000), amp(x, 1000), places=5)
        z = mv.edit_hum(x, "no_hum_50_450")
        self.assertLess(amp(z, 50), 1e-6)
        self.assertLess(amp(z, 150), 1e-6)
        self.assertAlmostEqual(amp(z, 1000), amp(x, 1000), places=5)
        with self.assertRaises(ValueError):
            mv.edit_hum(x, "other")


class VariantRunTest(unittest.TestCase):
    # A synthetic data/raw of its own (test_m4_compare.RunCase's folder is
    # removed with that module's cleanup)
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = Path(cls._tmp.name)
        cls.root = build_raw(cls.tmp)
        with (
            mock.patch.object(fixed_eval, "M4_EXPECTED_TRAIN", None),
            mock.patch.object(fixed_eval, "M4_EXPECTED_EVAL", None),
        ):
            cls.names = mc.list_train(cls.root).train
        with production_ok(cls.names):
            cls.result = mc.run(
                cls.root,
                loader=make_loader(),
                trainer=mc.Trainer(fake_fit([]), fake_score),
            )

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_cv_and_final(self):
        with production_ok(self.names):
            lines, rows = mv.cv(
                self.root,
                "mfe40f200",
                fake_fit([]),
                fake_score,
                make_loader(),
            )
        self.assertEqual(rows["fixed"].op, mc.Op("P2", enter=0.85, exit=0.65, refractory_s=0.5))
        self.assertEqual(rows["grid"].post, "P2")
        self.assertTrue(any("mfe40f200" in s for s in lines))
        # the same ensemble as m4_compare.run's C4 (same fake trainings)
        want = mc.make_row("C4", "P2", self.result.scored["C4"], fixed=rows["fixed"].op)
        self.assertEqual(rows["fixed"].metrics, want.metrics)
        out = self.tmp / "variant.npz"
        with production_ok(self.names):
            fl = mv.final(
                self.root,
                "mfe40f500",
                out,
                cnn_fit_random,
                compare.cnn_score,
                make_loader(dims=(98, 40)),
            )
        m = m4_infer.load_model(out)
        self.assertEqual(m["config"], mv.variant_config("mfe40f500"))
        self.assertTrue(any(s.startswith("- model: path=") for s in fl))
        with self.assertRaises(SystemExit):
            mv.final(self.root, "mfe40", self.tmp / "x.npz")

    def test_hum_aggregates_only(self):
        rng = np.random.default_rng(3)
        n = 20 * features_np.SR
        x = (0.01 * rng.standard_normal(n)).astype(np.float32)
        sample_t = np.arange(n + 1) / features_np.SR
        start_idx = np.arange(0, n - 16000 + 1, 4000)
        valid = np.ones(len(start_idx), bool)
        t1 = n / features_np.SR

        def windows_fn(p):
            return x, sample_t, None, start_idx, valid, 0.0, t1

        paths = []
        for s in ("mfe40", "mfe40f500"):
            p = self.tmp / f"hum-{s}.npz"
            if not p.exists():
                cfg = mv.variant_config(s)
                models = {k: cnn_fit_random([], "", k) for k in (0, 1, 2)}
                mc.save_model(p, cfg, models)
            paths.append(p)
        with production_ok(self.names):
            lines = mv.hum(self.root, paths, windows_fn)
        rows = [s for s in lines if s.startswith("| hum-")]
        self.assertEqual(len(rows), 2 * len(mv.HUM_CASES))
        quiet = mv.quiet_sessions(self.names)
        self.assertTrue(quiet)
        self.assertIn(f"sessions: {len(quiet)}", "\n".join(lines))


def fake_recording(name, n=40, swallows=(), false_t=(), dims=(98, 40), seed=0):
    rng = np.random.default_rng(seed)
    t = 0.25 * np.arange(n)
    X = rng.normal(0, 1, (n, *dims)).astype(np.float32)
    feats = features.WindowFeatures(
        session=name, t_start_s=t, X=X, valid=np.ones(n, bool)
    )
    d = train_eval.SessionData(
        name=name, feats=feats, swallows=list(swallows), t0_s=0.0,
        duration_s=float(t[-1] + 1.0) if n else 0.0,
        labels=train_eval.window_labels(t, list(swallows)),
    )
    spec = studio.Spec(Path(name), None, None)
    return studio.Recording(spec=spec, data=d, false_indications=list(false_t))


class ExtraTest(unittest.TestCase):
    def test_weighted_fit(self):
        calls = []

        def fit(data, where, seed, sample_weight=None):
            calls.append(None if sample_weight is None else [w.copy() for w in sample_weight])
            return "m"

        a = fake_recording("studio:a", n=5).data
        b = fake_recording("b", n=3).data
        self.assertIs(mv.weighted_fit(["studio:a"], 1.0, fit), fit)
        f = mv.weighted_fit(["studio:a"], 10.0, fit)
        f([b], "", 0)
        f([b, a], "", 0)
        self.assertIsNone(calls[0])
        np.testing.assert_array_equal(calls[1][0], np.ones(3))
        np.testing.assert_array_equal(calls[1][1], np.full(5, 10.0))

    def test_recording_metrics(self):
        cfg = m4_infer.parse_config(fixed_eval.M4_CONFIG)
        r = fake_recording("studio:r", n=40, false_t=[2.0])
        p = np.zeros(40)
        p[8:20] = 0.95  # centres 2.5 .. 5.25: one indication inside the touch
        p[30:33] = 0.9  # a second one outside
        m = mv.recording_metrics(p, r.data, r.false_indications, cfg)
        self.assertEqual(m["indications"], 2)
        self.assertAlmostEqual(m["frac_enter"], 15 / 40)
        c = r.data.feats.t_start_s + 0.5
        inside = (c >= 2.0) & (c < 9.0)
        self.assertEqual(m["n_in"], int(inside.sum()))
        self.assertAlmostEqual(m["median_in"], float(np.median(p[inside])))
        self.assertAlmostEqual(m["median_out"], float(np.median(p[~inside])))


class VariantExtraRunTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = Path(cls._tmp.name)
        cls.root = build_raw(cls.tmp)
        with (
            mock.patch.object(fixed_eval, "M4_EXPECTED_TRAIN", None),
            mock.patch.object(fixed_eval, "M4_EXPECTED_EVAL", None),
        ):
            cls.names = mc.list_train(cls.root).train

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_cv_with_extras_and_tests(self):
        calls = []
        ex = fake_recording("studio:h@0:10", dims=(2,))
        te = fake_recording("studio:h@10:", dims=(2,), false_t=[1.0])
        with production_ok(self.names):
            lines, rows = mv.cv(
                self.root, "mfe40", fake_fit(calls), fake_score, make_loader(),
                extras=[ex], tests=[te], extra_weight=10.0,
            )
            plain_lines, plain = mv.cv(
                self.root, "mfe40", fake_fit([]), fake_score, make_loader()
            )
        # every training has the extra recording, weighted; it is never scored
        # as a validation session
        self.assertTrue(calls)
        self.assertTrue(all("studio:h@0:10" in c[2] and c[1] for c in calls))
        self.assertNotIn("studio:h@0:10", rows["fixed"].per_session)
        self.assertEqual(set(rows["fixed"].per_session), set(plain["fixed"].per_session))
        text = "\n".join(lines)
        self.assertIn("loss weight 10", text)
        self.assertIn("mean of", text)
        self.assertEqual(sum(s.startswith("| fold ") for s in lines),
                         len(mc.outer_groups([mc._Name(n) for n in self.names])))
        self.assertNotIn("Test recordings", "\n".join(plain_lines))

    def test_final_with_extras(self):
        ex = fake_recording("studio:h", dims=(98, 40))
        out = self.tmp / "extra.npz"
        calls = []

        def fit(data, where, seed, sample_weight=None):
            calls.append(sample_weight is not None)
            return cnn_fit_random(data, where, seed, sample_weight)

        with production_ok(self.names):
            fl = mv.final(
                self.root, "mfe40", out, fit, compare.cnn_score,
                make_loader(dims=(98, 40)), extras=[ex], extra_weight=10.0,
            )
        self.assertEqual(calls, [True, True, True])
        m = m4_infer.load_model(out)
        self.assertEqual(m["config"], fixed_eval.M4_CONFIG)
        self.assertTrue(any("extra training recordings: studio:h" in s for s in fl))


def spiky_signal(seconds=12.0, bursts=(3.0, 8.0), seed=0):
    """Noise, hum and bursts twice per 50 Hz period in [a, b) s (float32)."""
    import mains_spike as ms

    rng = np.random.default_rng(seed)
    n = int(seconds * ms.SR)
    t = np.arange(n) / ms.SR
    x = 2e-3 * np.sin(2 * np.pi * 50.0 * t) + 3e-4 * rng.standard_normal(n)
    k = 0
    while True:
        i = int(bursts[0] * ms.SR + 37 + k * 160)
        if i + 24 >= bursts[1] * ms.SR:
            break
        x[i : i + 24] += 0.005 * np.hanning(24) * rng.standard_normal(24)
        k += 1
    return x.astype(np.float32)


class FrontCheckTest(unittest.TestCase):
    def test_parse_front_params(self):
        self.assertEqual(mv.parse_front_params(""), {})
        self.assertEqual(mv.parse_front_params("current"), {})
        self.assertEqual(
            mv.parse_front_params("ALPHA=1/16, WARMUP=16"),
            {"ALPHA": 0.0625, "WARMUP": 16},
        )
        self.assertIsInstance(mv.parse_front_params("WARMUP=8")["WARMUP"], int)
        for bad in ("ALPHA", "LP_TAPS=101", "alpha=0.1", "ALPHA=x"):
            with self.subTest(bad), self.assertRaises(ValueError):
                mv.parse_front_params(bad)
        self.assertEqual(mv.front_label({}), "front-end (current)")
        self.assertEqual(mv.front_label({"ALPHA": 0.0625}), "front-end ALPHA=0.0625")

    def test_front_constants_are_restored(self):
        import mains_spike as ms

        x = spiky_signal(seconds=3.0, bursts=(0.5, 3.0))
        before = ms.ALPHA
        y0 = ms.apply(x)
        with mv.front_constants({"ALPHA": 1.0 / 2.0, "WARMUP": 4}):
            self.assertEqual(ms.ALPHA, 0.5)
            y1 = ms.apply(x)
        self.assertEqual(ms.ALPHA, before)
        self.assertFalse(np.array_equal(y0, y1))
        np.testing.assert_array_equal(ms.apply(x), y0)

    def test_sync_touches(self):
        touches = mv.sync_touches(spiky_signal())
        self.assertEqual(len(touches), 1)
        a, b = touches[0]
        self.assertLess(abs(a - 3.0), 0.3)
        self.assertLess(abs(b - 8.0), 0.3)
        # a broadband transient alone is not a touch
        rng = np.random.default_rng(1)
        x = spiky_signal(bursts=(0.0, 0.0))
        x[3 * 16000 : 6 * 16000] += 0.01 * rng.standard_normal(3 * 16000).astype(np.float32)
        self.assertEqual(mv.sync_touches(x), [])

    def test_touch_metrics_and_swallow_probs(self):
        cfg = m4_infer.parse_config(fixed_eval.M4_CONFIG)
        t = 0.25 * np.arange(40)
        c = t + 0.5
        p = np.full(40, 0.1)
        p[(c >= 2.0) & (c < 3.0)] = 0.95  # the first second of the touch [2, 6]
        p[(c >= 3.0) & (c < 3.5)] = 0.6
        m = mv.touch_metrics(t, p, [(2.0, 6.0)], cfg)[0]
        self.assertAlmostEqual(m["to_enter"], 1.0)
        self.assertAlmostEqual(m["to_half"], 1.5)
        self.assertEqual(m["indications"], 1)
        inside = (c >= 2.0) & (c <= 6.0)
        self.assertAlmostEqual(m["frac_enter"], 4 / inside.sum())
        self.assertEqual(mv.touch_metrics(t, np.full(40, 0.1), [(2.0, 6.0)], cfg)[0]["to_enter"], 0.0)
        self.assertEqual(mv.touch_metrics(t, np.full(40, 0.9), [(2.0, 6.0)], cfg)[0]["to_enter"], float("inf"))
        sp = mv.swallow_probs(t, p, [2.0, 8.0, 30.0])
        self.assertAlmostEqual(sp[0], 0.95)
        self.assertAlmostEqual(sp[1], 0.1)
        self.assertTrue(np.isnan(sp[2]))

    def test_damage_sessions(self):
        names = [
            "20261011-101644_self_water", "20261011-102354_self_talk",
            "20261011-102854_self_saliva", "20261025-200807_self_meal",
            "20261026-190244_self_meal",
        ]
        self.assertEqual(
            mv.damage_sessions(names),
            ["20261011-101644_self_water", "20261011-102854_self_saliva",
             "20261025-200807_self_meal"],
        )

    def test_main_refuses_front_combinations(self):
        err = io.StringIO()
        for argv in (
            ["x", "--front-param", "ALPHA=1/8"],
            ["x", "--front", "m.npz", "--set", "mfe40", "--cv"],
            ["x", "--front", "m.npz", "--front-param", "NOPE=1"],
        ):
            with self.subTest(argv), contextlib.redirect_stderr(err):
                with self.assertRaises(SystemExit):
                    mv.main(argv)


class FrontCheckRunTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = Path(cls._tmp.name)
        cls.root = build_raw(cls.tmp)
        with (
            mock.patch.object(fixed_eval, "M4_EXPECTED_TRAIN", None),
            mock.patch.object(fixed_eval, "M4_EXPECTED_EVAL", None),
        ):
            cls.names = mc.list_train(cls.root).train

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_front_check(self):
        import wave

        rec = self.tmp / "studio" / "r"
        rec.mkdir(parents=True)
        x = spiky_signal()
        with wave.open(str(rec / "r.wav"), "wb") as f:
            f.setnchannels(1)
            f.setsampwidth(2)
            f.setframerate(16000)
            f.writeframes(np.round(x * 32768).astype("<i2").tobytes())
        (rec / "r.marks.csv").write_text(
            "sample,seconds,kind,probability,text\n160000,10.000,swallow,,\n",
            encoding="utf-8",
        )
        model = self.tmp / "front.npz"
        mc.save_model(model, fixed_eval.M4_CONFIG,
                      {k: cnn_fit_random([], "", k) for k in (0, 1, 2)})
        n = 12 * 16000
        start_idx = np.arange(0, n - 16000 + 1, 4000)
        seen = []

        def windows_fn(p):
            seen.append(p.name)
            return (x, np.arange(n + 1) / 16000, start_idx / 16000, start_idx,
                    np.ones(len(start_idx), bool), 0.0, n / 16000)

        with production_ok(self.names):
            lines = mv.front_check(
                self.root, model, [str(rec)], [{}, {"ALPHA": 0.0625}],
                windows_fn=windows_fn, events_fn=lambda p: [4.0, 9.0],
            )
        text = "\n".join(lines)
        self.assertEqual(seen, mv.damage_sessions(self.names))
        touch_rows = [s for s in lines if s.startswith("| r | ")]
        self.assertEqual(len(touch_rows), 3 * 2)  # raw and two cases, swallow rows too
        self.assertIn("front-end (current)", text)
        self.assertIn("front-end ALPHA=0.0625", text)
        swallows = 2 * len(seen)
        self.assertIn(f"/{swallows} |", text)
        other = self.tmp / "other.npz"
        mc.save_model(other, mv.variant_config("mfe40f500"),
                      {k: cnn_fit_random([], "", k) for k in (0, 1, 2)})
        with self.assertRaises(SystemExit):
            mv.front_check(self.root, other, [], [{}])


if __name__ == "__main__":
    unittest.main()
