# model/

Training and evaluation tools of the throat-microphone swallowing indication
detector "M4" (a small CNN on 40 log-mel bands, 0-4000 Hz, of 1.0 s windows
every 0.25 s; three seeds averaged; hysteresis enter 0.85 / exit 0.65 and a
0.5 s refractory period), with the recordings it was trained on. The
detector's output is an *indication* of swallowing for the user's own notes.
It is not a medical device and makes no judgement or diagnosis.

The files come from the author's repository zaitaku-mealpace; a few
scripts were extended here for the experimental feature variants below
(`ORIGIN.md` lists the edited and added files). The copied comments,
messages and decision records are in Japanese.

## Layout

| Path | Contents |
|---|---|
| `analysis/` | the scripts, their tests, the mel matrices, `models/m4-0038.npz` (the M4 model), `models/m4-f200.npz`, `models/m4-f500.npz`, `models/m4-ms.npz` and `models/m4-heater-w10.npz` (experimental variants) |
| `tools/throat_check.py` | reading and checking a throat-microphone session (used by `analysis/features.py`) |
| `docs/` | evaluation rules (`evaluation.md`), session format (`data-schema.md`), recording protocol, the decision records the code checks or implements (`decisions/`), the markers of the one-time evaluation-side scoring (`log/`) |
| `data/` | the recordings (not in Git) |
| `.venv/` | the Python environment (not in Git) |

The scripts take the repository root as the parent of `analysis/` (this
directory), so run them from `model/`.

## Data

`data/raw/<yyyymmdd>-<HHMMSS>_<subject>_<cond>/` is one session
(`docs/data-schema.md`). The throat-microphone sessions (`meta.json` `fw`
= `pc-throat`) hold `throat.wav` (48 kHz, 2 ch, 16 bit; the left channel is
used, decimated to 16 kHz), `throat_chunks.csv` (sample index and PC time of
each block), `events.csv` (`t_ms,label,note`: `o start` begins the scored
range, `s` is a swallow marker; `t`, `c`, `n` other markers) and
`meta.json`. `cond` is `meal` or one of the condition sessions (`quiet`,
`water`, `saliva`, `talk`, `cough`, `neck`). Earlier sessions of other
milestones (IMU, detector firmware) are in the same directory; the M4 tools
skip them. `data/sample/` is a 10 s example of the format.

M4 training side (`analysis/fixed_eval.py` `M4_EXPECTED_TRAIN`, decision
0037): the 21 condition sessions of 2026-10-11/12 and the 5 meals of
2026-10-25 to 10-28. Evaluation side (`M4_EXPECTED_EVAL`): the 4 meals of
2026-10-30 to 11-01.

## Environment

```
python3 -m venv model/.venv
model/.venv/bin/pip install --index-url https://download.pytorch.org/whl/cpu torch==2.14.1+cpu
model/.venv/bin/pip install numpy==2.5.3 scipy==1.18.1 scikit-learn==1.9.1 librosa==1.0.0 matplotlib==3.11.2
```

Use `-B`, so no `__pycache__` is written beside the scripts (one test
starts a child interpreter with `-I`, which still writes
`analysis/__pycache__/`; it is ignored by Git).

## Tests

```
cd model
.venv/bin/python -B -m unittest discover -s analysis   # 416 tests, about 2 minutes
.venv/bin/python -B -m unittest discover -s tools      # 10 tests
```

The tests use synthetic data in temporary directories, not `data/`.

## Training and evaluation

```
cd model
.venv/bin/python -B analysis/m4_compare.py data/raw [--figures docs/log/<dir>]
.venv/bin/python -B analysis/m4_compare.py data/raw --final-model analysis/models/<name>.npz
```

- The first command compares 25 candidate rows (classifier C1-C5 x
  post-processing P0-P4) by cross-validation on the training side only
  (folds by collection day) and prints a Markdown report. It takes about
  2.5 hours of CPU (81 CNN trainings).
- `--final-model` trains the configuration `fixed_eval.M4_CONFIG` (which
  must equal the `M4-CONFIG:` line of `docs/decisions/0038`) on all training
  sessions, saves it, reads it back and prints its sha256. It refuses an
  existing output file. About 16 minutes (41 minutes of CPU, 4.7 GiB of
  memory); with the same package versions it reproduces
  `analysis/models/m4-0038.npz` bit for bit.
- Both refuse to start when a tracked file of the Git repository has an
  uncommitted change (the repository is the whole xiao-esp32s3 tree), so
  that the report's commit reproduces it.
- `--smoke` (the dry run) only works while `M4_EXPECTED_TRAIN` is `None`.
- `analysis/m4_final_eval.py data/raw` scores evaluation-side sessions
  recorded by the source's device (`detect.csv`); it stops here because the
  marker `docs/log/m4-final-49.txt` exists.

