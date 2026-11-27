# Where AudioStudio comes from

Part of AudioStudio is taken from ImuStudio, the desktop application of
[rp2040-connect](https://github.com/owhinata/rp2040-connect)
(`host/ImuStudio/`, at commit `9a4426a`, the last commit that changed it):
the layout of the solution, the Telnet shell client and the finding of the
board, the settings file, the spectrogram picture, the window's layout with
its shell pane, and the test setup. Its IMU-specific parts (MQTT, attitude,
calibration, position, the 3D rendering project) are not here.

The files of ImuStudio say `SPDX-License-Identifier: Apache-2.0`. Their
author, who is also this repository's, has put the copies here under this
repository's MIT License, so every file of AudioStudio says
`SPDX-License-Identifier: MIT`.

To compare a copied file with the original:

```sh
diff /path/to/rp2040-connect/host/ImuStudio/src/ImuStudio.Core/Board/BoardShell.cs \
     src/AudioStudio.Core/Board/BoardShell.cs
```

## Copied without change but the names

| File | Original (`host/ImuStudio/`) | Change |
|---|---|---|
| `Directory.Build.props` | `Directory.Build.props` | none |
| `src/AudioStudio.Core/Board/TerminalFilter.cs` | `src/ImuStudio.Core/Board/TerminalFilter.cs` | the namespace |
| `src/AudioStudio.Core/Dsp/Fft.cs` | `src/ImuStudio.Core/Processing/Fft.cs` | the namespace |
| `src/AudioStudio.App/app.manifest` | `src/ImuStudio.App/app.manifest` | the assembly name |
| `src/AudioStudio.App/App.axaml` | `src/ImuStudio.App/App.axaml` | the class name |
| `src/AudioStudio.App/Program.cs` | `src/ImuStudio.App/Program.cs` | the namespace, and the `SPDX-License-Identifier` line the original lacks |
| `tools/install_desktop.sh` | `tools/install_desktop.sh` | the application's name, and the `Comment` of the desktop entry |

## Copied and changed

| File | Original (`host/ImuStudio/`) | Change |
|---|---|---|
| `AudioStudio.slnx`, the three `.csproj` files | `ImuStudio.slnx`, `src/*/ImuStudio.*.csproj`, `tests/ImuStudio.Core.Tests/*.csproj` | The names; no rendering project (no Silk.NET), no `AllowUnsafeBlocks`; the test project copies `tools/make_test_model.py`, the M4 model files (`models/`) and the fixtures (`Fixtures/`) to its output. The package versions are the same. |
| `src/AudioStudio.Core/Board/BoardShell.cs` | `src/ImuStudio.Core/Board/BoardShell.cs` | The namespace; `QueryAsync` runs a line whose echo, output and prompt are not raised as `Output` (the application's queries of the board's state), `Finished` tells whether a capture ended with the prompt, and spaces that end the last prompt after a line was sent are passed on instead of being taken as the line's echo. |
| `src/AudioStudio.Core/Board/BoardLocator.cs` | `src/ImuStudio.Core/Board/BoardLocator.cs` | The namespace, and the comment on the gateway address, which named rp2040-connect's `script/wifi_ap.sh`. |
| `src/AudioStudio.Core/Settings/AppSettings.cs` | `src/ImuStudio.App/Services/AppSettings.cs` | Moved to the core project (no UI dependency), the file under `~/.config/AudioStudio/`; the settings are this application's (view, recording, model, firmware repository) instead of MQTT, filter and position model; `Load` and `Save` take a path; `ResolveFirmwareImage()` is new; recordings go to `recordings/` beside the sources. |
| `src/AudioStudio.Core/Dsp/Spectrogram.cs` | `src/ImuStudio.Core/Processing/SpectrumAnalyzer.cs`, `src/ImuStudio.App/Controls/SpectrogramControl.cs` | The window and level conventions of `SpectrumAnalyzer` (Hann window after removing the mean, a sine of amplitude A reads 20·log10(A)) and the mapping of bins to rows of `SpectrogramControl` (`Map`, `Frequency`, `HeightOf`, linear or logarithmic) in a new class: columns at fixed sample positions of a sample source, kept in a cache, in dBFS; the logarithmic axis starts at 10 Hz instead of 2 Hz. `Level` is new. |
| `src/AudioStudio.App/App.axaml.cs` | `src/ImuStudio.App/App.axaml.cs` | This application's arguments; the `SPDX-License-Identifier` line the original lacks. |
| `src/AudioStudio.App/Controls/SpectrogramControl.cs` | `src/ImuStudio.App/Controls/SpectrogramControl.cs` | The palette, the frequency ticks, the latest-spectrum curve and the title and peak text are kept. The ring of columns is replaced by the columns of a `Spectrogram` for the stretch in view, so that a file can be scrolled and the live view and a file are drawn the same way; the margins are public for the other controls to line up with. |
| `src/AudioStudio.App/ViewModels/MainViewModel.cs` | `src/ImuStudio.App/ViewModels/MainViewModel.cs` | Kept: the shell part (Find, connect and disconnect, the shell pane's text and history, running a line) and the handling of the settings. Everything about the audio stream, the views, recording, files, the model and the firmware update is new; MQTT, calibration, attitude, position and replay are gone. |
| `src/AudioStudio.App/Views/MainWindow.axaml`, `MainWindow.axaml.cs` | `src/ImuStudio.App/Views/MainWindow.axaml`, `MainWindow.axaml.cs` | Kept: the styles, the bar of groups at the top, the status lines and **Shell pane** at the bottom, the shell pane with its splitter, its height and its history keys. The tabs and their content (audio views with their settings and the model, the firmware update) are new, as are the file pickers and the wheel. |
| `tests/AudioStudio.Core.Tests/BoardShellTests.cs` | `tests/ImuStudio.Core.Tests/BoardShellTests.cs` | The prompt is `xiao:~$ ` and the commands and their output are this board's (`uac hum`, `uac stop`); the test of the NuttX build's shell is left out. |
| `tests/AudioStudio.Core.Tests/BoardTests.cs` | `tests/ImuStudio.Core.Tests/BoardTests.cs` | The output-lines example is this board's `uac volume`. |
| `tests/AudioStudio.Core.Tests/FftTests.cs` | `tests/ImuStudio.Core.Tests/SpectrumTests.cs` | Only its two FFT tests. |

## The M4 swallow detector (zaitaku-mealpace)

The swallow detector comes from
[zaitaku-mealpace](https://github.com/owhinata/zaitaku-mealpace), at commit
`c5628c9`. Its author is also this repository's; it is under the MIT
License, as this repository.

| File | Original | What |
|---|---|---|
| `models/m4-0038.npz` | `analysis/models/m4-0038.npz` (unchanged, SHA-256 `c786086f069c55c5...`) | The weights of the "M4" detector: three seeds of a small CNN with their normalisation, and its configuration string. Trained on the author's own throat-microphone recordings, as that repository's README and `docs/decisions/0038-m4-detector-config.md` describe. |
| `models/mel40_htk_0_4000.npy` | `analysis/mel40_htk_0_4000.npy` (unchanged, SHA-256 `3afb1f205d69ab4c...`) | The mel matrix (40 bands, 0 to 4000 Hz, for an FFT of 512 at 16 kHz) that its features use. |

The C# code of the detector (`src/AudioStudio.Core/Model/M4Config.cs`,
`M4Model.cs`, `M4Steppers.cs`) is written for AudioStudio after that
repository's Python code: `parse_config`, `_check_arrays`, `model_prob` and
`Detector` of `analysis/m4_infer.py`, `mfe40` and `cnn_forward` of
`analysis/features_np.py`, `PostStepper` of `analysis/postprocess.py`, and
`LedStepper` of `analysis/led_rule.py`. The fixtures in
`tests/AudioStudio.Core.Tests/Fixtures/` are the values that Python code
gives for synthetic signals, written by `tools/make_m4_fixtures.py`.

## Written for AudioStudio

Everything else: the audio stream (`src/AudioStudio.Core/Stream/`), the
sample sources and WAV files (`Audio/`), the model contract and runner, the
`.npy` / `.npz` reader and the M4 detector's runner (`Model/`), the OTA
client (`Ota/`, the sequence of `script/ota.py`), the waveform, class strip
and probability trace controls and `StateButton`, the tests of these,
`tools/make_test_model.py`, `tools/make_m4_fixtures.py`,
`tools/make_icon.py` (standard library only; ImuStudio's draws its cube with
Pillow) and the icon it draws, and `README.md`.
