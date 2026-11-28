"""m4_shift.py on synthetic data (no data/)."""

from __future__ import annotations
import contextlib, io, unittest
from pathlib import Path
from unittest import mock

import numpy as np

import compare, features, features_np, features_v2, fixed_eval, m4_infer
import m4_shift as ms
import m4_variant as mv
import studio, train_eval
from train_eval import POSITIVE, NEGATIVE, UNUSED


def throat_signal(seconds=12.0, seed=0, t0=0.3, drift=1.0002):
    """(x, sample_t, t_lo, t_hi) like features._throat_windows' signal."""
    rng = np.random.default_rng(seed)
    n = int(seconds * 16000)
    x = (0.01 * rng.standard_normal(n)).astype(np.float32)
    sample_t = t0 + drift * np.arange(n + 1) / 16000
    return x, sample_t, t0 + 0.01, float(sample_t[-1]) - 0.02


def grid_of(x, sample_t, t_lo, t_hi):
    """features._throat_windows' grid loop on the given signal."""
    import math

    span = t_hi - t_lo
    nw = int(math.floor((span - 1.0) / 0.25 + 1e-9)) + 1
    t = t_lo + 0.25 * np.arange(nw)
    idx = np.zeros(nw, np.int64)
    for k, st in enumerate(t):
        i = int(np.searchsorted(sample_t[:-1], st, side="left"))
        if i > 0 and (i == len(x) or st - sample_t[i - 1] <= sample_t[i] - st):
            i -= 1
        idx[k] = i
    return t, idx


class SpecTest(unittest.TestCase):
    def test_parse(self):
        s = ms.parse_shift("dense:4")
        self.assertEqual((s.mode, s.k, s.epochs), ("dense", 4, compare.CNN_EPOCHS))
        self.assertEqual(s.name, "d4e20")
        self.assertEqual(ms.parse_shift("random:10:5").name, "r10e5")
        self.assertIn("62.5 ms", s.describe())
        for bad in ("dense", "dense:3", "sparse:4", "random:4:0", "dense:x", "dense:4:2:1"):
            with self.subTest(bad), self.assertRaises(ValueError):
                ms.parse_shift(bad)


class WindowsTest(unittest.TestCase):
    def test_phase_times(self):
        x, st, lo, hi = throat_signal()
        t1, slot1, ph1 = ms.phase_times(lo, hi, 1)
        g, _ = grid_of(x, st, lo, hi)
        np.testing.assert_array_equal(t1, g)  # bit for bit
        t4, slot4, ph4 = ms.phase_times(lo, hi, 4)
        np.testing.assert_array_equal(t4[ph4 == 0], g)
        self.assertTrue(np.all(np.diff(t4) > 0))
        self.assertTrue(np.all(t4 + 1.0 <= hi + 1e-9))
        np.testing.assert_allclose(t4 - (lo + 0.25 * slot4), 0.0625 * ph4, atol=1e-9)
        # one hop later the shifted windows would leave the span
        self.assertLessEqual(len(t4), 4 * len(g))
        self.assertGreater(len(t4), 4 * len(g) - 4)
        self.assertEqual(len(ms.phase_times(0.0, 0.5, 4)[0]), 0)

    def test_nearest_samples(self):
        x, st, lo, hi = throat_signal()
        g, idx = grid_of(x, st, lo, hi)
        np.testing.assert_array_equal(ms.nearest_samples(st, len(x), g), idx)
        rng = np.random.default_rng(1)
        t = rng.uniform(st[0] - 0.1, st[-1] + 0.1, 200)
        want = []
        for v in t:
            i = int(np.searchsorted(st[:-1], v, side="left"))
            if i > 0 and (i == len(x) or v - st[i - 1] <= st[i] - v):
                i -= 1
            want.append(i)
        np.testing.assert_array_equal(ms.nearest_samples(st, len(x), t), want)

    def test_session_windows_labels_and_grid(self):
        x, st, lo, hi = throat_signal()
        sw = [3.0, 7.4]
        w = ms.session_windows(x, st, lo, hi, 4, sw, "mfe40", "s")
        c = w.t_start_s + 0.5
        np.testing.assert_array_equal(w.labels, train_eval.window_labels(w.t_start_s, sw))
        # the rule on each window's own centre
        for t in sw:
            pos = (c >= t) & (c <= t + 1.0)
            self.assertTrue(np.all(w.labels[pos] == POSITIVE))
        self.assertTrue(np.any(w.labels == UNUSED) and np.any(w.labels == NEGATIVE))
        # shifted windows add positives a grid misses
        g, idx = grid_of(x, st, lo, hi)
        n_grid_pos = int((train_eval.window_labels(g, sw) == POSITIVE).sum())
        self.assertGreater(int((w.labels == POSITIVE).sum()), 3 * n_grid_pos)
        # phase 0 is the 0.25 s grid with features_v2.mfe_windows' features
        M = features_v2.mfe_windows(x, st, idx, hi, "mfe40")
        np.testing.assert_array_equal(w.X[w.phase == 0], M)
        # keep drops windows before their features
        k = ms.session_windows(x, st, lo, hi, 4, sw, "mfe40", "s", keep=ms._usable)
        self.assertTrue(np.all(k.labels != UNUSED))
        u = ms._usable(w.labels, w.valid)
        np.testing.assert_array_equal(k.X, w.X[u])

    def test_recording_windows(self):
        x, _, _, _ = throat_signal(seconds=6.0)
        w = ms.recording_windows(x, 10, [2.0], "mfe40", "r")
        g = studio.window_starts(len(x), 0, len(x))
        np.testing.assert_array_equal(w.t_start_s[w.phase == 0], g / 16000)
        np.testing.assert_allclose(np.diff(w.t_start_s), 0.025)
        np.testing.assert_array_equal(w.X[w.phase == 0][1], features_v2.mfe_one(x[4000:20000]))
        with self.assertRaises(ValueError):
            ms.recording_windows(x, 3, [], "mfe40", "r")


