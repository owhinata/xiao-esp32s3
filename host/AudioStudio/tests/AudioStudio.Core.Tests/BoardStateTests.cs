// SPDX-License-Identifier: MIT
using System.Net;
using System.Net.Sockets;
using System.Text;
using FluentAssertions;
using AudioStudio.Core.Board;
using AudioStudio.Core.Targets;
using Xunit;

namespace AudioStudio.Core.Tests;

/// <summary>The replies of shell/port/usb_shell.c and uac_shell.c, read with the XIAO's target profile.</summary>
public class BoardStateTests
{
    private static readonly BoardState Xiao = TargetProfile.Resolve("xiao-esp32s3").State;

    [Fact]
    public void Usb_status_stopped() =>
        Xiao.ParseUsbHost(["host: stopped"]).Should().Be(UsbHostState.Stopped);

    [Fact]
    public void Usb_status_running() =>
        Xiao.ParseUsbHost([
            "host: running",
            "root port: power on, connected, enabled, full speed, D+ 1 D- 0",
            "devices: 1, clients: 1",
        ]).Should().Be(UsbHostState.Running);

    [Fact]
    public void Usb_status_not_answering() =>
        Xiao.ParseUsbHost(["host: not answering; it stays as it is until a reboot"])
            .Should().Be(UsbHostState.NotAnswering);

    [Theory]
    [InlineData("usb: command not found")]
    [InlineData("host: something else")]
    public void Usb_status_unknown(string line) =>
        Xiao.ParseUsbHost([line]).Should().Be(UsbHostState.Unknown);

    [Fact]
    public void Usb_status_without_output() =>
        Xiao.ParseUsbHost([]).Should().Be(UsbHostState.Unknown);

    [Fact]
    public void Uac_stats_running() =>
        Xiao.ParseMicrophone([
            "stream: running, 48000 Hz, 1 channel, decimated to 16000 Hz",
            "data: from 0.002 s to 12.345 s after the start",
        ]).Should().Be(MicrophoneState.Running);

    [Fact]
    public void Uac_stats_stopped() =>
        Xiao.ParseMicrophone(["stream: stopped, 48000 Hz, 2 channels, decimated to 16000 Hz", "data: none yet"])
            .Should().Be(MicrophoneState.Stopped);

    [Fact]
    public void Uac_stats_before_any_stream_or_without_the_host() =>
        Xiao.ParseMicrophone([
            "No stream since the USB host started",
            "TCP stream (port 3233): no client; last client: 0 blocks, 0 samples sent, 0 lost; 0 clients dropped",
        ]).Should().Be(MicrophoneState.Stopped);

    [Theory]
    [InlineData("uac: command not found")]
    [InlineData("stream: unclear")]
    public void Uac_stats_unknown(string line) =>
        Xiao.ParseMicrophone([line]).Should().Be(MicrophoneState.Unknown);

    [Fact]
    public void Uac_stats_without_output() =>
        Xiao.ParseMicrophone([]).Should().Be(MicrophoneState.Unknown);

    // uac volume (cmd_uac_volume in shell/port/uac_shell.c)

    [Fact]
    public void Uac_volume_all_known() =>
        Xiao.ParseVolume(["volume 50% (-12.50 dB), set to 50%; mute off"])
            .Should().Be(new VolumeReading(50, -12.5, 50, false, null));

    [Fact]
    public void Uac_volume_device_default_and_muted() =>
        Xiao.ParseVolume(["volume 100% (0.00 dB), device default; mute on"])
            .Should().Be(new VolumeReading(100, 0.0, null, true, null));

    [Fact]
    public void Uac_volume_positive_db() =>
        Xiao.ParseVolume(["volume 0% (6.00 dB), set to 0%; mute off"])
            .Should().Be(new VolumeReading(0, 6.0, 0, false, null));

    [Fact]
    public void Uac_volume_percent_unknown_db_known()
    {
        // A UAC 2.0 device without a volume range: the dB only.
        VolumeReading r = Xiao.ParseVolume(["volume ? (-0.50 dB), device default; mute off"]);
        r.Should().Be(new VolumeReading(null, -0.5, null, false, null));
        r.HasVolume.Should().BeTrue();
    }

    [Fact]
    public void Uac_volume_db_unknown_percent_known() =>
        Xiao.ParseVolume(["volume 30% (? dB), set to 30%; mute on"])
            .Should().Be(new VolumeReading(30, null, 30, true, null));

    [Fact]
    public void Uac_volume_no_controls()
    {
        VolumeReading r = Xiao.ParseVolume(["volume ? (? dB), device default; mute no control"]);
        r.Should().Be(new VolumeReading(null, null, null, null, null));
        r.Reported.Should().BeTrue();
        r.HasVolume.Should().BeFalse();
        r.HasMute.Should().BeFalse();
    }

