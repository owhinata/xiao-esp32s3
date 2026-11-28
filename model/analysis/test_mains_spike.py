"""mains_spike on synthetic signals (no data/)."""

from __future__ import annotations
import unittest

import numpy as np
import scipy.signal

import features_v2, m4_infer
import mains_spike as ms

SR = ms.SR


def high(y):
    sos = scipy.signal.butter(4, [500, 4000], "bandpass", fs=SR, output="sos")
    return scipy.signal.sosfilt(sos, np.asarray(y, np.float64))


def db(v) -> float:
    return 10.0 * np.log10(np.mean(np.asarray(v) ** 2))


def make(seconds=6.0, mains=50.02, spikes=True, transient=None, seed=0,
         spike_amp=0.005, per_period=2):
    """Noise floor, hum, optional spikes (per_period per mains period at a
    fixed place, about 1.5 ms long, amplitude wobbling at about 14 Hz),
    optional broadband transient (start s, length s), as float64. Returns
    (x, spike mask, transient mask)."""
    rng = np.random.default_rng(seed)
    n = int(seconds * SR)
    t = np.arange(n) / SR
    x = 2e-3 * np.sin(2 * np.pi * mains * t) + 3e-4 * rng.standard_normal(n)
    smask = np.zeros(n, bool)
    if spikes:
        period = SR / mains
        k = 0
        while True:
            c = 37.0 + k * period / per_period
            i = int(c)
            if i + 24 >= n:
                break
            amp = spike_amp * (0.55 + 0.45 * np.sin(2 * np.pi * 14.3 * i / SR))
            w = np.hanning(24)
            x[i : i + 24] += amp * w * rng.standard_normal(24)
            smask[i : i + 24] = True
            k += 1
    tmask = np.zeros(n, bool)
    if transient is not None:
        a, L = int(transient[0] * SR), int(transient[1] * SR)
        env = np.hanning(L)
        x[a : a + L] += 0.01 * env * rng.standard_normal(L)
        tmask[a : a + L] = True
    return x, smask, tmask


class ReferenceTest(unittest.TestCase):
    def test_apply_equals_per_sample(self):
        x, _, _ = make(seconds=1.5, transient=(0.8, 0.3))
        y = ms.apply(x)
        m = ms.MainsSpike()
        z = np.array([m.step(v) for v in x])
        self.assertEqual(y.dtype, np.float32)
        self.assertEqual(len(y), len(x))
        np.testing.assert_allclose(y, z, atol=1e-6)

    def test_causal(self):
        x, _, _ = make(seconds=2.0)
        y = ms.apply(x)
        for k in (1, 1000, 12345, 20000):
            np.testing.assert_array_equal(ms.apply(x[:k]), y[:k])

    def test_empty(self):
        self.assertEqual(len(ms.apply(np.zeros(0))), 0)

    def test_coefficients(self):
        taps = ms.lp_taps()
        self.assertEqual(len(taps), ms.LP_TAPS)
        np.testing.assert_allclose(taps, taps[::-1])
        self.assertAlmostEqual(float(taps.sum()), 1.0, places=12)
        w, H = scipy.signal.freqz(taps, [1.0], [0.0, 100.0, 600.0, 1000.0], fs=SR)
        H = np.abs(H)
        self.assertLess(abs(H[1] - 1.0), 0.01)  # the hum stays in the low band
        self.assertLess(H[2], 0.01)  # the high band starts below 600 Hz
        self.assertLess(H[3], 0.001)
        w, hz = scipy.signal.freqz(
            ms.bp_coeffs()[:3], [1.0, *ms.bp_coeffs()[3:]], [ms.MAINS_HZ], fs=SR
        )
        self.assertAlmostEqual(abs(hz[0]), 1.0, places=6)

    def test_delay_only_without_spikes(self):
        # a signal with no high band at all comes out delayed by D samples
        t = np.arange(SR) / SR
        x = 0.01 * np.sin(2 * np.pi * 50.0 * t)
        y = ms.apply(x)
        D = (ms.LP_TAPS - 1) // 2
        np.testing.assert_allclose(y[D:], x[:-D], atol=1e-6)


D = (ms.LP_TAPS - 1) // 2


def aligned(y, *xs):
    """The output without its delay, and the inputs cut to the same length."""
    return (np.asarray(y)[D:], *(np.asarray(v)[: len(v) - D] for v in xs))