def grid_session(name, n=60, swallows=(4.0, 9.0), seed=0):
    rng = np.random.default_rng(seed)
    t = 0.25 * np.arange(n)
    X = rng.normal(0, 1, (n, 98, 40)).astype(np.float32)
    lab = train_eval.window_labels(t, list(swallows))
    X[lab == POSITIVE] += 0.5
    valid = np.ones(n, bool)
    valid[-1] = False
    feats = features.WindowFeatures(session=name, t_start_s=t, X=X, valid=valid)
    return train_eval.SessionData(
        name=name, feats=feats, swallows=list(swallows), t0_s=0.0,
        duration_s=float(t[-1] + 1.0), labels=lab,
    )


def store_from_grid(data, spec):
    out = {}
    for d in data:
        u = d.feats.valid & (d.labels != UNUSED)
        r = np.flatnonzero(u)
        out[d.name] = ms.Windows(d.name, d.feats.t_start_s[r], r, np.zeros(len(r), np.int64),
                                 1, d.feats.valid[r], d.feats.X[r], d.labels[r])
    return ms.ShiftStore(spec, out)


class FitTest(unittest.TestCase):
    def test_dense_1_is_cnn_fit(self):
        data = [grid_session("a"), grid_session("b", seed=1, swallows=(2.0,))]
        spec = ms.ShiftSpec("dense", 1, 2)
        with mock.patch.object(compare, "CNN_EPOCHS", 2):
            want = compare.cnn_fit(data, "w", 1)
        got = ms.shift_fit(store_from_grid(data, spec))(data, "w", 1)
        self.assertEqual(got.pos_weight, want.pos_weight)
        self.assertEqual(got.n_fit_rows, want.n_fit_rows)
        np.testing.assert_array_equal(got.mean, want.mean)
        for k in want.params:
            np.testing.assert_array_equal(got.params[k], want.params[k])

    def test_random_picks_one_per_slot(self):
        rng = np.random.default_rng(0)
        gid = np.repeat(np.arange(50), 4)[rng.permutation(200)]
        picks = [ms._slot_pick(gid, rng) for _ in range(400)]
        for p in picks[:5]:
            self.assertEqual(sorted(gid[p]), list(range(50)))
        # uniform among the slot's entries
        first = np.flatnonzero(gid == 0)
        counts = np.array([sum(int(np.isin(e, p).any()) for p in picks) for e in first])
        self.assertTrue(np.all(np.abs(counts - 100) < 40), counts)

    def test_random_fit_from_session_windows(self):
        x, st, lo, hi = throat_signal(seconds=10.0)
        sw = [3.0, 6.0]
        spec = ms.ShiftSpec("random", 4, 1)
        w = ms.session_windows(x, st, lo, hi, 4, sw, "mfe40", "s", keep=ms._usable)
        g, idx = grid_of(x, st, lo, hi)
        feats = features.WindowFeatures(
            session="s", t_start_s=g, X=features_v2.mfe_windows(x, st, idx, hi),
            valid=np.ones(len(g), bool),
        )
        d = train_eval.SessionData("s", feats, sw, lo, hi - lo, train_eval.window_labels(g, sw))
        m = ms.shift_fit(ms.ShiftStore(spec, {"s": w}))([d], "w", 0)
        self.assertEqual(m.n_fit_rows, len(np.unique(w.slot)))
        y = w.labels
        self.assertAlmostEqual(m.pos_weight, (y == NEGATIVE).sum() / (y == POSITIVE).sum())
        with self.assertRaises(SystemExit):
            ms.shift_fit(ms.ShiftStore(spec, {"s": w}))([d], "w", 0, sample_weight=[1])


