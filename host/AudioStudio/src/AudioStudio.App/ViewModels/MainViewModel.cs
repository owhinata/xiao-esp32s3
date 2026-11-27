// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using AudioStudio.Core.Audio;
using AudioStudio.Core.Board;
using AudioStudio.Core.Dsp;
using AudioStudio.Core.Model;
using AudioStudio.Core.Ota;
using AudioStudio.Core.Settings;
using AudioStudio.Core.Stream;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace AudioStudio.App.ViewModels;

/// <summary>One bar of the class scores.</summary>
public sealed partial class ScoreItem : ObservableObject
{
    [ObservableProperty] private string _name = "";
    [ObservableProperty] private double _value;
    [ObservableProperty] private string _text = "";
    [ObservableProperty] private Avalonia.Media.IBrush _brush = Avalonia.Media.Brushes.Gray;
}

/// <summary>One swallow indication of the M4 detector in a file: the centre of its first positive window.</summary>
public sealed record IndicationItem(double Seconds, string Text);

public sealed partial class MainViewModel : ObservableObject, IDisposable
{
    private const int ShellLogLimit = 40_000;
    private const int OtaLogLimit = 20_000;
    /// <summary>Longest run of zeros a gap gets in a recording; the loss is counted in full.</summary>
    private const long MaxGapFillSeconds = 60;
    private static readonly TimeSpan CommandWait = TimeSpan.FromSeconds(2);

    private readonly AppSettings _settings = AppSettings.Load();
    private readonly DispatcherTimer _timer;
    private readonly StringBuilder _shellLog = new();
    private readonly StringBuilder _otaLog = new();
    private readonly object _recordLock = new();
    private AudioStreamClient? _stream;
    private volatile LiveSource _live = new(16000);
    private BoardShell? _shell;
    private WavWriter? _writer;
    private FileSource? _file;
    private AudioModel? _model;
    private Classifier? _liveClassifier, _fileClassifier;
    private M4Model? _m4;
    private M4Runner? _liveM4, _fileM4;
    private long _listedIndications = -1;
    private CancellationTokenSource? _fileAnalysis;
    private Task _fileAnalysisTask = Task.CompletedTask;
    private CancellationTokenSource? _update;
    private float[] _levelBuffer = [];
    private int _frame;
    /// <summary>The audio stream's state at the last status update; null for a stream not seen yet.</summary>
    private AudioStreamState? _seenStreamState;

    public const int AudioView = 0;
    public const int FirmwareView = 1;

