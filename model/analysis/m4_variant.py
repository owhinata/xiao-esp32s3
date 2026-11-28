"""Experimental variants of the M4 detector: the recorded configuration of
docs/decisions/0038 (fixed_eval.M4_CONFIG) with only the CNN feature set
changed. The variants raise the lower edge of the 40 mel bands so that the
mains-hum region is not an input (features_np.CNN_FMIN_HZ: mfe40f200 from
200 Hz, mfe40f500 from 500 Hz; same framing, pre-emphasis, window, FFT, HTK
scale, 40 bands, upper edge 4000 Hz and log as mfe40).

This is not the recorded M4 result and does not change it: fixed_eval's
constants, the 0037 / 0038 lines and m4_compare.py / m4_final_eval.py are
untouched and still only take set=mfe40. Everything here runs on the
training side only (m4_compare.list_train, M4_EXPECTED_TRAIN): no
evaluation-side session is opened.

Usage (from model/):
  python analysis/m4_variant.py data/raw --set mfe40f200 --cv
      The cross-validation of m4_compare's candidate C4 x P2 (seeds 0, 1, 2
      averaged, no hard negatives, hysteresis + 0.5 s refractory period;
      folds by collection day) with the variant's features. Prints the
      numbers at the configuration's operating point (enter 0.85, exit 0.65)
      and at the point m4_compare's rule picks from the P2 grid.
  python analysis/m4_variant.py data/raw --set mfe40f200 \\
          --final-model analysis/models/m4-f200.npz
      The final model of the variant on all training-side sessions (as
      m4_compare --final-model; refuses an existing file). --cv and
      --final-model may be given together (the features are read once).
  python analysis/m4_variant.py data/raw --hum MODEL.npz [MODEL.npz ...]
      Hum sensitivity: each model over the training-side quiet sessions as
      recorded, with the 50 Hz line changed by -7 dB, and with the hum lines
      50-450 Hz removed (FFT of the whole 16 kHz signal, +-3 Hz around each
      multiple of 50 Hz). Prints the median probability and the fraction of
      windows at or above 0.85 per model and case.

  python analysis/m4_variant.py data/raw --set mfe40 --cv \\
          --extra data/studio/REC@0:42 --extra-weight 10 --test data/studio/REC@42:
      AudioStudio recordings (studio.py): --extra adds one to the training
      side of every fold and of the final model (never a validation
      session; --extra-weight puts a loss weight on its windows), --test
      scores one by each fold's models (--cv) or by the saved model
      (--final-model). With --extra, --set mfe40 --final-model is allowed.
  python analysis/m4_variant.py data/raw --models M.npz [...] --test REC
      Scores the --test recordings with the given models (no training).
  --set mfe40ms is the front-end set of mains_spike.py (then mfe40).
  python analysis/m4_variant.py data/raw --front M.npz --test REC [...] \\
          [--front-param current] [--front-param ALPHA=1/16,WARMUP=16 ...]
      An mfe40 model without and with the front-end (no training), one case
      per --front-param (mains_spike constants changed for that case): per
      touch of each recording (sync_touches) how long the probability stays
      at or above 0.85 / 0.5, the indications and the windows >= 0.85; the
      probabilities at the recordings' swallows; and the training-side
      swallow check (damage_sessions: swallows found, false indications,
      the positive windows' mean probability and band-energy change).

  --allow-dirty  run although a tracked file has an uncommitted change (the
                 report says so; its numbers are then not reproducible from
                 the commit alone).

Only aggregate numbers are printed (counts, rates, medians). No waveform,
feature value or per-window probability is printed or written; edited
signals stay in memory.
"""

from __future__ import annotations
import argparse, contextlib, sys, time
from pathlib import Path
from typing import Sequence

import numpy as np

import evaluate, features, features_np, features_v2, fixed_eval, m4_compare as mc
import postprocess, studio, train_eval
from m4_infer import (
    CNN_SET,
    CNN_SETS,
    FRONT_SETS,
    config_mask,
    load_model,
    model_prob,
    parse_config,
)

VARIANT_SETS = tuple(s for s in CNN_SETS if s != CNN_SET)
INPUT_SETS = CNN_SETS + FRONT_SETS
TOUCH_S = 7.0  # a false indication's interval: from its time for 7 s
HUM_F0_HZ = 50.0
HUM_HALF_WIDTH_HZ = 3.0
HUM_LINE_DB = -7.0
HUM_MAX_HZ = 450.0
HUM_CASES = ("recorded", "50hz_m7db", "no_hum_50_450")
HUM_CASE_LABEL = {
    "recorded": "as recorded",
    "50hz_m7db": "50 Hz line -7 dB",
    "no_hum_50_450": "hum lines 50-450 Hz removed",
}
HUM_PROB = 0.85  # the configuration's enter
QUIET = "quiet"