class GridTest(unittest.TestCase):
    def test_torch_prob_matches_model_prob(self):
        rng = np.random.default_rng(2)
        cfg = fixed_eval.M4_CONFIG
        arrays = {"config": np.array(cfg), "format": np.array(m4_infer.MODEL_FORMAT)}
        for s in (0, 1, 2):
            p = features_np.random_cnn(rng)
            for k in m4_infer.CNN_SHAPES:
                arrays[f"s{s}/{k}"] = np.asarray(p[k], np.float32)
            arrays[f"s{s}/mean"] = rng.normal(0, 0.1, 40)
            arrays[f"s{s}/std"] = 1.0 + rng.random(40)
        model = {"config": cfg, "cfg": m4_infer._check_arrays(arrays), "arrays": arrays}
        X = rng.normal(0, 1, (20, 98, 40)).astype(np.float32)
        np.testing.assert_allclose(
            ms.torch_prob(ms.npz_members(model), X), m4_infer.model_prob(model, X), atol=1e-5
        )

    def test_swallow_grid(self):
        cfg = m4_infer.parse_config(fixed_eval.M4_CONFIG)
        n = 800  # 20 s of 25 ms positions
        t = 0.025 * np.arange(n)
        q = np.arange(n)
        w = ms.Windows("r", t, q // 10, q % 10, 10, np.ones(n, bool),
                       np.zeros((n, 1), np.float32), train_eval.window_labels(t, []))
        c = t + 0.5
        p = np.full(n, 0.1)
        # swallow at 5 s: 0.95 for centres in [5.2, 5.3) (4 positions, 100 ms)
        p[(c >= 5.2 - 1e-9) & (c < 5.3 - 1e-9)] = 0.95
        # swallow at 12 s: high for a full second
        p[(c >= 12.0 - 1e-9) & (c < 13.0 - 1e-9)] = 0.9
        # one burst far from both
        p[(c >= 17.0 - 1e-9) & (c < 17.1 - 1e-9)] = 0.99
        r = ms.swallow_grid(w, p, [5.0, 12.0], cfg, 0.0, 20.0)
        a, b = r["swallows"]
        self.assertAlmostEqual(a["span_ms"], 100.0)
        self.assertEqual(a["phases"], 4)
        self.assertAlmostEqual(a["max"], 0.95)
        self.assertAlmostEqual(b["span_ms"], 1000.0)
        self.assertEqual(b["phases"], 10)
        self.assertEqual(r["out_enter"], 4)
        self.assertEqual(r["fp_runs"], 4)  # the burst is seen by 4 of the 10 phases
        agg = ms.aggregate([r])
        self.assertEqual(agg["n"], 2)
        self.assertAlmostEqual(agg["span_short"], 0.5)
        self.assertAlmostEqual(agg["phase_rate"], 14 / 20)
        self.assertAlmostEqual(agg["all_phases"], 0.5)
        with self.assertRaises(ValueError):
            ms.swallow_grid(ms.Windows("r", t, q, q * 0, 1, w.valid, w.X, w.labels),
                            p, [], cfg, 0.0, 20.0)

    def test_main_refuses(self):
        err = io.StringIO()
        for argv in (
            ["x", "--shift", "dense:4"],
            ["x", "--set", "mfe40", "--cv", "--shift", "dense:3"],
            ["x", "--set", "mfe40", "--cv", "--shift", "dense:4", "--extra", "d",
             "--extra-weight", "3"],
            ["x", "--grid", "m.npz"],
            ["x", "--grid", "m.npz", "--test", "r", "--set", "mfe40"],
            ["x", "--grid-train"],
            ["x", "--set", "mfe40", "--final-model", "m.npz", "--grid-cv"],
        ):
            with self.subTest(argv), contextlib.redirect_stderr(err):
                with self.assertRaises(SystemExit):
                    mv.main(argv)


if __name__ == "__main__":
    unittest.main()
