// SPDX-License-Identifier: MIT
using System.Globalization;
using System.Text.RegularExpressions;

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

    /// <summary><c>uac volume &lt;0..100&gt;</c>: sets the volume, then prints it as <c>uac volume</c> does.</summary>
    public static BoardCommand UacVolume(int percent)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(percent, 0);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(percent, 100);
        return new(string.Create(CultureInfo.InvariantCulture, $"uac volume {percent}"), "volume ");
    }

    /// <summary><c>uac mute on|off</c>, which prints <c>Mute on</c> or <c>Mute off</c>.</summary>
    public static BoardCommand UacMute(bool on) =>
        on ? new("uac mute on", "Mute on") : new("uac mute off", "Mute off");

    /// <summary><c>uac hum 50|60|off</c>: sets the hum filter, then prints it as <c>uac hum</c> does.</summary>
    public static BoardCommand UacHum(string setting) => setting is "50" or "60" or "off"
        ? new("uac hum " + setting, "hum filter:")
        : throw new ArgumentException($"Not 50, 60 or off: {setting}", nameof(setting));

    /// <summary><c>uac hpf on|off</c>: sets the high-pass filter, then prints it as <c>uac hpf</c> does.</summary>
    public static BoardCommand UacHpf(bool on) => new(on ? "uac hpf on" : "uac hpf off", "high-pass filter:");

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

/// <summary>
/// The microphone's volume and mute as <c>uac volume</c> reports them. The
/// board reads them only while the microphone stream runs.
/// </summary>
/// <param name="Percent">Percent of the device's range; null when the board prints <c>?</c>.</param>
/// <param name="Db">In dB; null when the board prints <c>? dB</c>.</param>
/// <param name="SetPercent">The volume set in this session (set again at every stream start); null for the device's default.</param>
/// <param name="Muted">The mute; null when the device has no mute control.</param>
/// <param name="Refusal">Null when the board reported the volume; otherwise its line saying why not.</param>
public sealed record VolumeReading(int? Percent, double? Db, int? SetPercent, bool? Muted, string? Refusal)
{
    public bool Reported => Refusal is null;

    /// <summary>The device reports a volume, in percent or in dB.</summary>
    public bool HasVolume => Reported && (Percent is not null || Db is not null);

    public bool HasMute => Reported && Muted is not null;

    public static VolumeReading Refused(string line) => new(null, null, null, null, line);
}

/// <summary>The board's 30 Hz high-pass filter, as <c>uac hpf</c> reports it.</summary>
public enum HighPassState
{
    /// <summary>Not known: no shell, or a reply this application does not know (a firmware without the command).</summary>
    Unknown,
    Off,
    On,
}

/// <summary>The queries of the board's state and the parsing of their replies.</summary>
public static partial class BoardState
{
    public const string UsbStatusLine = "usb status";
    public const string UacStatsLine = "uac stats";
    public const string UacVolumeLine = "uac volume";
    public const string UacHumLine = "uac hum";
    public const string UacHpfLine = "uac hpf";

    private const string NoAnswer = "no answer from the board";

    // "volume 50% (-12.50 dB), set to 50%; mute off", any field unknown as
    // "?", "? dB", "device default", "mute no control".
    [GeneratedRegex(@"^volume (?:(?<percent>\d{1,3})%|\?) \((?:(?<db>-?\d+\.\d{2}) dB|\? dB)\), " +
                    @"(?:set to (?<set>\d{1,3})%|device default); mute (?<mute>on|off|no control)$")]
    private static partial Regex VolumeLine();

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

    /// <summary>
    /// The volume and mute from the output of <c>uac volume</c> (or of
    /// <c>uac volume &lt;0..100&gt;</c>, which prints the same line when the
    /// setting was taken): one line <c>volume P% (D dB), set to S%; mute M</c>
    /// with <c>?</c>, <c>? dB</c>, <c>device default</c> and
    /// <c>no control</c> for what the device does not give. Any other output
    /// is the board's refusal: the stream does not run, the USB host does not
    /// run, there is no microphone, or the request failed.
    /// </summary>
    public static VolumeReading ParseUacVolume(IReadOnlyList<string> lines)
    {
        foreach (string line in lines)
        {
            Match m = VolumeLine().Match(line.Trim());
            if (!m.Success)
            {
                continue;
            }
            int? percent = m.Groups["percent"].Success ? int.Parse(m.Groups["percent"].Value, CultureInfo.InvariantCulture) : null;
            double? db = m.Groups["db"].Success ? double.Parse(m.Groups["db"].Value, CultureInfo.InvariantCulture) : null;
            int? set = m.Groups["set"].Success ? int.Parse(m.Groups["set"].Value, CultureInfo.InvariantCulture) : null;
            bool? muted = m.Groups["mute"].Value switch
            {
                "on" => true,
                "off" => false,
                _ => null,
            };
            return new VolumeReading(percent, db, set, muted, null);
        }
        return VolumeReading.Refused(lines.Count > 0 ? lines[0].Trim() : NoAnswer);
    }

    /// <summary>
    /// The hum filter from the output of <c>uac hum</c>: <c>hum filter: off</c>
    /// or <c>hum filter: 50 Hz mains (notches at ...)</c> (or 60). The
    /// result is the argument of <c>uac hum</c> that sets it (<c>50</c>,
    /// <c>60</c> or <c>off</c>); null for any other output.
    /// </summary>
    public static string? ParseUacHum(IReadOnlyList<string> lines)
    {
        foreach (string raw in lines)
        {
            string line = raw.Trim();
            if (line == "hum filter: off")
            {
                return "off";
            }
            if (line.StartsWith("hum filter: 50 Hz mains", StringComparison.Ordinal))
            {
                return "50";
            }
            if (line.StartsWith("hum filter: 60 Hz mains", StringComparison.Ordinal))
            {
                return "60";
            }
        }
        return null;
    }

    /// <summary>
    /// The high-pass filter from the output of <c>uac hpf</c>:
    /// <c>high-pass filter: 30 Hz</c> or <c>high-pass filter: off</c>. Any
    /// other output (a firmware without the command prints an error) is
    /// <see cref="HighPassState.Unknown"/>.
    /// </summary>
    public static HighPassState ParseUacHpf(IReadOnlyList<string> lines)
    {
        foreach (string raw in lines)
        {
            switch (raw.Trim())
            {
                case "high-pass filter: 30 Hz":
                    return HighPassState.On;
                case "high-pass filter: off":
                    return HighPassState.Off;
            }
        }
        return HighPassState.Unknown;
    }
}
