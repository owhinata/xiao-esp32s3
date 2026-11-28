# AudioStudio

A desktop application for the board's audio: it receives the stream of the
throat microphone (16 kHz, mono, 16-bit), shows it as a waveform and a
spectrogram, records it to WAV files, shows recorded WAV files the same way,
runs the M4 swallow detector or an ONNX classification model on either,
operates the board's shell and, for the XIAO, updates its firmware over
Wi-Fi.

Two targets are described by profiles in `targets/` (see "Targets" below):
the XIAO ESP32S3 with this repository's firmware (Wi-Fi: the audio stream on
TCP, a Telnet shell, OTA), and the Linux side of an Arduino UNO Q (ssh: the
stream is the output of `tools/unoq/audio_stream.py` run on the target, the
shell an ssh login shell).

- Framework: .NET 10 / Avalonia 11, ONNX Runtime
- Target OS: Linux (the `ip` command is used to find the XIAO, the system's
  `ssh` program to reach the UNO Q)

## Build and run

```bash
cd host/AudioStudio
dotnet build AudioStudio.slnx
dotnet test tests/AudioStudio.Core.Tests
dotnet run --project src/AudioStudio.App
```

The model tests run `tools/make_test_model.py` with `python3` (standard library
only); the M4 tests read the fixtures in `tests/AudioStudio.Core.Tests/Fixtures/`
(see "Regenerating the fixtures" below) and the model in `models/`; the ssh
tests run a fake ssh program (a shell script the tests write) and reach no
network.

Arguments (all optional):

| Argument | Meaning |
| --- | --- |
| `--target <name\|file.json>` | The target profile: a name in `targets/` (`xiao-esp32s3`, `unoq`) or the path of a profile (otherwise the last one chosen; `xiao-esp32s3` at first) |
| `--board <address>` | Connect the audio stream to this address on start (for the UNO Q, the ssh destination `user@host`) |
| `--shell` | With `--board`: connect the shell too |
| `--open <file.wav>` | Show a WAV file |
| `--model <file.onnx\|file.npz>` | Load a model (otherwise the last one loaded, or `models/m4-0038.npz` when none was loaded before) |
| `--image <file.bin>` | The firmware image to offer for an update |
| `--view <audio\|firmware>` | The tab to start with (`firmware` only for a target with OTA) |
| `--spike-filter <on\|off>` | The spike filter before the M4 detector (below), on or off (otherwise as last time; off at first) |
| `--mains <50\|60>` | The mains frequency the spike filter follows (otherwise as last time; 50 at first) |

