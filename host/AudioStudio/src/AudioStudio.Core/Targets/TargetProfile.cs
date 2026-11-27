// SPDX-License-Identifier: MIT
using System.Text.Json;
using System.Text.Json.Serialization;
using AudioStudio.Core.Board;

namespace AudioStudio.Core.Targets;

/// <summary>A target profile that cannot be read or does not make sense.</summary>
public sealed class TargetProfileException(string message) : Exception(message);

public enum ShellTransport
{
    /// <summary>A Telnet session on <see cref="ShellSpec.Port"/> of the address.</summary>
    Telnet,
    /// <summary>An interactive <c>ssh -tt</c> session to the address (<c>user@host</c>).</summary>
    Ssh,
}

public enum StreamTransport
{
    /// <summary>The XAUD stream on TCP <see cref="StreamSpec.Port"/> of the address.</summary>
    Tcp,
    /// <summary>The standard output of <see cref="StreamSpec.Command"/>, run over ssh on the address.</summary>
    Ssh,
}

/// <summary>Which lines of a reply a <see cref="ReplySpec"/> looks at.</summary>
public enum ReplyLines
{
    /// <summary>Every line, in order; the first that matches a reply decides.</summary>
    Any,
    /// <summary>The first line only.</summary>
    First,
}

/// <summary>
/// What the application knows about one kind of target: how to reach its
/// shell and its audio stream, which controls it has and the commands behind
/// them, and whether it takes firmware updates. Read from a JSON file
/// (comments allowed) in <c>targets/</c> beside the application.
/// </summary>
public sealed class TargetProfile
{
    public const string DefaultName = "xiao-esp32s3";
    public const string DirectoryName = "targets";

