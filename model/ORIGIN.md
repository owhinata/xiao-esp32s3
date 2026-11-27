# Origin of model/

Everything under `model/` except this file and `README.md` comes from the
author's own repository **zaitaku-mealpace** (MIT License, same author,
Copyright (c) 2026 k.ouwa), commit `c5628c9`. Only the throat-microphone /
M4 path (the swallowing indication detector: a small CNN on 40 log-mel bands)
was taken.

The directory layout of the source is kept (`analysis/`, `tools/`, `docs/`,
`data/`), because the scripts find the repository root as
`Path(__file__).resolve().parent.parent` and refer to `docs/decisions/...`,
`docs/log/...`, `analysis/models/...` and `tools/throat_check.py` relative to
it. With the same layout, no file needed an edit to run.

All files were copied with `cp -p` (code, constants, documents) or `cp -a`
(data). The scripts are now this repository's own code: the files listed
under "Edited" below were changed here, and the files under "Added" are new;
every other file is an unchanged copy.

## Edited (the experimental CNN feature sets)

The lower edge of the 40 mel bands of the CNN input became a parameter of
the feature set: `mfe40` (0-4000 Hz, the recorded M4 set, unchanged) and the
experimental `mfe40f200` (200-4000 Hz) and `mfe40f500` (500-4000 Hz), which
leave the mains hum below their lower edge out. The recorded M4 configuration, its guards
(`fixed_eval.py`, the 0037 / 0038 lines) and `m4-0038.npz` are unchanged.

