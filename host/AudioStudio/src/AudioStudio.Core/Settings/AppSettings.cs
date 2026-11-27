// SPDX-License-Identifier: MIT
using System.Text.Json;
using System.Text.Json.Serialization;
using AudioStudio.Core.Targets;

namespace AudioStudio.Core.Settings;

/// <summary>What the application remembers between runs, in <c>~/.config/AudioStudio/settings.json</c>.</summary>
public sealed class AppSettings
{
    public const string ImageName = "xiao_esp32s3_threadx.bin";
    /// <summary>The model loaded when none was chosen before: the M4 swallow detector in <c>host/AudioStudio/models/</c>.</summary>
    public const string DefaultModel = "models/m4-0038.npz";

    /// <summary>The target profile: a name in <c>targets/</c> beside the application, or the path of a profile.</summary>
    public string Target { get; set; } = TargetProfile.DefaultName;
    /// <summary>The address entered for each target, by the profile's name.</summary>
    public Dictionary<string, string> Addresses { get; set; } = new();
    /// <summary>
    /// The board address of a settings file written before there were
    /// targets: read once as the XIAO's address, and not written again.
    /// </summary>
    [JsonPropertyName("BoardAddress")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? FormerBoardAddress { get; set; }
    /// <summary>The ssh program; empty for <c>ssh</c> on the PATH. The environment variable AUDIOSTUDIO_SSH overrides it.</summary>
    public string SshPath { get; set; } = "";
    public string RecordingsDirectory { get; set; } = "";
    public string RecordingLabel { get; set; } = "";
    public double ShellPaneHeight { get; set; } = 190;
    public bool ShellPaneVisible { get; set; } = true;
    public double SpanSeconds { get; set; } = 5;
    public int AmplitudeZoom { get; set; } = 1;
    public int FftSize { get; set; } = 2048;
    /// <summary>Top of the spectrogram's frequency axis: "Nyquist" or a number of Hz.</summary>
    public string MaxFrequency { get; set; } = "Nyquist";
    public bool LogFrequency { get; set; } = true;
    public double MinDb { get; set; } = -110;
    public double MaxDb { get; set; } = -20;
    public string ModelPath { get; set; } = "";
    /// <summary>Samples per window for a model whose input length is not fixed.</summary>
    public int ModelWindowSamples { get; set; } = 16000;
    /// <summary>Samples between windows; 0 for half a window.</summary>
    public int ModelHopSamples { get; set; }
    /// <summary>The firmware repository; empty to look for it from the application's directory upwards.</summary>
    public string RepositoryDirectory { get; set; } = "";

    public static string DefaultFilePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "AudioStudio", "settings.json");

    public static AppSettings Load(string? path = null)
    {
        AppSettings settings;
        try
        {
            settings = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(path ?? DefaultFilePath)) ?? new AppSettings();
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
            return new AppSettings();
        }
        if (!string.IsNullOrWhiteSpace(settings.FormerBoardAddress))
        {
            settings.Addresses.TryAdd(TargetProfile.DefaultName, settings.FormerBoardAddress);
        }
        settings.FormerBoardAddress = null;
        return settings;
    }

    public void Save(string? path = null)
    {
        path ??= DefaultFilePath;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }

    /// <summary>
    /// Where recordings go: the configured directory, or <c>recordings/</c>
    /// beside this application's sources when it runs from a build inside
    /// them, or a directory in the user's home when run from elsewhere.
    /// </summary>
    public string ResolveRecordingsDirectory()
    {
        if (!string.IsNullOrWhiteSpace(RecordingsDirectory))
        {
            return RecordingsDirectory;
        }
        return SourceDirectory() is DirectoryInfo sources
            ? Path.Combine(sources.FullName, "recordings")
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "AudioStudio", "recordings");
    }

    /// <summary>
    /// The firmware image the build of the repository makes
    /// (<c>build/xiao_esp32s3_threadx.bin</c>): in the configured repository,
    /// or in the first directory from the application's own upwards that has
    /// one. Null when there is none.
    /// </summary>
    public string? ResolveFirmwareImage()
    {
        if (!string.IsNullOrWhiteSpace(RepositoryDirectory))
        {
            string configured = Path.Combine(RepositoryDirectory, "build", ImageName);
            return File.Exists(configured) ? configured : null;
        }
        for (DirectoryInfo? dir = new(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            string candidate = Path.Combine(dir.FullName, "build", ImageName);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }
        return null;
    }

    /// <summary>
    /// The model to load when none was chosen before: <see cref="DefaultModel"/>
    /// in the first directory from the application's own upwards that has
    /// it (the application's sources, for a build inside them). Null when
    /// there is none.
    /// </summary>
    public static string? ResolveDefaultModel()
    {
        for (DirectoryInfo? dir = new(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            string candidate = Path.Combine(dir.FullName, DefaultModel);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }
        return null;
    }

    /// <summary>The directory of this application's sources, if it runs from a build inside it.</summary>
    private static DirectoryInfo? SourceDirectory()
    {
        for (DirectoryInfo? dir = new(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "AudioStudio.slnx")))
            {
                return dir;
            }
        }
        return null;
    }
}