The training labels: a window is positive when its centre is in
`[t, t + 1.0]` after a swallow marker `t`, not used in the 0.5 s around
that, negative otherwise (decision 0014). Counting: a swallow is found when
a positive window's centre is in `[t - 0.5, t + 1.5]`; false indications
per minute are runs of positive windows outside those intervals per minute
of the rest (`docs/evaluation.md`, decision 0011).

## Experimental feature variants

The CNN feature set names the lower edge of its 40 mel bands
(`features_np.CNN_FMIN_HZ`): `mfe40` is 0-4000 Hz (the M4 set),
`mfe40f200` is 200-4000 Hz and `mfe40f500` 500-4000 Hz. Framing,
pre-emphasis, window, FFT, HTK scale, 40 bands, the upper edge 4000 Hz and
the log are the same; only the mel matrix differs
(`analysis/mel40_htk_<low>_4000.npy`, built by
`make_features_np_consts.py`). A model's `config` string names its set
(`set=mfe40f200 ...`), and `m4_infer` (`Detector`, `model_prob` callers)
computes that set's features.

`analysis/m4_variant.py` runs a variant with the otherwise identical 0038
configuration (`classifier=cnn seeds=0,1,2 hard_negative=none post=hyst
enter=0.85 exit=0.65 cap_s=none refractory_s=0.5`) on the training-side
sessions only (`M4_EXPECTED_TRAIN`; the evaluation side is not opened). It
is not the recorded M4 result: `fixed_eval.M4_CONFIG`, the 0037 / 0038
lines, `m4_compare.py --final-model` and `m4_final_eval.py` still take only
`set=mfe40`.

```
cd model
.venv/bin/python -B analysis/m4_variant.py data/raw --set mfe40f200 --cv
.venv/bin/python -B analysis/m4_variant.py data/raw --set mfe40f200 --final-model analysis/models/m4-f200.npz
.venv/bin/python -B analysis/m4_variant.py data/raw --hum analysis/models/m4-0038.npz analysis/models/m4-f200.npz analysis/models/m4-f500.npz
```

- `--cv`: the cross-validation of the candidate C4 x P2 of `m4_compare.py`
  (folds by collection day, 18 CNN trainings, about an hour), with the
  numbers at the configuration's operating point and at the point the rule
  picks from the P2 grid. `--set mfe40 --cv` repeats the recorded C4 x P2
  numbers.
- `--final-model`: trains on all training sessions, saves, reads back and
  prints the sha256 (about 16 minutes); refuses an existing file and the set
  `mfe40` (that is `m4_compare.py --final-model`).
- `--hum`: each model over the training-side `quiet` sessions as recorded,
  with the 50 Hz line at -7 dB, and with the hum lines 50-450 Hz removed
  (FFT of the whole 16 kHz signal, +-3 Hz around each multiple of 50 Hz);
  prints the median probability and the fraction of windows at or above
  0.85. The edited signals stay in memory. The quiet sessions are in every
  model's training data.
- `--allow-dirty` runs although tracked files have uncommitted changes; the
  report says so.

### AudioStudio recordings

`analysis/studio.py` reads an AudioStudio recording (16 kHz mono 16-bit
WAV and its `<name>.marks.csv`) as a session. Copy the recording's two
files into `data/studio/<name>/` (not in Git). Labels follow the
application's reading of the marks (`MarkReview.cs`): an indication without
a `false` mark, and every `swallow` mark, is a swallow (a `swallow` mark
near a hit is the same swallow); the window labels are those of a
throat-microphone session. Windows are 1.0 s every 0.25 s from the start of
the range, all inside it; the features come from the 16 kHz samples (no
48 kHz step). A spec `DIR@A:B` takes only the seconds `[A, B)`.

```
cd model
R=data/studio/<name>
.venv/bin/python -B analysis/m4_variant.py data/raw --models analysis/models/m4-0038.npz --test $R
.venv/bin/python -B analysis/m4_variant.py data/raw --set mfe40 --cv --extra $R@0:42 --extra-weight 10 --test $R@42:
.venv/bin/python -B analysis/m4_variant.py data/raw --set mfe40 --final-model analysis/models/<name>.npz --extra $R --extra-weight 10 --test $R
```

- `--extra SPEC` (repeatable) adds a recording to the training side of
  every cross-validation fold and of the final model; the folds stay those
  of the training-side sessions, and an extra recording is never a
  validation session. `--extra-weight W` puts the loss weight `W` on its
  windows (the `sample_weight` path of `hard_negative=w3.0`; the positive
  weight still counts windows). With `--extra`, `--set mfe40 --final-model`
  is allowed: the model's config is the 0038 line and loads with the
  unchanged inference code and AudioStudio, but it is not the recorded M4
  model.
