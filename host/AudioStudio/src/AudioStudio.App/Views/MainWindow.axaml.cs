// SPDX-License-Identifier: MIT
using System.ComponentModel;
using AudioStudio.App.Controls;
using AudioStudio.App.ViewModels;
using AudioStudio.Core.Audio;
using AudioStudio.Core.Dsp;
using AudioStudio.Core.Model;
using AudioStudio.Core.Settings;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;

namespace AudioStudio.App.Views;

/// <summary>What the command line asks for at the start.</summary>
public sealed record StartOptions(string? Board, bool Shell, string? Open, string? Model, string? Image, string? View);

public partial class MainWindow : Window
{
    /// <summary>Spectrogram columns computed per frame at most, so that a jump in a file does not stall the window.</summary>
    private const int ColumnBudget = 120;

    private readonly MainViewModel _viewModel;
    private Spectrogram? _analysis;
    private ISampleSource? _analysisSource;
    private (ISampleSource? Source, long From, long To, double Zoom, double Width) _waveKey;
    private (long From, long To, double Min, double Max) _spectrumKey;
    private bool _spectrumComplete;
    private int _historyIndex = -1;

    public MainWindow() : this(new StartOptions(null, false, null, null, null, null))
    {
    }

    public MainWindow(StartOptions options)
    {
        _viewModel = new MainViewModel(options.Board, options.Shell, options.Image);
        InitializeComponent();
        if (string.Equals(options.View, "firmware", StringComparison.OrdinalIgnoreCase))
        {
            _viewModel.SelectedView = MainViewModel.FirmwareView;
        }
        DataContext = _viewModel;
        _viewModel.PropertyChanged += OnViewModelPropertyChanged;
        _viewModel.Frame += OnFrame;
        _viewModel.ViewChanged += () => _analysis = null;
        ShowShellPane(_viewModel.IsShellPaneVisible);
        ViewsGrid.PointerWheelChanged += OnViewsWheel;
        // The model of the command line, else the last one loaded, else the M4 swallow detector in models/.
        string? model = options.Model ??
            (string.IsNullOrWhiteSpace(_viewModel.ModelPath) ? AppSettings.ResolveDefaultModel() : _viewModel.ModelPath);
        if (model is not null && File.Exists(model))
        {
            _viewModel.LoadModel(model);
        }
        if (options.Open is not null)
        {
            _ = _viewModel.OpenFileAsync(options.Open);
        }
        Closed += (_, _) =>
        {
            KeepShellPaneHeight();
            _viewModel.Dispose();
        };
    }

    /// <summary>Draws the views: every frame for the live stream, on a change for a file.</summary>
    private void OnFrame()
    {
        if (_viewModel.SelectedView != MainViewModel.AudioView)
        {
            return;
        }
        ISampleSource source = _viewModel.ViewSource;
        (long from, long to) = _viewModel.ViewRange();
        double zoom = _viewModel.AmplitudeZoom;

        (ISampleSource?, long, long, double, double) waveKey = (source, from, to, zoom, Waveform.Bounds.Width);
        if (waveKey != _waveKey)
        {
            _waveKey = waveKey;
            Waveform.Show(source, from, to, zoom, relative: !_viewModel.IsFileShown);
        }

        if (_analysis is null || _analysisSource != source)
        {
            _analysis = new Spectrogram(source.SampleRate, _viewModel.FftSize, _viewModel.SpectrogramHop,
                                        SpectrogramControl.Rows, _viewModel.MaxHz, _viewModel.LogFrequency);
            _analysisSource = source;
            _spectrumKey = default;
        }
        Spectrogram.SetRange((float)_viewModel.MinDb, (float)_viewModel.MaxDb);
        (long, long, double, double) spectrumKey = (from, to, _viewModel.MinDb, _viewModel.MaxDb);
        if (spectrumKey != _spectrumKey || !_spectrumComplete)
        {
            _spectrumKey = spectrumKey;
            _spectrumComplete = Spectrogram.Show(_analysis, source, from, to, ColumnBudget);
        }

        if (_viewModel.IsM4Model)
        {
            M4Runner? runner = _viewModel.ViewM4Runner;
            (double enter, double exit) = _viewModel.M4Levels;
            if (runner is null)
            {
                ProbabilityTrace.Clear("no model loaded");
            }
            else if (runner.Error is string error && runner.History.Count == 0)
            {
                ProbabilityTrace.Clear(error);
            }
            else
            {
                ProbabilityTrace.Show(runner.History.Range(from, to), from, to, enter, exit);
            }
            return;
        }
        Classifier? classifier = _viewModel.ViewClassifier;
        if (classifier is null)
        {
            ClassStrip.Clear("no model loaded");
        }
        else
        {
            ClassStrip.Show(classifier.History.Range(from, to), classifier.Model.Labels, from, to, classifier.Hop);
        }
    }

