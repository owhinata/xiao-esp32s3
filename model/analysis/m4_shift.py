"""Shift augmentation of the M4 CNN's training windows and the grid
sensitivity of a model (experimental, used by m4_variant.py --shift / --grid).

The detector scores 1.0 s windows every 0.25 s (evaluate.HOP_S). The
training windows of m4_compare / compare.cnn_fit lie on that grid only. Here
the training side can also use windows at the offsets HOP_S * p / K
(p = 0 .. K-1) of the grid, labelled by train_eval.window_labels applied to
each window's own centre (positive when the centre is in [t, t + 1.0] after
a swallow marker t, unused in the 0.5 s around that, negative otherwise).

Shift specs (--shift):
  dense:K[:E]   every offset once per epoch (K times the windows), E epochs
                (default compare.CNN_EPOCHS = 20; E = 20 / K keeps the
                number of optimizer steps of the plain fit)
  random:K[:E]  per epoch one offset per grid position, drawn uniformly
                among that position's usable offsets (the plain fit's number
                of windows per epoch; the steps stay those of the plain fit)
The normalisation (mean / std per mel band) comes from the 0.25 s grid's
valid windows as in compare.cnn_fit; pos_weight is negatives / positives
over all the usable shifted windows. Validation and scoring stay on the
0.25 s grid. dense:1 is compare.cnn_fit exactly (same batches, same steps).

Grid sensitivity (--grid, --grid-cv): a model's probability for window
positions every FINE_S (25 ms; the 10 phases of the 0.25 s grid). Per
swallow: the highest probability of the windows whose centre is in its
counting interval [t - 0.5, t + 1.5], the span (number of those positions
at or above the configuration's enter, times 25 ms) and in how many of the
10 grid phases the post-processing (hysteresis + refractory period, run over
the whole stream of that phase) finds it. Outside the swallows: the fraction
of the fine-grid windows at or above enter and the false indications per
minute averaged over the phases.

Only aggregate numbers are reported; no waveform, feature value or
per-window probability is printed or written.
"""

from __future__ import annotations
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

import numpy as np

import evaluate, features, features_v2, studio, train_eval
from train_eval import POSITIVE, NEGATIVE, UNUSED
from m4_infer import CNN_SHAPES, config_mask

HOP_S = evaluate.HOP_S
WINDOW_S = evaluate.WINDOW_S
PHASES = 10  # grid phases of the sensitivity check
FINE_S = HOP_S / PHASES  # 25 ms
SHIFT_KS = (1, 2, 4, 5, 8, 10)  # offsets per hop (HOP_SAMPLES divisible by K)
SHIFT_MODES = ("dense", "random")
SPAN_SHORT_MS = 250.0
SCORE_BATCH = 512


# --- shift specs ---


@dataclass(frozen=True)
class ShiftSpec:
    mode: str  # "dense" | "random"
    k: int  # offsets per hop
    epochs: int

    @property
    def name(self) -> str:
        return f"{self.mode[0]}{self.k}e{self.epochs}"

    def describe(self) -> str:
        step = HOP_S / self.k * 1000.0
        what = (
            "every offset once per epoch" if self.mode == "dense"
            else "one random offset per grid position and epoch"
        )
        return (
            f"shift {self.name}: {self.mode}, offsets every {step:g} ms "
            f"({self.k} per 0.25 s), {what}, {self.epochs} epochs"
        )


def parse_shift(text: str) -> ShiftSpec:
    import compare

    parts = str(text).strip().split(":")
    if len(parts) not in (2, 3) or parts[0] not in SHIFT_MODES:
        raise ValueError(f"--shift is dense:K[:E] or random:K[:E]: {text!r}")
    try:
        k = int(parts[1])
        e = int(parts[2]) if len(parts) == 3 else compare.CNN_EPOCHS
    except ValueError:
        raise ValueError(f"--shift: K and E are integers: {text!r}") from None
    if k not in SHIFT_KS or e < 1:
        raise ValueError(
            f"--shift: K one of {', '.join(map(str, SHIFT_KS))}, E >= 1: {text!r}"
        )
    return ShiftSpec(parts[0], k, e)


