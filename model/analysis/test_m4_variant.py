"""m4_variant.py on synthetic data (the helpers of test_m4_compare; no data/)."""

from __future__ import annotations
import contextlib, io, tempfile, unittest
from pathlib import Path
from unittest import mock

import numpy as np

import compare, features_np, fixed_eval, m4_compare as mc, m4_infer
import m4_variant as mv
import train_eval
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


if __name__ == "__main__":
    unittest.main()