    private static readonly JsonSerializerOptions s_json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase, allowIntegerValues: false) },
    };

    /// <summary>The name <c>--target</c> and the settings use.</summary>
    public required string Name { get; init; }
    /// <summary>The name shown in the target selector.</summary>
    public string Title { get; init; } = "";
    public AddressSpec Address { get; init; } = new();
    public required ShellSpec Shell { get; init; }
    public required StreamSpec Stream { get; init; }
    public ControlsSpec Controls { get; init; } = new();
    /// <summary>The OTA service; null when the target takes no firmware update (no Firmware tab).</summary>
    public OtaSpec? Ota { get; init; }

    /// <summary>The file the profile was read from.</summary>
    [JsonIgnore] public string FilePath { get; private set; } = "";
    /// <summary>The controls' queries and commands, with their patterns compiled.</summary>
    [JsonIgnore] public BoardState State { get; private set; } = null!;
    [JsonIgnore] public ShellPrompt Prompt { get; private set; } = null!;

    /// <summary>The directory of the profiles that come with the application.</summary>
    public static string DefaultDirectory => Path.Combine(AppContext.BaseDirectory, DirectoryName);

    /// <summary>Reads and checks a profile.</summary>
    /// <exception cref="TargetProfileException">The file cannot be read or is not a valid profile.</exception>
    public static TargetProfile Load(string path)
    {
        TargetProfile? profile;
        try
        {
            profile = JsonSerializer.Deserialize<TargetProfile>(File.ReadAllText(path), s_json);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            throw new TargetProfileException($"Target profile {path}: {e.Message}");
        }
        if (profile is null)
        {
            throw new TargetProfileException($"Target profile {path}: empty");
        }
        profile.FilePath = Path.GetFullPath(path);
        try
        {
            profile.Check();
        }
        catch (TargetProfileException e)
        {
            throw new TargetProfileException($"Target profile {path}: {e.Message}");
        }
        return profile;
    }

    /// <summary>
    /// The profile of <paramref name="nameOrPath"/>: a file, when it names
    /// one (a path, or a name ending in <c>.json</c>), otherwise
    /// <c>&lt;name&gt;.json</c> in <paramref name="directory"/> (default
    /// <see cref="DefaultDirectory"/>).
    /// </summary>
    public static TargetProfile Resolve(string nameOrPath, string? directory = null)
    {
        string text = nameOrPath.Trim();
        if (text.Contains('/') || text.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
        {
            return Load(text);
        }
        string path = Path.Combine(directory ?? DefaultDirectory, text + ".json");
        if (!File.Exists(path))
        {
            throw new TargetProfileException($"No target \"{text}\" (no {path})");
        }
        return Load(path);
    }

    /// <summary>The names of the profiles in <paramref name="directory"/> (default <see cref="DefaultDirectory"/>), sorted.</summary>
    public static IReadOnlyList<string> Available(string? directory = null)
    {
        try
        {
            return Directory.GetFiles(directory ?? DefaultDirectory, "*.json")
                .Select(Path.GetFileNameWithoutExtension).OfType<string>()
                .Order(StringComparer.Ordinal).ToList();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return [];
        }
    }

    private void Check()
    {
        if (string.IsNullOrWhiteSpace(Name))
        {
            throw new TargetProfileException("\"name\" is empty");
        }
        switch (Shell.Transport)
        {
            case ShellTransport.Telnet when Shell.Port is not (>= 1 and <= 65535):
                throw new TargetProfileException("shell: a Telnet shell needs \"port\" (1 .. 65535)");
            case ShellTransport.Ssh when Shell.Port is not null:
                throw new TargetProfileException("shell: an ssh shell takes no \"port\" (the address is user@host)");
        }
        switch (Stream.Transport)
        {
            case StreamTransport.Tcp when Stream.Port is not (>= 1 and <= 65535) || Stream.Command is not null:
                throw new TargetProfileException("stream: a TCP stream needs \"port\" (1 .. 65535) and no \"command\"");
            case StreamTransport.Ssh when string.IsNullOrWhiteSpace(Stream.Command) || Stream.Port is not null:
                throw new TargetProfileException("stream: an ssh stream needs \"command\" and no \"port\"");
        }
        if (Stream.Transport == StreamTransport.Ssh && Shell.Transport != ShellTransport.Ssh)
        {
            throw new TargetProfileException("stream: an ssh stream needs an ssh shell (the address is the ssh destination)");
        }
        if (Ota is not null && Ota.Port is not (>= 1 and <= 65535))
        {
            throw new TargetProfileException("ota: \"port\" is 1 .. 65535");
        }
        if (Ota is not null && Shell.Transport != ShellTransport.Telnet)
        {
            throw new TargetProfileException("ota: the OTA service is on the address of a Telnet target");
        }
        Prompt = new ShellPrompt(Shell.Prompt);
        State = new BoardState(Controls);
    }
}

/// <summary>The board's address as typed in the application.</summary>
public sealed class AddressSpec
{
    /// <summary>The address when none was entered for this target before.</summary>
    public string Default { get; init; } = "";
    /// <summary>The text shown in the empty address field.</summary>
    public string Hint { get; init; } = "address";
    /// <summary>The board can be found in the neighbour table of the access point this PC runs (the Find button).</summary>
    public bool Find { get; init; }
}

public sealed class ShellSpec
{
    public ShellTransport Transport { get; init; }
    /// <summary>The Telnet port, used when the address has none.</summary>
    public int? Port { get; init; }
    /// <summary>A regular expression that matches the shell's prompt at the end of its output.</summary>
    public required string Prompt { get; init; }
    /// <summary>The tooltip of the Shell button.</summary>
    public string Tip { get; init; } = "Connect or disconnect the shell";
    /// <summary>A line typed in the shell pane whose first word is one of these makes the application read the board's state again.</summary>
    public List<string> RefreshAfter { get; init; } = [];
}

public sealed class StreamSpec
{
    public StreamTransport Transport { get; init; }
    public int? Port { get; init; }
    /// <summary>The command run over ssh whose standard output is the XAUD stream.</summary>
    public string? Command { get; init; }
}