    [Theory]
    [InlineData("Volume and mute are read and set while the stream runs (\"uac start\")")]
    [InlineData("The microphone has no such control")]
    [InlineData("The USB host is not running (\"usb host start\")")]
    [InlineData("No microphone")]
    [InlineData("The audio task did not answer; it stays as it is until a reboot")]
    [InlineData("Failed (-5); \"dmesg\" shows why")]
    [InlineData("Not a volume (0 .. 100): 101")]
    [InlineData("uac: command not found")]
    public void Uac_volume_refused(string line)
    {
        VolumeReading r = Xiao.ParseVolume([line]);
        r.Should().Be(VolumeReading.Refused(line));
        r.Reported.Should().BeFalse();
        r.HasVolume.Should().BeFalse();
        r.HasMute.Should().BeFalse();
    }

    [Fact]
    public void Uac_volume_without_output() =>
        Xiao.ParseVolume([]).Reported.Should().BeFalse();

    // uac hum (print_hum in shell/port/uac_shell.c)

    [Theory]
    [InlineData("hum filter: off", "off")]
    [InlineData("hum filter: 50 Hz mains (notches at 50, 100, .. 400 Hz)", "50")]
    [InlineData("hum filter: 60 Hz mains (notches at 60, 120, .. 420 Hz)", "60")]
    public void Uac_hum(string line, string setting) =>
        Xiao.ParseHum([line]).Should().Be(setting);

    [Theory]
    [InlineData("Not 50, 60 or off: 55")]
    [InlineData("The audio task did not answer; it stays as it is until a reboot")]
    [InlineData("Failed (-5); \"dmesg\" shows why")]
    [InlineData("hum filter: 70 Hz mains (notches at 70, 140, .. 420 Hz)")]
    public void Uac_hum_unknown(string line) =>
        Xiao.ParseHum([line]).Should().BeNull();

    [Fact]
    public void Uac_hum_without_output() =>
        Xiao.ParseHum([]).Should().BeNull();

    // uac hpf

    [Theory]
    [InlineData("high-pass filter: 30 Hz", HighPassState.On)]
    [InlineData("high-pass filter: off", HighPassState.Off)]
    [InlineData("hpf: command not found", HighPassState.Unknown)]
    [InlineData("uac - USB audio input (UAC 1.0 or 2.0 microphone)", HighPassState.Unknown)]
    [InlineData("high-pass filter: 20 Hz", HighPassState.Unknown)]
    public void Uac_hpf(string line, HighPassState state) =>
        Xiao.ParseHighPass([line]).Should().Be(state);

    [Fact]
    public void Uac_hpf_without_output() =>
        Xiao.ParseHighPass([]).Should().Be(HighPassState.Unknown);

    public static TheoryData<string, string> Successes => new()
    {
        { "usb host start", "USB host started; the console is on UART0 and Telnet only" },
        { "usb host stop", "USB host stopped; USB-C is the console's again" },
        { "uac start", "Streaming: 48000 Hz, 1 channel, 16 bits; 16000 Hz out" },
        { "uac stop", "Stopped" },
        { "uac volume 40", "volume 40% (-15.00 dB), set to 40%; mute off" },
        { "uac mute on", "Mute on" },
        { "uac mute off", "Mute off" },
        { "uac hum 60", "hum filter: 60 Hz mains (notches at 60, 120, .. 420 Hz)" },
        { "uac hum off", "hum filter: off" },
        { "uac hpf on", "high-pass filter: 30 Hz" },
        { "uac hpf off", "high-pass filter: off" },
    };

    public static TheoryData<string, string> Failures => new()
    {
        { "usb host start", "The USB host is already running" },
        { "usb host start", "Out of memory" },
        { "usb host start", "The USB host did not answer; it stays as it is until a reboot" },
        { "usb host start", "Failed (-5); \"dmesg\" shows why" },
        { "usb host stop", "The USB host is not running" },
        { "usb host stop", "The USB host did not answer; it stays as it is until a reboot" },
        { "uac start", "The USB host is not running (\"usb host start\")" },
        { "uac start", "No microphone" },
        { "uac start", "The stream is already running" },
        { "uac start", "The microphone has no such format (\"uac info\")" },
        { "uac start", "Not a format this input takes (1 or 2 channels, up to 48000 Hz stereo)" },
        { "uac start", "The audio task did not answer; it stays as it is until a reboot" },
        { "uac stop", "The stream is not running" },
        { "uac stop", "The USB host is not running (\"usb host start\")" },
        { "uac stop", "Failed (-5); \"dmesg\" shows why" },
        { "uac volume 40", "Volume and mute are read and set while the stream runs (\"uac start\")" },
        { "uac volume 40", "The microphone has no such control" },
        { "uac volume 40", "No microphone" },
        { "uac mute on", "Volume and mute are read and set while the stream runs (\"uac start\")" },
        { "uac mute off", "The microphone has no such control" },
        { "uac hum 60", "The audio task did not answer; it stays as it is until a reboot" },
        { "uac hpf on", "hpf: command not found" },
    };

