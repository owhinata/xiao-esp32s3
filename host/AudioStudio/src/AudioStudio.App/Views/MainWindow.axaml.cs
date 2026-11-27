// SPDX-License-Identifier: MIT
using System.ComponentModel;
using AudioStudio.App.Controls;
using AudioStudio.App.ViewModels;
using AudioStudio.Core.Audio;
using AudioStudio.Core.Dsp;
using AudioStudio.Core.Marks;
using AudioStudio.Core.Model;
using AudioStudio.Core.Settings;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace AudioStudio.App.Views;

/// <summary>What the command line asks for at the start.</summary>
public sealed record StartOptions(string? Board, bool Shell, string? Open, string? Model, string? Image, string? View,
                                  string? Target);

public partial class MainWindow : Window
{
    /// <summary>Spectrogram columns computed per frame at most, so that a jump in a file does not stall the window.</summary>
    private const int ColumnBudget = 120;
    /// <summary>How near to a mark's line, in pixels, a click in the views selects the mark.</summary>
    private const double MarkHitTolerance = 4;

    private readonly MainViewModel _viewModel;
    private Spectrogram? _analysis;
    private ISampleSource? _analysisSource;
    private (ISampleSource? Source, long From, long To, double Zoom, double Width) _waveKey;
    private (long From, long To, double Min, double Max) _spectrumKey;
    private bool _spectrumComplete;
    private (long From, long To, int Version, double Width) _marksKey;
    private int _historyIndex = -1;

    public MainWindow() : this(new StartOptions(null, false, null, null, null, null, null))
    {
    }

    public MainWindow(StartOptions options)
    {
        _viewModel = new MainViewModel(options.Board, options.Shell, options.Image, options.Target);
        InitializeComponent();
        if (string.Equals(options.View, "firmware", StringComparison.OrdinalIgnoreCase) && _viewModel.HasOta)
        {
            _viewModel.SelectedView = MainViewModel.FirmwareView;
        }
        DataContext = _viewModel;
        _viewModel.PropertyChanged += OnViewModelPropertyChanged;
        _viewModel.Frame += OnFrame;
        _viewModel.ViewChanged += () => _analysis = null;
        // Once the box is shown; a click on a mark button has taken the focus from it.
        _viewModel.MemoOpened += () => Dispatcher.UIThread.Post(() => MemoBox.Focus(), DispatcherPriority.Input);
        _viewModel.MarkTextWanted += () => Dispatcher.UIThread.Post(() => MarkTextBox.Focus(), DispatcherPriority.Input);
        ShowShellPane(_viewModel.IsShellPaneVisible);
        ViewsGrid.PointerWheelChanged += OnViewsWheel;
        ViewsGrid.PointerPressed += OnViewsPressed;
        // Tunnel: the mark keys reach the window before a focused button or list takes them.
        AddHandler(KeyDownEvent, OnMarkKeyDown, RoutingStrategies.Tunnel);
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

        (long, long, int, double) marksKey = (from, to, _viewModel.MarksVersion, MarksOverlay.Bounds.Width);
        if (marksKey != _marksKey)
        {
            _marksKey = marksKey;
            MarksOverlay.Show(_viewModel.MarksInView(), from, to, _viewModel.SelectedMarkIndex, _viewModel.PickedSample);
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

    /// <summary>
    /// A click in the views of a file whose marks can be edited: on or near
    /// a mark's line it selects the mark, elsewhere it picks the position.
    /// A text box with the focus gives it up first (its text is stored).
    /// </summary>
    private void OnViewsPressed(object? sender, PointerPressedEventArgs e)
    {
        if (!_viewModel.CanEditMarks || !e.GetCurrentPoint(ViewsGrid).Properties.IsLeftButtonPressed)
        {
            return;
        }
        if (FocusManager?.GetFocusedElement() is TextBox)
        {
            FocusManager.ClearFocus();
        }
        double x = e.GetPosition(MarksOverlay).X;
        if (MarksOverlay.MarkAt(x, MarkHitTolerance) is int index)
        {
            _viewModel.SelectMark(index);
        }
        else if (MarksOverlay.PositionAt(x) is long position)
        {
            _viewModel.PickPosition(position);
        }
        else
        {
            return;
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
            case nameof(MainViewModel.IsMemoOpen):
                if (!_viewModel.IsMemoOpen && MemoBox.IsFocused)
                {
                    // Back to the window, where the mark keys work.
                    FocusManager?.ClearFocus();
                }
                break;
        }
    }

    /// <summary>
    /// Not while a text box has the focus: 1 swallow, 2 false, 3 other (also
    /// on the number pad) mark the recording in progress, or else a file's
    /// picked position; Delete deletes a file's selected mark; Escape takes
    /// the selection and the picked position away.
    /// </summary>
    private void OnMarkKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.KeyModifiers != KeyModifiers.None || FocusManager?.GetFocusedElement() is TextBox)
        {
            return;
        }
        MarkKind? kind = e.Key switch
        {
            Key.D1 or Key.NumPad1 => MarkKind.Swallow,
            Key.D2 or Key.NumPad2 => MarkKind.False,
            Key.D3 or Key.NumPad3 => MarkKind.Other,
            _ => null,
        };
        if (kind is MarkKind k)
        {
            e.Handled = true;
            if (_viewModel.IsRecording)
            {
                _viewModel.AddMark(k);
            }
            else
            {
                _viewModel.AddMarkAtPick(k);
            }
        }
        else if (e.Key == Key.Delete && _viewModel.CanEditMarks)
        {
            e.Handled = _viewModel.DeleteSelectedMark();
        }
        else if (e.Key == Key.Escape && _viewModel.CanEditMarks &&
                 FocusManager?.GetFocusedElement() is not (ComboBox or ComboBoxItem))
        {
            _viewModel.ClearMarkSelection();
        }
    }

    /// <summary>Enter stores the mark's text and gives the focus back to the window; Escape goes back to the stored text.</summary>
    private void OnMarkTextKeyDown(object? sender, KeyEventArgs e)
    {
        switch (e.Key)
        {
            case Key.Enter:
                _viewModel.StoreMarkText();
                FocusManager?.ClearFocus();
                e.Handled = true;
                break;
            case Key.Escape:
                _viewModel.RevertMarkText();
                FocusManager?.ClearFocus();
                e.Handled = true;
                break;
        }
    }

    private void OnMarkTextLostFocus(object? sender, RoutedEventArgs e) => _viewModel.StoreMarkText();

    private void OnMemoKeyDown(object? sender, KeyEventArgs e)
    {
        switch (e.Key)
        {
            case Key.Enter:
                _viewModel.StoreMemo();
                e.Handled = true;
                break;
            case Key.Escape:
                _viewModel.CancelMemo();
                e.Handled = true;
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
