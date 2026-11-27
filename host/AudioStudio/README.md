# AudioStudio

A desktop application for the board's audio: it receives the stream of the
throat microphone over Wi-Fi (16 kHz, mono, 16-bit), shows it as a waveform
and a spectrogram, records it to WAV files, shows recorded WAV files the same
way, runs an ONNX classification model on either, operates the board's shell
(Telnet) and updates the board's firmware over Wi-Fi.

- Framework: .NET 10 / Avalonia 11, ONNX Runtime
- Target OS: Linux (the `ip` command is used to find the board)

## Build and run

```bash
cd host/AudioStudio
dotnet build AudioStudio.slnx
dotnet test tests/AudioStudio.Core.Tests
dotnet run --project src/AudioStudio.App
```

The model tests run `tools/make_test_model.py` with `python3` (standard library only).

Arguments (all optional):

| Argument | Meaning |
| --- | --- |
| `--board <address>` | Connect the audio stream to this address on start |
| `--shell` | With `--board`: connect the shell too |
| `--open <file.wav>` | Show a WAV file |
| `--model <file.onnx>` | Load a model (otherwise the last one loaded is loaded again) |
| `--image <file.bin>` | The firmware image to offer for an update |
| `--view <audio\|firmware>` | The tab to start with |

The board address, the view settings, the recording label, the model and its
settings, and the height of the shell pane are saved to
`~/.config/AudioStudio/settings.json` on exit. Two settings exist only there:
`RecordingsDirectory` (empty: `host/AudioStudio/recordings/` for a build
inside the repository, `~/AudioStudio/recordings/` otherwise) and
`RepositoryDirectory` (where to find `build/xiao_esp32s3_threadx.bin`; empty:
the first directory from the application's own upwards that has it).

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
   off (the state is not known). **Volume** with **Set** sends
   `uac volume <0..100>`, **Mute** `uac mute on|off`, **Hum** `uac hum
   50|60|off`. Other commands (`uac info`, `uac stats`, `uac level`, ...) are
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

The waveform, the spectrogram and the model's class strip share one time
axis: the newest samples at the right for the live stream (the time is
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
  -3 dBFS RMS).

### Model

**Load…** loads an ONNX model. On the live stream it runs on the newest
window whenever a new one is complete; a window that comes while the model
is still busy is dropped (counted as `dropped`), so the results never fall
behind the stream. On a file it runs on every window of the whole file in
the background. The bars show the class scores of the newest window (live)
or of the last window that ends in view (file), the large text the top class,
and the class strip under the spectrogram the top class of each window along
the time axis. **Step** is the number of samples between windows (0: half a
window), **Open window** the window length for a model that does not fix it;
**Apply** loads the model again with them.

#### The model contract

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
| `src/AudioStudio.Core` | The audio stream protocol and client, the live ring and WAV files, the spectrogram (FFT) and levels, the model contract and its runner, the OTA client, the Telnet shell and board discovery, the settings |
| `src/AudioStudio.App` | The Avalonia user interface (MVVM): the waveform, spectrogram and class strip controls, the view model, the window |
| `tests/AudioStudio.Core.Tests` | xUnit tests: the stream parser (every split point), the client against a stand-in server, WAV writing and reading, the FFT and the spectrogram's levels and axis, the OTA client against a stand-in of the board's OTA service, the Telnet prompt handling and the quiet queries, the parsing of the board's replies on its USB host and microphone, the model contract with the test model |
| `tools/` | `make_test_model.py`, `make_icon.py`, `install_desktop.sh` |

`ORIGIN.md` lists what was copied from ImuStudio (rp2040-connect) and what was
changed.