    /// <summary>The wheel scrolls a file along; with Ctrl it changes the span.</summary>
    private void OnViewsWheel(object? sender, PointerWheelEventArgs e)
    {
        if (e.KeyModifiers.HasFlag(KeyModifiers.Control))
        {
            _viewModel.Zoom(e.Delta.Y > 0 ? -1 : 1);
        }
        else
        {
            _viewModel.Scroll(-(e.Delta.Y + e.Delta.X) * 0.25);
        }
        e.Handled = true;
    }

    private const double ShellPaneMinHeight = 70;
    private const double ViewsMinHeight = 200;

    /// <summary>Gives the shell pane its row, or takes the row and the bar above it away.</summary>
    private void ShowShellPane(bool visible)
    {
        RowDefinition views = WorkArea.RowDefinitions[0];
        RowDefinition bar = WorkArea.RowDefinitions[1];
        RowDefinition pane = WorkArea.RowDefinitions[2];
        if (!visible)
        {
            KeepShellPaneHeight();
        }
        views.MinHeight = ViewsMinHeight;
        pane.MinHeight = visible ? ShellPaneMinHeight : 0;
        pane.Height = new GridLength(visible ? Math.Max(ShellPaneMinHeight, _viewModel.ShellPaneHeight) : 0);
        bar.Height = new GridLength(visible ? 6 : 0);
        ShellSplitter.IsVisible = visible;
        ShellPane.IsVisible = visible;
    }

    private void KeepShellPaneHeight()
    {
        if (ShellPane.IsVisible && ShellPane.Bounds.Height > 0)
        {
            _viewModel.ShellPaneHeight = WorkArea.RowDefinitions[2].ActualHeight;
        }
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(MainViewModel.IsShellPaneVisible):
                ShowShellPane(_viewModel.IsShellPaneVisible);
                break;
            case nameof(MainViewModel.ShellText):
                // Keep the newest output in view.
                ShellLogBox.CaretIndex = _viewModel.ShellText.Length;
                break;
            case nameof(MainViewModel.OtaText):
                OtaLogBox.CaretIndex = _viewModel.OtaText.Length;
                break;
        }
    }

    private void OnShellInputKeyDown(object? sender, KeyEventArgs e)
    {
        List<string> history = _viewModel.ShellHistory;
        switch (e.Key)
        {
            case Key.Enter:
                _historyIndex = -1;
                _viewModel.SendShellCommand.Execute(null);
                e.Handled = true;
                break;
            case Key.Up when history.Count > 0:
                _historyIndex = _historyIndex < 0 ? history.Count - 1 : Math.Max(0, _historyIndex - 1);
                ShowHistory(history[_historyIndex]);
                e.Handled = true;
                break;
            case Key.Down when _historyIndex >= 0:
                _historyIndex++;
                if (_historyIndex >= history.Count)
                {
                    _historyIndex = -1;
                    ShowHistory("");
                }
                else
                {
                    ShowHistory(history[_historyIndex]);
                }
                e.Handled = true;
                break;
        }
    }

    private void ShowHistory(string line)
    {
        _viewModel.ShellInput = line;
        ShellInputBox.CaretIndex = line.Length;
    }

    private async Task<string?> PickFileAsync(string title, string? startDirectory, string filterName, params string[] patterns)
    {
        IStorageFolder? start = startDirectory is not null && Directory.Exists(startDirectory)
            ? await StorageProvider.TryGetFolderFromPathAsync(startDirectory) : null;
        IReadOnlyList<IStorageFile> files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = title,
            AllowMultiple = false,
            SuggestedStartLocation = start,
            FileTypeFilter = [new FilePickerFileType(filterName) { Patterns = patterns }, FilePickerFileTypes.All],
        });
        return files.Count == 1 ? files[0].TryGetLocalPath() : null;
    }

    private async void OnOpenWavClick(object? sender, RoutedEventArgs e)
    {
        if (await PickFileAsync("WAV file to show (16-bit PCM, mono)", _viewModel.RecordingsDirectory, "WAV files", "*.wav")
            is string path)
        {
            await _viewModel.OpenFileAsync(path);
        }
    }

    private async void OnLoadModelClick(object? sender, RoutedEventArgs e)
    {
        string? start = _viewModel.ModelPath.Length > 0 ? Path.GetDirectoryName(_viewModel.ModelPath) : null;
        if (await PickFileAsync("Model: ONNX, or the M4 swallow detector's .npz", start, "Models", "*.onnx", "*.npz") is string path)
        {
            _viewModel.LoadModel(path);
        }
    }

    private async void OnChooseImageClick(object? sender, RoutedEventArgs e)
    {
        string? start = _viewModel.ImagePath.Length > 0 ? Path.GetDirectoryName(_viewModel.ImagePath) : null;
        if (await PickFileAsync("Firmware image", start, "Application images", "*.bin") is string path)
        {
            _viewModel.ImagePath = path;
        }
    }
}
