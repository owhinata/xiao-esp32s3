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
    [NotifyPropertyChangedFor(nameof(CanToggleMicrophone), nameof(MicrophoneTip))]
    private bool _isShellConnected;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsUsbHostOn), nameof(CanToggleMicrophone), nameof(MicrophoneTip))]
    private UsbHostState _usbHost;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsMicrophoneOn), nameof(CanToggleMicrophone), nameof(MicrophoneTip))]
    private MicrophoneState _microphone;
    [ObservableProperty] private string _shellText = "";
    [ObservableProperty] private string _shellInput = "";
    [ObservableProperty] private bool _isShellPaneVisible;
    [ObservableProperty] private decimal? _volume = 50;
    [ObservableProperty] private bool _isMuted;
    [ObservableProperty] private string? _hum;
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

    /// <summary>The classifier of what is in view, if a model is loaded.</summary>
    public Classifier? ViewClassifier => _file is null ? _liveClassifier : _fileClassifier;

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

    /// <summary>Runs a button's command, shows the board's answer, and asks the board for its state again.</summary>
    private async Task RunStateCommandAsync(BoardCommand command)
    {
        if (await RunAsync(command.Line) is not string output)
        {
            return;
        }
        List<string> lines = BoardShell.OutputLines(output, command.Line);
        Message = $"{command.Line}: " + (command.Failure(lines) ?? string.Join(" ", lines));
        await RefreshBoardStateAsync();
    }

    /// <summary>
    /// Asks the board for the USB host's and the microphone's state with
    /// queries that the shell pane does not show; they wait behind a command
    /// that is running.
    /// </summary>
    private async Task RefreshBoardStateAsync()
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
            if (_shell != shell)
            {
                return;
            }
            UsbHost = host;
            Microphone = microphone;
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
            Microphone = MicrophoneFromStream(state);
        }
        else if (_shell is null)
        {
            Microphone = MicrophoneState.Unknown;
        }
    }

    [RelayCommand]
    private async Task SetVolumeAsync()
    {
        if (Volume is not decimal v || v is < 0 or > 100)
        {
            Message = "The volume is 0 .. 100";
            return;
        }
        await RunAsync(string.Create(CultureInfo.InvariantCulture, $"uac volume {(int)v}"));
    }

    [RelayCommand]
    private async Task ToggleMuteAsync()
    {
        bool mute = !IsMuted;
        // The board answers "Mute on" or "Mute off" when it took the setting.
        if (await RunAsync(mute ? "uac mute on" : "uac mute off") is string output &&
            output.Contains(mute ? "Mute on" : "Mute off", StringComparison.Ordinal))
        {
            IsMuted = mute;
        }
    }

    partial void OnHumChanged(string? value)
    {
        if (value is not null)
        {
            _ = RunAsync("uac hum " + value);
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
        _file = null;
        IsFileShown = false;
        SourceText = "Live";
        PositionMaximum = 0;
        ViewChanged?.Invoke();
    }

    // ---- the model -------------------------------------------------------

    public void LoadModel(string path)
    {
        UnloadModel();
        try
        {
            int window = ModelWindow is decimal w && w >= 1 ? (int)w : 16000;
            _model = new AudioModel(path, window);
        }
        catch (Exception e) when (e is ModelContractException or IOException or UnauthorizedAccessException)
        {
            ModelText = e.Message;
            return;
        }
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
        if (model is not null)
        {
            _ = DisposeWhenIdleAsync(model, idle);
        }
        Scores.Clear();
        TopClass = "";
        ModelText = "No model loaded";
    }

    /// <summary>Stops the analysis of a file; completes when no run uses the model any more.</summary>
    private Task ModelIdle()
    {
        _fileAnalysis?.Cancel();
        // Completes however the runs end: cancelled, failed or done.
        return Task.WhenAll(_fileAnalysisTask, _liveClassifier?.WhenIdle() ?? Task.CompletedTask)
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
    }

    /// <summary>Runs the model over the whole of the opened file, in the background.</summary>
    private void AnalyzeFile()
    {
        _fileAnalysis?.Cancel();
        _fileClassifier = null;
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

    private void ShowModel()
    {
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
        (double peak, double rms) = Level.Measure(all[^Math.Min(source.SampleRate / 4, length)..]);
        (double viewPeak, double viewRms) = Level.Measure(all);
        LevelText = string.Create(CultureInfo.InvariantCulture, $"""
                          peak     RMS  [dBFS]
            end 0.25 s  {Db(peak),6} {Db(rms),7}
            whole view  {Db(viewPeak),6} {Db(viewRms),7}
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