public sealed class OtaSpec
{
    public int Port { get; init; }
}

/// <summary>The controls in the bar; a control that is absent is hidden.</summary>
public sealed class ControlsSpec
{
    /// <summary>The USB host button. Reply values: <c>stopped</c>, <c>running</c>, <c>notAnswering</c>.</summary>
    public ToggleSpec? UsbHost { get; init; }
    /// <summary>The Microphone button. Reply values: <c>stopped</c>, <c>running</c>.</summary>
    public ToggleSpec? Microphone { get; init; }
    public VolumeSpec? Volume { get; init; }
    /// <summary>The Mute button; its state is the <c>mute</c> group of the volume's reply.</summary>
    public MuteSpec? Mute { get; init; }
    /// <summary>The Hum selector. Reply values: the options.</summary>
    public ChoiceSpec? Hum { get; init; }
    /// <summary>The HPF button. Reply values: <c>on</c>, <c>off</c>.</summary>
    public ToggleSpec? HighPass { get; init; }
}

/// <summary>A query that reads a state: the command, and the replies it knows.</summary>
public sealed class ReplySpec
{
    public required string Command { get; init; }
    public ReplyLines Lines { get; init; } = ReplyLines.Any;
    /// <summary>Tried in order on each line (trimmed); the first that matches gives the value.</summary>
    public required List<ReplyValue> Replies { get; init; }
}

public sealed class ReplyValue
{
    public required string Value { get; init; }
    /// <summary>A regular expression for the trimmed line.</summary>
    public required string Match { get; init; }
}

/// <summary>A command that changes the state, and the line it prints when it succeeds.</summary>
public sealed class CommandSpec
{
    /// <summary>The line sent; <c>{value}</c> stands for the value set.</summary>
    public required string Command { get; init; }
    /// <summary>A regular expression; a line of the output that matches it means the command succeeded.</summary>
    public required string Success { get; init; }
}

/// <summary>A button with two states.</summary>
public sealed class ToggleSpec
{
    public required ReplySpec Query { get; init; }
    public required CommandSpec On { get; init; }
    public required CommandSpec Off { get; init; }
    /// <summary>What the control is, for its tooltip.</summary>
    public required string Tip { get; init; }
    /// <summary>Added to the tooltip while the shell is connected.</summary>
    public string? Note { get; init; }
    /// <summary>The tooltip while the shell is connected and the state is not known.</summary>
    public string? UnknownTip { get; init; }
    /// <summary>Microphone only: the stream needs the USB host; a stopped host has no stream (no query).</summary>
    public bool RequiresUsbHost { get; init; }
}

/// <summary>The volume field and its Set button.</summary>
public sealed class VolumeSpec
{
    public required string Query { get; init; }
    /// <summary>
    /// A regular expression for the trimmed line of the reading, with the
    /// optional groups <c>percent</c>, <c>db</c>, <c>set</c> (the percent set
    /// in this session) and <c>mute</c> (<c>on</c>, <c>off</c>, anything
    /// else: no mute control). Any other output is the target's refusal.
    /// </summary>
    public required string Match { get; init; }
    public required CommandSpec Set { get; init; }
    public int Minimum { get; init; }
    public int Maximum { get; init; } = 100;
    /// <summary>The target reads and sets the volume and mute only while the microphone stream runs.</summary>
    public bool WhileMicrophone { get; init; }
    public required string Tip { get; init; }
}

public sealed class MuteSpec
{
    public required CommandSpec On { get; init; }
    public required CommandSpec Off { get; init; }
    public required string Tip { get; init; }
}

/// <summary>A selector of one of a few settings.</summary>
public sealed class ChoiceSpec
{
    public required ReplySpec Query { get; init; }
    public required List<string> Options { get; init; }
    public required CommandSpec Set { get; init; }
    public required string Tip { get; init; }
    public string? Note { get; init; }
}
