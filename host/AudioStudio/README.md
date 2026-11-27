# AudioStudio

A desktop application for the board's audio: it receives the stream of the
throat microphone over Wi-Fi (16 kHz, mono, 16-bit), shows it as a waveform
and a spectrogram, records it to WAV files, shows recorded WAV files the same
way, runs the M4 swallow detector or an ONNX classification model on either,
operates the board's shell (Telnet) and updates the board's firmware over
Wi-Fi.

- Framework: .NET 10 / Avalonia 11, ONNX Runtime
- Target OS: Linux (the `ip` command is used to find the board)

## Build and run

```bash
cd host/AudioStudio
dotnet build AudioStudio.slnx
dotnet test tests/AudioStudio.Core.Tests
dotnet run --project src/AudioStudio.App
```

The model tests run `tools/make_test_model.py` with `python3` (standard library
only); the M4 tests read the fixtures in `tests/AudioStudio.Core.Tests/Fixtures/`
(see "Regenerating the fixtures" below) and the model in `models/`.

Arguments (all optional):

| Argument | Meaning |
| --- | --- |
| `--board <address>` | Connect the audio stream to this address on start |
| `--shell` | With `--board`: connect the shell too |
| `--open <file.wav>` | Show a WAV file |
| `--model <file.onnx\|file.npz>` | Load a model (otherwise the last one loaded, or `models/m4-0038.npz` when none was loaded before) |
| `--image <file.bin>` | The firmware image to offer for an update |
| `--view <audio\|firmware>` | The tab to start with |

The board address, the view settings, the recording label, the model and its
settings, and the height of the shell pane are saved to
`~/.config/AudioStudio/settings.json` on exit. Two settings exist only there:
`RecordingsDirectory` (empty: `host/AudioStudio/recordings/` for a build
inside the repository, `~/AudioStudio/recordings/` otherwise) and
`RepositoryDirectory` (where to find `build/xiao_esp32s3_threadx.bin`; empty:
the first directory from the application's own upwards that has it).
The board's own settings (volume, mute, hum and high-pass filters) are not
saved: the application shows what the board reports.

To have the application in the GNOME application list and dock, run
`tools/install_desktop.sh` once (it builds the Release configuration and
writes `~/.local/share/applications/AudioStudio.desktop`; `--remove` takes it
away). The icon is drawn by `tools/make_icon.py`.

## The board's interfaces

All on the board's address (the PC runs the Wi-Fi access point at
10.42.0.1; the board is usually 10.42.0.100):

| Port | What | Used for |
| --- | --- | --- |
| TCP 3233 | Audio stream (one client at a time) | **Stream** |
| TCP 23 | Telnet shell, prompt `xiao:~$ ` (one session) | **Shell** and the command buttons |
| TCP 3232 | OTA service | **Firmware** tab |

## Usage

1. **Find** looks up the board's address in the access point's neighbour
   table; typing it works too.
2. **Stream** connects the audio stream. The state next to it is
   `connecting`, `connected` (the stream header came), `idle` (the board
   sends its keep-alive once a second: no microphone stream runs),
   `streaming`, or `no connection, retrying`. A connection that ends, or that
   is silent for 5 s, is made again every 2 s until **Stream** is pressed
   again. The status line at the bottom shows the sample rate, the samples
   received per second (over the last 5 s), the samples received, those lost
   (index jumps, and how many gaps), the starts of new streams on the board,
   and the number of connections.