The target, the address of each target, the view settings, the recording
label, the model and its settings, the spike filter and its mains frequency
(`SpikeFilter`, `MainsHz`; the frequency has no control in the window), and
the height of the shell pane are saved to `~/.config/AudioStudio/settings.json` on exit (the `BoardAddress` of
an older settings file is taken as the XIAO's address). Three settings exist
only there: `RecordingsDirectory` (empty: `host/AudioStudio/recordings/` for
a build inside the repository, `~/AudioStudio/recordings/` otherwise),
`RepositoryDirectory` (where to find `build/xiao_esp32s3_threadx.bin`; empty:
the first directory from the application's own upwards that has it) and
`SshPath` (the ssh program; empty: `ssh` on the `PATH`; the environment
variable `AUDIOSTUDIO_SSH` overrides it).
The board's own settings (volume, mute, hum and high-pass filters) are not
saved: the application shows what the board reports.

To have the application in the GNOME application list and dock, run
`tools/install_desktop.sh` once (it builds the Release configuration and
writes `~/.local/share/applications/AudioStudio.desktop`; `--remove` takes it
away). The icon is drawn by `tools/make_icon.py`.

## Targets

A target profile is a JSON file (comments allowed) in `targets/`, which the
build copies beside the application. **Target** at the top left chooses one
(while neither the stream nor the shell is connected); `--target` gives a
name or the path of a profile of one's own. What the profile has decides
what the window shows: a control that is absent is hidden, and the
**Firmware** tab is there only with `ota`. A profile that does not make
sense (a bad regular expression, an unknown key, a reply value the control
does not have) is refused with the reason.

| Key | Meaning |
| --- | --- |
| `name`, `title` | The name of `--target` and the settings; the name shown |
| `address` | `default` (the address when none was entered for this target), `hint` (shown in the empty field), `find` (the **Find** button: the XIAO's access point neighbour table) |
| `shell` | `transport`: `telnet` with `port` (an `address:port` overrides it), or `ssh` (the address is the destination); `prompt`, a .NET regular expression for the end of the shell's output; `tip`, the **Shell** button's tooltip; `refreshAfter`, the first words of typed lines after which the state is read again |
| `stream` | `transport`: `tcp` with `port`, or `ssh` with `command`, run on the target, whose standard output is the XAUD stream |
| `controls` | Any of `usbHost`, `microphone`, `volume`, `mute`, `hum`, `highPass` (below) |
| `ota` | `port` of the OTA service; absent: no firmware update |

A button with two states (`usbHost`, `microphone`, `highPass`) has `query`
(`command`; `lines`, `any` or `first`; and `replies`, a list of `value` and
`match`, a regular expression tried on each trimmed line in order), `on` and
`off` (`command`, and `success`, a regular expression for a line of the
output that means it worked; otherwise the first line is shown as the
board's error), `tip`, `note` (added while the shell is connected),
`unknownTip` (while the state is not known) and, for the microphone,
`requiresUsbHost`. The reply values are `stopped`, `running`, `notAnswering`
(USB host), `stopped`, `running` (microphone) and `on`, `off` (high-pass).
`volume` has `query` (a command), `match` (a regular expression with the
optional groups `percent`, `db`, `set` and `mute`), `set` (a command with
`{value}`), `minimum`, `maximum`, `whileMicrophone` (read and set only while
the microphone stream runs) and `tip`; `mute` has `on`, `off` and `tip`, and
its state is the volume's `mute` group; `hum` has `query`, `options` (its
reply values, and the values of `{value}` in `set`), `set`, `tip` and `note`.

`targets/xiao-esp32s3.json` holds the XIAO's commands and replies described
below. `targets/unoq.json` has no controls (the mixer controls of the UNO Q's
adapter are not described yet) and no OTA.

## The XIAO's interfaces

All on the board's address (the PC runs the Wi-Fi access point at
10.42.0.1; the board is usually 10.42.0.100):

| Port | What | Used for |
| --- | --- | --- |
| TCP 3233 | Audio stream (one client at a time) | **Stream** |
| TCP 23 | Telnet shell, prompt `xiao:~$ ` (one session) | **Shell** and the command buttons |
| TCP 3232 | OTA service | **Firmware** tab |

## The UNO Q over ssh

The application runs the system's `ssh` program as child processes, with
`BatchMode=yes` (it never asks for a password: the target needs key
authentication for the user, and a refusal is shown in the status line with
ssh's own words) and connection sharing: a master process
(`ControlMaster=auto`, its control socket in a private temporary directory
that is removed on exit) that the shell, the stream and any query use as
separate channels. The master starts with the first of them and ends after
the last; its own session reads the application's end of a pipe, so it also
ends when the application ends without cleaning up. The address is the ssh
destination (`arduino@unoq.local` by default; a host of `~/.ssh/config`
works too).

- **Shell** opens an interactive login shell on a terminal (`ssh -tt`,
  `TERM=dumb`), kept like the Telnet one; the profile's prompt (`$` or `#`
  at the end) tells when a typed command has finished. The queries and
  button commands of a profile that has any run as channels of their own.
- **Stream** runs the profile's command, for the UNO Q
  `flock -n -F -E 75 /home/arduino python3 -B ~/audiostudio-unoq/audio_stream.py`.
  Connecting starts the capture; disconnecting ends the ssh channel, and the
  tool on the target then ends at its next write and ends arecord. Its
  standard error lines go to the status line. Exit code 75 is shown as "the
  adapter is in use on the target (detector or a recording)" (the detector
  service or a recording holds the lock), any other end with the exit code
  and the last error line. As for the XIAO, a stream that ends is started
  again every 2 s until **Stream** is pressed again.

`tools/unoq/audio_stream.py` (Python 3 and numpy, on the target) starts
`arecord -D <device> -f S16_LE -r 48000 -c 2 -t raw -B 2000000 -F 125000 -v -`
(`--device`, default `hw:CARD=Audio,DEV=0`; only `hw:` and `plughw:` card
and device names), takes the left channel, decimates it to 16 kHz with the
61-tap low-pass filter of zaitaku-mealpace's `features_np.decimate3` (the
coefficients of scipy's `resample_poly(x, 1, 3)`; block by block, the output
equals `decimate3`'s, 30 input samples late), rounds to 16 bits, and writes
the XAUD stream of the XIAO's audio service: blocks of 320 samples, the
first with the new-stream flag, and a keep-alive block when arecord gives
nothing for a second. arecord's overruns do not say how many frames were
dropped: each is counted and written to standard error (and so shows in the
status line). The tool ends, and ends arecord, when its output is closed, on
SIGTERM, SIGHUP or SIGINT, and when arecord ends; it writes no file.
`--stdin` reads the raw 48 kHz stereo bytes from standard input instead
(for tests).

`tools/unoq/deploy.sh [user@host]` copies the tool to `~/audiostudio-unoq/`
on the target with ssh and scp. The tool's tests run on any computer with
numpy, with a fake arecord and the fixtures below:

```bash
python3 -B -m unittest discover -s tools/unoq -p 'test_*.py'
```

## Usage

1. **Target** chooses the target profile. **Find** (XIAO) looks up the
   board's address in the access point's neighbour table; typing it works
   too. For the UNO Q the address is the ssh destination.
2. **Stream** connects the audio stream. The state next to it is
   `connecting`, `connected` (the stream header came), `idle` (the board
   sends its keep-alive once a second: no microphone stream runs),
   `streaming`, or `no connection, retrying`. A connection that ends, or that
   is silent for 5 s, is made again every 2 s until **Stream** is pressed
   again. The status line at the bottom shows the sample rate, the samples
   received per second (over the last 5 s), the samples received, those lost
   (index jumps, and how many gaps), the starts of new streams on the board,
   and the number of connections.
3. **Shell** connects the shell (for the UNO Q see "The UNO Q over ssh"; its
   profile has none of the buttons and fields below). The XIAO has a single
   Telnet session:
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
   Recording goes on whatever the views show. While recording, **Swallow
   (1)**, **Wrong (2)** and **Other (3)** leave marks in the recording
   (see "Marks" below).
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
  Hamming window, |FFT(512)|²/512, 40 mel bands from the mel matrix of the
  configuration's feature set (`mfe40`: 0 to 4000 Hz; the experimental
  `mfe40f200` and `mfe40f500`: 200 or 500 to 4000 Hz), log(x + 1e-10).
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
- **Spike filter** (off unless switched on): the detector's windows are
  taken from the output of `MainsSpike`, the front-end of
  `model/analysis/mains_spike.py` (set `mfe40ms` there), instead of the raw
  samples. It limits short bursts above about 450 Hz that come back at the
  same place of every mains period (seen while the wearer touches an
  electric heater) and leaves sound that is not synchronous with the mains
  alone: at a burst phase it takes away no more than the burst it expects
  there, so a swallow on a burst phase keeps what it has above it; the
  Python module describes the algorithm. Its output is the input
  delayed by 80 samples (5 ms) where it does not act; the windows read it 80
  samples late, so window positions, indications and their times refer to
  the raw samples as without the filter, and a window waits for 80 samples
  after its end. Lost samples within 80 samples of a window make it not
  valid. The filter runs over the samples in order: over a file from its
  first sample (as the Python code over a recording), on the live stream
  from 2 s before the first window run (as far as the live buffer holds
  them) and afresh after a new stream or after the detector fell behind the
  live buffer. Only the detector's input changes: the waveform, the
  spectrogram, the levels and the recorded WAV file are the raw samples.

