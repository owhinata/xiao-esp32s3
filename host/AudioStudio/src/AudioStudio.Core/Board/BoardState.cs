// SPDX-License-Identifier: MIT
namespace AudioStudio.Core.Board;

/// <summary>The board's USB host, as <c>usb status</c> reports it.</summary>
public enum UsbHostState
{
    /// <summary>Not known: no shell, or a reply this application does not know.</summary>
    Unknown,
    Stopped,
    Running,
    /// <summary>The host did not answer a request; it stays so until the board restarts.</summary>
    NotAnswering,
}

/// <summary>The board's microphone stream, as <c>uac stats</c> or the audio stream reports it.</summary>
public enum MicrophoneState
{
    /// <summary>Not known: neither the shell nor the audio stream tells it.</summary>
    Unknown,
    Stopped,
    Running,
}

/// <summary>A shell command that changes the board's state, and the start of the line it prints when it succeeds.</summary>
public sealed record BoardCommand(string Line, string SuccessPrefix)
{
    public static readonly BoardCommand UsbHostStart = new("usb host start", "USB host started");
    public static readonly BoardCommand UsbHostStop = new("usb host stop", "USB host stopped");
    public static readonly BoardCommand UacStart = new("uac start", "Streaming:");
    public static readonly BoardCommand UacStop = new("uac stop", "Stopped");

    /// <summary>
    /// Null when the output (<see cref="BoardShell.OutputLines"/>) has the
    /// success line; otherwise the board's first line (its error), or a
    /// note that it printed nothing.
    /// </summary>
    public string? Failure(IReadOnlyList<string> lines)
    {
        foreach (string line in lines)
        {
            if (line.StartsWith(SuccessPrefix, StringComparison.Ordinal))
            {
                return null;
            }
        }
        return lines.Count > 0 ? lines[0] : "no answer from the board";
    }
}

/// <summary>The queries of the board's state and the parsing of their replies.</summary>
public static class BoardState
{
    public const string UsbStatusLine = "usb status";
    public const string UacStatsLine = "uac stats";

    /// <summary>
    /// The host's state from the output of <c>usb status</c>: its first line
    /// is <c>host: stopped</c>, <c>host: running</c> (then the root port and
    /// the devices) or <c>host: not answering; ...</c>.
    /// </summary>
    public static UsbHostState ParseUsbStatus(IReadOnlyList<string> lines)
    {
        if (lines.Count == 0)
        {
            return UsbHostState.Unknown;
        }
        string first = lines[0].Trim();
        return first switch
        {
            "host: stopped" => UsbHostState.Stopped,
            "host: running" => UsbHostState.Running,
            _ when first.StartsWith("host: not answering", StringComparison.Ordinal) => UsbHostState.NotAnswering,
            _ => UsbHostState.Unknown,
        };
    }

    /// <summary>
    /// The microphone stream's state from the output of <c>uac stats</c>: its
    /// first line is <c>stream: running, ...</c> or <c>stream: stopped, ...</c>
    /// once a stream has been started since the host started, and
    /// <c>No stream since the USB host started</c> before that and while the
    /// host does not run.
    /// </summary>
    public static MicrophoneState ParseUacStats(IReadOnlyList<string> lines)
    {
        if (lines.Count == 0)
        {
            return MicrophoneState.Unknown;
        }
        string first = lines[0].Trim();
        if (first.StartsWith("stream: running,", StringComparison.Ordinal))
        {
            return MicrophoneState.Running;
        }
        if (first.StartsWith("stream: stopped,", StringComparison.Ordinal) ||
            first.StartsWith("No stream since the USB host started", StringComparison.Ordinal))
        {
            return MicrophoneState.Stopped;
        }
        return MicrophoneState.Unknown;
    }
}
