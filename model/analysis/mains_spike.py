"""Mains-synchronous spike suppression: an experimental front-end on the
16 kHz signal, before the log-mel features (set mfe40ms = this front-end,
then mfe40).

What it removes: short bursts in the high band that repeat at the same
place of every mains period (seen with a hand on or near an electric heater:
about 1-2 ms long, up to two per period, the amplitude changing from period
to period), and nothing that is not synchronous with the mains.

Algorithm (causal; per sample O(1) apart from the LP_TAPS-tap FIR; state:
LP_TAPS input samples, a template and its spread of ceil(1.03 P) + 1
values each and a few scalars; P = samples per mains period, 320 at 50 Hz):

  1. Band split: lp = FIR low-pass (LP_TAPS taps, Hamming-windowed sinc at
     LP_HZ, linear phase, delay D = (LP_TAPS - 1) / 2), xd = x delayed by D,
     h = xd - lp (exactly complementary: xd = lp + h). The hum and its
     harmonics up to 150 Hz stay in lp; from about 450 Hz up all is in h.
     Envelope: e = one-pole smoothing of h*h (time constant ENV_TAU samples).
  2. Mains period: a band-pass biquad (RBJ, 0 dB peak, Q = HUM_Q) at the
     nominal MAINS_HZ isolates the hum; each rising zero crossing (linear
     interpolation, at least REFRACTORY periods after the previous one)
     gives a period measurement d, used only within +-PERIOD_TOL of the
     estimate: P <- P + PERIOD_RATE (d - P). Without hum P stays nominal.
  3. Phase: a free-running counter that wraps every P samples (P fractional:
     a period block has ceil(P - r) samples, r the fractional phase left at
     its start; P is the estimate at the block start). Sample j of a block
     is template bin j. The template follows the spikes' place by itself, so
     the counter needs no lock to the hum's phase.
  4. Template: T[j] <- T[j] + ALPHA (e - T[j]) and the spread
     V[j] <- V[j] + ALPHA (|e - T[j]| - V[j]) (T[j] before its update),
     after use (once per period per bin). F: a running median of the
     template values, updated at the end of every block by
     F <- F exp(QUANT_STEP (n_above - n_below)) (the block's bins with
     T > F and T <= F, counted before the block's updates). R: the mean of
     the output envelope g^2 e over the previous block. ref = max(F, R).
  5. Gain on the high band, after WARMUP blocks: at a bin with
     T[j] > MARGIN ref (a mains-synchronous burst is expected there),
     g^2 = clip(max(LIMIT ref / e, 1 - B / e), FLOOR_GAIN2, 1) with
     B = T[j] + SPREAD V[j], the most a burst at that phase is expected to
     have; elsewhere g = 1. Output y = xd - (1 - g) h = lp + g h. A burst
     (e <= B) is limited to LIMIT times the reference level, by at most
     -10 log10(FLOOR_GAIN2) dB; of a louder envelope only B is taken away
     (g^2 e >= e - B), so sound that is not synchronous with the mains (a
     swallow) keeps what it has above the expected burst also where it
     falls on a burst phase. Such sound raises all bins together, so it
     raises F and R and the condition T[j] > MARGIN ref stops holding; a
     burst limited by the gain does not raise R.

The output is the input delayed by D samples (5 ms) wherever g = 1.

apply(x) runs it over a whole signal (blocks of one period vectorized with
numpy). MainsSpike is the same per sample (the reference for ports: C# and
firmware); the tests check they agree.
"""

from __future__ import annotations
import math

import numpy as np

SR = 16000
MAINS_HZ = 50.0  # nominal mains frequency (60.0 in 60 Hz regions)
LP_HZ = 300.0  # cutoff of the linear-phase low-pass of the band split
LP_TAPS = 161  # odd; delay (LP_TAPS - 1) / 2 = 80 samples (5 ms)
ENV_TAU = 4.0  # samples
HUM_Q = 2.0
REFRACTORY = 0.75  # periods
PERIOD_TOL = 0.03
PERIOD_RATE = 1.0 / 256.0
ALPHA = 1.0 / 8.0  # template update per period (m4-ms.npz was trained with 1 / 16)
QUANT_STEP = 1.0 / 512.0
MARGIN = 3.0  # T[j] / ref above which a bin is a burst phase
LIMIT = 2.0  # limit of a burst, times ref
SPREAD = 10.0  # B = T + SPREAD V: the expected burst, in mean absolute deviations
FLOOR_GAIN2 = 0.01  # -20 dB
F_INIT = 1e-12
WARMUP = 32  # blocks (periods) before any gain is applied