Every window is run, in order: the post-processing needs each one. On the
live stream the detector runs on its own thread as samples come, also while
a file is shown; if it falls
behind, the status line says by how many windows (from 4 on), and windows
whose samples left the live buffer (about 2 minutes) before it reached them
are counted as not valid. A window takes about 6 ms in the Release build
and 50 ms in the Debug build (one thread, AMD Ryzen 7 8840U). A file is run
in the background, batches of windows in parallel.

The **Step** and **Window** fields show the detector's fixed 0.25 s and
1.0 s. **Spike filter** below them switches the filter (remembered); the
detector then starts again with or without it: on the live stream from the
newest complete window, and over the whole of an opened file, so the same
recording can be compared with and without the filter. The filter follows
50 Hz mains, or 60 Hz with `--mains 60` (or `MainsHz` in the settings
file). Below the button:

- the LED as the device shows it: **running** (yellow) or **swallow
  indication** (green), for the newest window (live) or the last window in
  view (file);
- the last window's probability, the number of swallow indications in view
  and since the start of the live stream (or in the file), and the time since
  the last one (live) or of the last one in view (file);
- for a file, the list of its swallow indications (the centre of the first
  positive window of each); clicking one moves the view there;
- the weights file, its configuration string, the detector's input (`none`
  or the spike filter with its mains frequency), the feature set with its
  mel bands and mel matrix file, and the time per window.