- `--test SPEC` (repeatable) scores recordings: with `--cv` by each fold's
  models (mean of the seeds), with `--final-model` by the saved model, and
  with `--models M.npz ...` by the given models without training. It prints
  per recording the windows, the indications (rising edges of the
  post-processing output), the fraction of windows at or above 0.85, and the
  median probability inside and outside the intervals `[t, t + 7 s)` of the
  indications marked `false`.

### Front-end set mfe40ms

`mfe40ms` is `analysis/mains_spike.py` on the 16 kHz signal, then `mfe40`.
The front-end limits short bursts in the band above about 450 Hz that come
back at the same place of every mains period (seen with a hand on or near
an electric heater), and leaves sound that is not synchronous with the
mains alone: at a burst phase it limits the signal to twice a reference
level, but takes away no more than the burst its template expects there,
so a swallow that falls on a burst phase keeps what it has above the
expected burst. It is causal and runs from the first sample of the signal (of
a session, of a recording, of a stream); the output is the input delayed
by 80 samples (5 ms) wherever it does not act. The module docstring gives
the algorithm and constants; `MainsSpike` is the per-sample reference for
ports, `apply` the same over a whole signal. A model of this set names
`set=mfe40ms` in its config; `m4_infer.model_prob` scores its features,
`Detector` (which takes windows) and AudioStudio reject it, since the
front-end has to run on the stream before the windows are cut. AudioStudio
has the front-end as its **Spike filter** switch (a C# port of
`MainsSpike`, checked against it) before the `mfe40` models it loads.

```
.venv/bin/python -B analysis/m4_variant.py data/raw --set mfe40ms --cv --final-model analysis/models/m4-ms.npz --test $R
.venv/bin/python -B analysis/m4_variant.py data/raw --front analysis/models/m4-0038.npz --test $R [--test ...] \
    [--front-param current] [--front-param ALPHA=1/16 ...]
```

`analysis/models/m4-ms.npz` was trained with earlier constants of the
front-end (template rate `ALPHA = 1/16`, now `1/8`, and the limit without
the bound by the expected burst, `SPREAD`); it was not retrained, so its
training input differs from what `mfe40ms` now computes.

`--front M.npz` (an `mfe40` model, no training) scores the model on its own
and behind the front-end, once per `--front-param` case (`NAME=VALUE,...`
of the constants `ALPHA`, `MARGIN`, `LIMIT`, `SPREAD`, `WARMUP`, `QUANT_STEP`,
`FLOOR_GAIN2`, `ENV_TAU`, `PERIOD_RATE`, values as numbers or fractions;
`current` or none: the module's constants), and prints:

- per touch of each `--test` recording: the time from the touch's start
  until the probability stays below 0.85 and below 0.5 (window centres),
  the indications from 0.5 s before the touch to 1 s after it, and the
  fraction of the touch's windows at or above 0.85. The touches are found
  offline (`sync_touches`, non-causal, only to measure the causal
  front-end): the 500-4000 Hz envelope folded over 12 mains periods every
  5 ms, the per-bin median over the periods, its top 10 % against its lower
  half above 11 dB, runs of at least 2 s;
- the probability at each swallow of each recording (the highest in its
  counting interval) and the swallows found. The recording's windows start
  at its sample 0, every 0.25 s; the front-end's output is read 80 samples
  late, as AudioStudio reads it, so a window holds the output for the raw
  samples of the same window. A short or light swallow's probability
  depends on where the 0.25 s grid falls (on AudioStudio's live stream the
  grid starts with the stream, not with the recording): judge it over
  several grid offsets, not one;
- the training-side swallow check, on the `water` and `saliva` condition
  sessions and the first meal (10 sessions, 162 swallows; in the model's
  training data): swallows found, false indications, the mean probability
  of the positive windows, and the change of their 500-4000 Hz energy by
  the front-end (mean and lowest, dB). About 1 minute per case.

## The evaluation side

The M4 evaluation side has been scored once (2026-11-01, the marker in
`docs/log/`). Do not use those sessions, or any session dated after
`M4_TRAIN_CUTOFF` (2026-10-28), for training, threshold or configuration
choice: `m4_compare.py` does not open them (not even `meta.json`). Choose
anything new by cross-validation on the training side, and score a new
held-out set, fixed before it is looked at, only once.

## Privacy

The recordings are the user's own meal and speech audio. Keep them only in
`model/data/`. Do not print, plot, upload or summarise their audio content
or samples; the scripts print only aggregate numbers (counts, durations,
rates). The figures of `--figures` draw meal sessions only as window-level
summaries. No tool here uses the network.