3. **Shell** connects the Telnet shell. The board has a single Telnet session:
   while the application holds it, a terminal cannot have it, and the other
   way round. **USB host** and **Microphone** show the board's state: lit
   when the board's USB host runs (its USB-C connector is the USB host port;
   Telnet is not affected) and when its microphone stream runs. Pressing one
   sends `usb host start` / `usb host stop` or `uac start` / `uac stop`, the
   one that changes the state shown; the button is disabled until the board
   has answered, and an error the board prints goes to the status line.
   **Microphone** needs the USB host to start. The application asks the board
   for the state (`usb status`, `uac stats`; neither the commands nor their
   replies appear in the shell pane) when the shell connects, after each
   button, and after each `usb` or `uac` command typed in the shell pane.
   While the audio stream is connected, its samples and keep-alives keep
   **Microphone** up to date too, also without the shell. Without the shell
   both buttons are disabled, and without the shell and the stream they show
   off (the state is not known).
   **Volume**, **Mute**, **Hum** and **HPF** show the board's settings too,
   read with the same quiet queries (`uac volume`, `uac hum`, `uac hpf`), and
   are empty (`?`) or off while they are not known. **Set** sends
   `uac volume <0..100>` with the field's value, **Mute** (lit when muted)
   `uac mute on|off`, choosing in **Hum** `uac hum 50|60|off`, and **HPF**
   (the 30 Hz high-pass filter that removes the DC offset; lit when on)
   `uac hpf on|off`; after each the application asks the board again and
   shows what it reports, not what was sent. The board reads and sets the
   volume and mute only while the microphone stream runs: otherwise the
   field is empty and **Set** and **Mute** are disabled (the tooltip says
   why, and gives the board's own reply when it refuses), and they are read
   when the stream starts, also when the audio stream shows that it started.
   A volume typed in the field and not yet sent is kept: a refresh replaces
   the field only while it still shows the board's last value, **Set**
   replaces it with the board's answer, and it is emptied when the volume
   can no longer be read. The hum filter and the high-pass filter are read
   and set whether or not a stream runs (a change applies to the running
   stream or the next one). After the board starts, the volume and mute are
   the microphone's own defaults until set (a volume or mute set is kept
   until the board restarts and set again at every stream start), the hum
   filter is 50 Hz and the high-pass filter is on. A firmware without
   `uac hpf` leaves **HPF** disabled. Other commands (`uac info`, `uac stats`, `uac level`, ...) are
   typed in the shell pane at the bottom (Enter to send, Up/Down for history).
   Drag the bar above the shell pane to change its height; **Shell pane** at
   the bottom right hides and shows it.
4. **Record** writes the live stream to a WAV file until it is pressed again
   (see "Recordings" below). The text in **label** goes into the file name.
   Recording goes on whatever the views show.
5. **Open WAV…** shows a WAV file instead of the live stream; **Live** goes
   back. The scroll bar under the views, or the mouse wheel over them, moves
   along the file; Ctrl + wheel changes the span.

### Views

The waveform, the spectrogram and the model's strip (the class strip of an
ONNX model, the probability trace of the M4 detector) share one time axis: the newest samples at the right for the live stream (the time is
relative to now), the chosen place for a file (the time from the file's
start).

- **Waveform**: the smallest and the largest sample of each pixel column.
  **Span** is the time across the view (1 to 60 s), **Amplitude** a zoom
  (full scale is ±1). Lost samples are shaded red (they are zeros); a yellow
  line marks the start of a new stream on the board or a new connection.
- **Spectrogram**: each column is the amplitude spectrum of the **FFT size**
  samples around it (Hann window, mean removed), in dBFS: a sine of
  amplitude A (full scale 1) reads 20·log10(A) at its frequency. The columns
  are about 800 across the view, and never closer than 1/16 of the FFT size. FFT 4096
  gives 3.9 Hz bins (0.26 s), enough to see the 50 Hz mains hum apart from
  the low band of the throat microphone; **Logarithmic frequency** (from
  10 Hz) gives the low end most of the height. **Max freq** sets the top of
  the axis, **Min dB** / **Max dB** the levels drawn darkest and brightest.
  The curve on the right is the newest spectrum in view; `peak` its
  strongest frequency.
- **Levels** on the right: peak and RMS in dBFS over the last 0.25 s of the
  view and over the whole view, as the board's `uac level` computes them
  (relative to a full-scale square wave: a full-scale sine is 0 dBFS peak,
  -3 dBFS RMS), and DC, the mean of the same samples in steps of a 16-bit
  sample: the offset that is left with the board's high-pass (**HPF**) off.

### Model

**Load…** loads a model: a `.npz` file is the M4 swallow detector (below),
an `.onnx` file a model of the contract further below. Without
`--model` the application loads the model loaded last, or, when none was
loaded before, `models/m4-0038.npz` (looked up from the application's
directory upwards, so it is found for a build inside the repository).

#### The M4 swallow detector

