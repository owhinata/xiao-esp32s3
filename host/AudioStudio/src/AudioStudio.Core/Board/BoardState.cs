// SPDX-License-Identifier: MIT
using System.Globalization;
using System.Text.RegularExpressions;
using AudioStudio.Core.Targets;

namespace AudioStudio.Core.Board;

/// <summary>The target's USB host, as its query reports it.</summary>
public enum UsbHostState
{
    /// <summary>Not known: no shell, or a reply the profile does not know.</summary>
    Unknown,
    Stopped,
    Running,
    /// <summary>The host did not answer a request; it stays so until the board restarts.</summary>
    NotAnswering,
}

/// <summary>The target's microphone stream, as its query or the audio stream reports it.</summary>
public enum MicrophoneState
{
    /// <summary>Not known: neither the shell nor the audio stream tells it.</summary>
    Unknown,
    Stopped,
    Running,
}

/// <summary>A high-pass filter, as its query reports it.</summary>
public enum HighPassState
{
    /// <summary>Not known: no shell, or a reply the profile does not know (a firmware without the command).</summary>
    Unknown,
    Off,
    On,
}

/// <summary>A shell command that changes the target's state, and the pattern of the line it prints when it succeeds.</summary>
public sealed class BoardCommand(string line, Regex success)
{
    public string Line { get; } = line;

    /// <summary>
    /// Null when the output (<see cref="ShellPrompt.OutputLines"/>) has the
    /// success line; otherwise the target's first line (its error), or a
    /// note that it printed nothing.
    /// </summary>
    public string? Failure(IReadOnlyList<string> lines)
    {
        foreach (string text in lines)
        {
            if (success.IsMatch(text))
            {
                return null;
            }
        }
        return lines.Count > 0 ? lines[0] : "no answer from the board";
    }
}

/// <summary>
/// The microphone's volume and mute as the volume query reports them. The
/// XIAO reads them only while the microphone stream runs.
/// </summary>
/// <param name="Percent">Percent of the device's range; null when the target does not report it.</param>
/// <param name="Db">In dB; null when the target does not report it.</param>
/// <param name="SetPercent">The volume set in this session (set again at every stream start); null for the device's default.</param>
/// <param name="Muted">The mute; null when the device has no mute control.</param>
/// <param name="Refusal">Null when the target reported the volume; otherwise its line saying why not.</param>
public sealed record VolumeReading(int? Percent, double? Db, int? SetPercent, bool? Muted, string? Refusal)
{
    public bool Reported => Refusal is null;

    /// <summary>The device reports a volume, in percent or in dB.</summary>
    public bool HasVolume => Reported && (Percent is not null || Db is not null);

    public bool HasMute => Reported && Muted is not null;

    public static VolumeReading Refused(string line) => new(null, null, null, null, line);
}

/// <summary>
/// The queries of a target's state, the parsing of their replies and the
/// commands that change it, as its profile (<see cref="ControlsSpec"/>)
/// describes them. A control the profile does not have is null here and
/// its query is not run.
/// </summary>
public sealed class BoardState
{
    private const string NoAnswer = "no answer from the board";
    private static readonly TimeSpan MatchTimeout = TimeSpan.FromSeconds(1);

    private readonly Reader? _usbHost, _microphone, _hum, _highPass;
    private readonly Regex? _volume;
    private readonly Command? _usbHostOn, _usbHostOff, _microphoneOn, _microphoneOff, _volumeSet, _muteOn, _muteOff,
                              _humSet, _highPassOn, _highPassOff;

    public ControlsSpec Spec { get; }

    /// <exception cref="TargetProfileException">A pattern is not a regular expression, or a reply value is not one the control has.</exception>
    public BoardState(ControlsSpec spec)
    {
        Spec = spec;
        if (spec.UsbHost is ToggleSpec usb)
        {
            _usbHost = new Reader("controls.usbHost", usb.Query, ["stopped", "running", "notAnswering"]);
            (_usbHostOn, _usbHostOff) = (new("controls.usbHost.on", usb.On), new("controls.usbHost.off", usb.Off));
        }
        if (spec.Microphone is ToggleSpec mic)
        {
            if (mic.RequiresUsbHost && spec.UsbHost is null)
            {
                throw new TargetProfileException("controls.microphone: \"requiresUsbHost\" without controls.usbHost");
            }
            _microphone = new Reader("controls.microphone", mic.Query, ["stopped", "running"]);
            (_microphoneOn, _microphoneOff) = (new("controls.microphone.on", mic.On), new("controls.microphone.off", mic.Off));
        }
        if (spec.Volume is VolumeSpec volume)
        {
            if (volume.Minimum > volume.Maximum)
            {
                throw new TargetProfileException("controls.volume: \"minimum\" is above \"maximum\"");
            }
            if (volume.WhileMicrophone && spec.Microphone is null)
            {
                throw new TargetProfileException("controls.volume: \"whileMicrophone\" without controls.microphone");
            }
            _volume = Compile("controls.volume.match", volume.Match);
            _volumeSet = new("controls.volume.set", volume.Set);
        }
        if (spec.Mute is MuteSpec mute)
        {
            if (_volume is null || !_volume.GetGroupNames().Contains("mute"))
            {
                throw new TargetProfileException("controls.mute: needs controls.volume with a \"mute\" group in its match");
            }
            (_muteOn, _muteOff) = (new("controls.mute.on", mute.On), new("controls.mute.off", mute.Off));
        }
        if (spec.Hum is ChoiceSpec hum)
        {
            if (hum.Options.Count == 0)
            {
                throw new TargetProfileException("controls.hum: no \"options\"");
            }
            _hum = new Reader("controls.hum", hum.Query, hum.Options);
            _humSet = new("controls.hum.set", hum.Set);
        }
        if (spec.HighPass is ToggleSpec hpf)
        {
            _highPass = new Reader("controls.highPass", hpf.Query, ["on", "off"]);
            (_highPassOn, _highPassOff) = (new("controls.highPass.on", hpf.On), new("controls.highPass.off", hpf.Off));
        }
    }