# --- window positions ---


def nearest_samples(sample_t: np.ndarray, n: int, t: np.ndarray) -> np.ndarray:
    """The sample index nearest to each time t (features._throat_windows'
    rule, ties to the earlier sample)."""
    st = np.asarray(t, np.float64)
    i = np.searchsorted(sample_t[:-1], st, side="left").astype(np.int64)
    prev = np.maximum(i - 1, 0)
    take_prev = (i > 0) & (
        (i == n) | (st - sample_t[prev] <= sample_t[np.minimum(i, n)] - st)
    )
    return np.where(take_prev, i - 1, i)


def phase_times(t_lo: float, t_hi: float, k: int) -> tuple:
    """Windows at the offsets HOP_S * p / k of the 0.25 s grid inside
    [t_lo, t_hi], ordered by time: (t_start, slot, phase). Phase 0 is the
    grid of features._throat_windows exactly (same times, same count)."""
    span = t_hi - t_lo
    if span < WINDOW_S:
        z = np.zeros(0)
        return z, z.astype(np.int64), z.astype(np.int64)
    nw = int(math.floor((span - WINDOW_S) / HOP_S + 1e-9)) + 1
    slot = np.repeat(np.arange(nw, dtype=np.int64), k)
    phase = np.tile(np.arange(k, dtype=np.int64), nw)
    off = HOP_S * phase / k
    keep = HOP_S * slot + off <= span - WINDOW_S + 1e-9 * HOP_S
    slot, phase, off = slot[keep], phase[keep], off[keep]
    t = t_lo + HOP_S * slot.astype(np.float64)
    t = np.where(phase == 0, t, t + off)
    return t, slot, phase


@dataclass(frozen=True, eq=False)
class Windows:
    """Windows of one session or recording off the 0.25 s grid."""

    name: str
    t_start_s: np.ndarray
    slot: np.ndarray  # index of the grid position the window belongs to
    phase: np.ndarray  # offset HOP_S * phase / k
    k: int
    valid: np.ndarray
    X: np.ndarray  # (n, 98, 40) float32
    labels: np.ndarray


def session_windows(x, sample_t, t_lo, t_hi, k, swallows, feature_set, name,
                    keep=None) -> Windows:
    """The windows of phase_times of a throat-microphone session's 16 kHz
    signal (features._throat_windows' tuple): nearest sample, 16000 samples,
    the bounds of features_v2.mfe_windows, valid when the 16000 samples are
    inside the signal. keep(labels, valid) -> bool mask, if given, drops the
    other windows before their features are computed."""
    t, slot, phase = phase_times(t_lo, t_hi, k)
    n = len(x)
    idx = nearest_samples(sample_t, n, t)
    valid = idx + studio.WINDOW_SAMPLES <= n
    labels = train_eval.window_labels(t, swallows)
    sel = np.ones(len(t), bool) if keep is None else np.asarray(keep(labels, valid))
    grid_idx = idx[phase == 0]
    lo, hi = features_v2._session_bounds(x, sample_t, grid_idx, t_hi)
    y, base = features_v2.cnn_signal(x, feature_set)
    rows = np.flatnonzero(sel)
    M = np.zeros((len(rows), 98, features_v2.N_MEL_CNN), dtype=np.float32)
    for j, r in enumerate(rows):
        a, b = features_v2.segment_bounds(idx[r], 1.0, lo, hi)
        M[j] = features_v2.mfe_one(y[a:b], base)
    return Windows(name, t[rows], slot[rows], phase[rows], k, valid[rows], M,
                   labels[rows])


