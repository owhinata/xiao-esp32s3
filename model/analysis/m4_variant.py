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
import train_eval
from m4_infer import CNN_SET, CNN_SETS, load_model, model_prob, parse_config

VARIANT_SETS = tuple(s for s in CNN_SETS if s != CNN_SET)
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
    if feature_set not in CNN_SETS:
        raise SystemExit(
            f"--set は {' / '.join(VARIANT_SETS)} のどれかです: {feature_set!r}"
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


def cv(root: Path, feature_set: str, fit=None, score=None, loader=None):
    """The C4 x P2 cross-validation of m4_compare.run for one CNN set.
    Returns (report lines, {"fixed": M4Row, "grid": M4Row})."""
    cfg = parse_config(variant_config(feature_set))
    t_all = time.perf_counter()
    root = Path(root)
    listing = mc.list_train(root)
    names = listing.train
    d_cnn, _, _ = (loader or _loader(feature_set))(root, names)
    trainer = mc.Trainer(fit, score)
    plain, folds = {}, None
    for seed in cfg["seeds"]:
        f, plain[seed] = mc.cnn_cv(trainer, d_cnn, seed, False)
        folds = folds or f
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
    lines = [
        f"## Cross-validation C4 x P2, set={feature_set} "
        f"(lower mel edge {features_np.CNN_FMIN_HZ[feature_set]:.0f} Hz)",
        "",
        f"- training side: {len(names)} sessions (M4_EXPECTED_TRAIN); "
        f"folds by collection day: {', '.join(f.name for f in folds)}",
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
    return lines, rows


def final(root: Path, feature_set: str, out: Path, fit=None, score=None,
          loader=None) -> list:
    """m4_compare.train_final with the variant's configuration."""
    if feature_set == CNN_SET:
        raise SystemExit(
            "mfe40 の最終モデルは m4_compare.py --final-model で作ります"
        )
    config = variant_config(feature_set)
    lines = mc.train_final(
        root, out, config, fit, score, loader or _loader(feature_set)
    )
    out_lines = [f"## Final model, set={feature_set} (experimental)", ""]
    for s in lines[2:]:
        if s.startswith("- `M4-MODEL:"):
            s = "- model: " + s[len("- `M4-MODEL: ") : -1]
        elif s.startswith("- コミット:"):
            continue
        out_lines.append(s)
    return out_lines


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
    ap.add_argument("--set", choices=CNN_SETS, help="CNN の特徴量の組")
    ap.add_argument("--cv", action="store_true", help="C4 x P2 の交差検証")
    ap.add_argument("--final-model", type=Path, help="最終モデルの出力先")
    ap.add_argument("--hum", type=Path, nargs="+", help="ハムの感度を見るモデル")
    ap.add_argument("--allow-dirty", action="store_true")
    a = ap.parse_args(argv)
    if a.hum is not None:
        if a.set or a.cv or a.final_model:
            ap.error("--hum は --set・--cv・--final-model と一緒に使わない")
    elif not a.set or not (a.cv or a.final_model):
        ap.error("--set と、--cv か --final-model（または --hum）が要る")
    elif a.set == CNN_SET and a.final_model:
        ap.error("mfe40 の最終モデルは m4_compare.py --final-model で作る")
    notes = check_inputs(a.allow_dirty)  # before any data is read
    print("# M4 experimental variant (not the recorded M4 result)\n")
    print("\n".join(notes))
    if a.hum is not None:
        print("\n" + "\n".join(hum(a.root, a.hum)))
        return
    print(f"- config: `{variant_config(a.set)}`")
    loader = cached_loader(_loader(a.set))
    if a.cv:
        lines, _ = cv(a.root, a.set, loader=loader)
        print("\n" + "\n".join(lines), flush=True)
    if a.final_model:
        print("\n" + "\n".join(final(a.root, a.set, a.final_model, loader=loader)))


if __name__ == "__main__":
    main()
