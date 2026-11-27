// SPDX-License-Identifier: MIT
using AudioStudio.Core.Model;

namespace AudioStudio.Core.Marks;

/// <summary>
/// Turns the M4 detector's windows of the live stream into indication marks
/// of a recording: one per run of positive windows, at the centre of the
/// run's first window (the time the list of indications shows), with the
/// highest probability of the run's windows. The mark is written when the
/// run starts and written again with its highest probability when the run
/// ends. A run that started before the recording is left out.
/// </summary>
public sealed class IndicationTracker
{
    private readonly MarkLog _log;
    private readonly RecordingTimeline _timeline;
    private int? _open;
    private double _peak;
    private bool _peakSaved;

    public IndicationTracker(MarkLog log, RecordingTimeline timeline)
    {
        _log = log;
        _timeline = timeline;
    }

    /// <summary>The live position of the last window fed; windows are fed in order, each once.</summary>
    public long LastFed { get; private set; } = long.MinValue;

    /// <summary>Takes the windows of <paramref name="windows"/> that start after the last one fed.</summary>
    public void Feed(IEnumerable<M4Window> windows)
    {
        foreach (M4Window w in windows)
        {
            if (w.Start <= LastFed)
            {
                continue;
            }
            LastFed = w.Start;
            Feed(w);
        }
    }

    private void Feed(M4Window w)
    {
        if (w.Rise)
        {
            Close();
            long centre = w.Start + M4Model.WindowSamples / 2;
            if (_timeline.ToWav(centre) is long wav && wav >= 0)
            {
                _peak = w.Step.Probability;
                _peakSaved = true;
                _open = _log.AddIndication(wav, _peak);
            }
        }
        else if (w.Step.Positive && _open is int open)
        {
            if (w.Step.Probability > _peak)
            {
                _peak = w.Step.Probability;
                _peakSaved = false;
                _log.SetProbability(open, _peak, save: false);
            }
        }
        else if (!w.Step.Positive)
        {
            Close();
        }
    }

    /// <summary>Ends a run that is open (the end of the recording, or another detector).</summary>
    public void Close()
    {
        if (_open is int open && !_peakSaved)
        {
            _log.SetProbability(open, _peak, save: true);
        }
        _open = null;
    }
}