def recording_windows(x, k, swallows, feature_set, name) -> Windows:
    """The windows of an AudioStudio recording (the whole of it) at the
    offsets HOP_SAMPLES * p / k of studio.window_starts' grid."""
    step = studio.HOP_SAMPLES // k
    if step * k != studio.HOP_SAMPLES:
        raise ValueError(f"k must divide {studio.HOP_SAMPLES}: {k}")
    y, base = features_v2.cnn_signal(x, feature_set)
    starts = np.arange(0, len(y) - studio.WINDOW_SAMPLES + 1, step, dtype=np.int64)
    M = np.zeros((len(starts), 98, features_v2.N_MEL_CNN), dtype=np.float32)
    for j, s in enumerate(starts):
        M[j] = features_v2.mfe_one(y[s : s + studio.WINDOW_SAMPLES], base)
    t = starts / studio.SR
    q = starts // step
    return Windows(name, t, q // k, q % k, k, np.ones(len(starts), bool), M,
                   train_eval.window_labels(t, swallows))


# --- training store and fit ---


@dataclass(eq=False)
class ShiftStore:
    """The usable (valid, not unused) shifted windows of the training-side
    sessions, one array per session."""

    spec: ShiftSpec
    sessions: dict  # name -> Windows

    def windows(self) -> int:
        return sum(len(w.labels) for w in self.sessions.values())


def _usable(labels, valid):
    return np.asarray(valid) & (np.asarray(labels) != UNUSED)


def build_store(root: Path, names: Sequence[str], feature_set: str,
                spec: ShiftSpec, windows_fn=None, events_fn=None) -> ShiftStore:
    """Reads the sessions (features._throat_windows / evaluate.load_events;
    tests replace them) and keeps the usable windows of spec.k offsets."""
    windows_fn = windows_fn or features._throat_windows
    events_fn = events_fn or evaluate.load_events
    out = {}
    for n in names:
        x, sample_t, _, _, _, t_lo, t_hi = windows_fn(Path(root) / n)
        out[n] = session_windows(x, sample_t, t_lo, t_hi, spec.k,
                                 events_fn(Path(root) / n), feature_set, n,
                                 keep=_usable)
        del x
    return ShiftStore(spec, out)


def _slot_pick(slot_gid: np.ndarray, rng) -> np.ndarray:
    """One index per distinct slot_gid, uniformly among its entries."""
    r = rng.random(len(slot_gid))
    order = np.lexsort((r, slot_gid))
    s = slot_gid[order]
    first = np.ones(len(s), bool)
    first[1:] = s[1:] != s[:-1]
    return order[first]