FRONT_SETS = {"mfe40ms": "mfe40"}  # front-end set -> base CNN set


def lp_taps(fc: float = LP_HZ, n: int = LP_TAPS, sr: int = SR) -> np.ndarray:
    """The symmetric FIR low-pass of the band split: a Hamming-windowed
    sinc, w[k] sin(2 pi fc (k - D) / sr) / (pi (k - D)) (2 fc / sr at
    k = D), D = (n - 1) / 2, scaled to sum 1."""
    D = (n - 1) // 2
    k = np.arange(n) - D
    w = np.hamming(n)
    hk = w * (2.0 * fc / sr) * np.sinc(2.0 * fc / sr * k)
    return hk / hk.sum()


def bp_coeffs(f0: float = MAINS_HZ, q: float = HUM_Q, sr: int = SR) -> tuple:
    """(b0, b1, b2, a1, a2) of the RBJ band-pass with 0 dB peak gain."""
    w = 2.0 * math.pi * f0 / sr
    al = math.sin(w) / (2.0 * q)
    a0 = 1.0 + al
    return (al / a0, 0.0, -al / a0, -2.0 * math.cos(w) / a0, (1.0 - al) / a0)


def env_coeff(tau: float = ENV_TAU) -> float:
    return math.exp(-1.0 / tau)


def _lfilter(c, x):
    import scipy.signal

    b0, b1, b2, a1, a2 = c
    return scipy.signal.lfilter([b0, b1, b2], [1.0, a1, a2], x)


def _periods(z: np.ndarray, mains_hz: float, sr: int):
    """(crossing times in samples, the period estimate after each crossing)
    of the hum z, as MainsSpike updates them."""
    p_nom = sr / mains_hz
    idx = np.flatnonzero((z[:-1] < 0.0) & (z[1:] >= 0.0)) + 1
    times, est = [], []
    p = p_nom
    last = None
    for n in idx:
        t = (n - 1) + (-z[n - 1]) / (z[n] - z[n - 1])
        if last is not None and t - last < REFRACTORY * p:
            continue
        if last is not None:
            d = t - last
            if abs(d - p) <= PERIOD_TOL * p:
                p += PERIOD_RATE * (d - p)
        last = t
        times.append(n)
        est.append(p)
    return np.asarray(times, dtype=np.int64), np.asarray(est)


def n_bins(mains_hz: float = MAINS_HZ, sr: int = SR) -> int:
    return int(math.ceil((1.0 + PERIOD_TOL) * sr / mains_hz)) + 1


def apply(x, mains_hz: float = MAINS_HZ, sr: int = SR) -> np.ndarray:
    """The front-end over a whole signal from its first sample (state
    starts empty). Returns float32 of x's length."""
    x = np.asarray(x, dtype=np.float64)
    n = len(x)
    if n == 0:
        return np.zeros(0, dtype=np.float32)
    import scipy.signal

    D = (LP_TAPS - 1) // 2
    xd = np.concatenate((np.zeros(D), x[: n - D])) if n > D else np.zeros(n)
    h = xd - scipy.signal.lfilter(lp_taps(LP_HZ, LP_TAPS, sr), [1.0], x)
    a = env_coeff()
    e = scipy.signal.lfilter([1.0 - a], [1.0, -a], h * h)
    z = _lfilter(bp_coeffs(mains_hz, HUM_Q, sr), x)
    ct, cp = _periods(z, mains_hz, sr)
    T = np.zeros(n_bins(mains_hz, sr))
    V = np.zeros(len(T))
    F = F_INIT
    R = 0.0
    p = sr / mains_hz
    r = 0.0
    s = 0
    k = 0  # crossings seen before the block start
    blocks = 0
    g = np.ones(n)
    while s < n:
        while k < len(ct) and ct[k] < s:
            p = cp[k]
            k += 1
        L = int(math.ceil(p - r))
        b = min(n, s + L)
        m = b - s
        Tj = T[:m]
        Vj = V[:m]
        es = e[s:b]
        ref = max(F, R)
        if blocks >= WARMUP:
            with np.errstate(divide="ignore", invalid="ignore"):
                g2 = np.where(
                    (Tj > MARGIN * ref) & (es > 0.0),
                    np.maximum(LIMIT * ref / es, 1.0 - (Tj + SPREAD * Vj) / es),
                    1.0,
                )
            g2 = np.clip(g2, FLOOR_GAIN2, 1.0)
            g[s:b] = np.sqrt(g2)
        else:
            g2 = np.ones(m)
        R = float(np.sum(g2 * es)) / m
        up = int(np.count_nonzero(Tj > F))
        F *= math.exp(QUANT_STEP * (up - (m - up)))
        V[:m] = Vj + ALPHA * (np.abs(es - Tj) - Vj)
        T[:m] = Tj + ALPHA * (es - Tj)
        r = r + L - p
        s = b
        blocks += 1
    return (xd - (1.0 - g) * h).astype(np.float32)