def variant_config(feature_set: str) -> str:
    """fixed_eval.M4_CONFIG with only set= replaced."""
    base = fixed_eval.M4_CONFIG
    if base is None or not base.startswith(f"set={CNN_SET} "):
        raise SystemExit("fixed_eval.M4_CONFIG が set=mfe40 で始まりません")
    if feature_set not in INPUT_SETS:
        raise SystemExit(
            f"--set は {' / '.join(INPUT_SETS)} のどれかです: {feature_set!r}"
        )
    config = f"set={feature_set} " + base[len(f"set={CNN_SET} ") :]
    a, b = parse_config(base), parse_config(config)
    if {k: v for k, v in a.items() if k != "set"} != {
        k: v for k, v in b.items() if k != "set"
    }:
        raise SystemExit("変えたのは set だけのはずです")
    return config


def check_inputs(allow_dirty: bool) -> list:
    """The checks before any data is read: the fixed training list and the
    0037 / 0038 lines as m4_compare's production run checks them, and no
    uncommitted change unless allow_dirty. Returns the report's note lines."""
    if fixed_eval.M4_EXPECTED_TRAIN is None:
        raise SystemExit("fixed_eval.M4_EXPECTED_TRAIN が None です")
    text = (fixed_eval.REPO_ROOT / fixed_eval.M4_DECISION).read_text(
        encoding="utf-8"
    )
    if not fixed_eval.m4_lines_match(text):
        raise SystemExit(
            f"{fixed_eval.M4_DECISION} の M4- の行が fixed_eval.py の定数と一致しません"
        )
    if not fixed_eval.m4_config_file_matches():
        raise SystemExit(
            f"{fixed_eval.M4_CONFIG_DECISION} の M4-CONFIG の行が fixed_eval.M4_CONFIG と一致しません"
        )
    notes = [f"- commit: {train_eval.git_commit()}"]
    if train_eval.git_dirty():
        if not allow_dirty:
            raise SystemExit(
                "未コミットの変更があります（または git が使えません）。コミットしてから実行するか、"
                "実験として --allow-dirty を付けてください"
            )
        notes.append(
            "- **--allow-dirty**: tracked files had uncommitted changes; the "
            "numbers are not reproducible from the commit alone"
        )
    return notes


def _loader(feature_set: str):
    def load(root, names, lr=False):
        return mc.load_all(root, names, lr=False, cnn_set=feature_set)

    return load


def cached_loader(loader):
    """Reads once and returns the same data to later calls with the same
    names (--cv and --final-model in one run)."""
    cache: dict = {}

    def load(root, names, lr=False):
        key = tuple(names)
        if key not in cache:
            cache[key] = loader(root, names)
        return cache[key]

    return load


def _cells(m: dict | None) -> str:
    if m is None:
        return "- | - | -"
    return (
        f"{train_eval._rate(m['detection_rate'])} ({m['detected']}/{m['swallows']}) | "
        f"{train_eval._per_min(m['false_positives_per_min'])} "
        f"({m['false_positive_runs']} / {m['non_swallow_min']:.2f} min)"
    )


def weighted_fit(extra_names, weight: float, fit=None):
    """The CNN fit with the loss weight `weight` on every window of the
    sessions named in extra_names (compare.cnn_fit's sample_weight, the path
    of hard_negative=w3.0; the positive weight still counts windows). With
    weight 1 or no extra session in the data it is the plain fit."""
    if fit is None:
        import compare

        fit = compare.cnn_fit
    extra_names = frozenset(extra_names)
    if weight == 1.0 or not extra_names:
        return fit

    def f(data, where, seed):
        if not any(d.name in extra_names for d in data):
            return fit(data, where, seed)
        sw = [
            np.full(len(d.labels), weight)
            if d.name in extra_names
            else np.ones(len(d.labels))
            for d in data
        ]
        return fit(data, where, seed, sample_weight=sw)

    return f


def recording_metrics(prob, d, false_t, cfg) -> dict:
    """Aggregate numbers of one recording's windows: indications (rising
    edges of the configuration's post-processing output, as the stream shows
    them), the fraction of windows with probability >= enter, and the median
    probability of the windows whose centre is inside / outside the false
    indications' intervals [t, t + TOUCH_S)."""
    f = d.feats
    prob = np.asarray(prob)
    m = config_mask(cfg, prob, f.valid, f.t_start_s)
    rises = int(np.sum(m & ~np.concatenate(([False], m[:-1])))) if len(m) else 0
    c = f.t_start_s + 0.5
    inside = np.zeros(len(c), dtype=bool)
    for t in false_t:
        inside |= (c >= t) & (c < t + TOUCH_S)
    med = lambda a: float(np.median(a)) if len(a) else float("nan")
    return {
        "windows": len(prob),
        "indications": rises,
        "frac_enter": float(np.mean(prob >= cfg["enter"])) if len(prob) else float("nan"),
        "median_in": med(prob[inside]),
        "median_out": med(prob[~inside]),
        "n_in": int(inside.sum()),
    }