The detector of [zaitaku-mealpace](https://github.com/owhinata/zaitaku-mealpace)
(the same author; MIT): a small CNN over log-mel features of the throat
microphone, whose output is a swallow *indication* that drives the LED
matrix of that project's device. It is an indication, not a judgement: the
device is not a medical device and does not detect or diagnose aspiration.

What it computes, with the same arithmetic as that repository's Python code
(`analysis/m4_infer.py`, `features_np.py`, `postprocess.py`, `led_rule.py`):

- **Windows**: 16000 samples (1.0 s) every 4000 samples (0.25 s); window k
  starts 4000·k samples after the start of its stretch. A file is one
  stretch from its first sample. On the live stream a stretch starts with a
  new connection or a new stream on the board (the yellow line in the
  waveform), where the detector starts afresh; a window of the stretch before
  that would reach past it is not run. When the model is loaded during a
  stream, the detector starts at the newest complete window.
- **Features**: the samples divided by 32768, pre-emphasis 0.97 over the
  whole window, frames of 400 samples every 160 (98 frames), a symmetric
  Hamming window, |FFT(512)|²/512, 40 mel bands (0 to 4000 Hz) from the mel
  matrix, log(x + 1e-10).
- **Probability**: per seed, the features normalised with that seed's mean
  and standard deviation, then three 3×3 convolutions with ReLU (2×2 max
  pooling after the first two), the global mean, one fully connected output
  and a sigmoid; the probability is the mean of the seeds' probabilities.
- **Post-processing**: hysteresis (a run starts at a probability of at least
  **enter** and goes on while it is at least **exit**), a refractory period
  (a run that starts within 0.5 s of the last window of the run before is
  suppressed) and a cap on a run's length, as the configuration in the
  weights file sets them; the windows of a run that is not suppressed are the
  positive windows. Each run of positive windows is one swallow indication.
- **LED**: green (**swallow indication**) while the window starts at most
  1.75 s after the start of the last positive window, otherwise yellow
  (**running**); off when no window came for a second (the live stream
  stopped).
- **Lost samples**: a window that overlaps lost samples (shaded red in the
  waveform) is not valid: it is not run, it ends a run and cannot start one,
  and the trace has no probability there.

Every window is run, in order: the post-processing needs each one. On the
live stream the detector runs on its own thread as samples come, also while
a file is shown; if it falls
behind, the status line says by how many windows (from 4 on), and windows
whose samples left the live buffer (about 2 minutes) before it reached them
are counted as not valid. A window takes about 6 ms in the Release build
and 50 ms in the Debug build (one thread, AMD Ryzen 7 8840U). A file is run
in the background, batches of windows in parallel.

The **Step** and **Window** fields show the detector's fixed 0.25 s and
1.0 s. Below them:

- the LED as the device shows it: **running** (yellow) or **swallow
  indication** (green), for the newest window (live) or the last window in
  view (file);
- the last window's probability, the number of swallow indications in view
  and since the start of the live stream (or in the file), and the time since
  the last one (live) or of the last one in view (file);
- for a file, the list of its swallow indications (the centre of the first
  positive window of each); clicking one moves the view there;
- the weights file, its configuration string, and the time per window.

Under the spectrogram the **probability trace** shows each window's
probability over the 0.25 s around the window's centre, so that the windows
tile the time axis and line up with the sound they hold: dashed lines at
enter (0.85) and exit (0.65) from the configuration, green bands for the
positive windows, red bands for windows that were not valid.

The files, in `models/`:

| File | What |
| --- | --- |
| `m4-0038.npz` | The weights: three seeds of the CNN with their normalisation, and the configuration string `config` (`set=mfe40 classifier=cnn seeds=0,1,2 hard_negative=none post=hyst threshold=- enter=0.85 exit=0.65 cap_s=none refractory_s=0.5`) |
| `mel40_htk_0_4000.npy` | The mel matrix (40 × 257, float64); it is looked up beside the weights file, under this name |

The weights file is read without code execution (numpy's `.npz`, a zip of
`.npy` arrays) and checked as the Python code checks it: exactly the keys the
configuration needs, each with its shape and dtype, and `format` 1. Only the
`mfe40` CNN configurations are supported.

#### Regenerating the fixtures

The tests compare the C# detector with values that the Python code gives
for synthetic signals (sines, a chirp, fixed-seed noise, bursts; no
recordings), in `tests/AudioStudio.Core.Tests/Fixtures/`. They are written by
`tools/make_m4_fixtures.py`, run with a zaitaku-mealpace checkout's own
interpreter (numpy only; it writes nothing into the checkout):

```bash
/path/to/zaitaku-mealpace/.venv/bin/python tools/make_m4_fixtures.py \
    --mealpace /path/to/zaitaku-mealpace --out tests/AudioStudio.Core.Tests/Fixtures
```

#### ONNX models

An ONNX model of the contract below. On the live stream it runs on the newest
window whenever a new one is complete; a window that comes while the model
is still busy is dropped (counted as `dropped`), so the results never fall
behind the stream. On a file it runs on every window of the whole file in
the background. The bars show the class scores of the newest window (live)
or of the last window that ends in view (file), the large text the top class,
and the class strip under the spectrogram the top class of each window along
the time axis. **Step** is the number of samples between windows (0: half a
window), **Open window** the window length for a model that does not fix it;
**Apply** loads the model again with them.

##### The model contract

- **Input**: exactly one input, float32, of shape `[1, N]` or `[1, 1, N]`:
  N consecutive samples of 16 kHz mono audio scaled to -1..1 (the 16-bit
  sample divided by 32768). N is read from the model; if the model leaves it
  open (a symbolic dimension), **Open window** gives it (default 16000, one
  second).
- **Output**: the first output, float32, of shape `[1, C]` (or `[C]`): one
  score per class. If the scores are all in 0..1 and sum to 1 they are shown
  as they are, otherwise the application applies a softmax.
- **Class names**: from `<model>.labels.txt` beside the model (`name.onnx`
  has `name.labels.txt`; one name per line, empty lines skipped), else from
  the model's metadata key `labels` (comma-separated), else `class 0`,
  `class 1`, ... Missing names are made up, extra names ignored.
- **Features**: the model gets raw samples; feature extraction (for example
  a log-mel spectrogram) belongs inside the ONNX graph.
- **Rate**: the model runs only on 16 kHz sources; a WAV file of another rate
  is shown but not classified.

`tools/make_test_model.py <file.onnx> [--samples N]` writes a trivial model
that fulfils the contract (energy and slope of the signal; classes
`silence`, `low`, `high`), using only the Python standard library.

### Recordings

- Format: WAV, 16-bit PCM, mono, at the stream's rate (16000 Hz).
- Name: `yyyyMMdd-HHmmss.wav`, or `yyyyMMdd-HHmmss_<label>.wav` with a label
  (letters, digits, `.`, `-` and `_` are kept; anything else becomes `-`).
  An existing name gets `-2`, `-3`, ... appended.
- Lost samples are written as zeros, so the file keeps the stream's timing
  (at most 60 s of zeros per gap); the status shows how many, and the message
  at the end of the recording the number of gaps and zero samples.
- The header's sizes are brought up to date every second, so the file of a
  recording that did not end normally reads up to that point.

### Firmware

The **Firmware** tab updates the board over Wi-Fi, as `script/ota.py` does.

1. **Image** is the application image to send: by default
   `build/xiao_esp32s3_threadx.bin` of the firmware repository
   (**Repository build** picks it again), or any file chosen with **Choose…**.
   Below it, the project, version, build time and ELF SHA-256 (the first 8
   bytes, which the confirmation names) from the image's application
   description; they are read again when the update starts.
2. **Update** sends the image to the board's OTA service, with progress. The
   board writes it to the application slot that is not running, checks it,
   makes it the one to boot and restarts; the application then connects again
   until the new image answers (up to 60 s) and confirms it. Each step and
   its result go to the log below. Pressing **Update** again cancels; an
   image that was written but not confirmed goes back to the previous one at
   the next reset.
3. The board stops a running microphone stream when the update starts. The
   audio stream connects again by itself after the restart; the microphone is
   started again with **USB host** and **Microphone**. A shell that was
   connected before the update is connected again.

The OTA service has no authentication: anyone on the network can replace the
firmware.

## Layout

| Path | Role |
| --- | --- |
| `src/AudioStudio.Core` | The audio stream protocol and client, the live ring and WAV files, the spectrogram (FFT) and levels, the ONNX model contract and its runner, the M4 swallow detector (the `.npy` / `.npz` reader, features, CNN, post-processing, LED, and its runner), the OTA client, the Telnet shell and board discovery, the settings |
| `src/AudioStudio.App` | The Avalonia user interface (MVVM): the waveform, spectrogram, class strip and probability trace controls, the view model, the window |
| `models/` | The M4 swallow detector's weights and mel matrix |
| `tests/AudioStudio.Core.Tests` | xUnit tests: the stream parser (every split point), the client against a stand-in server, WAV writing and reading, the FFT and the spectrogram's levels and axis, the OTA client against a stand-in of the board's OTA service, the Telnet prompt handling and the quiet queries, the parsing of the board's replies on its USB host, microphone, volume and mute, hum filter and high-pass filter, the model contract with the test model, the M4 detector against its Python code's values (`Fixtures/`) and its windows, gaps and new streams |
| `tools/` | `make_test_model.py`, `make_m4_fixtures.py`, `make_icon.py`, `install_desktop.sh` |

`ORIGIN.md` lists what was copied from ImuStudio (rp2040-connect) and what was
changed, and where the M4 model files come from.