| File | Change |
|---|---|
| `analysis/features_np.py` | `CNN_FMIN_HZ` / `CNN_SETS` (set -> lower edge), `mel40_path`, `cnn_mel` (the variants' matrices are read on first use), `mfe(seg, set)`; `mfe40` is `mfe(seg, "mfe40")` with the same values |
| `analysis/features_v2.py` | `CNN_SETS`; `mfe_one` and `extract_mfe` take the set (default `mfe40`); `mfe_windows` (the CNN input of an in-memory signal, the body of `extract_mfe`) |
| `analysis/compare.py` | `_extract_fn` reads any of `CNN_SETS` with `extract_mfe` |
| `analysis/m4_infer.py` | `CNN_SETS`; `parse_config` accepts `set=` any of them for `classifier=cnn`; `Detector` computes the features of the model's set (FEAT, `feat=True`, stays mfe40-only) |
| `analysis/m4_compare.py` | `load_all` takes `cnn_set`; the body of `final_model` after its checks is `train_final(root, out, config, ...)` (used by `m4_variant.py`); the read-back message no longer names `M4_CONFIG` |
| `analysis/make_features_np_consts.py` | `MEL_SPECS` holds the lower edge; it also builds and checks the two variant matrices |
| `analysis/test_features_np.py` | `ref_mel` takes the lower edge; `MelTest.test_variant_sets` |
| `analysis/test_m4_infer.py` | `DetectorTest.test_variant_sets_use_their_mel_matrix` |

## Added

| File | What |
|---|---|
| `analysis/m4_variant.py` | The experimental entry point: the C4 x P2 cross-validation (`--cv`) and the final model (`--final-model`) of a variant with the otherwise identical 0038 configuration on the training side only, and the hum sensitivity check (`--hum`); `--allow-dirty` |
| `analysis/test_m4_variant.py` | its tests (synthetic data) |
| `analysis/mel40_htk_200_4000.npy`, `analysis/mel40_htk_500_4000.npy` | the variants' mel matrices (`make_features_np_consts.py`) |
| `analysis/models/m4-f200.npz`, `analysis/models/m4-f500.npz` | the variants' final models (sha256 below) |

## Code, constants, model, tests (copied; edited ones above)

`analysis/`:

| File | Role |
|---|---|
| `m4_compare.py` | M4 candidate comparison, training-side cross-validation, hard negatives, final model (`--final-model`) |
| `m4_final_eval.py` | one-time scoring of the M4 evaluation side from device sessions (`detect.csv`) |
| `m4_infer.py` | M4 inference shared by the device and the PC (`load_model`, `model_prob`, `Detector`) |
| `compare.py` | CNN training (`cnn_fit`, PyTorch), `load_train`; imported by `m4_compare.py` |
| `visualize.py` | figures (`m4_compare.py --figures`) |
| `train_eval.py` | labels, sessions, cross-validation groups, logistic regression, git checks |
| `evaluate.py` | event counting (`docs/evaluation.md`, decision 0011) |
| `evaluate_detector.py` | detector-session helpers used by `m4_final_eval.py` |
| `fixed_eval.py` | fixed training / evaluation lists and the M4 configuration constants |
| `postprocess.py` | threshold, hysteresis, refractory period, run cap (`PostStepper`) |
| `split.py` | session split checks |
| `features.py` | window grid and throat-microphone session reading (also M1 IMU features) |
| `features_v2.py` | throat-microphone feature sets (`mfe40`, `a52`, ...) via librosa |
| `features_np.py` | numpy-only decimation, `mfe40`, `a52`, CNN forward pass |
| `led_rule.py` | LED state rule (`LedStepper`) |
| `make_features_np_consts.py` | regenerates / checks the mel matrices and `features_np_ref.npz` |
| `mel40_htk_0_4000.npy`, `mel26_htk_0_4000.npy` | mel filter banks (HTK, 0-4000 Hz) |
| `features_np_ref.npz` | reference values for `test_features_np.py` (synthetic input, no audio) |
| `models/m4-0038.npz` | the M4 model (sha256 `c786086f069c55c55b27eea860963c3c950c04b728fe65d52bc35a7a4a806d90`) |
| `requirements-cnn.txt` | torch version |
| `test_m4_compare.py`, `test_m4_final_eval.py`, `test_m4_infer.py`, `test_features_np.py`, `test_features_v2.py`, `test_features.py`, `test_evaluate.py`, `test_evaluate_detector.py`, `test_split.py`, `test_train_eval.py`, `test_fixed_eval.py`, `test_postprocess.py`, `test_led_rule.py`, `test_throat_compare.py`, `test_visualize.py` | tests |

`tools/`: `throat_check.py` (reads a throat-microphone session; loaded by
`features.py`), `test_throat_check.py`, `requirements.txt` (the source's
package list; `pyserial` there is only for the source's recorder).

Not taken (used only by earlier milestones or the source's device):
`ei_upload.py`, `ei_testing_result.py`, `features_m2.py`, `m2_*`,
`final_eval.py` (M3), `make_runtime_models.py`, `cnn_rand0.*`,
`requirements-runtimes.txt`, `requirements-convert-litert.txt`, their tests,
`analysis/README.md`, `tools/record.py` and the other recorder tools,
`device/`.

## Documents (unchanged)

`docs/`: `evaluation.md`, `data-schema.md`, `recording-protocol.md`.

`docs/decisions/`: 0011 (evaluation interpretation), 0012 (feature
definitions), 0014 (training procedure and labels), 0018 (LED rule), 0021
(detector session span), 0027 (M2 numbers, read by `test_m4_final_eval.py`),
0029 (throat session format and clock), 0031, 0032, 0033, 0034 (read by
`test_fixed_eval.py`), 0036 (M3 numbers, read by `test_m4_final_eval.py`),
0037 (M4 split), 0038 (M4 configuration, `M4-CONFIG` / `M4-MODEL` lines
checked by the code), 0041 (M4 evaluation procedure), 0042 (M4 gate result).

`docs/log/`: `m3-final-40.txt` and `m4-final-49.txt`, the markers that
record that the M3 and M4 evaluation sides were scored once. With the M4
marker present, `m4_final_eval.py` stops before opening any evaluation-side
file, as in the source.

The code and documents still mention files that were not copied
(`docs/plan.md`, `docs/log/<date>.md`, `device/unoq/...`,
`tools/record.py`, `analysis/README.md`). Those references are left as they
are.

## Data (unchanged, not in Git)

`data/raw/` (78 session directories and `trial-throat`, 393 files,
2,471,022,511 bytes), `data/sample/` (6 files) and `data/README.md`, copied
with `cp -a` / `cp -p`. `/model/data/` is ignored by Git.