def shift_fit(store: ShiftStore):
    """A fit(data, where, seed) for m4_compare.Trainer: compare.cnn_fit's
    network, optimiser, batch and normalisation, trained on the store's
    shifted windows of the sessions in data (a session not in the store,
    e.g. an --extra recording, contributes its grid windows)."""
    import compare

    spec = store.spec

    def fit(data, where, seed, sample_weight=None):
        if sample_weight is not None:
            raise SystemExit(f"{where}: --shift has no sample weights")
        names = ", ".join(d.name for d in data)
        used = [d for d in data if d.feats.valid.any()]
        if not used:
            raise SystemExit(f"{where}: no valid window ({names})")
        allv = np.concatenate([d.feats.X[d.feats.valid] for d in used]).astype(
            np.float64
        )
        mean = allv.mean(axis=(0, 1))
        std = allv.std(axis=(0, 1))
        std[std == 0] = 1.0
        del allv
        Xs, ys, gids, starts = [], [], [], [0]
        g0 = 0
        for d in data:
            w = store.sessions.get(d.name)
            if w is not None:
                X, y, sl = w.X, w.labels, w.slot
            else:
                u = d.feats.valid & (d.labels != UNUSED)
                X, y, sl = d.feats.X[u], d.labels[u], np.flatnonzero(u)
            Xs.append(X)
            ys.append(y)
            gids.append(g0 + np.asarray(sl, np.int64))
            g0 += int(sl.max()) + 1 if len(sl) else 0
            starts.append(starts[-1] + len(y))
        y = np.concatenate(ys)
        gid = np.concatenate(gids)
        starts = np.asarray(starts)
        n_pos, n_neg = int((y == POSITIVE).sum()), int((y == NEGATIVE).sum())
        if n_pos == 0 or n_neg == 0:
            raise SystemExit(
                f"{where}: positives {n_pos}, negatives {n_neg} ({names})"
            )
        pos_weight = n_neg / n_pos
        torch = compare._torch_setup(seed)
        net = compare._make_net(torch)
        yt = torch.from_numpy((y == POSITIVE).astype(np.float32))
        loss_fn = torch.nn.BCEWithLogitsLoss(
            pos_weight=torch.tensor(pos_weight, dtype=torch.float32)
        )
        opt = torch.optim.Adam(net.parameters(), lr=compare.CNN_LR)
        g = torch.Generator().manual_seed(seed)
        rng = np.random.default_rng(seed)
        buf = np.empty((compare.CNN_BATCH, 98, features_v2.N_MEL_CNN), np.float32)
        per_epoch = 0
        net.train()
        for _ in range(spec.epochs):
            if spec.mode == "dense":
                pool = np.arange(len(y))
            else:
                pool = np.sort(_slot_pick(gid, rng))
            per_epoch = len(pool)
            perm = torch.randperm(len(pool), generator=g).numpy()
            for k in range(0, len(pool), compare.CNN_BATCH):
                idx = pool[perm[k : k + compare.CNN_BATCH]]
                xb = _gather(Xs, starts, idx, buf[: len(idx)])
                xb = torch.from_numpy(((xb - mean) / std).astype(np.float32))
                opt.zero_grad()
                loss = loss_fn(net(xb), yt[torch.from_numpy(idx)])
                loss.backward()
                opt.step()
        params = {
            k: v.detach().cpu().numpy().copy() for k, v in net.state_dict().items()
        }
        return compare.CnnModel(
            mean=mean, std=std, pos_weight=pos_weight, params=params,
            sessions=tuple(d.name for d in used), n_fit_rows=per_epoch,
        )

    return fit


def _gather(Xs, starts, idx, out):
    s = np.searchsorted(starts, idx, side="right") - 1
    for u in np.unique(s):
        m = s == u
        out[m] = Xs[u][idx[m] - starts[u]]
    return out


def store_lines(store: ShiftStore, grid_data) -> list:
    """Report lines on the store: windows and labels against the grid."""
    pos = sum(int((w.labels == POSITIVE).sum()) for w in store.sessions.values())
    neg = sum(int((w.labels == NEGATIVE).sum()) for w in store.sessions.values())
    gpos = sum(int(((d.labels == POSITIVE) & d.feats.valid).sum()) for d in grid_data)
    gneg = sum(int(((d.labels == NEGATIVE) & d.feats.valid).sum()) for d in grid_data)
    return [
        f"- training windows: {store.spec.describe()}",
        f"- usable shifted windows: {pos} positive / {neg} negative "
        f"(0.25 s grid: {gpos} / {gneg}); pos_weight {neg / pos:.2f} "
        f"(grid {gneg / gpos:.2f})" if pos and gpos else "- usable shifted windows: -",
    ]


# --- scoring ---