    private static BoardCommand Command(string line) =>
        new[]
        {
            Xiao.UsbHost(true), Xiao.UsbHost(false), Xiao.Microphone(true), Xiao.Microphone(false),
            Xiao.Volume(40), Xiao.Mute(true), Xiao.Mute(false),
            Xiao.Hum("60"), Xiao.Hum("off"), Xiao.HighPass(true), Xiao.HighPass(false),
        }.Single(c => c.Line == line);

    [Theory]
    [MemberData(nameof(Successes))]
    public void Command_success(string line, string reply) =>
        Command(line).Failure([reply]).Should().BeNull();

    [Theory]
    [MemberData(nameof(Failures))]
    public void Command_failure_is_the_boards_line(string line, string reply) =>
        Command(line).Failure([reply]).Should().Be(reply);

    [Fact]
    public void Commands_take_only_what_the_board_takes()
    {
        FluentActions.Invoking(() => Xiao.Volume(101)).Should().Throw<ArgumentOutOfRangeException>();
        FluentActions.Invoking(() => Xiao.Volume(-1)).Should().Throw<ArgumentOutOfRangeException>();
        FluentActions.Invoking(() => Xiao.Hum("55")).Should().Throw<ArgumentException>();
        Xiao.Volume(0).Line.Should().Be("uac volume 0");
        Xiao.Hum("50").Line.Should().Be("uac hum 50");
    }

    [Fact]
    public void Command_without_output_failed() =>
        Xiao.Microphone(false).Failure([]).Should().NotBeNull();

    [Fact]
    public void A_capture_is_finished_when_the_prompt_ends_it()
    {
        ShellPrompt prompt = TargetProfile.Resolve("xiao-esp32s3").Prompt;
        prompt.Finished("host: stopped\nxiao:~$ ").Should().BeTrue();
        prompt.Finished("peak  -20.1 dBFS  RMS  -23.0 dBFS\n").Should().BeFalse();
    }

    [Fact]
    public async Task A_query_is_not_shown_and_the_next_command_is()
    {
        const string prompt = "xiao:~$ ";
        TcpListener listener = new(IPAddress.Loopback, 0);
        listener.Start();
        Task server = Task.Run(async () =>
        {
            using TcpClient client = await listener.AcceptTcpClientAsync();
            NetworkStream stream = client.GetStream();
            await stream.WriteAsync(Encoding.ASCII.GetBytes("\r\n\x1b[1;32m" + prompt + "\x1b[m"));
            StringBuilder line = new();
            byte[] one = new byte[1];
            while (await stream.ReadAsync(one) == 1)
            {
                if (one[0] != '\n')
                {
                    line.Append((char)one[0]);
                    continue;
                }
                string text = line.ToString().TrimEnd('\r');
                line.Clear();
                string reply = text == "usb status" ? "host: stopped\r\n" : "Stopped\r\n";
                // The prompt's trailing space and colour reset in a piece of their own.
                await stream.WriteAsync(Encoding.ASCII.GetBytes(text + "\r\n" + reply + "\x1b[1;32mxiao:~$"));
                await stream.FlushAsync();
                await Task.Delay(50);
                await stream.WriteAsync(Encoding.ASCII.GetBytes(" \x1b[m"));
            }
        });
        try
        {
            using BoardShell shell = await BoardShell.ConnectAsync("127.0.0.1", ((IPEndPoint)listener.LocalEndpoint).Port,
                                                                   TargetProfile.Resolve("xiao-esp32s3").Prompt);
            StringBuilder shown = new();
            shell.Output += text => { lock (shown) { shown.Append(text); } };
            await shell.SettleAsync();
            Task<string> command = shell.ExecuteAsync("uac stop", TimeSpan.FromSeconds(5));
            Task<IReadOnlyList<string>> query = shell.QueryAsync("usb status", TimeSpan.FromSeconds(5));
            shell.Prompt.OutputLines(await command, "uac stop").Should().Equal("Stopped");
            (await query).Should().Equal("host: stopped");
            await Task.Delay(200);
            lock (shown)
            {
                // The first prompt can come before the handler is attached.
                shown.ToString().Should().EndWith("\nStopped\n" + prompt,
                    "the query, which waited behind the command, shows nothing, not even its prompt")
                    .And.NotContain("usb status").And.NotContain("host: stopped");
            }
        }
        finally
        {
            listener.Stop();
            await Task.WhenAny(server, Task.Delay(1000));
        }
    }
}
