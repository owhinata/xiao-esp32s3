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

  --allow-dirty  run although a tracked file has an uncommitted change (the
                 report says so; its numbers are then not reproducible from
                 the commit alone).

Only aggregate numbers are printed (counts, rates, medians). No waveform,
feature value or per-window probability is printed or written; edited
signals stay in memory.
"""

from __future__ import annotations
import argparse, sys, time
from pathlib import Path
from typing import Sequence

import numpy as np

import features, features_np, features_v2, fixed_eval, m4_compare as mc
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
    ap.add_argument("--allow-dirty", action="store_true")
    a = ap.parse_args(argv)
    if a.hum is not None:
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
