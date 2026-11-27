// SPDX-License-Identifier: MIT
using System.Net;
using System.Net.Sockets;
using System.Text;
using FluentAssertions;
using AudioStudio.Core.Board;
using Xunit;

namespace AudioStudio.Core.Tests;

/// <summary>The replies of shell/port/usb_shell.c and uac_shell.c.</summary>
public class BoardStateTests
{
    [Fact]
    public void Usb_status_stopped() =>
        BoardState.ParseUsbStatus(["host: stopped"]).Should().Be(UsbHostState.Stopped);

    [Fact]
    public void Usb_status_running() =>
        BoardState.ParseUsbStatus([
            "host: running",
            "root port: power on, connected, enabled, full speed, D+ 1 D- 0",
            "devices: 1, clients: 1",
        ]).Should().Be(UsbHostState.Running);

    [Fact]
    public void Usb_status_not_answering() =>
        BoardState.ParseUsbStatus(["host: not answering; it stays as it is until a reboot"])
            .Should().Be(UsbHostState.NotAnswering);

    [Theory]
    [InlineData("usb: command not found")]
    [InlineData("host: something else")]
    public void Usb_status_unknown(string line) =>
        BoardState.ParseUsbStatus([line]).Should().Be(UsbHostState.Unknown);

    [Fact]
    public void Usb_status_without_output() =>
        BoardState.ParseUsbStatus([]).Should().Be(UsbHostState.Unknown);

    [Fact]
    public void Uac_stats_running() =>
        BoardState.ParseUacStats([
            "stream: running, 48000 Hz, 1 channel, decimated to 16000 Hz",
            "data: from 0.002 s to 12.345 s after the start",
        ]).Should().Be(MicrophoneState.Running);

    [Fact]
    public void Uac_stats_stopped() =>
        BoardState.ParseUacStats(["stream: stopped, 48000 Hz, 2 channels, decimated to 16000 Hz", "data: none yet"])
            .Should().Be(MicrophoneState.Stopped);

    [Fact]
    public void Uac_stats_before_any_stream_or_without_the_host() =>
        BoardState.ParseUacStats([
            "No stream since the USB host started",
            "TCP stream (port 3233): no client; last client: 0 blocks, 0 samples sent, 0 lost; 0 clients dropped",
        ]).Should().Be(MicrophoneState.Stopped);

    [Theory]
    [InlineData("uac: command not found")]
    [InlineData("stream: unclear")]
    public void Uac_stats_unknown(string line) =>
        BoardState.ParseUacStats([line]).Should().Be(MicrophoneState.Unknown);

    [Fact]
    public void Uac_stats_without_output() =>
        BoardState.ParseUacStats([]).Should().Be(MicrophoneState.Unknown);

    public static TheoryData<string, string> Successes => new()
    {
        { "usb host start", "USB host started; the console is on UART0 and Telnet only" },
        { "usb host stop", "USB host stopped; USB-C is the console's again" },
        { "uac start", "Streaming: 48000 Hz, 1 channel, 16 bits; 16000 Hz out" },
        { "uac stop", "Stopped" },
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
    };

    private static BoardCommand Command(string line) =>
        new[] { BoardCommand.UsbHostStart, BoardCommand.UsbHostStop, BoardCommand.UacStart, BoardCommand.UacStop }
            .Single(c => c.Line == line);

    [Theory]
    [MemberData(nameof(Successes))]
    public void Command_success(string line, string reply) =>
        Command(line).Failure([reply]).Should().BeNull();

    [Theory]
    [MemberData(nameof(Failures))]
    public void Command_failure_is_the_boards_line(string line, string reply) =>
        Command(line).Failure([reply]).Should().Be(reply);

    [Fact]
    public void Command_without_output_failed() =>
        BoardCommand.UacStop.Failure([]).Should().NotBeNull();

    [Fact]
    public void A_capture_is_finished_when_the_prompt_ends_it()
    {
        BoardShell.Finished("host: stopped\nxiao:~$ ").Should().BeTrue();
        BoardShell.Finished("peak  -20.1 dBFS  RMS  -23.0 dBFS\n").Should().BeFalse();
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
            using BoardShell shell = await BoardShell.ConnectAsync("127.0.0.1", ((IPEndPoint)listener.LocalEndpoint).Port);
            StringBuilder shown = new();
            shell.Output += text => { lock (shown) { shown.Append(text); } };
            await shell.SettleAsync();
            Task<string> command = shell.ExecuteAsync("uac stop", TimeSpan.FromSeconds(5));
            Task<string> query = shell.QueryAsync("usb status", TimeSpan.FromSeconds(5));
            BoardShell.OutputLines(await command, "uac stop").Should().Equal("Stopped");
            BoardShell.OutputLines(await query, "usb status").Should().Equal("host: stopped");
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