Under the spectrogram the **probability trace** shows each window's
probability over the 0.25 s around the window's centre, so that the windows
tile the time axis and line up with the sound they hold: dashed lines at
enter (0.85) and exit (0.65) from the configuration, green bands for the
positive windows, red bands for windows that were not valid.

The files, in `models/`:

| File | What |
| --- | --- |
| `m4-0038.npz` | The weights: three seeds of the CNN with their normalisation, and the configuration string `config` (`set=mfe40 classifier=cnn seeds=0,1,2 hard_negative=none post=hyst threshold=- enter=0.85 exit=0.65 cap_s=none refractory_s=0.5`) |
| `mel40_htk_0_4000.npy` | The mel matrix of `mfe40` (40 × 257, float64, 0 to 4000 Hz) |
| `m4-f200.npz`, `m4-f500.npz` | Experimental variants: the same configuration with `set=mfe40f200` / `set=mfe40f500`, trained the same way on the same training-side sessions (`model/analysis/m4_variant.py`); their mel bands start at 200 / 500 Hz, so the mains hum below that edge is not an input (for `mfe40f500` all lines from 50 to 450 Hz). Not the recorded M4 detector |
| `m4-heater-w10.npz` | Experimental: the M4 configuration (`set=mfe40`) trained with one more recording as negative examples, silence with the bursts an electric heater puts into the signal when the wearer touches it (loss weight 10; `model/analysis/m4_variant.py --extra`) |
| `mel40_htk_200_4000.npy`, `mel40_htk_500_4000.npy` | The variants' mel matrices |

The mel matrix is looked up beside the weights file under the name of the
configuration's set (`mel40_htk_<low>_4000.npy`). The weights file is read
without code execution (numpy's `.npz`, a zip of `.npy` arrays) and checked
as the Python code checks it: exactly the keys the configuration needs, each
with its shape and dtype, and `format` 1. The CNN configurations of the sets
`mfe40`, `mfe40f200` and `mfe40f500` are supported.

A variant is loaded like any model, with **Load…** or on the command line,
for example (from `host/AudioStudio/`):

```bash
dotnet run --project src/AudioStudio.App -- --model models/m4-f200.npz
```

The spike filter works with any of these models (it changes the input, not
the model). The recorded detector with the filter, on a file:

```bash
dotnet run --project src/AudioStudio.App -- --model models/m4-0038.npz --spike-filter on --open recordings/<name>.wav
```

A model trained on the front-end's output (`set=mfe40ms`, such as
`model/analysis/models/m4-ms.npz`) is not loaded: its configuration names a
set this application does not take.

The model's panel then shows `set=mfe40f200` in the configuration string and
`features mfe40f200: 40 mel bands, 200 to 4000 Hz`. The model loaded last
is loaded again at the next start; load `models/m4-0038.npz` to go back.

#### Regenerating the fixtures

The tests compare the C# detector with values that the Python code gives
for synthetic signals (sines, a chirp, fixed-seed noise, bursts; no
recordings), in `tests/AudioStudio.Core.Tests/Fixtures/`. They are written by
`tools/make_m4_fixtures.py`, run with the interpreter of this repository's
`model/` (numpy only; it writes nothing there); from the variant models
there it also writes `m4_variant_<set>.npz`, and from
`analysis/mains_spike.py` (which also needs scipy) `m4_mains_spike.npz`: a
synthetic signal (hum, bursts twice per mains period with a wobbling
amplitude, transients, swallow-like bursts; also one at 60 Hz), the
front-end's output and the Python detector's probabilities, positive
windows and LED over that output read 80 samples late. The C# filter keeps
its state and computes in double precision as the Python code's float64
and rounds the output to float as its float32; its samples are compared
within 1e-6 (they come out equal) and the probabilities within 1e-5 (as for
the detector without the filter):

```bash
../../model/.venv/bin/python tools/make_m4_fixtures.py \
    --mealpace ../../model --out tests/AudioStudio.Core.Tests/Fixtures
```

