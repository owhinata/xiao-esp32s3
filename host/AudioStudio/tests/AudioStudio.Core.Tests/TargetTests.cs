// SPDX-License-Identifier: MIT
using AudioStudio.Core.Settings;
using AudioStudio.Core.Targets;
using FluentAssertions;
using Xunit;

namespace AudioStudio.Core.Tests;

/// <summary>The target profiles that come with the application, the checks of a profile, and the target settings.</summary>
public sealed class TargetTests : IDisposable
{
    private readonly DirectoryInfo _dir = Directory.CreateTempSubdirectory("audiostudio-targets-");

    public void Dispose() => _dir.Delete(recursive: true);

    [Fact]
    public void Both_profiles_come_with_the_application()
    {
        TargetProfile.Available().Should().Equal("unoq", "xiao-esp32s3");
        TargetProfile.Resolve(TargetProfile.DefaultName).Name.Should().Be("xiao-esp32s3");
    }

    [Fact]
    public void The_xiao_has_everything_on_its_address()
    {
        TargetProfile xiao = TargetProfile.Resolve("xiao-esp32s3");
        xiao.Shell.Transport.Should().Be(ShellTransport.Telnet);
        xiao.Shell.Port.Should().Be(23);
        xiao.Shell.RefreshAfter.Should().Equal("usb", "uac");
        xiao.Stream.Transport.Should().Be(StreamTransport.Tcp);
        xiao.Stream.Port.Should().Be(3233);
        xiao.Ota!.Port.Should().Be(3232);
        xiao.Address.Find.Should().BeTrue();
        ControlsSpec c = xiao.Controls;
        new object?[] { c.UsbHost, c.Microphone, c.Volume, c.Mute, c.Hum, c.HighPass }.Should().NotContainNulls();
        xiao.State.UsbHostQuery.Should().Be("usb status");
        xiao.State.MicrophoneQuery.Should().Be("uac stats");
        xiao.State.VolumeQuery.Should().Be("uac volume");
        xiao.State.HumQuery.Should().Be("uac hum");
        xiao.State.HighPassQuery.Should().Be("uac hpf");
        xiao.State.UsbHost(true).Line.Should().Be("usb host start");
        xiao.State.Microphone(false).Line.Should().Be("uac stop");
        xiao.State.Mute(true).Line.Should().Be("uac mute on");
        xiao.State.HighPass(false).Line.Should().Be("uac hpf off");
        c.Microphone!.RequiresUsbHost.Should().BeTrue();
        c.Volume!.WhileMicrophone.Should().BeTrue();
        c.Hum!.Options.Should().Equal("50", "60", "off");
    }

    [Fact]
    public void The_uno_q_has_an_ssh_shell_and_stream_and_no_controls()
    {
        TargetProfile unoq = TargetProfile.Resolve("unoq");
        unoq.Shell.Transport.Should().Be(ShellTransport.Ssh);
        unoq.Address.Default.Should().Be("arduino@unoq.local");
        unoq.Address.Find.Should().BeFalse();
        unoq.Stream.Transport.Should().Be(StreamTransport.Ssh);
        unoq.Stream.Command.Should().Be("flock -n -F -E 75 /home/arduino python3 -B ~/audiostudio-unoq/audio_stream.py");
        unoq.Ota.Should().BeNull();
        ControlsSpec c = unoq.Controls;
        new object?[] { c.UsbHost, c.Microphone, c.Volume, c.Mute, c.Hum, c.HighPass }.Should().OnlyContain(x => x == null);
        unoq.State.UsbHostQuery.Should().BeNull();
        unoq.State.HighPassQuery.Should().BeNull();
        unoq.Prompt.IsPrompt("Linux unoq\narduino@unoq:~$ ").Should().BeTrue();
        unoq.Prompt.IsPrompt("root@unoq:/# ").Should().BeTrue();
        unoq.Prompt.IsPrompt("total 0\n").Should().BeFalse();
    }

    private string Write(string json)
    {
        string path = Path.Combine(_dir.FullName, $"t{Guid.NewGuid():N}.json");
        File.WriteAllText(path, json);
        return path;
    }

    private const string Minimal = """
        // A comment, as the profiles have.
        { "name": "x", "shell": { "transport": "telnet", "port": 23, "prompt": "x\\$" },
          "stream": { "transport": "tcp", "port": 3233 }, %CONTROLS% }
        """;

