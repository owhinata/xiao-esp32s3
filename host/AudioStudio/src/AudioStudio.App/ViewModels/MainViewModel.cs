// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using AudioStudio.Core.Audio;
using AudioStudio.Core.Board;
using AudioStudio.Core.Dsp;
using AudioStudio.Core.Marks;
using AudioStudio.Core.Model;
using AudioStudio.Core.Ota;
using AudioStudio.Core.Settings;
using AudioStudio.Core.Ssh;
using AudioStudio.Core.Stream;
using AudioStudio.Core.Targets;
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

/// <summary>One mark of the recording in view, for the list of marks.</summary>
/// <param name="Index">The mark's place in the sorted marks.</param>
public sealed record MarkItem(int Index, double Seconds, string Text);

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
    private TargetProfile _profile;
    /// <summary>The ssh connection of an ssh target, for its address; shared by the shell and the stream.</summary>
    private SshConnection? _ssh;
    private AudioStreamClient? _stream;
    private volatile LiveSource _live = new(16000);
    private BoardShell? _shell;
    private WavWriter? _writer;
    /// <summary>The live positions of the recording's samples; set under <see cref="_recordLock"/> with the writer.</summary>
    private RecordingTimeline? _timeline;
    /// <summary>The marks of the recording in progress, or of the last one (shown on the live view).</summary>
    private MarkLog? _marks;
    private IndicationTracker? _tracker;
    private M4Runner? _trackedRunner;
    /// <summary>The marks of the file in view when they cannot be edited (the recording in progress).</summary>
    private List<Mark> _fileMarks = [];
    /// <summary>The marks of the file in view when they can be edited; null otherwise.</summary>
    private MarkEditor? _editor;
    /// <summary>The mark selected for editing (an index into the editor's marks); null when none is.</summary>
    private int? _selectedIndex;
    /// <summary>The position picked in the views of the file, for a new mark or a move.</summary>
    private long? _pickedSample;
    /// <summary>The list of marks is being filled: its selection changes are not the user's.</summary>
    private bool _showingMarks;
    /// <summary>The editor's fields are being filled from the selected mark: their changes are not the user's.</summary>
    private bool _loadingEditor;
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
    /// <summary>The target profile chosen: a name in <c>targets/</c>, or the path of a profile.</summary>
    [ObservableProperty] private string _target;
    [ObservableProperty] private string _boardAddress;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanChangeTarget), nameof(CanEditAddress))]
    private bool _isStreamConnected;
    [ObservableProperty] private string _streamState = "not connected";
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanToggleMicrophone), nameof(MicrophoneTip), nameof(CanSetVolume),
                              nameof(VolumeTip), nameof(CanToggleMute), nameof(MuteTip), nameof(HumTip),
                              nameof(CanToggleHighPass), nameof(HighPassTip), nameof(CanChangeTarget),
                              nameof(CanEditAddress))]
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
    /// <summary>The memo box of a <c>false</c> or <c>other</c> mark is open.</summary>
    [ObservableProperty] private bool _isMemoOpen;
    [ObservableProperty] private string _memoText = "";
    [ObservableProperty] private string _memoHint = "";
    /// <summary>The counts of the marks in view: indications, marked wrong, hits, missed swallows.</summary>
    [ObservableProperty] private string _marksSummary = "";
    [ObservableProperty] private MarkItem? _selectedMark;
    [ObservableProperty] private bool _hasMarks;
    /// <summary>The marks of the file in view can be edited.</summary>
    [ObservableProperty] private bool _canEditMarks;
    /// <summary>What the editor works on: the selected mark and the picked position.</summary>
    [ObservableProperty] private string _markEditorText = "";
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanMoveMark))]
    private bool _isUserMarkSelected;
    [ObservableProperty] private bool _isIndicationSelected;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanMoveMark))]
    private bool _hasPick;
    /// <summary>The kind of the selected user's mark (swallow, false, other); null for an indication or none.</summary>
    [ObservableProperty] private string? _editKind;
    /// <summary>The text of the selected user's mark as typed; stored with Enter or when the box loses the focus.</summary>
    [ObservableProperty] private string _editText = "";
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
    /// <summary>The spike filter runs before the M4 detector's input.</summary>
    [ObservableProperty] private bool _isSpikeFilterOn;
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
    /// <summary>The target profiles to choose from: those beside the application, and one given by its path.</summary>
    public ObservableCollection<string> Targets { get; } = new();
    public ObservableCollection<ScoreItem> Scores { get; } = new();
    /// <summary>The swallow indications of the file in view.</summary>
    public ObservableCollection<IndicationItem> Indications { get; } = new();
    /// <summary>The marks of the file in view, or of the recording on the live view.</summary>
    public ObservableCollection<MarkItem> Marks { get; } = new();
    /// <summary>Changes whenever the marks to draw change.</summary>
    public int MarksVersion { get; private set; }
    public List<string> ShellHistory { get; } = new();
    /// <summary>The kinds a user's mark can be given.</summary>
    public IReadOnlyList<string> UserMarkKinds { get; } =
        [.. new[] { MarkKind.Swallow, MarkKind.False, MarkKind.Other }.Select(MarkFile.KindName)];
    public bool CanMoveMark => IsUserMarkSelected && HasPick;
    /// <summary>The mark selected for editing, an index into the marks <see cref="MarksInView"/> gives; null when none is.</summary>
    public int? SelectedMarkIndex => _selectedIndex;
    /// <summary>The position picked in the views of the file; null when none is.</summary>
    public long? PickedSample => _pickedSample;

    /// <summary>Raised on the UI thread at the frame rate, for the views to draw what <see cref="ViewSource"/> holds.</summary>
    public event Action? Frame;
    /// <summary>The view settings changed: the pictures are made anew.</summary>
    public event Action? ViewChanged;
    /// <summary>The memo box opened for a new mark (also when it was open for the mark before).</summary>
    public event Action? MemoOpened;
    /// <summary>A mark of the file was made and its text can be typed.</summary>
    public event Action? MarkTextWanted;

    /// <param name="board">Board address to connect the audio stream to at once.</param>
    /// <param name="shell">Connect the shell at once too.</param>
    /// <param name="target">The target profile (a name or a path); otherwise the one of the settings.</param>
    /// <param name="spikeFilter">The spike filter on or off; otherwise as the settings say.</param>
    /// <param name="mainsHz">The spike filter's mains frequency (50 or 60); otherwise the settings'.</param>
    public MainViewModel(string? board = null, bool shell = false, string? image = null, string? target = null,
                         bool? spikeFilter = null, double? mainsHz = null)
    {
        _settings.SpikeFilter = spikeFilter ?? _settings.SpikeFilter;
        _settings.MainsHz = mainsHz ?? (_settings.MainsHz is 50 or 60 ? _settings.MainsHz : MainsSpike.DefaultMainsHz);
        _isSpikeFilterOn = _settings.SpikeFilter;
        foreach (string name in TargetProfile.Available())
        {
            Targets.Add(name);
        }
        string wanted = target ?? _settings.Target;
        TargetProfile? profile = LoadProfile(wanted);
        if (profile is null && wanted != TargetProfile.DefaultName)
        {
            string why = Message;
            profile = LoadProfile(TargetProfile.DefaultName);
            Message = $"{why}; using {TargetProfile.DefaultName}";
        }
        _profile = profile ?? throw new InvalidOperationException(Message);
        _target = NameOf(profile);
        if (!Targets.Contains(_target))
        {
            Targets.Add(_target);
        }
        _settings.Target = _target;
        _boardAddress = board ?? AddressOf(_profile);
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

    // ---- the target --------------------------------------------------------

    /// <summary>The profile of a name or path; null (and the reason in <see cref="Message"/>) when it cannot be read.</summary>
    private TargetProfile? LoadProfile(string nameOrPath)
    {
        try
        {
            return TargetProfile.Resolve(nameOrPath);
        }
        catch (TargetProfileException e)
        {
            Message = e.Message;
            return null;
        }
    }

    /// <summary>How the selector names a profile: its name when it is the one beside the application, else its path.</summary>
    private static string NameOf(TargetProfile profile) =>
        Path.GetFullPath(Path.Combine(TargetProfile.DefaultDirectory, profile.Name + ".json")) == profile.FilePath
            ? profile.Name : profile.FilePath;

    /// <summary>The address entered for a target before, else its profile's default.</summary>
    private string AddressOf(TargetProfile profile) =>
        _settings.Addresses.TryGetValue(profile.Name, out string? address) ? address : profile.Address.Default;

    /// <summary>The target can be changed while nothing is connected to it.</summary>
    public bool CanChangeTarget => !IsStreamConnected && !IsShellConnected;

    /// <summary>The address is fixed while the stream runs, and while an ssh shell runs (it uses the ssh connection).</summary>
    public bool CanEditAddress => !IsStreamConnected && !(IsShellConnected && IsSshTarget);

    partial void OnTargetChanged(string value)
    {
        if (value is null || NameOf(_profile) == value)
        {
            return;
        }
        if (!CanChangeTarget || LoadProfile(value) is not TargetProfile profile)
        {
            // Back to the target in use, after the selector's own change.
            string current = NameOf(_profile);
            Dispatcher.UIThread.Post(() => Target = current);
            return;
        }
        _settings.Addresses[_profile.Name] = BoardAddress.Trim();
        _profile = profile;
        _settings.Target = value;
        _ssh?.Dispose();
        _ssh = null;
        BoardAddress = AddressOf(profile);
        UsbHost = UsbHostState.Unknown;
        Microphone = MicrophoneState.Unknown;
        ShowVolume(null, replaceEdit: true);
        ShowHum(null);
        HighPass = HighPassState.Unknown;
        if (!HasOta)
        {
            SelectedView = AudioView;
        }
        foreach (string name in (string[])[nameof(IsSshTarget), nameof(CanEditAddress), nameof(HasFind),
                                           nameof(AddressHint), nameof(StreamTip), nameof(ShellTip), nameof(HasUsbHost),
                                           nameof(UsbHostTip), nameof(HasMicrophone), nameof(CanToggleMicrophone),
                                           nameof(MicrophoneTip), nameof(HasVolume), nameof(VolumeMinimum),
                                           nameof(VolumeMaximum), nameof(CanSetVolume), nameof(VolumeTip),
                                           nameof(HasMute), nameof(CanToggleMute), nameof(MuteTip), nameof(HasHum),
                                           nameof(HumOptions), nameof(HumTip), nameof(HasHighPass),
                                           nameof(CanToggleHighPass), nameof(HighPassTip), nameof(HasBoardSettings),
                                           nameof(HasOta)])
        {
            OnPropertyChanged(name);
        }
        Message = $"Target: {(profile.Title.Length > 0 ? profile.Title : profile.Name)} ({profile.FilePath})";
    }

    public bool IsSshTarget => _profile.Shell.Transport == ShellTransport.Ssh;
    public bool HasFind => _profile.Address.Find;
    public string AddressHint => _profile.Address.Hint;
    public bool HasUsbHost => _profile.Controls.UsbHost is not null;
    public bool HasMicrophone => _profile.Controls.Microphone is not null;
    public bool HasVolume => _profile.Controls.Volume is not null;
    public bool HasMute => _profile.Controls.Mute is not null;
    public bool HasHum => _profile.Controls.Hum is not null;
    public bool HasHighPass => _profile.Controls.HighPass is not null;
    public bool HasBoardSettings => HasVolume || HasMute || HasHum || HasHighPass;
    /// <summary>The target takes firmware updates: the Firmware tab is shown.</summary>
    public bool HasOta => _profile.Ota is not null;

    public string StreamTip => _profile.Stream.Transport == StreamTransport.Tcp
        ? string.Create(CultureInfo.InvariantCulture, $"Connect or disconnect the audio stream (TCP port {_profile.Stream.Port})")
        : $"Connect or disconnect the audio stream: runs \"{_profile.Stream.Command}\" on the target over ssh (connecting starts the capture, disconnecting stops it)";

    public string ShellTip => _profile.Shell.Tip;

    /// <summary>
    /// The ssh connection to the destination in the address field: the one
    /// in use, or a new one. Null (and the reason in <see cref="Message"/>)
    /// when the field is not an ssh destination.
    /// </summary>
    private SshConnection? Ssh()
    {
        string destination = BoardAddress.Trim();
        string executable = SshConnection.ResolveExecutable(_settings.SshPath);
        if (_ssh is not null && _ssh.Destination == destination && _ssh.Executable == executable)
        {
            return _ssh;
        }
        if (SshConnection.CheckDestination(destination) is string error)
        {
            Message = error;
            return null;
        }
        _ssh?.Dispose();
        try
        {
            _ssh = new SshConnection(destination, executable);
        }
        catch (Exception e) when (e is SshException or IOException or UnauthorizedAccessException)
        {
            _ssh = null;
            Message = e.Message;
        }
        return _ssh;
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
        if (string.IsNullOrWhiteSpace(BoardAddress) && HasFind)
        {
            await FindBoardAsync();
        }
        ConnectStream();
    }

    private void ConnectStream()
    {
        IAudioStreamSource source;
        if (_profile.Stream.Transport == StreamTransport.Tcp)
        {
            if (Host() is not string host)
            {
                Message = HasFind ? "Enter the board's address (or press Find)" : "Enter the board's address";
                return;
            }
            source = new TcpAudioSource(host, _profile.Stream.Port!.Value);
        }
        else
        {
            if (Ssh() is not SshConnection ssh)
            {
                return;
            }
            SshAudioSource command = new(ssh, _profile.Stream.Command!);
            command.ErrorLine += line => Dispatcher.UIThread.Post(() => Message = "Audio stream: " + line);
            source = command;
        }
        AudioStreamClient client = new(source);
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
        _settings.Addresses[_profile.Name] = BoardAddress.Trim();
        Message = $"Audio stream: connecting to {source.Name}";
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
        long blockStart = live.End - block.Samples.Length;
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
                    _timeline?.Anchor(blockStart, writer.Samples);
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
            BoardShell shell;
            if (_profile.Shell.Transport == ShellTransport.Telnet)
            {
                if (string.IsNullOrWhiteSpace(BoardAddress) && HasFind)
                {
                    BoardAddress = await BoardLocator.FindAsync();
                }
                // "address" or "address:port"; the port is for a shell that is not on the profile's Telnet port.
                string[] parts = BoardAddress.Trim().Split(':');
                int port = _profile.Shell.Port!.Value;
                if (parts.Length > 2 || (parts.Length == 2 && (!int.TryParse(parts[1], out port) || port is < 1 or > 65535)))
                {
                    Message = "The board address is not of the form address or address:port";
                    return;
                }
                shell = await BoardShell.ConnectAsync(parts[0], port, _profile.Prompt);
            }
            else
            {
                if (Ssh() is not SshConnection ssh)
                {
                    return;
                }
                Message = $"Shell: connecting to {ssh.Destination} with ssh";
                shell = await BoardShell.OpenSshAsync(ssh, _profile.Prompt);
            }
            shell.Output += text => Dispatcher.UIThread.Post(() => AppendShell(text));
            shell.Closed += reason => Dispatcher.UIThread.Post(() => OnShellClosed(shell, reason));
            _shell = shell;
            IsShellConnected = true;
            _settings.Addresses[_profile.Name] = BoardAddress.Trim();
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

    /// <summary>Runs a line typed in the shell pane and shows it there. Null when it could not be sent.</summary>
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
        // A command of the profile's "refreshAfter" (the XIAO's "usb" and
        // "uac") can change what the buttons show. One that is still printing
        // after the wait is left alone: a query sent now would take its output.
        if (await RunAsync(line) is string output && _shell?.Prompt.Finished(output) == true &&
            _profile.Shell.RefreshAfter.Contains(line.Split(' ', 2)[0]))
        {
            _ = RefreshBoardStateAsync();
        }
    }

    // ---- the board's USB host and microphone -------------------------------

    public bool IsUsbHostOn => UsbHost == UsbHostState.Running;
    public bool IsMicrophoneOn => Microphone == MicrophoneState.Running;

    /// <summary>The microphone needs the shell, and the USB host to start it where the profile says so.</summary>
    public bool CanToggleMicrophone =>
        IsShellConnected && (_profile.Controls.Microphone?.RequiresUsbHost != true ||
                             UsbHost == UsbHostState.Running || Microphone == MicrophoneState.Running);

    public string UsbHostTip => _profile.Controls.UsbHost?.Tip + "; needs the shell";

    public string MicrophoneTip
    {
        get
        {
            string what = _profile.Controls.Microphone?.Tip ?? "";
            return !IsShellConnected ? what + "; needs the shell"
                : !CanToggleMicrophone ? "The microphone needs the USB host: press USB host first"
                : what;
        }
    }

    [RelayCommand]
    private Task ToggleUsbHostAsync() => RunStateCommandAsync(_profile.State.UsbHost(!IsUsbHostOn));

    [RelayCommand]
    private Task ToggleMicrophoneAsync() => RunStateCommandAsync(_profile.State.Microphone(!IsMicrophoneOn));

    /// <summary>
    /// Runs a button's command, shows the board's answer, and asks the board
    /// for its state again. <paramref name="replaceVolumeEdit"/>: the volume
    /// field then shows the board's volume even if it was edited.
    /// </summary>
    private async Task RunStateCommandAsync(BoardCommand command, bool replaceVolumeEdit = false)
    {
        BoardShell? shell = _shell;
        if (shell is null)
        {
            Message = "The shell is not connected";
            return;
        }
        AppendShell(command.Line);
        IReadOnlyList<string> lines;
        try
        {
            lines = await shell.CommandAsync(command.Line, CommandWait);
        }
        catch (Exception e) when (e is BoardException or ObjectDisposedException)
        {
            Message = e.Message;
            return;
        }
        Message = $"{command.Line}: " + (command.Failure(lines) ?? string.Join(" ", lines));
        await RefreshBoardStateAsync(replaceVolumeEdit);
    }

    /// <summary>
    /// Asks the board for what its profile has of: the USB host's and the
    /// microphone's state, the microphone's volume and mute (on the XIAO
    /// only while its stream runs: the board reads them only then), the hum
    /// filter and the high-pass filter, with queries that the shell pane
    /// does not show; over Telnet they wait behind a command that is running.
    /// </summary>
    private async Task RefreshBoardStateAsync(bool replaceVolumeEdit = false)
    {
        BoardShell? shell = _shell;
        if (shell is null)
        {
            return;
        }
        BoardState state = _profile.State;
        ControlsSpec controls = _profile.Controls;
        try
        {
            UsbHostState host = state.UsbHostQuery is string usbQuery
                ? state.ParseUsbHost(await shell.QueryAsync(usbQuery, CommandWait))
                : UsbHostState.Unknown;
            MicrophoneState microphone = MicrophoneFromStream(_stream?.Status().State);
            if (state.MicrophoneQuery is string micQuery)
            {
                // A host that does not run has no stream.
                microphone = controls.Microphone!.RequiresUsbHost && host == UsbHostState.Stopped
                    ? MicrophoneState.Stopped
                    : state.ParseMicrophone(await shell.QueryAsync(micQuery, CommandWait));
            }
            VolumeReading? volume = null;
            if (state.VolumeQuery is string volumeQuery &&
                (!controls.Volume!.WhileMicrophone || microphone == MicrophoneState.Running))
            {
                volume = state.ParseVolume(await shell.QueryAsync(volumeQuery, CommandWait));
            }
            string? hum = state.HumQuery is string humQuery
                ? state.ParseHum(await shell.QueryAsync(humQuery, CommandWait))
                : null;
            HighPassState highPass = state.HighPassQuery is string hpfQuery
                ? state.ParseHighPass(await shell.QueryAsync(hpfQuery, CommandWait))
                : HighPassState.Unknown;
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
        // The XIAO reads and sets the volume and mute only while the stream runs.
        if (value != MicrophoneState.Running && _profile.Controls.Volume?.WhileMicrophone == true)
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

    /// <summary>The volume and mute can be read now: always, or on the XIAO while the microphone stream runs.</summary>
    private bool VolumeReadable => _profile.Controls.Volume?.WhileMicrophone != true || IsMicrophoneOn;

    public bool CanSetVolume => IsShellConnected && VolumeReadable && BoardVolume is { HasVolume: true };
    public bool CanToggleMute => IsShellConnected && VolumeReadable && BoardVolume is { HasMute: true };
    public bool IsMuted => BoardVolume?.Muted == true;
    public bool IsHighPassOn => HighPass == HighPassState.On;
    public bool CanToggleHighPass => IsShellConnected && HighPass != HighPassState.Unknown;
    public int VolumeMinimum => _profile.Controls.Volume?.Minimum ?? 0;
    public int VolumeMaximum => _profile.Controls.Volume?.Maximum ?? 100;
    public IReadOnlyList<string> HumOptions => _profile.Controls.Hum?.Options ?? [];

    /// <summary>Why the volume or mute cannot be set now; null when it can be.</summary>
    private string? ControlsBlocked() =>
        !IsShellConnected ? "needs the shell"
        : !VolumeReadable ? "the board reads and sets it only while the microphone runs: press Microphone first"
        : BoardVolume is null ? "not read from the board yet"
        : BoardVolume.Refusal is string refusal ? "the board says: " + refusal
        : null;

    public string VolumeTip
    {
        get
        {
            string what = _profile.Controls.Volume?.Tip ?? "";
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
            string what = _profile.Controls.Mute?.Tip ?? "";
            if (ControlsBlocked() is string blocked)
            {
                return $"{what}; {blocked}";
            }
            return BoardVolume!.HasMute ? what : what + "; the microphone has no mute control";
        }
    }

    public string HumTip
    {
        get
        {
            ChoiceSpec? hum = _profile.Controls.Hum;
            return !IsShellConnected ? hum?.Tip + "; needs the shell"
                : hum?.Note is string note ? $"{hum.Tip}; {note}"
                : hum?.Tip ?? "";
        }
    }

    public string HighPassTip
    {
        get
        {
            ToggleSpec? hpf = _profile.Controls.HighPass;
            return !IsShellConnected ? hpf?.Tip + "; needs the shell"
                : HighPass == HighPassState.Unknown ? hpf?.UnknownTip ?? hpf?.Tip ?? ""
                : hpf?.Note is string note ? $"{hpf.Tip}; {note}"
                : hpf?.Tip ?? "";
        }
    }

    [RelayCommand]
    private async Task SetVolumeAsync()
    {
        if (Volume is not decimal v || v < VolumeMinimum || v > VolumeMaximum)
        {
            Message = string.Create(CultureInfo.InvariantCulture, $"The volume is {VolumeMinimum} .. {VolumeMaximum}");
            return;
        }
        await RunStateCommandAsync(_profile.State.Volume((int)v), replaceVolumeEdit: true);
    }

    [RelayCommand]
    private Task ToggleMuteAsync() => RunStateCommandAsync(_profile.State.Mute(!IsMuted));

    [RelayCommand]
    private Task ToggleHighPassAsync() => RunStateCommandAsync(_profile.State.HighPass(!IsHighPassOn));

    partial void OnHumChanged(string? value)
    {
        if (!_humFromBoard && value is not null)
        {
            _ = RunStateCommandAsync(_profile.State.Hum(value));
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
            RecordingTimeline timeline = new();
            MarkLog marks = new(MarkFile.PathFor(path), _live.SampleRate);
            lock (_recordLock)
            {
                _writer = writer;
                _timeline = timeline;
            }
            _marks = marks;
            _tracker = new IndicationTracker(marks, timeline);
            _trackedRunner = null;
            IsRecording = true;
            ShowMarks();
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
        TrackIndications();
        _tracker?.Close();
        _tracker = null;
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
        MarkLog? marks = _marks;
        marks?.Finish(IsMemoOpen ? MemoText : "");
        CloseMemo();
        ShowMarks();
        string marked = marks is null || marks.Marks.Count == 0 ? ""
            : marks.Error is string error ? $"; the marks could not be written: {error}"
            : string.Create(CultureInfo.InvariantCulture, $"; {marks.Marks.Count} marks in {Path.GetFileName(marks.Path)}");
        try
        {
            writer.Dispose();
            Message = string.Create(CultureInfo.InvariantCulture,
                $"Recorded {writer.Samples / (double)writer.SampleRate:F1} s to {writer.Path}") +
                (writer.Gaps == 0 ? "" : $" ({writer.Gaps} gaps, {writer.GapSamples} samples filled with zeros)") + marked;
        }
        catch (IOException e)
        {
            Message = $"Cannot finish the recording: {e.Message}";
        }
    }

    // ---- marks -------------------------------------------------------------

    [RelayCommand]
    private void MarkSwallow() => AddMark(MarkKind.Swallow);

    [RelayCommand]
    private void MarkFalse() => AddMark(MarkKind.False);

    [RelayCommand]
    private void MarkOther() => AddMark(MarkKind.Other);

    /// <summary>
    /// A mark of the user at the recording's current sample: the moment of
    /// the key press or click. A memo still open keeps what was typed. For
    /// <c>false</c> and <c>other</c> the memo box opens for the new mark.
    /// </summary>
    public void AddMark(MarkKind kind)
    {
        long sample;
        lock (_recordLock)
        {
            if (_writer is null)
            {
                return;
            }
            sample = _writer.Samples;
        }
        if (_marks is not MarkLog log)
        {
            return;
        }
        log.Add(sample, kind, IsMemoOpen ? MemoText : "");
        MemoText = "";
        string at = Seconds(sample / (double)log.SampleRate);
        if (log.MemoMark is not null)
        {
            MemoHint = $"memo for the {MarkFile.KindName(kind)} mark at {at} (Enter stores it, Esc leaves it empty)";
            IsMemoOpen = true;
            MemoOpened?.Invoke();
        }
        else
        {
            IsMemoOpen = false;
        }
        Message = log.Error is string error ? $"The marks could not be written: {error}"
            : $"Mark {MarkFile.KindName(kind)} at {at}";
        ShowMarks();
    }

    /// <summary>Enter in the memo box.</summary>
    public void StoreMemo()
    {
        _marks?.StoreMemo(MemoText);
        CloseMemo();
        ShowMarks();
    }

    /// <summary>Escape in the memo box: the mark stays without text.</summary>
    public void CancelMemo()
    {
        _marks?.CancelMemo();
        CloseMemo();
        ShowMarks();
    }

    private void CloseMemo()
    {
        IsMemoOpen = false;
        MemoText = "";
    }

    /// <summary>
    /// Feeds the live detector's new windows to the recording's indications.
    /// Another detector (the model loaded again) ends an open indication; its
    /// windows are taken from after the last one fed. An ONNX model gives no
    /// indications.
    /// </summary>
    private void TrackIndications()
    {
        if (_tracker is not IndicationTracker tracker || _timeline is not { HasAnchor: true } || _marks is not MarkLog log)
        {
            return;
        }
        M4Runner? runner = _liveM4;
        if (runner != _trackedRunner)
        {
            tracker.Close();
            _trackedRunner = runner;
        }
        if (runner is null)
        {
            return;
        }
        int version = log.Version;
        tracker.Feed(runner.History.Range(Math.Max(long.MinValue / 2, tracker.LastFed), long.MaxValue / 2));
        if (log.Version != version)
        {
            ShowMarks();
        }
    }

    /// <summary>The marks to draw: their positions in the view's source, and their kinds.</summary>
    public List<(long Position, MarkKind Kind)> MarksInView()
    {
        if (_file is not null)
        {
            return FileMarks.Select(m => (m.Sample, m.Kind)).ToList();
        }
        List<(long, MarkKind)> marks = new();
        if (_marks is MarkLog log && _timeline is RecordingTimeline timeline)
        {
            foreach (Mark m in log.Marks)
            {
                if (timeline.ToLive(m.Sample) is long live)
                {
                    marks.Add((live, m.Kind));
                }
            }
        }
        return marks;
    }

    /// <summary>The marks of the file in view, sorted.</summary>
    private IReadOnlyList<Mark> FileMarks => _editor?.Marks ?? _fileMarks;

    /// <summary>The list of marks and their counts, for the file in view or the recording on the live view.</summary>
    private void ShowMarks()
    {
        MarksVersion++;
        int rate;
        IReadOnlyList<Mark> marks;
        if (_file is not null)
        {
            marks = FileMarks;
            rate = _file.SampleRate;
        }
        else if (_marks is MarkLog log)
        {
            marks = MarkFile.Sorted(log.Marks);
            rate = log.SampleRate;
        }
        else
        {
            marks = [];
            rate = 16000;
        }
        if (_selectedIndex >= marks.Count)
        {
            _selectedIndex = null;
        }
        MarkReviewResult r = MarkReview.Review(marks, rate);
        List<MarkItem> items = marks.Select((m, i) => ItemOf(m, i, rate, r)).ToList();
        _showingMarks = true;
        try
        {
            // In place when only texts change, so that a click in the list is not lost.
            if (items.Count != Marks.Count)
            {
                Marks.Clear();
                items.ForEach(Marks.Add);
            }
            for (int i = 0; i < items.Count; i++)
            {
                if (Marks[i] != items[i])
                {
                    Marks[i] = items[i];
                }
            }
            if (_file is not null)
            {
                SelectedMark = _selectedIndex is int s ? Marks[s] : null;
            }
        }
        finally
        {
            _showingMarks = false;
        }
        ShowMarkEditor(r);
        HasMarks = Marks.Count > 0 || (IsRecording && _file is null) || CanEditMarks;
        if (!HasMarks)
        {
            MarksSummary = "";
            return;
        }
        MarksSummary = string.Create(CultureInfo.InvariantCulture,
            $"indications      {r.Indications.Count}\nmarked wrong     {r.MarkedFalse} (hits {r.Hits})\nmissed swallows  {r.MissedSwallows.Count}");
    }

    private static MarkItem ItemOf(Mark m, int index, int rate, MarkReviewResult review)
    {
        double s = m.Seconds(rate);
        string probability = m.Probability is double p ? " " + p.ToString("0.000", CultureInfo.InvariantCulture) : "";
        string wrong = m.Kind == MarkKind.Indication &&
                       review.Indications.Any(i => ReferenceEquals(i.Indication, m) && !i.IsHit) ? " (marked wrong)" : "";
        string text = m.Text.Length > 0 ? "  " + m.Text.ReplaceLineEndings(" ") : "";
        return new MarkItem(index, s, $"{Seconds(s),9}  {MarkFile.KindName(m.Kind)}{probability}{wrong}{text}");
    }

    /// <summary>The editor's fields for the selected mark and the picked position.</summary>
    private void ShowMarkEditor(MarkReviewResult review)
    {
        Mark? m = _editor is not null && _selectedIndex is int i ? _editor.Marks[i] : null;
        IsUserMarkSelected = m is { Kind: not MarkKind.Indication };
        IsIndicationSelected = m is { Kind: MarkKind.Indication };
        HasPick = _pickedSample is not null;
        _loadingEditor = true;
        EditKind = IsUserMarkSelected ? MarkFile.KindName(m!.Kind) : null;
        EditText = m?.Text ?? "";
        _loadingEditor = false;
        if (_editor is null)
        {
            MarkEditorText = "";
            return;
        }
        int rate = _editor.SampleRate;
        string selected = m is null ? "selected: none (click a mark)"
            : $"selected: {Seconds(m.Seconds(rate))} {MarkFile.KindName(m.Kind)}" +
              (m.Probability is double p ? " " + p.ToString("0.000", CultureInfo.InvariantCulture) : "") +
              (review.Indications.Any(x => ReferenceEquals(x.Indication, m) && !x.IsHit) ? " (marked wrong)" : "");
        string picked = _pickedSample is long s ? $"picked: {Seconds(s / (double)rate)}" : "picked: none (click in the views)";
        MarkEditorText = selected + "\n" + picked;
    }

    /// <summary>A click in the list: scrolls the file so that the mark is in the middle of the view, and selects it for editing.</summary>
    partial void OnSelectedMarkChanged(MarkItem? value)
    {
        if (_showingMarks || _file is null)
        {
            return;
        }
        if (value is not null)
        {
            Position = Math.Clamp(value.Seconds - SpanSeconds / 2, 0, PositionMaximum);
        }
        if (_editor is not null)
        {
            CommitMarkText();
            _selectedIndex = value?.Index;
            ShowMarks();
        }
    }

    /// <summary>A click in the views on or near the line of a mark of the file.</summary>
    public void SelectMark(int index)
    {
        if (_editor is null || index < 0 || index >= _editor.Marks.Count)
        {
            return;
        }
        CommitMarkText();
        _selectedIndex = index;
        ShowMarks();
    }

    /// <summary>A click in the views of the file away from the marks: the position for a new mark or a move.</summary>
    public void PickPosition(long sample)
    {
        if (_editor is null || _file is not FileSource file)
        {
            return;
        }
        CommitMarkText();
        _pickedSample = Math.Clamp(sample, 0, Math.Max(0, file.End - 1));
        ShowMarks();
    }

    /// <summary>Escape in the views: no mark selected and no position picked.</summary>
    public void ClearMarkSelection()
    {
        if (_editor is null)
        {
            return;
        }
        CommitMarkText();
        _selectedIndex = null;
        _pickedSample = null;
        ShowMarks();
    }

    partial void OnEditKindChanged(string? value)
    {
        if (_loadingEditor || _editor is not MarkEditor editor || _selectedIndex is not int i ||
            value is null || MarkFile.ParseKind(value) is not MarkKind kind || !IsUserMarkSelected)
        {
            return;
        }
        Mark m = editor.Marks[i];
        bool ok = editor.Edit(i, kind, EditText);
        ShowEdit(ok, $"The mark at {Seconds(m.Seconds(editor.SampleRate))} is now {value}");
    }

    /// <summary>Enter in the mark's text box, or the box lost the focus: the text becomes the selected mark's.</summary>
    public void StoreMarkText()
    {
        if (CommitMarkText())
        {
            ShowMarks();
        }
    }

    /// <summary>
    /// The text typed for the selected user's mark becomes its text, with the
    /// message; whether there was a change to write. The caller shows the marks.
    /// </summary>
    private bool CommitMarkText()
    {
        if (_editor is not MarkEditor editor || _selectedIndex is not int i || !IsUserMarkSelected)
        {
            return false;
        }
        Mark m = editor.Marks[i];
        if (m.Text == EditText)
        {
            return false;
        }
        Message = editor.Edit(i, m.Kind, EditText)
            ? $"The text of the {MarkFile.KindName(m.Kind)} mark at {Seconds(m.Seconds(editor.SampleRate))} is stored"
            : NotWritten();
        return true;
    }

    /// <summary>Escape in the mark's text box: back to the text the mark has.</summary>
    public void RevertMarkText()
    {
        if (_editor is MarkEditor editor && _selectedIndex is int i)
        {
            _loadingEditor = true;
            EditText = editor.Marks[i].Text;
            _loadingEditor = false;
        }
    }

    [RelayCommand]
    private void AddSwallowAtPick() => AddMarkAtPick(MarkKind.Swallow);

    [RelayCommand]
    private void AddFalseAtPick() => AddMarkAtPick(MarkKind.False);

    [RelayCommand]
    private void AddOtherAtPick() => AddMarkAtPick(MarkKind.Other);

    /// <summary>A mark of the user at the picked position of the file; it becomes the selected one. False without a picked position.</summary>
    public bool AddMarkAtPick(MarkKind kind)
    {
        if (_editor is not MarkEditor editor || _pickedSample is not long sample)
        {
            return false;
        }
        CommitMarkText();
        int? added = editor.Add(sample, kind);
        string at = Seconds(sample / (double)editor.SampleRate);
        if (added is int i)
        {
            _selectedIndex = i;
            _pickedSample = null;
        }
        ShowEdit(added is not null, $"Mark {MarkFile.KindName(kind)} added at {at}");
        if (added is not null && kind is not MarkKind.Swallow)
        {
            MarkTextWanted?.Invoke();
        }
        return true;
    }

    /// <summary>Deletes the selected user's mark (an indication is not deleted). False when none is selected.</summary>
    [RelayCommand]
    private void DeleteMark() => DeleteSelectedMark();

    public bool DeleteSelectedMark()
    {
        if (_editor is not MarkEditor editor || _selectedIndex is not int i || !IsUserMarkSelected)
        {
            return false;
        }
        Mark m = editor.Marks[i];
        bool ok = editor.Delete(i);
        if (ok)
        {
            _selectedIndex = null;
        }
        ShowEdit(ok, $"The {MarkFile.KindName(m.Kind)} mark at {Seconds(m.Seconds(editor.SampleRate))} is deleted" +
                     (editor.Marks.Count == 0 ? $"; no marks left, {Path.GetFileName(editor.Path)} removed" : ""));
        return true;
    }

    /// <summary>
    /// The selected indication was wrong: a <c>false</c> mark at its sample,
    /// selected for its text; when it has one already, that one is selected.
    /// </summary>
    [RelayCommand]
    private void MarkWrong()
    {
        if (_editor is not MarkEditor editor || _selectedIndex is not int i || !IsIndicationSelected)
        {
            return;
        }
        string at = Seconds(editor.Marks[i].Seconds(editor.SampleRate));
        if (editor.FalseMarkOf(i) is int existing)
        {
            _selectedIndex = existing;
            ShowMarks();
            EnsureInView(editor.Marks[existing].Sample);
            Message = $"The indication at {at} is marked wrong already; its false mark is selected";
            return;
        }
        int? added = editor.MarkWrong(i);
        if (added is int f)
        {
            _selectedIndex = f;
        }
        ShowEdit(added is not null, $"The indication at {at} is marked wrong");
        if (added is not null)
        {
            MarkTextWanted?.Invoke();
        }
    }

    /// <summary>Moves the selected user's mark to the picked position.</summary>
    [RelayCommand]
    private void MoveMark()
    {
        if (_editor is not MarkEditor editor || _selectedIndex is not int i || !IsUserMarkSelected ||
            _pickedSample is not long sample)
        {
            return;
        }
        CommitMarkText();
        Mark m = editor.Marks[i];
        int? moved = editor.Move(i, sample);
        if (moved is int j)
        {
            _selectedIndex = j;
            _pickedSample = null;
        }
        ShowEdit(moved is not null, $"The {MarkFile.KindName(m.Kind)} mark is moved from " +
                                    $"{Seconds(m.Seconds(editor.SampleRate))} to {Seconds(sample / (double)editor.SampleRate)}");
    }

    /// <summary>The message after an edit of the file's marks, and the list and counts as they are now.</summary>
    private void ShowEdit(bool written, string done)
    {
        Message = written ? done : NotWritten();
        ShowMarks();
    }

    private string NotWritten() =>
        $"The marks could not be written: {_editor?.Error}; nothing changed (the list is as the file on disk)";

    /// <summary>Scrolls the file to a sample that is out of view.</summary>
    private void EnsureInView(long sample)
    {
        (long from, long to) = ViewRange();
        if (_file is FileSource file && (sample < from || sample > to))
        {
            Position = Math.Clamp(sample / (double)file.SampleRate - SpanSeconds / 2, 0, PositionMaximum);
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
        CommitMarkText();
        List<Mark> marks = [];
        bool editable = true;
        string marksPath = MarkFile.PathFor(path);
        string marksNote = "";
        if (File.Exists(marksPath))
        {
            try
            {
                marks = await Task.Run(() => MarkFile.Read(marksPath));
                marksNote = string.Create(CultureInfo.InvariantCulture, $", {marks.Count} marks");
            }
            catch (MarkFileException e)
            {
                editable = false;
                marksNote = $"; marks not shown and not editable: {e.Message}";
            }
        }
        if (editable && IsRecording && _marks is MarkLog log &&
            string.Equals(Path.GetFullPath(log.Path), Path.GetFullPath(marksPath), StringComparison.Ordinal))
        {
            editable = false;
            marksNote += "; the marks of the recording in progress are not editable";
        }
        _file = file;
        _editor = editable ? new MarkEditor(marksPath, file.SampleRate, marks) : null;
        _fileMarks = editable ? [] : marks;
        _selectedIndex = null;
        _pickedSample = null;
        CanEditMarks = editable;
        IsFileShown = true;
        SourceText = string.Create(CultureInfo.InvariantCulture,
            $"{Path.GetFileName(path)}  ({file.End / (double)file.SampleRate:F1} s, {file.SampleRate} Hz)");
        Position = 0;
        UpdatePositionRange();
        Message = $"Showing {path}{marksNote}";
        ShowMarks();
        AnalyzeFile();
        ViewChanged?.Invoke();
    }

    [RelayCommand]
    private void ShowLive()
    {
        CommitMarkText();
        _fileAnalysis?.Cancel();
        _fileClassifier = null;
        _fileM4 = null;
        _file = null;
        _fileMarks = [];
        _editor = null;
        _selectedIndex = null;
        _pickedSample = null;
        CanEditMarks = false;
        IsFileShown = false;
        SourceText = "Live";
        PositionMaximum = 0;
        ShowMarks();
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
        _liveM4 = _m4 is null ? null : new M4Runner(_m4, SpikeFilterMainsHz);
    }

    /// <summary>The mains frequency of the spike filter before the M4 detector; null while it is off.</summary>
    private double? SpikeFilterMainsHz => IsSpikeFilterOn ? _settings.MainsHz : null;

    public string SpikeFilterTip => string.Create(CultureInfo.InvariantCulture,
        $"Mains-synchronous spike filter ({_settings.MainsHz:0} Hz) before the swallow detector: limits short bursts above about 450 Hz that repeat in every mains period (an electric heater the wearer touches). Only the detector's input: the views, the levels and recordings keep the raw samples. Switching it runs the detector again (live: from the newest window; a file: from its start).");

    /// <summary>Switches the spike filter; the detector starts again with or without it.</summary>
    [RelayCommand]
    private void ToggleSpikeFilter()
    {
        IsSpikeFilterOn = !IsSpikeFilterOn;
        _settings.SpikeFilter = IsSpikeFilterOn;
        if (_m4 is not null)
        {
            MakeLiveClassifier();
            AnalyzeFile();
        }
        Message = IsSpikeFilterOn
            ? string.Create(CultureInfo.InvariantCulture, $"Spike filter on ({_settings.MainsHz:0} Hz mains) before the swallow detector")
            : "Spike filter off";
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
        M4Runner runner = new(model, SpikeFilterMainsHz);
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
        string filter = runner.SpikeFilterMainsHz is double hz
            ? string.Create(CultureInfo.InvariantCulture, $"spike filter, {hz:0} Hz mains (delay of {MainsSpike.Delay} samples taken off)")
            : "none";
        ModelText = string.Create(CultureInfo.InvariantCulture, $"""
            {Path.GetFileName(model.Path)} (M4 swallow detector)
            {c.Text}
            input    {filter}
            features {c.Set}: 40 mel bands, {c.MelLowHz} to 4000 Hz ({Path.GetFileName(model.MelPath)})
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
        if (_profile.Ota is not OtaSpec ota)
        {
            AppendOta("Not updated: the target takes no firmware update");
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
        AppendOta(string.Create(CultureInfo.InvariantCulture, $"Updating {host}:{ota.Port}"));
        try
        {
            OtaClient client = new(host, ota.Port);
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
        TrackIndications();
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
            status.Append(CultureInfo.InvariantCulture, $"Stream {client.Source.Name}: {StreamState}");
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
        _ssh?.Dispose();
        if (_model is not null)
        {
            // A window takes milliseconds; the wait only covers a run that is under way.
            idle.Wait(TimeSpan.FromSeconds(2));
            _model.Dispose();
        }
        _settings.Save();
    }
}