def torch_prob(models, X) -> np.ndarray:
    """Mean probability of the models [(params, mean, std)] over X
    (n, 98, 40), by PyTorch's forward of the same network (agrees with
    features_np.cnn_forward to float rounding)."""
    import compare

    torch = compare._torch_setup(0)
    X = np.asarray(X)
    out = np.zeros(len(X))
    for params, mean, std in models:
        net = compare._make_net(torch)
        net.load_state_dict({k: torch.from_numpy(np.asarray(params[k])) for k in CNN_SHAPES})
        net.eval()
        p = np.zeros(len(X))
        with torch.no_grad():
            for k in range(0, len(X), SCORE_BATCH):
                Z = ((X[k : k + SCORE_BATCH] - mean) / std).astype(np.float32)
                p[k : k + SCORE_BATCH] = torch.sigmoid(net(torch.from_numpy(Z))).numpy()
        out += p
    return out / len(models)


def npz_members(model: dict) -> list:
    a = model["arrays"]
    return [
        ({k: a[f"s{s}/{k}"] for k in CNN_SHAPES}, a[f"s{s}/mean"], a[f"s{s}/std"])
        for s in model["cfg"]["seeds"]
    ]


def cnn_members(models) -> list:
    return [(m.params, m.mean, m.std) for m in models]


# --- grid sensitivity ---


def swallow_grid(w: Windows, prob, swallows, cfg, t0_s, duration_s) -> dict:
    """Per-swallow and outside-swallow numbers of one fine-grid stream w
    (k = PHASES) with probabilities prob."""
    if w.k != PHASES:
        raise ValueError(f"the grid check needs k = {PHASES}")
    prob = np.asarray(prob, np.float64)
    c = w.t_start_s + WINDOW_S / 2
    enter = cfg["enter"]
    found = np.zeros((len(swallows), PHASES), bool)
    fpm = []
    for p in range(PHASES):
        s = w.phase == p
        t, pp, v = w.t_start_s[s], prob[s], w.valid[s]
        if len(t) == 0:
            continue
        mask = config_mask(cfg, pp, v, t)
        cm = c[s][mask]
        for i, ts in enumerate(swallows):
            found[i, p] = bool(np.any((cm >= ts - evaluate.PRE_S) & (cm <= ts + evaluate.POST_S)))
        em = evaluate.event_metrics(list(swallows), t[mask].tolist(), duration_s, t0_s)
        fpm.append((em["false_positive_runs"], em["non_swallow_min"]))
    per = []
    outside = w.valid.copy()
    for i, ts in enumerate(swallows):
        inside = (c >= ts - evaluate.PRE_S) & (c <= ts + evaluate.POST_S)
        outside &= ~inside
        q = prob[inside & w.valid]
        per.append({
            "max": float(q.max()) if len(q) else float("nan"),
            "span_ms": float(np.sum(q >= enter)) * FINE_S * 1000.0,
            "phases": int(found[i].sum()),
        })
    return {
        "swallows": per,
        "out_windows": int(outside.sum()),
        "out_enter": int(np.sum(prob[outside] >= enter)),
        "fp_runs": sum(a for a, _ in fpm),
        "fp_min": sum(b for _, b in fpm),  # summed over the phases
    }


def aggregate(results: Sequence[dict]) -> dict:
    per = [s for r in results for s in r["swallows"]]
    span = np.array([s["span_ms"] for s in per])
    ph = np.array([s["phases"] for s in per])
    mx = np.array([s["max"] for s in per])
    ow = sum(r["out_windows"] for r in results)
    oe = sum(r["out_enter"] for r in results)
    fr = sum(r["fp_runs"] for r in results)
    fm = sum(r["fp_min"] for r in results)
    nan = float("nan")
    return {
        "n": len(per),
        "span_median": float(np.median(span)) if len(span) else nan,
        "span_p10": float(np.percentile(span, 10)) if len(span) else nan,
        "span_short": float(np.mean(span < SPAN_SHORT_MS)) if len(span) else nan,
        "all_phases": float(np.mean(ph == PHASES)) if len(ph) else nan,
        "phase_rate": float(ph.sum() / (PHASES * len(ph))) if len(ph) else nan,
        "max_median": float(np.nanmedian(mx)) if len(mx) else nan,
        "out_frac": oe / ow if ow else nan,
        "fp_per_min": fr / fm if fm > 0 else nan,
    }