    private string Profile(string controls) => Write(Minimal.Replace("%CONTROLS%", $"\"controls\": {{ {controls} }}"));

    [Fact]
    public void A_profile_given_by_its_path_is_read()
    {
        TargetProfile p = TargetProfile.Resolve(Profile(""));
        p.Name.Should().Be("x");
        p.Ota.Should().BeNull();
        p.FilePath.Should().StartWith(_dir.FullName);
    }

    [Theory]
    [InlineData("\"usbHost\": { \"query\": { \"command\": \"a\", \"replies\": [ { \"value\": \"on\", \"match\": \"x\" } ] }, \"on\": { \"command\": \"a\", \"success\": \"x\" }, \"off\": { \"command\": \"a\", \"success\": \"x\" }, \"tip\": \"t\" }", "is not one of")]
    [InlineData("\"highPass\": { \"query\": { \"command\": \"a\", \"replies\": [ { \"value\": \"on\", \"match\": \"(\" } ] }, \"on\": { \"command\": \"a\", \"success\": \"x\" }, \"off\": { \"command\": \"a\", \"success\": \"x\" }, \"tip\": \"t\" }", "controls.highPass.query.replies[0].match")]
    [InlineData("\"mute\": { \"on\": { \"command\": \"a\", \"success\": \"x\" }, \"off\": { \"command\": \"a\", \"success\": \"x\" }, \"tip\": \"t\" }", "controls.mute")]
    [InlineData("\"volumes\": {}", "volumes")]
    public void A_profile_that_does_not_make_sense_is_refused(string controls, string message)
    {
        string path = Profile(controls);
        FluentActions.Invoking(() => TargetProfile.Resolve(path))
            .Should().Throw<TargetProfileException>().Which.Message.Should().Contain(message).And.Contain(path);
    }

    [Theory]
    [InlineData("""{ "name": "x", "shell": { "transport": "ssh", "prompt": "\\$" }, "stream": { "transport": "tcp" } }""", "stream")]
    [InlineData("""{ "name": "x", "shell": { "transport": "telnet", "prompt": "\\$" }, "stream": { "transport": "tcp", "port": 1 } }""", "shell")]
    [InlineData("""{ "name": "x", "shell": { "transport": "telnet", "port": 23, "prompt": "\\$" }, "stream": { "transport": "ssh", "command": "c" } }""", "ssh shell")]
    [InlineData("""{ "name": "x", "shell": { "transport": "ssh", "prompt": "\\$" }, "stream": { "transport": "ssh", "command": "c" }, "ota": { "port": 3232 } }""", "ota")]
    [InlineData("""{ "name": "x", "shell": { "transport": "ftp", "prompt": "\\$" }, "stream": { "transport": "tcp", "port": 1 } }""", "$.shell.transport")]
    public void Transports_must_fit_together(string json, string message)
    {
        string path = Write(json);
        FluentActions.Invoking(() => TargetProfile.Load(path))
            .Should().Throw<TargetProfileException>().Which.Message.Should().Contain(message);
    }

    [Fact]
    public void An_unknown_target_is_said()
    {
        FluentActions.Invoking(() => TargetProfile.Resolve("no-such-board"))
            .Should().Throw<TargetProfileException>().Which.Message.Should().Contain("No target \"no-such-board\"");
    }

    [Fact]
    public void Settings_keep_an_address_per_target_and_take_the_former_board_address()
    {
        string path = Path.Combine(_dir.FullName, "settings.json");
        File.WriteAllText(path, """{ "BoardAddress": "10.42.0.100", "SpanSeconds": 10 }""");
        AppSettings settings = AppSettings.Load(path);
        settings.Target.Should().Be("xiao-esp32s3");
        settings.Addresses.Should().Equal(new Dictionary<string, string> { ["xiao-esp32s3"] = "10.42.0.100" });
        settings.SpanSeconds.Should().Be(10);
        settings.Addresses["unoq"] = "arduino@unoq.local";
        settings.Target = "unoq";
        settings.Save(path);
        File.ReadAllText(path).Should().NotContain("BoardAddress");
        AppSettings again = AppSettings.Load(path);
        again.Target.Should().Be("unoq");
        again.Addresses.Should().HaveCount(2).And.Contain("unoq", "arduino@unoq.local");
    }
}