    private static Regex Compile(string where, string pattern)
    {
        try
        {
            return new Regex(pattern, RegexOptions.CultureInvariant, MatchTimeout);
        }
        catch (ArgumentException e)
        {
            throw new TargetProfileException($"{where}: {e.Message}");
        }
    }

    private sealed class Reader
    {
        private readonly ReplyLines _lines;
        private readonly (string Value, Regex Match)[] _replies;

        public string Command { get; }

        public Reader(string where, ReplySpec spec, IReadOnlyCollection<string> values)
        {
            Command = spec.Command;
            _lines = spec.Lines;
            _replies = spec.Replies.Select((r, i) =>
            {
                if (!values.Contains(r.Value))
                {
                    throw new TargetProfileException(
                        $"{where}.query.replies[{i}]: value \"{r.Value}\" is not one of {string.Join(", ", values)}");
                }
                return (r.Value, Compile($"{where}.query.replies[{i}].match", r.Match));
            }).ToArray();
        }

        /// <summary>The value of the first line that matches a reply; null when none does.</summary>
        public string? Read(IReadOnlyList<string> lines)
        {
            foreach (string raw in _lines == ReplyLines.First ? lines.Take(1) : lines)
            {
                string line = raw.Trim();
                foreach ((string value, Regex match) in _replies)
                {
                    if (match.IsMatch(line))
                    {
                        return value;
                    }
                }
            }
            return null;
        }
    }

    private sealed class Command(string where, CommandSpec spec)
    {
        private readonly Regex _success = Compile(where + ".success", spec.Success);

        public BoardCommand With(string? value = null) =>
            new(value is null ? spec.Command : spec.Command.Replace("{value}", value, StringComparison.Ordinal), _success);
    }

    // ---- the queries: null when the profile does not have the control -------

    public string? UsbHostQuery => _usbHost?.Command;
    public string? MicrophoneQuery => _microphone?.Command;
    public string? VolumeQuery => Spec.Volume?.Query;
    public string? HumQuery => _hum?.Command;
    public string? HighPassQuery => _highPass?.Command;

    /// <summary>The USB host's state from the output of its query.</summary>
    public UsbHostState ParseUsbHost(IReadOnlyList<string> lines) => _usbHost?.Read(lines) switch
    {
        "stopped" => UsbHostState.Stopped,
        "running" => UsbHostState.Running,
        "notAnswering" => UsbHostState.NotAnswering,
        _ => UsbHostState.Unknown,
    };

    /// <summary>The microphone stream's state from the output of its query.</summary>
    public MicrophoneState ParseMicrophone(IReadOnlyList<string> lines) => _microphone?.Read(lines) switch
    {
        "stopped" => MicrophoneState.Stopped,
        "running" => MicrophoneState.Running,
        _ => MicrophoneState.Unknown,
    };

    /// <summary>
    /// The volume and mute from the output of the volume query (or of the
    /// command that sets it, which prints the same line when the setting was
    /// taken): the first line that matches the profile's pattern. Any other
    /// output is the target's refusal.
    /// </summary>
    public VolumeReading ParseVolume(IReadOnlyList<string> lines)
    {
        foreach (string line in lines)
        {
            if (_volume?.Match(line.Trim()) is not { Success: true } m)
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

    /// <summary>The hum filter from the output of its query: one of the options, or null for any other output.</summary>
    public string? ParseHum(IReadOnlyList<string> lines) => _hum?.Read(lines);

    /// <summary>The high-pass filter from the output of its query.</summary>
    public HighPassState ParseHighPass(IReadOnlyList<string> lines) => _highPass?.Read(lines) switch
    {
        "on" => HighPassState.On,
        "off" => HighPassState.Off,
        _ => HighPassState.Unknown,
    };

    // ---- the commands ------------------------------------------------------

    public BoardCommand UsbHost(bool on) => Need(on ? _usbHostOn : _usbHostOff, "usbHost").With();
    public BoardCommand Microphone(bool on) => Need(on ? _microphoneOn : _microphoneOff, "microphone").With();
    public BoardCommand Mute(bool on) => Need(on ? _muteOn : _muteOff, "mute").With();
    public BoardCommand HighPass(bool on) => Need(on ? _highPassOn : _highPassOff, "highPass").With();

    /// <summary>Sets the volume, which the target then prints as its query does.</summary>
    public BoardCommand Volume(int percent)
    {
        VolumeSpec spec = Spec.Volume ?? throw new InvalidOperationException("The target has no volume control");
        ArgumentOutOfRangeException.ThrowIfLessThan(percent, spec.Minimum);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(percent, spec.Maximum);
        return _volumeSet!.With(percent.ToString(CultureInfo.InvariantCulture));
    }

    /// <summary>Sets the hum filter to one of the options, which the target then prints as its query does.</summary>
    public BoardCommand Hum(string setting)
    {
        ChoiceSpec spec = Spec.Hum ?? throw new InvalidOperationException("The target has no hum filter");
        return spec.Options.Contains(setting)
            ? _humSet!.With(setting)
            : throw new ArgumentException($"Not {string.Join(", ", spec.Options)}: {setting}", nameof(setting));
    }

    private static Command Need(Command? command, string control) =>
        command ?? throw new InvalidOperationException($"The target has no control {control}");
}
