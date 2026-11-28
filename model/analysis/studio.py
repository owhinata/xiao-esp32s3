"""AudioStudio recordings as sessions of the experimental entry point
(m4_variant.py --extra / --test).

A recording is a directory under data/studio/ holding one 16 kHz mono 16-bit
WAV file <name>.wav and, beside it, <name>.marks.csv (host/AudioStudio
README, "Marks": rows sample,seconds,kind,probability,text). Copy it there
from host/AudioStudio/recordings/; data/ is not in Git. Rows of kind `setup`
(the detector setup the application ran, in the text) and `other` are read
but give no label.

Labels follow the application's reading of the marks (MarkReview.cs):
  - a `false` mark refers to the latest indication that starts at or before
    it and that no earlier `false` mark took;
  - an indication without a `false` mark is a swallow (a hit), one with a
    `false` mark a false indication;
  - a `swallow` mark is the same swallow as the nearest hit that starts from
    2 s before to 3 s after it (each hit taken by one mark at most);
    otherwise it is a swallow the detector missed.
The swallow times are every `swallow` mark plus every hit no `swallow` mark
took (the indication's time, the centre of its first positive window). The
window labels are train_eval.window_labels of those times, as for a
throat-microphone session.

Windows: 1.0 s (16000 samples) every 0.25 s from the start of the range, all
inside the range (so two ranges of one recording share no sample). The
features are computed from the 16 kHz samples directly (no 48 kHz step); a
front-end set (mains_spike.FRONT_SETS) runs its front-end over the whole
recording from sample 0 (as on a stream) before the range is cut.

A spec is DIR or DIR@A:B (seconds; A or B may be left out). Nothing here
prints or writes audio, features or per-window values.
"""

from __future__ import annotations
import csv, wave
from dataclasses import dataclass
from pathlib import Path

import numpy as np

import evaluate, features, features_v2, train_eval

SR = 16000
HOP_SAMPLES = int(round(evaluate.HOP_S * SR))  # 4000
WINDOW_SAMPLES = features.WINDOW_SAMPLES  # 16000
SWALLOW_BEFORE_S = 2.0  # MarkReview.SwallowBefore
SWALLOW_AFTER_S = 3.0  # MarkReview.SwallowAfter
KINDS = ("swallow", "false", "other", "indication", "setup")


@dataclass(frozen=True)
class Spec:
    path: Path
    a_s: float | None
    b_s: float | None

    @property
    def name(self) -> str:
        if self.a_s is None and self.b_s is None:
            return f"studio:{self.path.name}"
        a = "" if self.a_s is None else f"{self.a_s:g}"
        b = "" if self.b_s is None else f"{self.b_s:g}"
        return f"studio:{self.path.name}@{a}:{b}"


def parse_spec(text: str) -> Spec:
    path, sep, rng = str(text).partition("@")
    a = b = None
    if sep:
        lo, colon, hi = rng.partition(":")
        if not colon:
            raise ValueError(f"range is A:B (seconds): {text!r}")
        a = float(lo) if lo else None
        b = float(hi) if hi else None
        if a is not None and b is not None and not a < b:
            raise ValueError(f"empty range: {text!r}")
    return Spec(Path(path), a, b)


def find_files(d: Path) -> tuple:
    d = Path(d)
    wavs = sorted(d.glob("*.wav"))
    if len(wavs) != 1:
        raise SystemExit(f"{d}: expected one .wav file, found {len(wavs)}")
    w = wavs[0]
    m = w.with_name(w.stem + ".marks.csv")
    return w, (m if m.exists() else None)


def read_wav(path: Path) -> np.ndarray:
    """16 kHz mono 16-bit PCM -> float32 / 32768 (the scale of the sessions'
    signal in features._throat_windows)."""
    with wave.open(str(path), "rb") as f:
        if (f.getframerate(), f.getnchannels(), f.getsampwidth()) != (SR, 1, 2):
            raise SystemExit(
                f"{path}: expected 16 kHz mono 16-bit, got "
                f"{f.getframerate()} Hz, {f.getnchannels()} ch, "
                f"{8 * f.getsampwidth()} bit"
            )
        raw = f.readframes(f.getnframes())
    return (np.frombuffer(raw, dtype="<i2") / features.AUDIO_SCALE).astype(
        np.float32
    )