AGG_HEAD = [
    "| model | sessions | swallows | span >= 0.85: median / p10 (ms) | span < 250 ms "
    "| found in all 10 phases | detection over phases | median max p "
    "| outside: windows >= 0.85 | false / min over phases |",
    "|---|---|---|---|---|---|---|---|---|---|",
]


def agg_line(label: str, what: str, a: dict) -> str:
    return (
        f"| {label} | {what} | {a['n']} | {a['span_median']:.0f} / {a['span_p10']:.0f} | "
        f"{a['span_short']:.3f} | {a['all_phases']:.3f} | {a['phase_rate']:.3f} | "
        f"{a['max_median']:.3f} | {a['out_frac']:.4f} | {a['fp_per_min']:.2f} |"
    )


def training_grid(root: Path, sessions, members_of, cfg, feature_set,
                  windows_fn=None, events_fn=None, span_fn=None) -> dict:
    """swallow_grid of each training-side session in sessions, scored by
    members_of(name) (a list of (params, mean, std)). Returns name -> result."""
    windows_fn = windows_fn or features._throat_windows
    events_fn = events_fn or evaluate.load_events
    span_fn = span_fn or evaluate.throat_session_span
    out = {}
    for n in sessions:
        p = Path(root) / n
        x, sample_t, _, _, _, t_lo, t_hi = windows_fn(p)
        sw = events_fn(p)
        w = session_windows(x, sample_t, t_lo, t_hi, PHASES, sw, feature_set, n)
        del x
        prob = torch_prob(members_of(n), w.X)
        t0, dur = span_fn(p)
        out[n] = swallow_grid(w, prob, sw, cfg, t0, dur)
    return out


def training_lines(label: str, per_session: dict, is_meal) -> list:
    allr = list(per_session.values())
    meals = [r for n, r in per_session.items() if is_meal(n)]
    conds = [r for n, r in per_session.items() if not is_meal(n)]
    return [
        agg_line(label, f"all ({len(allr)})", aggregate(allr)),
        agg_line(label, f"meals ({len(meals)})", aggregate(meals)),
        agg_line(label, f"conditions ({len(conds)})", aggregate(conds)),
    ]


def cv_grid_lines(root: Path, feature_set: str, fold_models: dict, shift=None,
                  **kw) -> list:
    """Report lines of --grid-cv: each fold's models (mean of the seeds) over
    that fold's validation sessions (m4_variant.cv's fold_models)."""
    import m4_compare as mc
    import m4_variant

    cfg = mc.parse_config(m4_variant.variant_config(feature_set))
    members, order = {}, []
    for g, f in fold_models.items():
        mem = cnn_members(f["models"][s] for s in sorted(f["models"]))
        for n in f["val"]:
            members[n] = mem
            order.append(n)
    per = training_grid(root, order, members.__getitem__, cfg, feature_set, **kw)
    label = shift.name if shift is not None else "grid (plain)"
    return [
        "## Grid sensitivity, cross-validated (each fold's models on its "
        "validation sessions; window positions every 25 ms)",
        "",
        "- span: positions whose window centre is in [t - 0.5, t + 1.5] with "
        "p >= 0.85, times 25 ms; phases: the 10 offsets of the 0.25 s grid in "
        "which the post-processing finds the swallow",
        "",
        *AGG_HEAD,
        *training_lines(label, per, mc.is_meal),
    ]