class SuppressionTest(unittest.TestCase):
    def check_spikes(self, mains_hz, true_hz, per_period=2):
        x, sm, _ = make(mains=true_hz, per_period=per_period)
        x0, _, _ = make(mains=true_hz, spikes=False)
        y, x, x0, sm = aligned(ms.apply(x, mains_hz=mains_hz), x, x0, sm)
        late = np.arange(len(x)) >= 2 * SR  # after the template has formed
        hx, hy, h0 = high(x), high(y), high(x0)
        # the spikes' high-band energy is mostly gone ...
        self.assertLess(db(hy[late & sm]) - db(hx[late & sm]), -10.0)
        # ... and the whole high band is within 2 dB of the spike-free
        # signal (it was 8 dB above)
        self.assertGreater(db(hx[late]) - db(h0[late]), 6.0)
        self.assertLess(abs(db(hy[late]) - db(h0[late])), 2.0)

    def test_50hz(self):
        self.check_spikes(50.0, 50.02)

    def test_period_off_nominal(self):
        self.check_spikes(50.0, 49.85)

    def test_60hz_parameter(self):
        self.check_spikes(60.0, 60.03)

    def test_one_spike_per_period(self):
        self.check_spikes(50.0, 50.02, per_period=1)

    def test_no_spikes_unchanged(self):
        x, _, _ = make(spikes=False)
        y, x = aligned(ms.apply(x), x)
        np.testing.assert_allclose(y, x, atol=1e-7)

    def test_transient_passes(self):
        # a 0.5 s broadband transient, not synchronous with the mains, passes
        # with little change, with and without spikes around it
        for spikes in (False, True):
            with self.subTest(spikes=spikes):
                x, _, tm = make(spikes=spikes, transient=(3.0, 0.5))
                xr, _, _ = make(spikes=spikes, transient=None)
                y, x, tm = aligned(ms.apply(x), x, tm)
                yr, xr = aligned(ms.apply(xr), xr)
                # transient energy: the output minus the output without it
                ex = db(high(x - xr)[tm])
                ey = db(high(y - yr)[tm])
                self.assertLess(abs(ey - ex), 0.5)

    def test_clicks_pass(self):
        # short loud broadband clicks (3 ms), some on burst phases, keep
        # their energy within 2 dB with weak and with strong bursts: only
        # the expected burst is taken away where they fall on a burst phase
        # (a limit to LIMIT ref alone takes about 10 dB of them)
        rng = np.random.default_rng(5)
        for spike_amp in (0.0005, 0.005):
            with self.subTest(spike_amp=spike_amp):
                x0, _, _ = make(spike_amp=spike_amp)
                c = np.zeros(len(x0))
                cm = np.zeros(len(x0), bool)
                for k in range(12):
                    a = int((3.0 + 0.21 * k) * SR)
                    c[a : a + 48] = 0.05 * np.hanning(48) * rng.standard_normal(48)
                    cm[a : a + 48] = True
                y, c, cm = aligned(ms.apply(x0 + c), c, cm)
                y0, = aligned(ms.apply(x0))
                self.assertLess(abs(db(high(y - y0)[cm]) - db(high(c)[cm])), 2.0)

    def test_amplitude_change_followed(self):
        x1, sm, _ = make(seconds=8.0, spike_amp=0.002, seed=1)
        x2, _, _ = make(seconds=8.0, spike_amp=0.01, seed=1)
        x = np.where(np.arange(len(x1)) < 4 * SR, x1, x2)
        y, x, sm = aligned(ms.apply(x), x, sm)
        late = np.arange(len(x)) >= 5 * SR
        self.assertLess(db(high(y)[late & sm]) - db(high(x)[late & sm]), -10.0)


    def test_onset(self):
        # bursts that start after 3 s without them are limited from about
        # 0.1 s on (the template follows at ALPHA per period)
        x1, sm, _ = make(seconds=6.0, seed=2)
        x0, _, _ = make(seconds=6.0, spikes=False, seed=2)
        n = np.arange(len(x1))
        x = np.where(n < 3 * SR, x0, x1)
        sm = sm & (n >= 3 * SR)
        y, x, sm = aligned(ms.apply(x), x, sm)
        soon = (np.arange(len(x)) >= int(3.1 * SR)) & (np.arange(len(x)) < int(3.5 * SR))
        self.assertLess(db(high(y)[soon & sm]) - db(high(x)[soon & sm]), -8.0)


class SetTest(unittest.TestCase):
    def test_front_set(self):
        self.assertEqual(ms.FRONT_SETS, {"mfe40ms": "mfe40"})
        self.assertEqual(m4_infer.FRONT_SETS, tuple(ms.FRONT_SETS))
        self.assertIn("mfe40ms", features_v2.CNN_INPUT_SETS)
        self.assertNotIn("mfe40ms", features_v2.CNN_SETS)
        x, _, _ = make(seconds=1.2)
        x = x.astype(np.float32)
        y, base = features_v2.cnn_signal(x, "mfe40ms")
        self.assertEqual(base, "mfe40")
        np.testing.assert_array_equal(y, ms.apply(x))
        y, base = features_v2.cnn_signal(x, "mfe40f200")
        self.assertIs(y, x)
        with self.assertRaises(ValueError):
            features_v2.cnn_signal(x, "a52")

    def test_mfe_windows_apply_front_end(self):
        x, _, _ = make(seconds=2.5)
        x = x.astype(np.float32)
        n = len(x)
        sample_t = np.arange(n + 1) / SR
        start_idx = np.arange(0, n - 16000 + 1, 4000)
        M = features_v2.mfe_windows(x, sample_t, start_idx, n / SR, "mfe40ms")
        y = ms.apply(x)
        for k, i in enumerate(start_idx):
            np.testing.assert_array_equal(
                M[k], features_v2.mfe_one(y[i : i + 16000], "mfe40")
            )

    def test_config_and_detector(self):
        base = "classifier=cnn seeds=0,1,2 hard_negative=none post=hyst threshold=- enter=0.85 exit=0.65 cap_s=none refractory_s=0.5"
        cfg = m4_infer.parse_config("set=mfe40ms " + base)
        self.assertEqual(cfg["set"], "mfe40ms")
        with self.assertRaises(ValueError):
            m4_infer.Detector({"cfg": cfg, "arrays": {}})


if __name__ == "__main__":
    unittest.main()