REC_HEAD = [
    "| model | recording | windows (in touch) | indications | windows >= 0.85 "
    "| median p, touch | median p, outside |",
    "|---|---|---|---|---|---|---|",
]


def _rec_line(label: str, rec, r: dict) -> str:
    return (
        f"| {label} | {rec.spec.name} | {r['windows']} ({r['n_in']}) | "
        f"{r['indications']} | {r['frac_enter']:.3f} | {r['median_in']:.3f} | "
        f"{r['median_out']:.3f} |"
    )


def _load_recordings(specs, feature_set):
    return [studio.load(s, feature_set) for s in specs]


def cv(root: Path, feature_set: str, fit=None, score=None, loader=None,
       extras=(), tests=(), extra_weight: float = 1.0):
    """The C4 x P2 cross-validation of m4_compare.run for one CNN set.
    extras (studio.Recording) join the training side of every fold (never
    the validation side; the folds stay those of the training-side
    sessions); tests (studio.Recording) are scored by each fold's models.
    Returns (report lines, {"fixed": M4Row, "grid": M4Row})."""
    cfg = parse_config(variant_config(feature_set))
    t_all = time.perf_counter()
    root = Path(root)
    listing = mc.list_train(root)
    names = listing.train
    d_cnn, _, _ = (loader or _loader(feature_set))(root, names)
    x_data = [r.data for r in extras]
    trainer = mc.Trainer(
        weighted_fit([d.name for d in x_data], extra_weight, fit), score
    )
    plain, folds = {}, []
    test_p: dict = {}  # (fold, test index) -> [prob per seed]
    for seed in cfg["seeds"]:
        scored = []
        for gname, val in mc.outer_groups(d_cnn):
            vn = {d.name for d in val}
            rest = [d for d in d_cnn if d.name not in vn] + x_data
            where = (
                f"CNN の交差検証の fold（検証側 = {gname}: "
                f"{', '.join(sorted(vn))}、種 {seed}）"
            )
            m = trainer.plain(rest, seed, where)
            if gname not in folds:
                folds.append(gname)
            scored.extend(trainer.scored(m, d, "外側") for d in val)
            for i, r in enumerate(tests):
                test_p.setdefault((gname, i), []).append(
                    trainer.scored(m, r.data, "test").prob
                )
        plain[seed] = scored
    scored = mc._ensemble([plain[s] for s in cfg["seeds"]])
    fixed_op = mc.Op(
        "P2",
        enter=cfg["enter"],
        exit=cfg["exit"],
        refractory_s=cfg["refractory_s"],
    )
    rows = {
        "fixed": mc.make_row(
            "C4", "P2", scored, fixed=fixed_op, label="fixed operating point"
        ),
        "grid": mc.make_row("C4", "P2", scored, label="rule's point"),
    }
    fmin = features_np.CNN_FMIN_HZ.get(feature_set)
    lines = [
        f"## Cross-validation C4 x P2, set={feature_set}"
        + (f" (lower mel edge {fmin:.0f} Hz)" if fmin is not None else ""),
        "",
        f"- training side: {len(names)} sessions (M4_EXPECTED_TRAIN); "
        f"folds by collection day: {', '.join(folds)}",
        *(
            [
                f"- extra training recordings in every fold: "
                f"{', '.join(d.name for d in x_data)} ({sum(len(d.labels) for d in x_data)} "
                f"windows, {sum(len(d.swallows) for d in x_data)} swallows, "
                f"loss weight {extra_weight:g})"
            ]
            if x_data
            else []
        ),
        f"- CNN trainings: {len(trainer.calls)}; "
        f"time {time.perf_counter() - t_all:.0f} s; max RSS "
        f"{mc.max_rss_kib() / 1048576:.2f} GiB",
        "",
        "| point | enter / exit | all: detection | all: false indications / min "
        "| meal: detection | meal: false / min | conditions: detection "
        "| conditions: false / min | positive windows |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for key, r in rows.items():
        lines.append(
            f"| {r.label} | {r.op.enter:.2f} / {r.op.exit:.2f} | "
            f"{_cells(r.metrics)} | {_cells(r.meal)} | {_cells(r.conds)} | "
            f"{r.positive_windows}/{r.windows} |"
        )
    if tests:
        lines += [
            "",
            "### Test recordings scored by each fold's models (mean of the seeds)",
            "",
            *REC_HEAD,
        ]
        for i, r in enumerate(tests):
            res = []
            for g in folds:
                p = postprocess.ensemble_mean(test_p[(g, i)])
                res.append(recording_metrics(p, r.data, r.false_indications, cfg))
                lines.append(_rec_line(f"fold {g}", r, res[-1]))
            mean = {
                k: float(np.mean([x[k] for x in res]))
                for k in ("indications", "frac_enter", "median_in", "median_out")
            }
            lines.append(
                f"| mean of {len(res)} folds | {r.spec.name} | - | "
                f"{mean['indications']:.2f} (range {min(x['indications'] for x in res)}-"
                f"{max(x['indications'] for x in res)}) | {mean['frac_enter']:.3f} | "
                f"{mean['median_in']:.3f} | {mean['median_out']:.3f} |"
            )
    return lines, rows


def final(root: Path, feature_set: str, out: Path, fit=None, score=None,
          loader=None, extras=(), extra_weight: float = 1.0) -> list:
    """m4_compare.train_final with the variant's configuration; extras
    (studio.Recording) are added to the training sessions."""
    x_data = [r.data for r in extras]
    if feature_set == CNN_SET and not x_data:
        raise SystemExit(
            "mfe40 の最終モデルは m4_compare.py --final-model で作ります"
        )
    config = variant_config(feature_set)
    base = loader or _loader(feature_set)

    def load(root, names, lr=False):
        d, lrd, info = base(root, names)
        return list(d) + x_data, lrd, info

    lines = mc.train_final(
        root,
        out,
        config,
        weighted_fit([d.name for d in x_data], extra_weight, fit),
        score,
        load,
    )
    out_lines = [f"## Final model, set={feature_set} (experimental)", ""]
    if x_data:
        out_lines.append(
            f"- extra training recordings: {', '.join(d.name for d in x_data)} "
            f"({sum(len(d.labels) for d in x_data)} windows, loss weight "
            f"{extra_weight:g}); the training-side line below counts the "
            "M4_EXPECTED_TRAIN sessions only"
        )
    for s in lines[2:]:
        if s.startswith("- `M4-MODEL:"):
            s = "- model: " + s[len("- `M4-MODEL: ") : -1]
        elif s.startswith("- コミット:"):
            continue
        out_lines.append(s)
    return out_lines


def score_models(model_paths: Sequence[Path], specs) -> list:
    """Report lines: each model over each test recording (its own set's
    features)."""
    lines = ["## Test recordings", "", *REC_HEAD]
    cache: dict = {}
    for p in model_paths:
        m = load_model(Path(p))
        if m["cfg"]["kind"] != "cnn":
            raise SystemExit(f"CNN のモデルではありません: {p}")
        s = m["cfg"]["set"]
        if s not in cache:
            cache[s] = _load_recordings(specs, s)
        for r in cache[s]:
            prob = model_prob(m, r.data.feats.X)
            res = recording_metrics(prob, r.data, r.false_indications, m["cfg"])
            lines.append(_rec_line(Path(p).name, r, res))
    return lines


# --- hum sensitivity ---


def edit_hum(x: np.ndarray, case: str, sr: int = features_np.SR) -> np.ndarray:
    """The whole signal through its FFT with the hum lines edited: "recorded"
    unchanged, "50hz_m7db" the bins within +-3 Hz of 50 Hz scaled by -7 dB,
    "no_hum_50_450" the bins within +-3 Hz of 50, 100, ..., 450 Hz set to 0.
    Returns float32 like features._throat_windows' signal."""
    if case == "recorded":
        return np.asarray(x, dtype=np.float32)
    x = np.asarray(x, dtype=np.float64)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    if case == "50hz_m7db":
        m = np.abs(f - HUM_F0_HZ) <= HUM_HALF_WIDTH_HZ
        X[m] *= 10.0 ** (HUM_LINE_DB / 20.0)
    elif case == "no_hum_50_450":
        n_lines = int(round(HUM_MAX_HZ / HUM_F0_HZ))
        for k in range(1, n_lines + 1):
            X[np.abs(f - k * HUM_F0_HZ) <= HUM_HALF_WIDTH_HZ] = 0.0
    else:
        raise ValueError(f"case: {case!r}")
    return np.fft.irfft(X, n=len(x)).astype(np.float32)


def quiet_sessions(names: Sequence[str]) -> list:
    return [n for n in names if mc.cond_of(n) == QUIET]


def hum(root: Path, model_paths: Sequence[Path], windows_fn=None) -> list:
    """Report lines of the hum sensitivity check. windows_fn(path) returns
    features._throat_windows' tuple (tests replace it)."""
    root = Path(root)
    models = []
    for p in model_paths:
        m = load_model(Path(p))
        if m["cfg"]["kind"] != "cnn" or m["cfg"]["set"] not in CNN_SETS:
            raise SystemExit(f"CNN のモデルではありません: {p}")
        models.append((Path(p), m))
    listing = mc.list_train(root)
    names = quiet_sessions(listing.train)
    if not names:
        raise SystemExit("学習側に quiet のセッションがありません")
    windows_fn = windows_fn or features._throat_windows
    probs = {(i, c): [] for i in range(len(models)) for c in HUM_CASES}
    for n in names:
        x, sample_t, _, start_idx, valid, _, t1 = windows_fn(root / n)
        for case in HUM_CASES:
            y = edit_hum(x, case)
            by_set = {}
            for i, (_, m) in enumerate(models):
                s = m["cfg"]["set"]
                if s not in by_set:
                    by_set[s] = features_v2.mfe_windows(
                        y, sample_t, start_idx, t1, s
                    )
                p = model_prob(m, by_set[s])
                probs[(i, case)].append(np.asarray(p)[np.asarray(valid)])
            del y, by_set
    lines = [
        "## Hum sensitivity (training-side quiet sessions)",
        "",
        f"- sessions: {len(names)} (cond quiet of M4_EXPECTED_TRAIN); "
        f"valid windows: {sum(len(a) for a in probs[(0, HUM_CASES[0])])}",
        "- edits: FFT of the whole 16 kHz session signal; bins within "
        f"+-{HUM_HALF_WIDTH_HZ:.0f} Hz of the lines",
        "- these sessions are in every model's training data (in-sample)",
        "",
        "| model | set | case | median probability | windows >= 0.85 |",
        "|---|---|---|---|---|",
    ]
    for i, (p, m) in enumerate(models):
        for case in HUM_CASES:
            a = np.concatenate(probs[(i, case)])
            lines.append(
                f"| {p.name} | {m['cfg']['set']} | {HUM_CASE_LABEL[case]} | "
                f"{np.median(a):.3f} | {np.mean(a >= HUM_PROB):.3f} |"
            )
    return lines


# --- front-end check (mains_spike.py before an unchanged mfe40 model) ---

FRONT_PARAMS = (  # mains_spike constants --front-param may change
    "ALPHA", "MARGIN", "LIMIT", "SPREAD", "WARMUP", "QUANT_STEP",
    "FLOOR_GAIN2", "ENV_TAU", "PERIOD_RATE",
)
TOUCH_BAND_HZ = (500.0, 4000.0)
TOUCH_SMOOTH = 16  # samples (1 ms) of envelope smoothing
TOUCH_PERIODS = 12  # mains periods per segment
TOUCH_STEP = 80  # samples (5 ms) between segments
TOUCH_SYNC_DB = 11.0
TOUCH_MIN_S = 2.0
TOUCH_GAP_S = 0.5
DAMAGE_CONDS = ("water", "saliva")  # + the first training-side meal


def parse_front_params(text: str) -> dict:
    """"NAME=VALUE,NAME=VALUE" (VALUE a number or a fraction a/b) -> dict;
    "" or "current" is the module's constants."""
    from fractions import Fraction

    out: dict = {}
    text = text.strip()
    if text in ("", "current"):
        return out
    for item in text.split(","):
        k, sep, v = item.partition("=")
        k = k.strip()
        if not sep or k not in FRONT_PARAMS:
            raise ValueError(
                f"--front-param: NAME=VALUE with NAME one of {', '.join(FRONT_PARAMS)}: {item!r}"
            )
        f = float(Fraction(v.strip()))
        out[k] = int(f) if k == "WARMUP" else f
    return out


def front_label(params: dict) -> str:
    if not params:
        return "front-end (current)"
    return "front-end " + ", ".join(f"{k}={v:g}" for k, v in params.items())


@contextlib.contextmanager
def front_constants(params: dict):
    """mains_spike's module constants set to params for the duration (both
    apply() and MainsSpike read them at call time); restored afterwards."""
    import mains_spike

    old = {k: getattr(mains_spike, k) for k in params}
    try:
        for k, v in params.items():
            setattr(mains_spike, k, v)
        yield
    finally:
        for k, v in old.items():
            setattr(mains_spike, k, v)


def sync_touches(x, mains_hz: float = 50.0, sr: int = features_np.SR) -> list:
    """Intervals [(a, b)] (seconds) in which mains-synchronous bursts are
    present, found offline (non-causal, only to measure the causal front-end
    against): the band TOUCH_BAND_HZ envelope (zero-phase filter, smoothed
    over TOUCH_SMOOTH samples) is folded over TOUCH_PERIODS nominal periods
    every TOUCH_STEP samples; the per-bin median over the periods (a
    transient in a few periods does not count) gives the profile, and its
    top 10 % against its lower half in dB is the segment's sync level,
    assigned to the segment's centre. Runs above TOUCH_SYNC_DB, joined over
    gaps below TOUCH_GAP_S, of at least TOUCH_MIN_S are the touches."""
    import scipy.signal

    P = int(round(sr / mains_hz))
    sos = scipy.signal.butter(4, TOUCH_BAND_HZ, "bandpass", fs=sr, output="sos")
    e = scipy.signal.sosfiltfilt(sos, np.asarray(x, np.float64)) ** 2
    e = scipy.signal.lfilter(np.ones(TOUCH_SMOOTH) / TOUCH_SMOOTH, [1.0], e)
    L = TOUCH_PERIODS * P
    top, low = max(1, P // 10), P // 2
    cs, on = [], []
    for a in range(0, len(e) - L, TOUCH_STEP):
        prof = np.sort(np.median(e[a : a + L].reshape(-1, P), axis=0))
        lo = prof[:low].mean()
        level = 10.0 * np.log10(prof[-top:].mean() / lo) if lo > 0 else 0.0
        cs.append((a + L / 2) / sr)
        on.append(level > TOUCH_SYNC_DB)
    runs: list = []
    start = None
    for c, o in zip(cs, on):
        if o and start is None:
            start = c
        elif not o and start is not None:
            runs.append([start, c])
            start = None
    if start is not None:
        runs.append([start, cs[-1]])
    merged: list = []
    for r in runs:
        if merged and r[0] - merged[-1][1] < TOUCH_GAP_S:
            merged[-1][1] = r[1]
        else:
            merged.append(r)
    return [(float(a), float(b)) for a, b in merged if b - a >= TOUCH_MIN_S]


def touch_metrics(t_start_s, prob, touches, cfg) -> list:
    """Per touch (a, b): the time from a until the probability stays below
    0.85 (enter) / 0.5 up to b (windows whose centre is in [a, b]; 0 when no
    window is at or above, inf when the last one is), the indications
    (rising edges of the post-processing output whose window centre is in
    [a - 0.5, b + 1)) and the fraction of the windows >= enter."""
    t_start_s = np.asarray(t_start_s, np.float64)
    prob = np.asarray(prob)
    c = t_start_s + 0.5
    m = config_mask(cfg, prob, np.ones(len(prob), bool), t_start_s)
    rise = m & ~np.concatenate(([False], m[:-1])) if len(m) else m
    out = []
    for a, b in touches:
        w = (c >= a) & (c <= b)
        cc, pp = c[w], prob[w]

        def stays(th):
            above = np.flatnonzero(pp >= th)
            if len(above) == 0:
                return 0.0
            k = above[-1]
            return float("inf") if k == len(pp) - 1 else float(cc[k + 1] - a)

        out.append({
            "to_enter": stays(cfg["enter"]),
            "to_half": stays(0.5),
            "indications": int(np.sum(rise & (c >= a - 0.5) & (c < b + 1.0))),
            "frac_enter": float(np.mean(pp >= cfg["enter"])) if len(pp) else float("nan"),
        })
    return out


def swallow_probs(t_start_s, prob, swallows) -> list:
    """The highest probability of the windows whose centre is in a swallow's
    counting interval [t - 0.5, t + 1.5] (nan when there is none)."""
    c = np.asarray(t_start_s, np.float64) + evaluate.WINDOW_S / 2
    prob = np.asarray(prob)
    out = []
    for t in swallows:
        w = (c >= t - evaluate.PRE_S) & (c <= t + evaluate.POST_S)
        out.append(float(prob[w].max()) if w.any() else float("nan"))
    return out


def damage_sessions(names: Sequence[str]) -> list:
    """The training-side sessions of the swallow check: the condition
    sessions whose task is swallowing (DAMAGE_CONDS) and the first meal."""
    picked = [n for n in names if mc.cond_of(n) in DAMAGE_CONDS]
    meals = [n for n in names if mc.is_meal(n)]
    return picked + meals[:1]


def _band(x, sr: int = features_np.SR):
    import scipy.signal

    sos = scipy.signal.butter(4, TOUCH_BAND_HZ, "bandpass", fs=sr, output="sos")
    return scipy.signal.sosfilt(sos, np.asarray(x, np.float64))


def _rec_windows(y) -> tuple:
    starts = studio.window_starts(len(y), 0, len(y))
    M = np.zeros((len(starts), 98, features_v2.N_MEL_CNN), dtype=np.float32)
    for k, s in enumerate(starts):
        M[k] = features_v2.mfe_one(y[s : s + studio.WINDOW_SAMPLES], CNN_SET)
    return starts / studio.SR, M


def _fmt_s(v: float) -> str:
    return "never above" if v == 0.0 else ("not before the end" if v == float("inf") else f"{v:.2f} s")


def front_check(root: Path, model_path: Path, specs, candidates,
                windows_fn=None, events_fn=None) -> list:
    """Report lines: the mfe40 model of model_path without the front-end
    and with it at each parameter set of candidates (dicts for
    front_constants; {} the current constants), over the recordings of specs
    (per touch of sync_touches: touch_metrics; the probabilities at the
    recording's swallows) and over the training-side damage_sessions
    (swallows found, false indications, mean probability of the positive
    windows, the change of the TOUCH_BAND_HZ energy of the positive windows
    by the front-end). windows_fn / events_fn replace
    features._throat_windows / evaluate.load_events (tests)."""
    import mains_spike

    m = load_model(Path(model_path))
    cfg = m["cfg"]
    if cfg["kind"] != "cnn" or cfg["set"] != CNN_SET:
        raise SystemExit(f"--front は set={CNN_SET} の CNN のモデルで使う: {model_path}")
    D = (mains_spike.LP_TAPS - 1) // 2
    cases = [("raw", None)] + [(front_label(p), p) for p in candidates]

    def run(x, params):
        if params is None:
            return np.asarray(x, np.float32)
        with front_constants(params):
            return mains_spike.apply(x)

    lines = [
        f"## Front-end check: {Path(model_path).name} without / with mains_spike",
        "",
        "- touches: sync_touches (offline: the median-folded "
        f"{TOUCH_BAND_HZ[0]:.0f}-{TOUCH_BAND_HZ[1]:.0f} Hz envelope above "
        f"{TOUCH_SYNC_DB:g} dB, at least {TOUCH_MIN_S:g} s); times are window "
        "centres from the touch start",
        "",
        "| recording | touch (s) | case | to < 0.85 | to < 0.5 | indications | windows >= 0.85 |",
        "|---|---|---|---|---|---|---|",
    ]
    sw_lines = [
        "",
        "| recording | case | probability at each swallow | swallows found |",
        "|---|---|---|---|",
    ]
    for spec in specs:
        spec = studio.parse_spec(spec) if not isinstance(spec, studio.Spec) else spec
        if spec.a_s is not None or spec.b_s is not None:
            raise SystemExit(f"--front takes whole recordings (no @A:B): {spec.name}")
        wav, marks = studio.find_files(spec.path)
        x = studio.read_wav(wav)
        swallows = studio.review(studio.read_marks(marks))["swallows"]
        touches = sync_touches(x)
        per_case = []
        for label, params in cases:
            y = run(x, params)
            if params is not None:
                # read the output D samples late, as AudioStudio's runner does:
                # window k holds the front-end's output for the raw samples
                # of window k (the last D samples of the last window read 0)
                y = np.concatenate((y[D:], np.zeros(D, np.float32)))
            t_start, M = _rec_windows(y)
            prob = model_prob(m, M)
            per_case.append((label, touch_metrics(t_start, prob, touches, cfg),
                             swallow_probs(t_start, prob, swallows), t_start, prob))
        for i, (a, b) in enumerate(touches):
            for label, tm, _, _, _ in per_case:
                r = tm[i]
                lines.append(
                    f"| {spec.path.name} | {a:.2f}-{b:.2f} | {label} | "
                    f"{_fmt_s(r['to_enter'])} | {_fmt_s(r['to_half'])} | "
                    f"{r['indications']} | {r['frac_enter']:.3f} |"
                )
        for label, _, sp, t_start, prob in per_case:
            mask = config_mask(cfg, prob, np.ones(len(prob), bool), t_start)
            got = evaluate.event_metrics(
                [t for t in swallows if 0 <= t <= len(x) / studio.SR],
                t_start[mask].tolist(), len(x) / studio.SR, 0.0,
            )["detected"] if swallows else 0
            sw_lines.append(
                f"| {spec.path.name} | {label} | "
                f"{' '.join(f'{v:.2f}' for v in sp) or '-'} | {got}/{len(swallows)} |"
            )
    lines += sw_lines

    root = Path(root)
    names = damage_sessions(mc.list_train(root).train)
    windows_fn = windows_fn or features._throat_windows
    events_fn = events_fn or evaluate.load_events
    acc = {label: {"per": {}, "p": [], "db": []} for label, _ in cases}
    for n in names:
        x, sample_t, t_start, start_idx, valid, t0, t1 = windows_fn(root / n)
        swallows = events_fn(root / n)
        t_start = np.asarray(t_start, np.float64)
        pos = (train_eval.window_labels(t_start, swallows) == train_eval.POSITIVE) & np.asarray(valid)
        hx = _band(x)
        for label, params in cases:
            y = run(x, params)
            M = features_v2.mfe_windows(y, sample_t, start_idx, t1, CNN_SET)
            prob = np.asarray(model_prob(m, M))
            mask = config_mask(cfg, prob, valid, t_start)
            acc[label]["per"][n] = evaluate.event_metrics(
                swallows, t_start[mask].tolist(), t1 - t0, t0
            )
            acc[label]["p"].append(prob[pos])
            if params is not None:
                hy = _band(y)
                for i in np.asarray(start_idx)[pos]:
                    ex = float(np.sum(hx[i : i + studio.WINDOW_SAMPLES] ** 2))
                    ey = float(np.sum(hy[i + D : i + D + studio.WINDOW_SAMPLES] ** 2))
                    if ex > 0 and ey > 0:
                        acc[label]["db"].append(10.0 * np.log10(ey / ex))
            del y, M
    lines += [
        "",
        f"### Training-side swallow check ({len(names)} sessions: "
        f"{' and '.join(DAMAGE_CONDS)} conditions and the first meal; in-sample for the model)",
        "",
        "| case | swallows found | false indications (runs / min) | mean p, positive windows "
        "| band energy change, positive windows: mean / min dB |",
        "|---|---|---|---|---|",
    ]
    for label, _ in cases:
        per = acc[label]["per"].values()
        tot = {k: sum(v[k] for v in per) for k in
               ("swallows", "detected", "false_positive_runs", "non_swallow_min")}
        tot["false_positives_per_min"] = (
            tot["false_positive_runs"] / tot["non_swallow_min"]
            if tot["non_swallow_min"] > 0 else None
        )
        p = np.concatenate(acc[label]["p"]) if acc[label]["p"] else np.zeros(0)
        db = np.asarray(acc[label]["db"])
        lines.append(
            f"| {label} | {tot['detected']}/{tot['swallows']} | "
            f"{tot['false_positive_runs']} ({train_eval._per_min(tot['false_positives_per_min'])}) | "
            f"{(p.mean() if len(p) else float('nan')):.3f} | "
            + (f"{db.mean():.2f} / {db.min():.2f} |" if len(db) else "- |")
        )
    return lines


def main(argv: Sequence[str] | None = None) -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("root", type=Path, help="セッションフォルダの親（data/raw）")
    ap.add_argument("--set", choices=INPUT_SETS, help="CNN の特徴量の組")
    ap.add_argument("--cv", action="store_true", help="C4 x P2 の交差検証")
    ap.add_argument("--final-model", type=Path, help="最終モデルの出力先")
    ap.add_argument("--hum", type=Path, nargs="+", help="ハムの感度を見るモデル")
    ap.add_argument(
        "--extra", action="append", default=[],
        help="training recording DIR[@A:B] (studio.py); repeatable",
    )
    ap.add_argument(
        "--extra-weight", type=float, default=1.0,
        help="loss weight of the --extra windows (default 1)",
    )
    ap.add_argument(
        "--test", action="append", default=[],
        help="recording DIR[@A:B] to score (studio.py); repeatable",
    )
    ap.add_argument(
        "--models", type=Path, nargs="+",
        help="score the --test recordings with these models (no training)",
    )
    ap.add_argument(
        "--front", type=Path,
        help="an mfe40 model without / with the mains_spike front-end over the "
             "--test recordings and the training-side swallow check (no training)",
    )
    ap.add_argument(
        "--front-param", action="append", default=None,
        help="with --front: a front-end case NAME=VALUE[,NAME=VALUE] of "
             "mains_spike constants (\"current\" = unchanged); repeatable; "
             "default one case with the current constants",
    )
    ap.add_argument("--allow-dirty", action="store_true")
    a = ap.parse_args(argv)
    if a.front_param is not None and a.front is None:
        ap.error("--front-param は --front と一緒に使う")
    if a.front is not None:
        if a.set or a.cv or a.final_model or a.extra or a.models or a.hum:
            ap.error("--front は --test（と --front-param）とだけ使う")
        try:
            front_cases = [parse_front_params(t) for t in (a.front_param or ["current"])]
        except ValueError as e:
            ap.error(str(e))
    elif a.hum is not None:
        if a.set or a.cv or a.final_model or a.extra or a.test or a.models:
            ap.error("--hum は単独で使う")
    elif a.models is not None:
        if a.set or a.cv or a.final_model or a.extra or not a.test:
            ap.error("--models は --test と一緒に、学習なしで使う")
    elif not a.set or not (a.cv or a.final_model):
        ap.error("--set と、--cv か --final-model（または --hum、--models）が要る")
    elif a.set == CNN_SET and a.final_model and not a.extra:
        ap.error("mfe40 の最終モデルは m4_compare.py --final-model で作る")
    if a.extra_weight <= 0:
        ap.error("--extra-weight > 0")
    if a.extra_weight != 1.0 and not a.extra:
        ap.error("--extra-weight は --extra と一緒に使う")
    notes = check_inputs(a.allow_dirty)  # before any data is read
    print("# M4 experimental variant (not the recorded M4 result)\n")
    print("\n".join(notes))
    if a.front is not None:
        print("\n" + "\n".join(front_check(a.root, a.front, a.test, front_cases)), flush=True)
        return
    if a.hum is not None:
        print("\n" + "\n".join(hum(a.root, a.hum)))
        return
    if a.models is not None:
        print("\n" + "\n".join(score_models(a.models, a.test)), flush=True)
        return
    print(f"- config: `{variant_config(a.set)}`")
    extras = _load_recordings(a.extra, a.set)
    tests = _load_recordings(a.test, a.set)
    loader = cached_loader(_loader(a.set))
    if a.cv:
        lines, _ = cv(
            a.root, a.set, loader=loader, extras=extras, tests=tests,
            extra_weight=a.extra_weight,
        )
        print("\n" + "\n".join(lines), flush=True)
    if a.final_model:
        print("\n" + "\n".join(final(
            a.root, a.set, a.final_model, loader=loader, extras=extras,
            extra_weight=a.extra_weight,
        )), flush=True)
        if tests:
            print("\n" + "\n".join(score_models([a.final_model], a.test)))


if __name__ == "__main__":
    main()