def read_marks(path: Path | None) -> list:
    """[(seconds, kind)] sorted by sample index; seconds = sample / 16000."""
    if path is None:
        return []
    rows = []
    with Path(path).open(encoding="utf-8", newline="") as f:
        r = csv.DictReader(f)
        if r.fieldnames[:4] != ["sample", "seconds", "kind", "probability"]:
            raise SystemExit(f"{path}: not a marks file (header {r.fieldnames})")
        for row in r:
            kind = row["kind"]
            if kind not in KINDS:
                raise SystemExit(f"{path}: unknown kind {kind!r}")
            rows.append((int(row["sample"]), kind))
    rows.sort(key=lambda r: r[0])  # stable: the file's order within a sample
    return [(s / SR, k) for s, k in rows]


def review(marks) -> dict:
    """MarkReview's reading. Returns {"swallows": [s], "false": [s] (the
    times of the indications marked false), "hits": [s]}."""
    ind = [t for t, k in marks if k == "indication"]
    false_of = [False] * len(ind)
    for t, k in marks:
        if k != "false":
            continue
        i = max((j for j, s in enumerate(ind) if s <= t), default=None)
        while i is not None and i >= 0 and false_of[i]:
            i -= 1
        if i is not None and i >= 0:
            false_of[i] = True
    hits = [s for s, f in zip(ind, false_of) if not f]
    taken = [False] * len(hits)
    swallows = []
    for t, k in marks:
        if k != "swallow":
            continue
        best = None
        for j, s in enumerate(hits):
            d = s - t
            if taken[j] or not -SWALLOW_BEFORE_S <= d <= SWALLOW_AFTER_S:
                continue
            if best is None or abs(d) < abs(hits[best] - t):
                best = j
        if best is not None:
            taken[best] = True
        swallows.append(t)
    swallows += [s for s, tk in zip(hits, taken) if not tk]
    return {
        "swallows": sorted(swallows),
        "false": [s for s, f in zip(ind, false_of) if f],
        "hits": hits,
    }


@dataclass(frozen=True, eq=False)
class Recording:
    spec: Spec
    data: train_eval.SessionData
    false_indications: list  # seconds, of the whole recording


def window_starts(n: int, a: int, b: int) -> np.ndarray:
    b = min(b, n)
    if b - a < WINDOW_SAMPLES:
        return np.zeros(0, dtype=np.int64)
    return np.arange(a, b - WINDOW_SAMPLES + 1, HOP_SAMPLES, dtype=np.int64)


def load(spec: Spec | str, feature_set: str) -> Recording:
    if not isinstance(spec, Spec):
        spec = parse_spec(spec)
    wav, marks_path = find_files(spec.path)
    x = read_wav(wav)
    marks = read_marks(marks_path)
    rv = review(marks)
    a = 0 if spec.a_s is None else int(round(spec.a_s * SR))
    b = len(x) if spec.b_s is None else min(len(x), int(round(spec.b_s * SR)))
    if not 0 <= a < b:
        raise SystemExit(f"{spec.name}: range outside the recording")
    y, base = features_v2.cnn_signal(x, feature_set)
    starts = window_starts(len(y), a, b)
    M = np.zeros((len(starts), 98, features_v2.N_MEL_CNN), dtype=np.float32)
    for k, s in enumerate(starts):
        M[k] = features_v2.mfe_one(y[s : s + WINDOW_SAMPLES], base)
    t_start = starts / SR
    t0, t1 = a / SR, b / SR
    swallows = [t for t in rv["swallows"] if t0 <= t <= t1]
    feats = features.WindowFeatures(
        session=spec.name,
        t_start_s=t_start.astype(np.float64),
        X=M,
        valid=np.ones(len(starts), dtype=bool),
    )
    data = train_eval.SessionData(
        name=spec.name,
        feats=feats,
        swallows=swallows,
        t0_s=t0,
        duration_s=t1 - t0,
        labels=train_eval.window_labels(t_start, swallows),
    )
    return Recording(spec=spec, data=data, false_indications=rv["false"])