    [ObservableProperty] private int _selectedView;
    [ObservableProperty] private string _boardAddress;
    [ObservableProperty] private bool _isStreamConnected;
    [ObservableProperty] private string _streamState = "not connected";
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanToggleMicrophone), nameof(MicrophoneTip), nameof(CanSetVolume),
                              nameof(VolumeTip), nameof(CanToggleMute), nameof(MuteTip), nameof(HumTip),
                              nameof(CanToggleHighPass), nameof(HighPassTip))]
    private bool _isShellConnected;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsUsbHostOn), nameof(CanToggleMicrophone), nameof(MicrophoneTip))]
    private UsbHostState _usbHost;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsMicrophoneOn), nameof(CanToggleMicrophone), nameof(MicrophoneTip),
                              nameof(CanSetVolume), nameof(VolumeTip), nameof(CanToggleMute), nameof(MuteTip))]
    private MicrophoneState _microphone;
    [ObservableProperty] private string _shellText = "";
    [ObservableProperty] private string _shellInput = "";
    [ObservableProperty] private bool _isShellPaneVisible;
    /// <summary>The volume field: the board's percent, or what the user has typed and not yet sent.</summary>
    [ObservableProperty] private decimal? _volume;
    /// <summary>The board's last answer to <c>uac volume</c>; null when it was not asked (no shell, no stream).</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanSetVolume), nameof(VolumeTip), nameof(IsMuted), nameof(CanToggleMute),
                              nameof(MuteTip))]
    private VolumeReading? _boardVolume;
    /// <summary>The board's hum filter (50, 60 or off); null when not known.</summary>
    [ObservableProperty] private string? _hum;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsHighPassOn), nameof(CanToggleHighPass), nameof(HighPassTip))]
    private HighPassState _highPass;
    [ObservableProperty] private bool _isRecording;
    [ObservableProperty] private string _recordingLabel;
    [ObservableProperty] private string _recordingText = "";
    [ObservableProperty] private bool _isFileShown;
    [ObservableProperty] private string _sourceText = "Live";
    [ObservableProperty] private double _position;
    [ObservableProperty] private double _positionMaximum;
    [ObservableProperty] private string _message = "";
    [ObservableProperty] private string _streamStatus = "";
    [ObservableProperty] private double _spanSeconds;
    [ObservableProperty] private int _amplitudeZoom;
    [ObservableProperty] private int _fftSize;
    [ObservableProperty] private string _maxFrequency = "Nyquist";
    [ObservableProperty] private bool _logFrequency;
    [ObservableProperty] private double _minDb;
    [ObservableProperty] private double _maxDb;
    [ObservableProperty] private string _levelText = "";
    [ObservableProperty] private string _modelPath;
    [ObservableProperty] private string _modelText = "No model loaded";
    [ObservableProperty] private string _topClass = "";
    [ObservableProperty] private decimal? _modelHop;
    [ObservableProperty] private decimal? _modelWindow;
    /// <summary>The loaded model is the M4 swallow detector (a .npz), not an ONNX model.</summary>
    [ObservableProperty] private bool _isM4Model;
    [ObservableProperty] private string _indicatorText = "";
    [ObservableProperty] private Avalonia.Media.IBrush _indicatorBrush = Avalonia.Media.Brushes.Gray;
    [ObservableProperty] private string _detectorText = "";
    [ObservableProperty] private IndicationItem? _selectedIndication;
    [ObservableProperty] private string _imagePath;
    [ObservableProperty] private string _imageText = "";
    [ObservableProperty] private bool _isUpdating;
    [ObservableProperty] private double _updateProgress;
    [ObservableProperty] private string _otaText = "";

    public IReadOnlyList<double> Spans { get; } = [1, 2, 5, 10, 20, 60];
    public IReadOnlyList<int> Zooms { get; } = [1, 2, 4, 8, 16, 32, 64, 128];
    public IReadOnlyList<int> FftSizes { get; } = Spectrogram.Sizes;
    public IReadOnlyList<string> MaxFrequencies { get; } = ["Nyquist", "4000", "2000", "1000", "500"];
    public IReadOnlyList<string> HumOptions { get; } = ["50", "60", "off"];
    public ObservableCollection<ScoreItem> Scores { get; } = new();
    /// <summary>The swallow indications of the file in view.</summary>
    public ObservableCollection<IndicationItem> Indications { get; } = new();
    public List<string> ShellHistory { get; } = new();

    /// <summary>Raised on the UI thread at the frame rate, for the views to draw what <see cref="ViewSource"/> holds.</summary>
    public event Action? Frame;
    /// <summary>The view settings changed: the pictures are made anew.</summary>
    public event Action? ViewChanged;

    /// <param name="board">Board address to connect the audio stream to at once.</param>
    /// <param name="shell">Connect the shell at once too.</param>
    public MainViewModel(string? board = null, bool shell = false, string? image = null)
    {
        _boardAddress = board ?? _settings.BoardAddress;
        _isShellPaneVisible = _settings.ShellPaneVisible;
        _spanSeconds = Spans.Contains(_settings.SpanSeconds) ? _settings.SpanSeconds : 5;
        _amplitudeZoom = Zooms.Contains(_settings.AmplitudeZoom) ? _settings.AmplitudeZoom : 1;
        _fftSize = FftSizes.Contains(_settings.FftSize) ? _settings.FftSize : 2048;
        _maxFrequency = MaxFrequencies.Contains(_settings.MaxFrequency) ? _settings.MaxFrequency : "Nyquist";
        _logFrequency = _settings.LogFrequency;
        _minDb = _settings.MinDb;
        _maxDb = _settings.MaxDb;
        _recordingLabel = _settings.RecordingLabel;
        _modelPath = _settings.ModelPath;
        _modelHop = _settings.ModelHopSamples;
        _modelWindow = _settings.ModelWindowSamples;
        _imagePath = image ?? _settings.ResolveFirmwareImage() ?? "";
        ShowImageInfo();
        _timer = new DispatcherTimer(TimeSpan.FromMilliseconds(1000.0 / 30), DispatcherPriority.Render, (_, _) => Tick());
        _timer.Start();
        if (board is not null)
        {
            ConnectStream();
            if (shell)
            {
                _ = ToggleShellAsync();
            }
        }
    }

    // ---- what the views show ---------------------------------------------

    /// <summary>The samples in view: the opened file, or the live stream.</summary>
    public ISampleSource ViewSource => (ISampleSource?)_file ?? _live;

    /// <summary>Samples shown across the views.</summary>
    public long SpanSamples => (long)(SpanSeconds * ViewSource.SampleRate);

    /// <summary>The stretch in view: the newest samples, or the place chosen in the file.</summary>
    public (long From, long To) ViewRange()
    {
        ISampleSource source = ViewSource;
        long span = SpanSamples;
        if (_file is FileSource file)
        {
            long from = Math.Clamp((long)(Position * file.SampleRate), 0, Math.Max(0, file.End - span));
            return (from, from + span);
        }
        long end = source.End;
        return (end - span, end);
    }

    /// <summary>The classifier of what is in view, if an ONNX model is loaded.</summary>
    public Classifier? ViewClassifier => _file is null ? _liveClassifier : _fileClassifier;

    /// <summary>The M4 detector's runner of what is in view, if the M4 model is loaded.</summary>
    public M4Runner? ViewM4Runner => _file is null ? _liveM4 : _fileM4;

    /// <summary>The M4 configuration's enter and exit levels (NaN where it has none).</summary>
    public (double Enter, double Exit) M4Levels =>
        _m4 is null ? (double.NaN, double.NaN)
        : (_m4.Config.Enter ?? _m4.Config.Threshold ?? double.NaN, _m4.Config.Exit ?? double.NaN);

    /// <summary>Upper end of the frequency axis in Hz.</summary>
    public double MaxHz =>
        double.TryParse(MaxFrequency, NumberStyles.Float, CultureInfo.InvariantCulture, out double hz) ? hz : double.PositiveInfinity;

    /// <summary>Samples between spectrogram columns: about 800 columns across, at least 1/16 of the FFT apart.</summary>
    public int SpectrogramHop => (int)Math.Max(FftSize / 16, Math.Ceiling(SpanSamples / 800.0));

    partial void OnSpanSecondsChanged(double value)
    {
        _settings.SpanSeconds = value;
        UpdatePositionRange();
        ViewChanged?.Invoke();
    }

    partial void OnAmplitudeZoomChanged(int value) => _settings.AmplitudeZoom = value;

    partial void OnFftSizeChanged(int value)
    {
        _settings.FftSize = value;
        ViewChanged?.Invoke();
    }

    partial void OnMaxFrequencyChanged(string value)
    {
        _settings.MaxFrequency = value;
        ViewChanged?.Invoke();
    }

    partial void OnLogFrequencyChanged(bool value)
    {
        _settings.LogFrequency = value;
        ViewChanged?.Invoke();
    }

    partial void OnMinDbChanged(double value) => _settings.MinDb = value;
    partial void OnMaxDbChanged(double value) => _settings.MaxDb = value;
    partial void OnIsShellPaneVisibleChanged(bool value) => _settings.ShellPaneVisible = value;
    partial void OnRecordingLabelChanged(string value) => _settings.RecordingLabel = value;

    /// <summary>Moves the view in the file by a number of spans (the wheel), or zooms.</summary>
    public void Scroll(double spans)
    {
        if (_file is not null)
        {
            Position = Math.Clamp(Position + spans * SpanSeconds, 0, PositionMaximum);
        }
    }

    public void Zoom(int steps)
    {
        int i = Math.Clamp(Spans.ToList().IndexOf(SpanSeconds) + steps, 0, Spans.Count - 1);
        SpanSeconds = Spans[i];
    }

    private void UpdatePositionRange()
    {
        if (_file is FileSource file)
        {
            PositionMaximum = Math.Max(0, (double)file.End / file.SampleRate - SpanSeconds);
            if (Position > PositionMaximum)
            {
                Position = PositionMaximum;
            }
        }
    }

    /// <summary>Height of the shell pane in the window; kept between runs.</summary>
    public double ShellPaneHeight
    {
        get => _settings.ShellPaneHeight;
        set => _settings.ShellPaneHeight = value;
    }

    // ---- the board's address ---------------------------------------------

    [RelayCommand]
    private async Task FindBoardAsync()
    {
        try
        {
            BoardAddress = await BoardLocator.FindAsync();
            Message = $"Board found at {BoardAddress}";
        }
        catch (BoardException e)
        {
            Message = e.Message;
        }
    }

    /// <summary>The host part of the address; the port part is for the Telnet shell only.</summary>
    private string? Host()
    {
        string[] parts = BoardAddress.Trim().Split(':');
        return parts.Length is 1 or 2 && parts[0].Length > 0 ? parts[0] : null;
    }

    // ---- the audio stream ------------------------------------------------

    [RelayCommand]
    private async Task ToggleStreamAsync()
    {
        if (_stream is not null)
        {
            DisconnectStream();
            return;
        }
        if (string.IsNullOrWhiteSpace(BoardAddress))
        {
            await FindBoardAsync();
        }
        ConnectStream();
    }

    private void ConnectStream()
    {
        if (Host() is not string host)
        {
            Message = "Enter the board's address (or press Find)";
            return;
        }
        AudioStreamClient client = new(host);
        client.Connected += OnStreamHeader;
        client.BlockReceived += OnBlock;
        client.Disconnected += reason => Dispatcher.UIThread.Post(() =>
        {
            if (_stream == client)
            {
                Message = $"Audio stream: {reason}; connecting again";
            }
        });
        _stream = client;
        _seenStreamState = null;
        client.Start();
        IsStreamConnected = true;
        _settings.BoardAddress = BoardAddress.Trim();
        Message = $"Audio stream: connecting to {host}:{client.Port}";
    }

    private void DisconnectStream()
    {
        AudioStreamClient? client = _stream;
        _stream = null;
        _seenStreamState = null;
        IsStreamConnected = false;
        client?.Dispose();
        if (_shell is null)
        {
            Microphone = MicrophoneState.Unknown;
        }
        Message = "Audio stream disconnected";
    }

    /// <summary>Runs on the receiving thread.</summary>
    private void OnStreamHeader(AudioStreamHeader header)
    {
        if (header.SampleRate != _live.SampleRate)
        {
            _live = new LiveSource(header.SampleRate);
            Dispatcher.UIThread.Post(() =>
            {
                MakeLiveClassifier();
                ViewChanged?.Invoke();
            });
        }
        else
        {
            // A new connection: the signal does not continue from the last one.
            _live.MarkNewStream();
        }
        lock (_recordLock)
        {
            if (_writer is not null && _writer.SampleRate != header.SampleRate)
            {
                Dispatcher.UIThread.Post(() =>
                {
                    StopRecording();
                    Message = $"Recording stopped: the stream now has {header.SampleRate} Hz";
                });
            }
        }
    }

    /// <summary>Runs on the receiving thread.</summary>
    private void OnBlock(AudioBlock block)
    {
        LiveSource live = _live;
        long fill = Math.Min(block.Lost, MaxGapFillSeconds * live.SampleRate);
        if (block.Lost > 0)
        {
            live.AppendGap(block.Lost);
        }
        if (block.NewStream)
        {
            live.MarkNewStream();
        }
        live.Append(block.Samples);
        lock (_recordLock)
        {
            if (_writer is WavWriter writer && writer.SampleRate == live.SampleRate)
            {
                try
                {
                    if (fill > 0)
                    {
                        writer.WriteGap(fill);
                    }
                    writer.Write(block.Samples);
                }
                catch (IOException e)
                {
                    Dispatcher.UIThread.Post(() =>
                    {
                        StopRecording();
                        Message = $"Recording stopped: {e.Message}";
                    });
                }
            }
        }
    }

    // ---- the board's shell -----------------------------------------------

    [RelayCommand]
    private async Task ToggleShellAsync()
    {
        if (_shell is not null)
        {
            _shell.Dispose();
            return;
        }
        try
        {
            if (string.IsNullOrWhiteSpace(BoardAddress))
            {
                BoardAddress = await BoardLocator.FindAsync();
            }
            // "address" or "address:port"; the port is for a shell that is not on the Telnet port.
            string[] parts = BoardAddress.Trim().Split(':');
            int port = BoardLocator.TelnetPort;
            if (parts.Length > 2 || (parts.Length == 2 && (!int.TryParse(parts[1], out port) || port is < 1 or > 65535)))
            {
                Message = "The board address is not of the form address or address:port";
                return;
            }
            BoardShell shell = await BoardShell.ConnectAsync(parts[0], port);
            shell.Output += text => Dispatcher.UIThread.Post(() => AppendShell(text));
            shell.Closed += reason => Dispatcher.UIThread.Post(() => OnShellClosed(shell, reason));
            _shell = shell;
            IsShellConnected = true;
            _settings.BoardAddress = BoardAddress.Trim();
            Message = $"Shell connected to {shell.Address}";
            await shell.SettleAsync();
            await RefreshBoardStateAsync();
        }
        catch (BoardException e)
        {
            Message = e.Message;
        }
    }

    private void OnShellClosed(BoardShell shell, string? reason)
    {
        if (_shell != shell)
        {
            return;
        }
        _shell = null;
        IsShellConnected = false;
        UsbHost = UsbHostState.Unknown;
        Microphone = MicrophoneFromStream(_stream?.Status().State);
        ShowVolume(null, replaceEdit: true);
        ShowHum(null);
        HighPass = HighPassState.Unknown;
        Message = reason is null ? "Shell disconnected" : $"Shell disconnected: {reason}";
    }

    private void AppendShell(string text)
    {
        _shellLog.Append(text);
        if (_shellLog.Length > ShellLogLimit)
        {
            _shellLog.Remove(0, _shellLog.Length - ShellLogLimit * 3 / 4);
        }
        ShellText = _shellLog.ToString();
    }

    /// <summary>Runs a line on the board and shows it in the shell pane. Null when it could not be sent.</summary>
    private async Task<string?> RunAsync(string line)
    {
        BoardShell? shell = _shell;
        if (shell is null)
        {
            Message = "The shell is not connected";
            return null;
        }
        AppendShell(line);
        try
        {
            return await shell.ExecuteAsync(line, CommandWait);
        }
        catch (Exception e) when (e is BoardException or ObjectDisposedException)
        {
            Message = e.Message;
            return null;
        }
    }

    [RelayCommand]
    private async Task SendShellAsync()
    {
        string line = ShellInput.Trim();
        if (line.Length == 0)
        {
            return;
        }
        ShellInput = "";
        ShellHistory.Remove(line);
        ShellHistory.Add(line);
        // A "usb" or "uac" command can change what the buttons show. One that
        // is still printing after the wait is left alone: a query sent now
        // would take its output.
        if (await RunAsync(line) is string output && BoardShell.Finished(output) &&
            line.Split(' ', 2)[0] is "usb" or "uac")
        {
            _ = RefreshBoardStateAsync();
        }
    }

    // ---- the board's USB host and microphone -------------------------------

    public bool IsUsbHostOn => UsbHost == UsbHostState.Running;
    public bool IsMicrophoneOn => Microphone == MicrophoneState.Running;

    /// <summary>The microphone needs the shell, and the USB host to start it.</summary>
    public bool CanToggleMicrophone =>
        IsShellConnected && (UsbHost == UsbHostState.Running || Microphone == MicrophoneState.Running);

    public string MicrophoneTip =>
        !IsShellConnected ? "Start or stop the board's microphone stream (uac start, uac stop); needs the shell"
        : !CanToggleMicrophone ? "The microphone needs the USB host: press USB host first"
        : "Start or stop the board's microphone stream (uac start, uac stop)";

    [RelayCommand]
    private Task ToggleUsbHostAsync() =>
        RunStateCommandAsync(IsUsbHostOn ? BoardCommand.UsbHostStop : BoardCommand.UsbHostStart);

    [RelayCommand]
    private Task ToggleMicrophoneAsync() =>
        RunStateCommandAsync(IsMicrophoneOn ? BoardCommand.UacStop : BoardCommand.UacStart);

    /// <summary>
    /// Runs a button's command, shows the board's answer, and asks the board
    /// for its state again. <paramref name="replaceVolumeEdit"/>: the volume
    /// field then shows the board's volume even if it was edited.
    /// </summary>
    private async Task RunStateCommandAsync(BoardCommand command, bool replaceVolumeEdit = false)
    {
        if (await RunAsync(command.Line) is not string output)
        {
            return;
        }
        List<string> lines = BoardShell.OutputLines(output, command.Line);
        Message = $"{command.Line}: " + (command.Failure(lines) ?? string.Join(" ", lines));
        await RefreshBoardStateAsync(replaceVolumeEdit);
    }

    /// <summary>
    /// Asks the board for the USB host's and the microphone's state, the
    /// microphone's volume and mute (while its stream runs: the board reads
    /// them only then), the hum filter and the high-pass filter, with queries
    /// that the shell pane does not show; they wait behind a command that is
    /// running.
    /// </summary>
    private async Task RefreshBoardStateAsync(bool replaceVolumeEdit = false)
    {
        BoardShell? shell = _shell;
        if (shell is null)
        {
            return;
        }
        try
        {
            string usb = await shell.QueryAsync(BoardState.UsbStatusLine, CommandWait);
            UsbHostState host = BoardState.ParseUsbStatus(BoardShell.OutputLines(usb, BoardState.UsbStatusLine));
            // A host that does not run has no stream.
            MicrophoneState microphone = MicrophoneState.Stopped;
            if (host != UsbHostState.Stopped)
            {
                string uac = await shell.QueryAsync(BoardState.UacStatsLine, CommandWait);
                microphone = BoardState.ParseUacStats(BoardShell.OutputLines(uac, BoardState.UacStatsLine));
            }
            VolumeReading? volume = null;
            if (microphone == MicrophoneState.Running)
            {
                string reply = await shell.QueryAsync(BoardState.UacVolumeLine, CommandWait);
                volume = BoardState.ParseUacVolume(BoardShell.OutputLines(reply, BoardState.UacVolumeLine));
            }
            string humReply = await shell.QueryAsync(BoardState.UacHumLine, CommandWait);
            string? hum = BoardState.ParseUacHum(BoardShell.OutputLines(humReply, BoardState.UacHumLine));
            string hpfReply = await shell.QueryAsync(BoardState.UacHpfLine, CommandWait);
            HighPassState highPass = BoardState.ParseUacHpf(BoardShell.OutputLines(hpfReply, BoardState.UacHpfLine));
            if (_shell != shell)
            {
                return;
            }
            UsbHost = host;
            Microphone = microphone;
            ShowVolume(volume, replaceVolumeEdit);
            ShowHum(hum);
            HighPass = highPass;
            if (host == UsbHostState.NotAnswering)
            {
                Message = "The board's USB host does not answer; it stays so until the board restarts";
            }
        }
        catch (Exception e) when (e is BoardException or ObjectDisposedException)
        {
        }
    }

    /// <summary>The microphone's state as the audio stream tells it: samples, or the idle keep-alive.</summary>
    private static MicrophoneState MicrophoneFromStream(AudioStreamState? state) => state switch
    {
        AudioStreamState.Streaming => MicrophoneState.Running,
        AudioStreamState.Idle => MicrophoneState.Stopped,
        _ => MicrophoneState.Unknown,
    };

    /// <summary>
    /// Follows the audio stream's changes between samples and keep-alives;
    /// a reply of the shell stands until the stream changes.
    /// </summary>
    private void FollowStream(AudioStreamState state)
    {
        if (state == _seenStreamState)
        {
            return;
        }
        _seenStreamState = state;
        if (state is AudioStreamState.Streaming or AudioStreamState.Idle)
        {
            bool started = state == AudioStreamState.Streaming && Microphone != MicrophoneState.Running;
            Microphone = MicrophoneFromStream(state);
            // A stream started from elsewhere: its volume and mute can be read now.
            if (started && _shell is not null)
            {
                _ = RefreshBoardStateAsync();
            }
        }
        else if (_shell is null)
        {
            Microphone = MicrophoneState.Unknown;
        }
    }

    // ---- the microphone's volume and mute, the hum and high-pass filters ----

    /// <summary>The volume field's value when the board last set it; the field differs from it while the user edits.</summary>
    private decimal? _shownVolume;
    /// <summary>The hum combo box is being set from the board's answer: no command is sent.</summary>
    private bool _humFromBoard;

    partial void OnMicrophoneChanged(MicrophoneState value)
    {
        // The board reads and sets the volume and mute only while the stream runs.
        if (value != MicrophoneState.Running)
        {
            ShowVolume(null, replaceEdit: true);
        }
    }

    /// <summary>
    /// Shows the board's volume and mute. The field is replaced only when it
    /// still shows the board's last value (the user has not edited it), when
    /// <paramref name="replaceEdit"/> is set (after Set), or when the volume
    /// cannot be read: an edit is never overwritten by a refresh.
    /// </summary>
    private void ShowVolume(VolumeReading? reading, bool replaceEdit)
    {
        BoardVolume = reading;
        bool edited = Volume != _shownVolume;
        decimal? board = reading?.Percent;
        if (!edited || replaceEdit || reading is not { Reported: true })
        {
            Volume = board;
        }
        _shownVolume = board;
    }

    private void ShowHum(string? hum)
    {
        _humFromBoard = true;
        try
        {
            Hum = hum;
        }
        finally
        {
            _humFromBoard = false;
        }
    }

    public bool CanSetVolume => IsShellConnected && IsMicrophoneOn && BoardVolume is { HasVolume: true };
    public bool CanToggleMute => IsShellConnected && IsMicrophoneOn && BoardVolume is { HasMute: true };
    public bool IsMuted => BoardVolume?.Muted == true;
    public bool IsHighPassOn => HighPass == HighPassState.On;
    public bool CanToggleHighPass => IsShellConnected && HighPass != HighPassState.Unknown;

    /// <summary>Why the volume or mute cannot be set now; null when it can be.</summary>
    private string? ControlsBlocked() =>
        !IsShellConnected ? "needs the shell"
        : !IsMicrophoneOn ? "the board reads and sets it only while the microphone runs: press Microphone first"
        : BoardVolume is null ? "not read from the board yet"
        : BoardVolume.Refusal is string refusal ? "the board says: " + refusal
        : null;

    public string VolumeTip
    {
        get
        {
            const string what = "The microphone's volume, percent of its range (uac volume <0..100>)";
            if (ControlsBlocked() is string blocked)
            {
                return $"{what}; {blocked}";
            }
            VolumeReading r = BoardVolume!;
            if (!r.HasVolume)
            {
                return what + "; the microphone has no volume control";
            }
            string percent = r.Percent is int p ? $"{p}%" : "?%";
            string db = r.Db is double d ? d.ToString("0.00", CultureInfo.InvariantCulture) + " dB" : "? dB";
            string set = r.SetPercent is int s ? $"set to {s}% in this session" : "the device's default";
            return $"{what}. Now {percent} ({db}), {set}. Set sends the field's value";
        }
    }

    public string MuteTip
    {
        get
        {
            const string what = "The microphone's mute, lit when muted (uac mute on|off)";
            if (ControlsBlocked() is string blocked)
            {
                return $"{what}; {blocked}";
            }
            return BoardVolume!.HasMute ? what : what + "; the microphone has no mute control";
        }
    }

    public string HumTip => IsShellConnected
        ? "The hum filter's mains frequency (uac hum 50|60|off); 50 after the board starts"
        : "The hum filter's mains frequency (uac hum 50|60|off); needs the shell";

    public string HighPassTip =>
        !IsShellConnected ? "The 30 Hz high-pass filter (DC removal), lit when on (uac hpf on|off); needs the shell"
        : HighPass == HighPassState.Unknown ? "The board's firmware does not have the command uac hpf (or its answer was not understood)"
        : "The 30 Hz high-pass filter (DC removal), lit when on (uac hpf on|off); on after the board starts";

    [RelayCommand]
    private async Task SetVolumeAsync()
    {
        if (Volume is not decimal v || v is < 0 or > 100)
        {
            Message = "The volume is 0 .. 100";
            return;
        }
        await RunStateCommandAsync(BoardCommand.UacVolume((int)v), replaceVolumeEdit: true);
    }

    [RelayCommand]
    private Task ToggleMuteAsync() => RunStateCommandAsync(BoardCommand.UacMute(!IsMuted));

    [RelayCommand]
    private Task ToggleHighPassAsync() => RunStateCommandAsync(BoardCommand.UacHpf(!IsHighPassOn));

    partial void OnHumChanged(string? value)
    {
        if (!_humFromBoard && value is not null)
        {
            _ = RunStateCommandAsync(BoardCommand.UacHum(value));
        }
    }

    // ---- recording -------------------------------------------------------

    public string RecordingsDirectory => _settings.ResolveRecordingsDirectory();

    [RelayCommand]
    private void ToggleRecord()
    {
        if (_writer is not null)
        {
            StopRecording();
            return;
        }
        try
        {
            string directory = RecordingsDirectory;
            Directory.CreateDirectory(directory);
            string stem = DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);
            string label = LabelForFileName(RecordingLabel);
            if (label.Length > 0)
            {
                stem += "_" + label;
            }
            string path = Path.Combine(directory, stem + ".wav");
            for (int n = 2; File.Exists(path); n++)
            {
                path = Path.Combine(directory, $"{stem}-{n}.wav");
            }
            WavWriter writer = new(path, _live.SampleRate);
            lock (_recordLock)
            {
                _writer = writer;
            }
            IsRecording = true;
            Message = $"Recording to {path}" + (_stream is null ? " (the audio stream is not connected)" : "");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Message = $"Cannot record: {e.Message}";
        }
    }

    /// <summary>The label as a part of a file name: letters, digits, '.', '-' and '_'; anything else becomes '-'.</summary>
    public static string LabelForFileName(string label) =>
        UnsafeInName().Replace(label.Trim(), "-").Trim('-', '.');

    [GeneratedRegex("[^A-Za-z0-9._-]+")]
    private static partial Regex UnsafeInName();

    private void StopRecording()
    {
        WavWriter? writer;
        lock (_recordLock)
        {
            writer = _writer;
            _writer = null;
        }
        IsRecording = false;
        RecordingText = "";
        if (writer is null)
        {
            return;
        }
        try
        {
            writer.Dispose();
            Message = string.Create(CultureInfo.InvariantCulture,
                $"Recorded {writer.Samples / (double)writer.SampleRate:F1} s to {writer.Path}") +
                (writer.Gaps == 0 ? "" : $" ({writer.Gaps} gaps, {writer.GapSamples} samples filled with zeros)");
        }
        catch (IOException e)
        {
            Message = $"Cannot finish the recording: {e.Message}";
        }
    }

    // ---- a WAV file in view ----------------------------------------------

    public async Task OpenFileAsync(string path)
    {
        FileSource file;
        try
        {
            file = await Task.Run(() => FileSource.Open(path));
        }
        catch (WavFormatException e)
        {
            Message = e.Message;
            return;
        }
        _file = file;
        IsFileShown = true;
        SourceText = string.Create(CultureInfo.InvariantCulture,
            $"{Path.GetFileName(path)}  ({file.End / (double)file.SampleRate:F1} s, {file.SampleRate} Hz)");
        Position = 0;
        UpdatePositionRange();
        Message = $"Showing {path}";
        AnalyzeFile();
        ViewChanged?.Invoke();
    }

    [RelayCommand]
    private void ShowLive()
    {
        _fileAnalysis?.Cancel();
        _fileClassifier = null;
        _fileM4 = null;
        _file = null;
        IsFileShown = false;
        SourceText = "Live";
        PositionMaximum = 0;
        ViewChanged?.Invoke();
    }

    // ---- the model -------------------------------------------------------

    /// <summary>Loads an ONNX model (.onnx) or the M4 swallow detector (.npz with the mel matrix beside it).</summary>
    public void LoadModel(string path)
    {
        UnloadModel();
        bool m4 = Path.GetExtension(path).Equals(".npz", StringComparison.OrdinalIgnoreCase);
        try
        {
            if (m4)
            {
                _m4 = M4Model.Load(path);
            }
            else
            {
                int window = ModelWindow is decimal w && w >= 1 ? (int)w : 16000;
                _model = new AudioModel(path, window);
            }
        }
        catch (Exception e) when (e is ModelContractException or IOException or UnauthorizedAccessException)
        {
            ModelText = e.Message;
            return;
        }
        IsM4Model = m4;
        ModelPath = path;
        _settings.ModelPath = path;
        MakeLiveClassifier();
        AnalyzeFile();
        Message = $"Model loaded: {path}";
    }

    [RelayCommand]
    private void UnloadModel()
    {
        AudioModel? model = _model;
        Task idle = ModelIdle();
        _liveClassifier = null;
        _fileClassifier = null;
        _model = null;
        _liveM4 = null;
        _fileM4 = null;
        _m4 = null;
        if (model is not null)
        {
            _ = DisposeWhenIdleAsync(model, idle);
        }
        Scores.Clear();
        TopClass = "";
        IsM4Model = false;
        IndicatorText = "";
        DetectorText = "";
        Indications.Clear();
        _listedIndications = -1;
        ModelText = "No model loaded";
    }

    /// <summary>Stops the analysis of a file; completes when no run uses the model any more.</summary>
    private Task ModelIdle()
    {
        _fileAnalysis?.Cancel();
        // Completes however the runs end: cancelled, failed or done.
        return Task.WhenAll(_fileAnalysisTask, _liveClassifier?.WhenIdle() ?? Task.CompletedTask,
                            _liveM4?.WhenIdle() ?? Task.CompletedTask)
            .ContinueWith(_ => { }, TaskScheduler.Default);
    }

    /// <summary>A run in progress still uses the session: it is closed once the runs have ended.</summary>
    private static async Task DisposeWhenIdleAsync(AudioModel model, Task idle)
    {
        await idle;
        model.Dispose();
    }

    [RelayCommand]
    private void ApplyModelSettings()
    {
        _settings.ModelHopSamples = ModelHop is decimal h && h >= 0 ? (int)h : 0;
        _settings.ModelWindowSamples = ModelWindow is decimal w && w >= 1 ? (int)w : 16000;
        if (_model is not null)
        {
            LoadModel(_model.Path);
        }
    }

    private int Hop(AudioModel model) => _settings.ModelHopSamples > 0 ? _settings.ModelHopSamples : Math.Max(1, model.WindowSamples / 2);

    private void MakeLiveClassifier()
    {
        _liveClassifier = _model is null ? null : new Classifier(_model, Hop(_model));
        _liveM4 = _m4 is null ? null : new M4Runner(_m4);
    }

    /// <summary>Runs the model over the whole of the opened file, in the background.</summary>
    private void AnalyzeFile()
    {
        _fileAnalysis?.Cancel();
        _fileClassifier = null;
        _fileM4 = null;
        Indications.Clear();
        _listedIndications = -1;
        if (_m4 is not null && _file is not null)
        {
            AnalyzeFileM4(_m4, _file);
            return;
        }
        if (_model is null || _file is null)
        {
            return;
        }
        Classifier classifier = new(_model, Hop(_model));
        CancellationTokenSource cts = new();
        _fileAnalysis = cts;
        _fileClassifier = classifier;
        FileSource file = _file;
        Progress<double> progress = new(p =>
        {
            if (_fileClassifier == classifier)
            {
                Message = string.Create(CultureInfo.InvariantCulture, $"Classifying {Path.GetFileName(file.Path)}: {p * 100:F0} %");
            }
        });
        _fileAnalysisTask = RunAnalysisAsync(classifier, file, progress, cts.Token);
    }

    private async Task RunAnalysisAsync(Classifier classifier, FileSource file, IProgress<double> progress, CancellationToken ct)
    {
        try
        {
            await classifier.AnalyzeAsync(file, progress, ct);
            if (_fileClassifier == classifier)
            {
                Message = classifier.Error is string error ? error
                    : $"Classified {Path.GetFileName(file.Path)}: {classifier.History.Count} windows";
            }
        }
        catch (OperationCanceledException)
        {
        }
    }

    /// <summary>Runs the M4 detector over the whole of the opened file, in the background.</summary>
    private void AnalyzeFileM4(M4Model model, FileSource file)
    {
        M4Runner runner = new(model);
        CancellationTokenSource cts = new();
        _fileAnalysis = cts;
        _fileM4 = runner;
        Progress<double> progress = new(p =>
        {
            if (_fileM4 == runner)
            {
                Message = string.Create(CultureInfo.InvariantCulture,
                    $"Running the swallow detector on {Path.GetFileName(file.Path)}: {p * 100:F0} %");
            }
        });
        _fileAnalysisTask = RunAnalysisM4Async(runner, file, progress, cts.Token);
    }

    private async Task RunAnalysisM4Async(M4Runner runner, FileSource file, IProgress<double> progress, CancellationToken ct)
    {
        try
        {
            await runner.AnalyzeAsync(file, progress, ct);
            if (_fileM4 == runner)
            {
                Message = runner.Error is string error ? error
                    : $"Swallow detector on {Path.GetFileName(file.Path)}: {runner.History.Count} windows, {runner.History.Indications} swallow indications";
            }
        }
        catch (OperationCanceledException)
        {
        }
    }

    /// <summary>Scrolls the file so that the chosen indication is in the middle of the view.</summary>
    partial void OnSelectedIndicationChanged(IndicationItem? value)
    {
        if (value is not null && _file is not null)
        {
            Position = Math.Clamp(value.Seconds - SpanSeconds / 2, 0, PositionMaximum);
        }
    }

    private static readonly Avalonia.Media.IBrush s_ledOff = new Avalonia.Media.SolidColorBrush(Avalonia.Media.Color.FromRgb(0x50, 0x50, 0x58));
    private static readonly Avalonia.Media.IBrush s_ledYellow = new Avalonia.Media.SolidColorBrush(Avalonia.Media.Color.FromRgb(0xE0, 0xB0, 0x30));
    private static readonly Avalonia.Media.IBrush s_ledGreen = new Avalonia.Media.SolidColorBrush(Avalonia.Media.Color.FromRgb(0x3F, 0xB9, 0x50));

    private static string Seconds(double s) => s < 60
        ? s.ToString("F2", CultureInfo.InvariantCulture) + " s"
        : string.Create(CultureInfo.InvariantCulture, $"{(int)(s / 60)}:{s % 60:00.00}");

    /// <summary>The time of an indication: the centre of its first positive window.</summary>
    private static double IndicationSeconds(M4Window rise) => (rise.Start + M4Model.WindowSamples / 2) / (double)M4Model.SampleRate;

    /// <summary>The M4 detector's state: the LED, the last probability, the indications, the model.</summary>
    private void ShowDetector(M4Model model)
    {
        M4Runner? runner = ViewM4Runner;
        if (runner is null)
        {
            return;
        }
        (long from, long to) = ViewRange();
        List<M4Window> inView = runner.History.Range(from, to);
        int inViewRises = inView.Count(w => w.Rise);
        M4Window? last;
        int led;
        string lastText;
        if (_file is null)
        {
            last = runner.History.Latest;
            // As the device: off when no window result came for a second.
            led = last is null || runner.SinceLastWindow > TimeSpan.FromSeconds(1) ? LedStepper.Off : last.Step.Led;
            M4Window? rise = runner.History.LastRise;
            lastText = rise is null ? "none yet"
                : Seconds(_live.End / (double)M4Model.SampleRate - IndicationSeconds(rise)) + " ago";
        }
        else
        {
            last = inView.Count > 0 ? inView[^1] : null;
            led = last?.Step.Led ?? LedStepper.Off;
            M4Window? rise = inView.LastOrDefault(w => w.Rise);
            lastText = rise is null ? "none in view" : "at " + Seconds(IndicationSeconds(rise));
            if (runner.History.Indications != _listedIndications)
            {
                _listedIndications = runner.History.Indications;
                Indications.Clear();
                foreach (M4Window w in runner.History.Rises(long.MinValue / 2, long.MaxValue / 2))
                {
                    Indications.Add(new IndicationItem(IndicationSeconds(w), Seconds(IndicationSeconds(w))));
                }
            }
        }
        (IndicatorText, IndicatorBrush) = led switch
        {
            LedStepper.Green => ("swallow indication", s_ledGreen),
            LedStepper.Yellow => ("running", s_ledYellow),
            _ => (_file is null && last is not null ? "off (no window for 1 s)" : "off", s_ledOff),
        };
        string probability = last is null ? "-"
            : last.Step.Valid ? last.Step.Probability.ToString("0.000", CultureInfo.InvariantCulture)
            : "not valid (lost samples)";
        DetectorText = string.Create(CultureInfo.InvariantCulture, $"""
            probability  {probability}
            indications  {inViewRises} in view, {runner.History.Indications} {(_file is null ? "since the start" : "in the file")}
            last         {lastText}
            """);

        M4Config c = model.Config;
        string post = c.Post == "none"
            ? string.Create(CultureInfo.InvariantCulture, $"threshold {c.Threshold}")
            : string.Create(CultureInfo.InvariantCulture,
                $"enter {c.Enter}, exit {c.Exit}, refractory {(c.RefractorySeconds is double r ? $"{r} s" : "none")}, cap {(c.CapSeconds is double cap ? $"{cap} s" : "none")}");
        string behind = _file is null && runner.Backlog > 0
            ? string.Create(CultureInfo.InvariantCulture, $", {runner.Backlog} behind") : "";
        string lagged = runner.Lagged > 0
            ? string.Create(CultureInfo.InvariantCulture, $", {runner.Lagged} not valid: the PC fell behind") : "";
        ModelText = string.Create(CultureInfo.InvariantCulture, $"""
            {Path.GetFileName(model.Path)} (M4 swallow detector)
            {c.Text}
            seeds    {string.Join(", ", c.Seeds)}
            post     {post}
            runs     {runner.Runs}{behind}{lagged}
            time     {runner.Milliseconds:F1} ms per window (mean {runner.MeanMilliseconds:F1})
            {(runner.Error is null ? "" : "error: " + runner.Error)}
            """);
    }

    private void ShowModel()
    {
        if (_m4 is M4Model m4)
        {
            ShowDetector(m4);
            return;
        }
        AudioModel? model = _model;
        Classifier? classifier = ViewClassifier;
        if (model is null || classifier is null)
        {
            return;
        }
        (long from, long to) = ViewRange();
        ClassResult? result = _file is null ? classifier.History.Latest : classifier.History.At(to);
        IReadOnlyList<string> labels = model.Labels;
        while (Scores.Count > labels.Count)
        {
            Scores.RemoveAt(Scores.Count - 1);
        }
        for (int i = 0; i < labels.Count; i++)
        {
            if (i == Scores.Count)
            {
                Scores.Add(new ScoreItem { Brush = Controls.ClassStripControl.BrushOf(i) });
            }
            float score = result is not null && i < result.Scores.Length ? result.Scores[i] : 0;
            Scores[i].Name = labels[i];
            Scores[i].Value = score;
            Scores[i].Text = score.ToString("0.000", CultureInfo.InvariantCulture);
        }
        TopClass = result is null ? "" : result.Top < labels.Count ? labels[result.Top] : $"class {result.Top}";
        ModelText = string.Create(CultureInfo.InvariantCulture, $"""
            {Path.GetFileName(model.Path)}
            window   {model.WindowSamples} samples ({model.WindowSamples / (double)AudioModel.SampleRate:F2} s){(model.DynamicWindow ? ", from the setting" : "")}
            step     {classifier.Hop} samples
            classes  {model.Classes} (names: {model.LabelSource})
            runs     {classifier.Runs}{(_file is null ? $", dropped {classifier.Dropped}" : "")}
            time     {classifier.Milliseconds:F1} ms per window
            {(classifier.Error is null ? "" : "error: " + classifier.Error)}
            """);
    }

    // ---- firmware update ---------------------------------------------------

    partial void OnImagePathChanged(string value) => ShowImageInfo();

    [RelayCommand]
    private void DefaultImage()
    {
        string? image = _settings.ResolveFirmwareImage();
        if (image is null)
        {
            ImageText = $"No build/{AppSettings.ImageName} found (RepositoryDirectory in settings.json, or upwards from the application)";
            return;
        }
        ImagePath = image;
    }

    private AppImage? ShowImageInfo()
    {
        if (string.IsNullOrWhiteSpace(ImagePath))
        {
            ImageText = "No image chosen";
            return null;
        }
        try
        {
            AppImage image = AppImage.Read(ImagePath);
            ImageText = string.Create(CultureInfo.InvariantCulture,
                $"{image.Project} {image.Version}, built {image.Built}\n{image.Bytes.Length} bytes, ELF SHA-256 {image.ElfShaHex}\nmodified {File.GetLastWriteTime(ImagePath):yyyy-MM-dd HH:mm:ss}");
            return image;
        }
        catch (AppImageException e)
        {
            ImageText = e.Message;
            return null;
        }
    }

    private void AppendOta(string line)
    {
        _otaLog.Append(DateTime.Now.ToString("HH:mm:ss ", CultureInfo.InvariantCulture)).Append(line).Append('\n');
        if (_otaLog.Length > OtaLogLimit)
        {
            _otaLog.Remove(0, _otaLog.Length - OtaLogLimit * 3 / 4);
        }
        OtaText = _otaLog.ToString();
    }

    [RelayCommand]
    private async Task UpdateFirmwareAsync()
    {
        if (_update is not null)
        {
            _update.Cancel();
            return;
        }
        AppImage? image = ShowImageInfo();
        if (image is null)
        {
            AppendOta("Not updated: " + ImageText);
            return;
        }
        if (Host() is not string host)
        {
            AppendOta("Not updated: enter the board's address (or press Find)");
            return;
        }
        bool shellWasConnected = _shell is not null;
        using CancellationTokenSource cts = new();
        _update = cts;
        IsUpdating = true;
        UpdateProgress = 0;
        AppendOta($"{image.Path}: {image.Project} {image.Version}, ELF SHA-256 {image.ElfShaHex}");
        AppendOta($"Updating {host}:{OtaClient.DefaultPort}");
        try
        {
            OtaClient client = new(host);
            OtaResult result = await client.UpdateAsync(image, new Progress<string>(AppendOta),
                                                        new Progress<double>(p => UpdateProgress = p * 100), cts.Token);
            Message = result.Outcome == OtaOutcome.Confirmed
                ? $"Firmware updated and confirmed ({result.Answer})"
                : $"Update not taken: another image runs ({result.Answer})";
            if (_stream is not null)
            {
                AppendOta("The audio stream connects again by itself; start the microphone again (USB host, Microphone)");
            }
            if (shellWasConnected && _shell is null)
            {
                AppendOta("Connecting the shell again");
                await ToggleShellAsync();
            }
        }
        catch (OperationCanceledException)
        {
            AppendOta("Cancelled: an image that is written but not confirmed goes back to the previous one at the next reset");
        }
        catch (Exception e) when (e is OtaException or IOException)
        {
            AppendOta("Failed: " + e.Message);
            Message = "Firmware update failed: " + e.Message;
        }
        finally
        {
            _update = null;
            IsUpdating = false;
        }
    }

    // ---- the frame ---------------------------------------------------------

    private void Tick()
    {
        if (_file is null)
        {
            _liveClassifier?.Offer(_live);
        }
        // The detector follows the live stream also while a file is shown: it needs every window.
        _liveM4?.Offer(_live);
        Frame?.Invoke();
        if (++_frame % 3 != 0)
        {
            return;
        }
        ShowModel();
        ShowLevel();
        ShowStatus();
        if (_frame % 30 == 0)
        {
            _liveClassifier?.History.TrimBefore(_live.Start);
            _liveM4?.History.TrimBefore(_live.Start);
            lock (_recordLock)
            {
                try
                {
                    _writer?.Flush();
                }
                catch (IOException)
                {
                }
            }
        }
    }

    private void ShowLevel()
    {
        ISampleSource source = ViewSource;
        (long from, long to) = ViewRange();
        // A file shorter than the span ends inside the view.
        to = Math.Min(to, source.End);
        int length = (int)Math.Max(0, to - from);
        if (_levelBuffer.Length < length)
        {
            _levelBuffer = new float[length];
        }
        Span<float> all = _levelBuffer.AsSpan(0, length);
        source.Read(from, all);
        Span<float> end = all[^Math.Min(source.SampleRate / 4, length)..];
        (double peak, double rms) = Level.Measure(end);
        (double viewPeak, double viewRms) = Level.Measure(all);
        LevelText = string.Create(CultureInfo.InvariantCulture, $"""
                          peak     RMS      DC
                        [dBFS]  [dBFS]   [LSB]
            end 0.25 s  {Db(peak),6} {Db(rms),7} {Level.DcLsb(end),7:F1}
            whole view  {Db(viewPeak),6} {Db(viewRms),7} {Level.DcLsb(all),7:F1}
            """);
    }

    private static string Db(double db) => double.IsNegativeInfinity(db) ? "-inf" : db.ToString("F1", CultureInfo.InvariantCulture);

    private void ShowStatus()
    {
        StringBuilder status = new();
        AudioStreamClient? client = _stream;
        if (client is null)
        {
            StreamState = "not connected";
            status.Append("Stream: not connected");
        }
        else
        {
            AudioStreamStatus s = client.Status();
            FollowStream(s.State);
            StreamState = s.State switch
            {
                AudioStreamState.Connecting => "connecting",
                AudioStreamState.Connected => "connected",
                AudioStreamState.Idle => "idle (no stream on the board)",
                AudioStreamState.Streaming => "streaming",
                AudioStreamState.Waiting => "no connection, retrying",
                _ => "stopped",
            };
            status.Append(CultureInfo.InvariantCulture, $"Stream {client.Host}:{client.Port}: {StreamState}");
            if (s.Header is AudioStreamHeader h)
            {
                status.Append(CultureInfo.InvariantCulture, $"  |  {h.SampleRate} Hz, {s.SamplesPerSecond:F0} samples/s");
            }
            status.Append(CultureInfo.InvariantCulture,
                $"  |  received {s.Samples} samples in {s.Blocks} blocks  |  lost {s.LostSamples} in {s.Gaps} gaps  |  new streams {s.NewStreams}  |  connections {s.Connections}");
            if (s.State == AudioStreamState.Waiting && s.Reason is string reason)
            {
                status.Append("  |  ").Append(reason);
            }
        }
        if (_liveM4 is M4Runner m4 && (m4.Backlog >= 4 || m4.Lagged > 0))
        {
            status.Append(CultureInfo.InvariantCulture, $"  |  swallow detector {m4.Backlog} windows behind the stream");
            if (m4.Lagged > 0)
            {
                status.Append(CultureInfo.InvariantCulture, $", {m4.Lagged} windows not valid: the PC fell behind");
            }
        }
        StreamStatus = status.ToString();
        lock (_recordLock)
        {
            if (_writer is WavWriter w)
            {
                RecordingText = string.Create(CultureInfo.InvariantCulture,
                    $"{w.Samples / (double)w.SampleRate:F1} s{(w.Gaps == 0 ? "" : $", {w.GapSamples} lost")}");
            }
        }
    }

    public void Dispose()
    {
        _timer.Stop();
        _update?.Cancel();
        Task idle = ModelIdle();
        StopRecording();
        _stream?.Dispose();
        _shell?.Dispose();
        if (_model is not null)
        {
            // A window takes milliseconds; the wait only covers a run that is under way.
            idle.Wait(TimeSpan.FromSeconds(2));
            _model.Dispose();
        }
        _settings.Save();
    }
}
