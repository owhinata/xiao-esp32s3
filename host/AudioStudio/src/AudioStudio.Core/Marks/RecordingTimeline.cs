// SPDX-License-Identifier: MIT
namespace AudioStudio.Core.Marks;

/// <summary>
/// How positions of the live stream relate to sample indices of the WAV file
/// that records it. The two move together except across a gap longer than
/// the zeros the recording gets for it; each block written gives an anchor
/// (its first sample's live position and its index in the file), and an
/// anchor is kept only where the offset between them changes. Any thread.
/// </summary>
public sealed class RecordingTimeline
{
    private readonly object _lock = new();
    private readonly List<(long Live, long Wav)> _anchors = new();

    /// <summary>Whether a block has been written yet.</summary>
    public bool HasAnchor
    {
        get { lock (_lock) { return _anchors.Count > 0; } }
    }

    /// <summary>The block whose first sample is at <paramref name="live"/> is written at <paramref name="wav"/>.</summary>
    public void Anchor(long live, long wav)
    {
        lock (_lock)
        {
            if (_anchors.Count == 0 || _anchors[^1].Live - _anchors[^1].Wav != live - wav)
            {
                _anchors.Add((live, wav));
            }
        }
    }

    /// <summary>The WAV index of a live position (negative before the recording); null before the first block.</summary>
    public long? ToWav(long live)
    {
        lock (_lock)
        {
            if (_anchors.Count == 0)
            {
                return null;
            }
            (long Live, long Wav) a = _anchors[0];
            foreach ((long Live, long Wav) b in _anchors)
            {
                if (b.Live > live)
                {
                    break;
                }
                a = b;
            }
            return a.Wav + (live - a.Live);
        }
    }

    /// <summary>The live position of a WAV index; null before the first block.</summary>
    public long? ToLive(long wav)
    {
        lock (_lock)
        {
            if (_anchors.Count == 0)
            {
                return null;
            }
            (long Live, long Wav) a = _anchors[0];
            foreach ((long Live, long Wav) b in _anchors)
            {
                if (b.Wav > wav)
                {
                    break;
                }
                a = b;
            }
            return a.Live + (wav - a.Wav);
        }
    }
}