def recording_grid(spec, members, cfg, feature_set) -> tuple:
    """(swallow_grid result, touches [(a, b)], indications in touches per
    phase) of one whole AudioStudio recording."""
    import m4_variant

    spec = studio.parse_spec(spec) if not isinstance(spec, studio.Spec) else spec
    if spec.a_s is not None or spec.b_s is not None:
        raise SystemExit(f"--grid takes whole recordings (no @A:B): {spec.name}")
    wav, marks = studio.find_files(spec.path)
    x = studio.read_wav(wav)
    sw = studio.review(studio.read_marks(marks))["swallows"]
    dur = len(x) / studio.SR
    sw = [t for t in sw if 0.0 <= t <= dur]
    w = recording_windows(x, PHASES, sw, feature_set, spec.name)
    touches = m4_variant.sync_touches(x)
    del x
    prob = torch_prob(members, w.X)
    res = swallow_grid(w, prob, sw, cfg, 0.0, dur)
    c = w.t_start_s + WINDOW_S / 2
    in_touch, res["rises_all"] = [], []
    for p in range(PHASES):
        s = w.phase == p
        m = config_mask(cfg, prob[s], w.valid[s], w.t_start_s[s])
        rise = m & ~np.concatenate(([False], m[:-1])) if len(m) else m
        cc = c[s][rise]
        in_touch.append(int(sum(np.sum((cc >= a - 0.5) & (cc < b + 1.0)) for a, b in touches)))
        res["rises_all"].append(int(rise.sum()))
    return res, touches, in_touch


def recording_lines(label: str, spec, res, touches, in_touch) -> list:
    name = Path(str(spec).partition("@")[0]).name
    sw = "; ".join(
        f"{s['max']:.2f} / {s['span_ms']:.0f} / {s['phases']}" for s in res["swallows"]
    ) or "-"
    touch_s = sum(b - a for a, b in touches)
    return [
        f"| {label} | {name} | {sw} | {res['out_enter'] / max(res['out_windows'], 1):.4f} | "
        f"{np.mean(res['rises_all']):.1f} | {len(touches)} ({touch_s:.0f} s) | "
        f"{np.mean(in_touch):.1f} (range {min(in_touch)}-{max(in_touch)}) |"
    ]


REC_HEAD = [
    "| model | recording | per swallow: max p / span >= 0.85 (ms) / phases found of 10 "
    "| outside swallows: windows >= 0.85 | indications (mean over phases) "
    "| touches (sync_touches) | indications in touches (mean over phases) |",
    "|---|---|---|---|---|---|---|",
]


def grid_models(root: Path, model_paths, specs, train_side: bool, list_train,
                is_meal) -> list:
    """Report lines of --grid: each model over the recordings (whole) and,
    with train_side, over the training-side sessions (in-sample for a model
    trained on them)."""
    from m4_infer import load_model

    models = []
    for p in model_paths:
        m = load_model(Path(p))
        if m["cfg"]["kind"] != "cnn" or m["cfg"]["set"] not in features_v2.CNN_INPUT_SETS:
            raise SystemExit(f"not a CNN model: {p}")
        models.append((Path(p), m))
    lines = [
        "## Grid sensitivity (window positions every 25 ms)",
        "",
        "- span: positions whose window centre is in [t - 0.5, t + 1.5] with "
        "p >= 0.85, times 25 ms; phases: the 10 offsets of the 0.25 s grid in "
        "which the post-processing finds the swallow",
        "- probabilities by PyTorch's forward of the saved weights (float rounding "
        "against m4_infer.model_prob)",
    ]
    if specs:
        lines += ["", "### AudioStudio recordings (whole)", "", *REC_HEAD]
        for p, m in models:
            for s in specs:
                res, touches, it = recording_grid(s, npz_members(m), m["cfg"], m["cfg"]["set"])
                lines += recording_lines(p.name, s, res, touches, it)
    if train_side:
        names = list_train(root).train
        lines += [
            "",
            f"### Training side ({len(names)} sessions; in-sample for a model "
            "trained on them)",
            "",
            *AGG_HEAD,
        ]
        for p, m in models:
            mem = npz_members(m)
            per = training_grid(root, names, lambda n: mem, m["cfg"], m["cfg"]["set"])
            lines += training_lines(p.name, per, is_meal)
    return lines