class MainsSpike:
    """The same front-end one sample at a time (the reference for ports).
    step(x) -> y. Within float rounding equal to apply()."""

    def __init__(self, mains_hz: float = MAINS_HZ, sr: int = SR):
        self.lp = [float(v) for v in lp_taps(LP_HZ, LP_TAPS, sr)]
        self.buf = [0.0] * LP_TAPS  # the last LP_TAPS inputs, circular
        self.pos = 0
        self.bp = bp_coeffs(mains_hz, HUM_Q, sr)
        self.a = env_coeff()
        self.p_nom = sr / mains_hz
        self.zs = [0.0, 0.0, 0.0, 0.0]  # the same of the hum band-pass
        self.e = 0.0
        self.z_prev = 0.0
        self.last = None  # time of the last accepted crossing (samples)
        self.p = self.p_nom  # period estimate
        self.p_block = self.p_nom  # period of the current block
        self.T = [0.0] * n_bins(mains_hz, sr)
        self.V = [0.0] * len(self.T)
        self.F = F_INIT
        self.r = 0.0
        self.j = 0  # sample in the block
        self.L = int(math.ceil(self.p_block - self.r))
        self.up = 0
        self.n = 0
        self.R = 0.0  # mean output high-band envelope of the last block
        self.acc = 0.0
        self.blocks = 0

    @staticmethod
    def _biquad(c, s, x):
        b0, b1, b2, a1, a2 = c
        y = b0 * x + b1 * s[0] + b2 * s[1] - a1 * s[2] - a2 * s[3]
        s[1], s[0], s[3], s[2] = s[0], x, s[2], y
        return y

    def step(self, x: float) -> float:
        n = self.n
        if self.j == 0:
            # block start: the period known from crossings at or before n
            self.p_block = self.p
            self.L = int(math.ceil(self.p_block - self.r))
            self.up = 0
        N = LP_TAPS
        self.buf[self.pos] = x
        lp = 0.0
        for i in range(N):  # lp = sum_i taps[i] x[n - i]
            lp += self.lp[i] * self.buf[(self.pos - i) % N]
        xd = self.buf[(self.pos - (N - 1) // 2) % N]
        self.pos = (self.pos + 1) % N
        h = xd - lp
        self.e = self.a * self.e + (1.0 - self.a) * h * h
        z = self._biquad(self.bp, self.zs, x)
        if self.z_prev < 0.0 <= z:
            t = (n - 1) + (-self.z_prev) / (z - self.z_prev)
            if self.last is None or t - self.last >= REFRACTORY * self.p:
                if self.last is not None:
                    d = t - self.last
                    if abs(d - self.p) <= PERIOD_TOL * self.p:
                        self.p += PERIOD_RATE * (d - self.p)
                self.last = t
        self.z_prev = z
        j = self.j
        Tj = self.T[j]
        Vj = self.V[j]
        ref = max(self.F, self.R)
        g2 = 1.0
        if self.blocks >= WARMUP and Tj > MARGIN * ref and self.e > 0.0:
            g2 = max(LIMIT * ref / self.e, 1.0 - (Tj + SPREAD * Vj) / self.e)
            g2 = min(1.0, max(FLOOR_GAIN2, g2))
        g = math.sqrt(g2)
        self.acc += g2 * self.e
        if Tj > self.F:
            self.up += 1
        self.V[j] = Vj + ALPHA * (abs(self.e - Tj) - Vj)
        self.T[j] = Tj + ALPHA * (self.e - Tj)
        self.j += 1
        self.n += 1
        if self.j == self.L:
            self.R = self.acc / self.L
            self.acc = 0.0
            self.F *= math.exp(QUANT_STEP * (self.up - (self.L - self.up)))
            self.r = self.r + self.L - self.p_block
            self.j = 0
            self.blocks += 1
        return xd - (1.0 - g) * h