The UNO Q tool's fixtures (`unoq_decimate3.s16`, the left channel of a
synthetic 0.6 s 48 kHz stereo signal through `features_np.decimate3`, and
`unoq_stream.xaud`, what `audio_stream.py --stdin` writes for that signal,
which the ssh tests stream through the fake ssh) are written the same way,
after checking that the tool's samples equal `decimate3`'s:

```bash
/path/to/zaitaku-mealpace/.venv/bin/python -B tools/unoq/make_unoq_fixtures.py \
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

### Marks

While recording, the user leaves marks: timed notes beside the WAV file.

| Button | Key | Kind | Meaning |
| --- | --- | --- | --- |
| **Swallow (1)** | 1 | `swallow` | I swallowed now (no text) |
| **Wrong (2)** | 2 | `false` | The indication just shown was wrong: the user's note about the indication (optional text) |
| **Other (3)** | 3 | `other` | Anything else (optional text) |

- The buttons are enabled only while recording. The keys work while the
  window has the focus and no text box has it (shell input, address,
  label, memo, volume); with a text box focused they do nothing.
- A mark's time is the moment of the key press or click: the number of
  samples written to the WAV file at that moment (the samples arrive in
  blocks, and the stream's delay to the PC is not taken off).
- After **Wrong** or **Other** a memo box opens beside the buttons and takes
  the focus. Enter stores its text; Escape leaves the mark without text. A
  mark made while the box is open (with a button) keeps the earlier mark
  with what was typed so far; the end of the recording does the same. The
  box takes nothing from the recording, which goes on in the background.
- The detector's indications are kept as well, without a key: kind
  `indication`, at the time of the indication (the centre of its first
  positive window, as the list of indications shows it), with the highest
  probability of its positive windows, so the notes can be read against
  what was shown. They are the indications of the detector as it ran, so
  they depend on the model and on the **Spike filter** switch at the time;
  the file does not say which were used. Switching either during a
  recording ends an open indication there. An indication that started before the recording is left
  out. Only the M4 detector gives indications; with an ONNX model no
  `indication` rows are written (its classes have no indication).
- **On disk at once**: each mark is written when it is made, without its
  text; the text follows when it is stored. Each time, the whole file is
  written to `<name>.marks.csv.tmp`, flushed to the disk and renamed over
  `<name>.marks.csv`, so the file is always complete and sorted, also when
  the application ends abnormally (an indication is written when it starts
  and again, with its highest probability, when it ends). A recording with
  neither marks nor indications has no marks file.
- The file: `<name>.marks.csv` beside `<name>.wav`, UTF-8, the header line
  and one row per mark sorted by the sample index. Columns: the sample
  index in the WAV file, the time in seconds (3 decimals; the sample index is
  what counts when the file is read), the kind, the probability (4
  decimals; only for `indication`) and the text (CSV quoting when it holds a
  comma, a quote or a line break):

  ```csv
  sample,seconds,kind,probability,text
  16000,1.000,swallow,,
  52000,3.250,indication,0.9731,
  60800,3.800,false,,"coughed, did not swallow"
  ```

- **Open WAV…** reads the file's marks when there is one, and the views
  show each mark as a dashed vertical line across the waveform, the
  spectrogram and the model's strip, coloured by kind, with its letter at the
  top (S `swallow` magenta, F `false` red, O `other` white, I `indication`
  green). During a recording the live view shows them the same way. The
  **Marks** section on the right lists them (time, kind, probability,
  "marked wrong" for an indication with a `false` mark, text); clicking one
  moves the view of a file there.
- **Counts**: the **Marks** section also counts the marks as the user's
  notes about the indications (they are notes about the indications, not a
  medical judgement):
  - an indication counts as a swallow found by the detector (a hit) unless
    the user marked it wrong;
  - a `false` mark refers to the latest indication that starts at or before
    it and that no earlier `false` mark took; with none it stays a note that
    refers to no indication;
  - an indication with a `false` mark is a false indication (**marked
    wrong**); **hits** are the indications less those marked wrong;
  - a `swallow` mark is the same swallow as the nearest indication not
    marked wrong that starts from 2 s before to 3 s after it (each indication
    is taken by one `swallow` mark at most), and is not counted twice;
    otherwise it is a **missed swallow**.

  The pairing is derived when the marks are read; the file holds only the
  rows above.

#### Editing the marks of a file

The marks of an opened WAV file can be edited (not those of the live view,
nor those of the recording in progress, nor a marks file that cannot be
read). An editor below the list of marks shows what is selected and
picked.

- **Select** a mark by clicking its row in the list, or by clicking on or
  within 4 pixels of its line in the views. The selected mark's line is
  drawn solid and thicker, with its letter in a box, and its row is
  highlighted.
- **Pick** a position by clicking in the views away from the marks: a solid
  yellow line. **Esc** (with no text box focused) clears the selection and
  the picked position.
- A selected `swallow`, `false` or `other` mark: the kind selector changes
  its kind, the text box its text (Enter or leaving the box stores it,
  Esc goes back to the stored text), **Delete** (button, or the Delete key
  with no text box focused) deletes it, and **Move here** moves it to the
  picked position.
- A selected `indication` is the record of what the detector showed: its
  time, probability, kind and text are not edited and it is not deleted.
  **Mark wrong** adds a `false` mark at the indication's own sample, so the
  pairing above attaches it to that indication, and selects it with the
  focus in the text box for an optional text. When the indication has a
  `false` mark already, **Mark wrong** selects that one instead.
- With a position picked, **Swallow (1)**, **Wrong (2)** and **Other (3)**
  under the editor add a mark there; the keys 1, 2 and 3 do the same when
  no text box is focused and no recording is in progress (during a
  recording they mark the recording). The new mark is selected, and for
  `false` and `other` the text box takes the focus.
- Every change is written at once, the whole file sorted, the same way as
  during a recording (temporary file, flush, rename). A WAV file without a
  marks file gets one with its first mark; when the last mark is deleted,
  the marks file is removed. A change that cannot be written is not made:
  the status message says so, and the list stays as the file on disk. The
  list and the counts follow each change.

### Firmware

The **Firmware** tab (XIAO only) updates the board over Wi-Fi, as
`script/ota.py` does.

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
| `src/AudioStudio.Core` | The target profiles (`Targets/`), the audio stream protocol, its sources (TCP, a command over ssh) and client, the ssh connection (`Ssh/`), the live ring and WAV files, the spectrogram (FFT) and levels, the ONNX model contract and its runner, the M4 swallow detector (the `.npy` / `.npz` reader, features, CNN, post-processing, LED, the spike filter before it, and its runner), the OTA client, the shell (Telnet or ssh), the reading of the board's state as its profile describes it, board discovery, the settings, the marks of a recording (`Marks/`: the file, the log written as marks come, the live-to-WAV timeline, the indications of the detector, the counts, the editing of an opened file's marks) |
| `src/AudioStudio.App` | The Avalonia user interface (MVVM): the waveform, spectrogram, class strip, probability trace and marks controls, the view model, the window |
| `models/` | The M4 swallow detector's weights and mel matrix |
| `targets/` | The target profiles: `xiao-esp32s3.json`, `unoq.json` |
| `tests/AudioStudio.Core.Tests` | xUnit tests: the stream parser (every split point), the client against a stand-in server, WAV writing and reading, the FFT and the spectrogram's levels and axis, the OTA client against a stand-in of the board's OTA service, the Telnet prompt handling and the quiet queries, the parsing of the board's replies on its USB host, microphone, volume and mute, hum filter and high-pass filter with the XIAO's profile, the profiles and their checks, the ssh transport with a fake ssh (the stream, exit code 75, a refused key, the shell and its queries, no process left behind), the model contract with the test model, the M4 detector against its Python code's values (`Fixtures/`) and its windows, gaps and new streams, the spike filter and the detector behind it against the Python front-end's values (file and live, the stream's samples unchanged), the marks (file round trip and quoting, sorting, the time and sample index, a mark during memo entry, indications with their highest probability, the counts, editing a file's marks: add, edit, move, delete, mark wrong, the indications kept, the file removed with the last mark, a write that fails) |
| `tools/` | `make_test_model.py`, `make_m4_fixtures.py`, `make_icon.py`, `install_desktop.sh` |
| `tools/unoq/` | `audio_stream.py` (runs on the UNO Q), `deploy.sh`, `make_unoq_fixtures.py`, `test_audio_stream.py` |

`ORIGIN.md` lists what was copied from ImuStudio (rp2040-connect) and what was
changed, and where the M4 model files and the UNO Q tool's arecord arguments
and decimation come from.
