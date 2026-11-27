#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes the test fixtures of the M4 swallow detector from its Python code.

Run with the interpreter of the zaitaku-mealpace checkout (numpy only):

    <mealpace>/.venv/bin/python tools/make_m4_fixtures.py \
        --mealpace <mealpace> --out tests/AudioStudio.Core.Tests/Fixtures

It imports features_np, m4_infer, postprocess and led_rule from
<mealpace>/analysis and reads the weights analysis/models/m4-0038.npz. It
writes nothing into that checkout (no bytecode either). All signals are
synthetic: formulas and a fixed-seed generator, written out as the samples
the C# tests read.

Files written to --out:
  m4_reference.npz  reference_signal() (float64), its mfe40, the probability
                    of each seed and of the ensemble.
  m4_windows.npz    synthetic int16 windows, their mfe40 and probabilities.
  m4_stream.npz     a 30 s int16 signal and Detector.step's probability,
                    positive and LED of every window; and the same signal
                    with some windows marked invalid, through model_prob,
                    PostStepper and LedStepper.
  m4_post.npz       probability sequences through PostStepper with several
                    settings, and window sequences through LedStepper.
  m4_configs.json   configuration strings and what parse_config makes of them.
"""

import argparse
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True

SR = 16000
WINDOW = 16000
HOP = 4000


def load(mealpace: Path):
    sys.path.insert(0, str(mealpace / "analysis"))
    import numpy as np
    import features_np
    import led_rule
    import m4_infer
    import postprocess

    return np, features_np, m4_infer, postprocess, led_rule


def to_int16(np, x):
    return np.clip(np.round(np.asarray(x) * 32768.0), -32768, 32767).astype(np.int16)


def seed_probs(np, m4, model, M):
    """The probability of each seed alone, and of the ensemble."""
    cfg = model["cfg"]
    out = []
    for s in cfg["seeds"]:
        single = dict(model)
        single["cfg"] = dict(cfg, seeds=(s,))
        out.append(float(m4.model_prob(single, np.asarray(M)[None])[0]))
    return out, float(m4.model_prob(model, np.asarray(M)[None])[0])


def event(np, rng, n_total):
    """A synthetic burst: a chirp, filtered noise or clicks under an envelope."""
    x = np.zeros(n_total)
    n = int(SR * rng.uniform(0.05, 0.6))
    s = int(rng.integers(0, n_total - n))
    tt = np.arange(n) / SR
    kind = int(rng.integers(0, 3))
    if kind == 0:
        f0, f1 = 10 ** rng.uniform(1.5, 3.5), 10 ** rng.uniform(1.5, 3.5)
        b = np.sin(2 * np.pi * (f0 * tt + (f1 - f0) * tt * tt / (2 * tt[-1])))
    elif kind == 1:
        b = rng.standard_normal(n)
        k = int(rng.integers(1, 60))
        b = np.convolve(b, np.ones(k) / k, "same")
        b /= np.abs(b).max()
    else:
        b = np.zeros(n)
        b[:: int(rng.integers(20, 800))] = 1.0
    env = np.hanning(n) ** rng.uniform(0.3, 3.0)
    x[s : s + n] = 10 ** rng.uniform(-3, -0.3) * env * b
    return x


def find_events(np, fnp, det, rng, want):
    """1 s windows of bursts, picked by the probability bands in `want`
    ((low, high) -> count), in the order the generator finds them."""
    found = {band: [] for band in want}
    for _ in range(20000):
        x = event(np, rng, WINDOW)
        if rng.uniform() < 0.5:
            x = x + event(np, rng, WINDOW)
        q = to_int16(np, np.clip(x, -1, 1))
        p = det.prob(fnp.mfe40((q / 32768.0).astype(np.float32)))
        for band, n in want.items():
            if band[0] <= p < band[1] and len(found[band]) < n:
                found[band].append(q)
        if all(len(found[b]) == n for b, n in want.items()):
            break
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mealpace", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()
    mealpace = a.mealpace.resolve()
    out = a.out.resolve()
    if out == mealpace or mealpace in out.parents:
        ap.error("--out must not be inside the zaitaku-mealpace checkout")
    np, fnp, m4, post, led = load(mealpace)
    out.mkdir(parents=True, exist_ok=True)

    model = m4.load_model(mealpace / "analysis" / "models" / "m4-0038.npz")
    det = m4.Detector(model)
    cfg = model["cfg"]

    # (a) the reference signal of features_np.
    sig = fnp.reference_signal()
    M = fnp.mfe40(sig)
    seeds, p = seed_probs(np, m4, model, M)
    np.savez_compressed(
        out / "m4_reference.npz",
        signal=sig.astype(np.float64),
        mfe40=M,
        seed_prob=np.array(seeds, dtype=np.float64),
        prob=np.float64(p),
    )

    # (b) synthetic int16 windows.
    t = np.arange(WINDOW) / SR
    rng = np.random.default_rng(20261127)
    names, wins = [], []

    def add(name, x):
        names.append(name)
        wins.append(to_int16(np, np.clip(x, -1, 1)))

    add("zeros", np.zeros(WINDOW))
    add("sine 440 Hz -6 dBFS", 0.5 * np.sin(2 * np.pi * 440 * t))
    add("sines 150 + 2300 Hz", 0.2 * np.sin(2 * np.pi * 150 * t) + 0.05 * np.sin(2 * np.pi * 2300 * t + 1.0))
    add("chirp 50..4000 Hz", 0.3 * np.sin(2 * np.pi * (50 * t + (4000 - 50) * t * t / 2)))
    add("white noise -30 dBFS", 0.0316 * rng.standard_normal(WINDOW))
    add("quiet noise", 3e-4 * rng.standard_normal(WINDOW))
    add("full-scale square 100 Hz", np.sign(np.sin(2 * np.pi * 100 * t + 0.1)))
    bursts = find_events(np, fnp, det, rng, {(0.95, 1.01): 1, (0.65, 0.85): 1})
    add("burst (high)", bursts[(0.95, 1.01)][0] / 32768.0)
    add("burst (between exit and enter)", bursts[(0.65, 0.85)][0] / 32768.0)
    W = np.stack(wins)
    mfe, probs, sp = [], [], []
    for w in W:
        Mw = fnp.mfe40((w / 32768.0).astype(np.float32))
        s_, p_ = seed_probs(np, m4, model, Mw)
        mfe.append(Mw)
        probs.append(p_)
        sp.append(s_)
    np.savez_compressed(
        out / "m4_windows.npz",
        names=np.array(names),
        windows=W,
        mfe40=np.stack(mfe),
        prob=np.array(probs),
        seed_prob=np.array(sp),
    )

    # (c) a 30 s stream: digital silence, a stretch of quiet noise, and bursts.
    n = 30 * SR
    x = np.zeros(n)
    x[12 * SR : 20 * SR] = 2e-4 * rng.standard_normal(8 * SR)
    picks = find_events(np, fnp, det, rng, {(0.97, 1.01): 6, (0.6, 0.85): 3})
    places = {  # seconds -> burst: close pairs reach the refractory period
        2.0: picks[(0.97, 1.01)][0],
        3.0: picks[(0.97, 1.01)][1],
        7.0: picks[(0.6, 0.85)][0],
        9.0: picks[(0.97, 1.01)][2],
        9.5: picks[(0.6, 0.85)][1],
        14.0: picks[(0.97, 1.01)][3],
        18.0: picks[(0.97, 1.01)][4],
        18.75: picks[(0.6, 0.85)][2],
        24.0: picks[(0.97, 1.01)][5],
    }
    for at, q in places.items():
        i = int(at * SR)
        x[i : i + WINDOW] += q / 32768.0
    stream = to_int16(np, np.clip(x, -1, 1))
    k_n = 1 + (n - WINDOW) // HOP
    d = m4.Detector(model)
    stream_rows = []
    for k in range(k_n):
        seg = (stream[k * HOP : k * HOP + WINDOW] / 32768.0).astype(np.float32)
        p_, pos, led_, _ = d.step(seg, k * 250)
        stream_rows.append((p_, pos, led_))
    # The same windows with some invalid (as if samples were lost there).
    invalid = np.zeros(k_n, dtype=bool)
    for lo, hi in ((10, 14), (34, 35), (70, 76)):
        invalid[lo:hi] = True
    ps = post.PostStepper(cfg["enter"], cfg["exit"], cfg["cap_s"], cfg["refractory_s"])
    ls = led.LedStepper()
    pos_inv, led_inv = [], []
    for k in range(k_n):
        v = not invalid[k]
        q = ps.step(stream_rows[k][0], v, k * m4.HOP_S)
        pos_inv.append(int(q))
        led_inv.append(ls.step(k * 250, q))
    np.savez_compressed(
        out / "m4_stream.npz",
        signal=stream,
        prob=np.array([r[0] for r in stream_rows], dtype=np.float64),
        positive=np.array([r[1] for r in stream_rows], dtype=np.int64),
        led=np.array([r[2] for r in stream_rows], dtype=np.int64),
        invalid=invalid,
        positive_invalid=np.array(pos_inv, dtype=np.int64),
        led_invalid=np.array(led_inv, dtype=np.int64),
    )

    # (d) PostStepper and LedStepper on hand-made sequences.
    cases = [
        # name, enter, exit, cap_s, refractory_s, probabilities, valid
        ("enter and exit", 0.85, 0.65, None, None,
         [0.1, 0.84, 0.85, 0.7, 0.65, 0.64, 0.9, 0.2, 0.86, 0.66, 0.1], None),
        ("an invalid window ends a run and cannot start one", 0.85, 0.65, None, None,
         [0.9, 0.9, 0.9, 0.95, 0.95, 0.2, 0.99, 0.99], [1, 1, 0, 1, 1, 1, 0, 1]),
        ("refractory suppression", 0.85, 0.65, None, 0.5,
         [0.9, 0.9, 0.1, 0.9, 0.1, 0.1, 0.9, 0.2, 0.1, 0.1, 0.9, 0.9, 0.1], None),
        ("a suppressed run restarts the refractory period", 0.85, 0.65, None, 0.5,
         [0.9, 0.1, 0.9, 0.1, 0.9, 0.1, 0.1, 0.1, 0.9, 0.9], None),
        ("invalid windows inside the refractory period", 0.85, 0.65, None, 0.5,
         [0.9, 0.9, 0.9, 0.9, 0.2, 0.9, 0.9], [1, 0, 1, 1, 1, 0, 1]),
        ("cap of a run", 0.85, 0.65, 1.5, None,
         [0.9] * 9 + [0.1, 0.9, 0.9, 0.7, 0.66, 0.65, 0.9, 0.9, 0.5], None),
        ("cap and refractory", 0.85, 0.65, 2.0, 0.5,
         [0.9] * 10 + [0.1, 0.1, 0.9, 0.9, 0.1, 0.1, 0.1, 0.9] + [0.9] * 9, None),
        ("exit equal to enter", 0.5, 0.5, 2.5, 0.5,
         [0.5, 0.49, 0.5, 0.5, 0.1, 0.1, 0.1, 0.6, 0.6, 0.6] + [0.7] * 12, None),
    ]
    rng_post = np.random.default_rng(7)
    cases.append(("random", 0.85, 0.65, 2.0, 0.5,
                  list(rng_post.uniform(0, 1, 400)),
                  list((rng_post.uniform(0, 1, 400) > 0.05).astype(int))))
    cases.append(("random, the device's settings", 0.85, 0.65, None, 0.5,
                  list(rng_post.beta(0.3, 0.3, 400)),
                  list((rng_post.uniform(0, 1, 400) > 0.03).astype(int))))
    arrays = {"names": np.array([c[0] for c in cases])}
    for i, (_, enter, exit_, cap, refr, probs_, valid) in enumerate(cases):
        valid = [1] * len(probs_) if valid is None else valid
        st = post.PostStepper(enter, exit_, cap, refr)
        res = [st.step(p_, bool(v), k * post.HOP_S) for k, (p_, v) in enumerate(zip(probs_, valid))]
        nan = float("nan")
        arrays[f"c{i}/settings"] = np.array(
            [enter, exit_, nan if cap is None else cap, nan if refr is None else refr])
        arrays[f"c{i}/prob"] = np.array(probs_, dtype=np.float64)
        arrays[f"c{i}/valid"] = np.array(valid, dtype=bool)
        arrays[f"c{i}/positive"] = np.array(res, dtype=bool)
    led_cases = [
        ([0, 250, 500, 750, 1000, 1250, 1500, 1750, 2000, 2250, 2500], [0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0]),
        ([0, 250, 2000, 2001, 3750, 3751, 9000], [1, 0, 0, 0, 0, 0, 0]),
        ([100, 200, 300], [0, 0, 0]),
        ([0, 1750, 1751, 3501, 3502], [1, 0, 1, 0, 0]),
    ]
    for i, (w, p_) in enumerate(led_cases):
        st = led.LedStepper()
        arrays[f"led{i}/window_t_ms"] = np.array(w, dtype=np.int64)
        arrays[f"led{i}/positive"] = np.array(p_, dtype=np.int64)
        arrays[f"led{i}/led"] = np.array([st.step(a_, b_) for a_, b_ in zip(w, p_)], dtype=np.int64)
    np.savez_compressed(out / "m4_post.npz", **arrays)

    # Configuration strings.
    base = "set=mfe40 classifier=cnn seeds=0,1,2 hard_negative=none post=hyst threshold=- enter=0.85 exit=0.65 cap_s=none refractory_s=0.5"
    texts = [
        base,
        model["config"],
        base.replace("seeds=0,1,2", "seeds=0"),
        base.replace("cap_s=none", "cap_s=2.0"),
        base.replace("cap_s=none", "cap_s=1.5").replace("refractory_s=0.5", "refractory_s=none"),
        base.replace("hard_negative=none", "hard_negative=w3.0"),
        "set=mfe40 classifier=cnn seeds=0 hard_negative=none post=none threshold=0.5 enter=- exit=- cap_s=none refractory_s=none",
        "set=a52 classifier=lr seeds=- hard_negative=none post=hyst threshold=- enter=0.7 exit=0.4 cap_s=none refractory_s=none",
        base.replace("cap_s=none", "cap_s=3.0"),
        base.replace("refractory_s=0.5", "refractory_s=1.0"),
        base.replace("seeds=0,1,2", "seeds=0,1"),
        base.replace("set=mfe40", "set=a52"),
        base.replace("enter=0.85", "enter=x"),
        base.replace("threshold=-", "threshold=0.5"),
        "set=mfe40 classifier=cnn seeds=0 hard_negative=none post=none threshold=0.5 enter=0.8 exit=- cap_s=none refractory_s=none",
        "set=mfe40 classifier=cnn seeds=0 hard_negative=none post=none threshold=0.5 enter=- exit=- cap_s=2.0 refractory_s=none",
        base.replace(" refractory_s=0.5", ""),
        base.replace("set=mfe40 classifier=cnn", "classifier=cnn set=mfe40"),
        base.replace(" ", "  ", 1),
        base + " extra=1",
        base.replace("enter=0.85", "enter=0.85=1"),
    ]
    rows = []
    for text in texts:
        try:
            c = m4.parse_config(text)
        except ValueError:
            rows.append({"text": text, "ok": False})
            continue
        rows.append({
            "text": text,
            "ok": True,
            "set": c["set"],
            "kind": c["kind"],
            "seeds": list(c["seeds"]),
            "hard_negative": c["hard_negative"],
            "post": c["post"],
            "threshold": c["threshold"],
            "enter": c["enter"],
            "exit": c["exit"],
            "cap_s": c["cap_s"],
            "refractory_s": c["refractory_s"],
        })
    (out / "m4_configs.json").write_text(json.dumps(rows, indent=1) + "\n", encoding="utf-8")

    print(
        f"stream: {k_n} windows, {sum(pos_ok for _, pos_ok, _ in stream_rows)} positive, "
        f"{sum(pos_inv)} positive with the invalid windows; written to {out}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
