// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using System.Text;
using AudioStudio.Core.Board;
using AudioStudio.Core.Ssh;
using AudioStudio.Core.Stream;
using AudioStudio.Core.Targets;
using FluentAssertions;
using Xunit;

namespace AudioStudio.Core.Tests;

/// <summary>
/// The ssh transport with a fake ssh program: a shell script that takes the
/// options AudioStudio passes, logs each run (its pid, whether it has a
/// terminal, whether the master's control socket existed, the command) and
/// acts as the master, the stream command, the interactive shell or a query.
/// </summary>
public sealed class SshTests : IDisposable
{
    private const string Destination = "arduino@unoq.local";
    private static readonly TargetProfile UnoQ = TargetProfile.Resolve("unoq");

    private readonly DirectoryInfo _dir = Directory.CreateTempSubdirectory("audiostudio-fake-ssh-");
    private string Log => Path.Combine(_dir.FullName, "log");

    public void Dispose() => _dir.Delete(recursive: true);

    private static string FixturePath(string name) => Path.Combine(AppContext.BaseDirectory, "Fixtures", name);

    /// <summary>Writes the fake: <paramref name="master"/> runs for the master's session, <paramref name="stream"/> for the stream command.</summary>
    private string FakeSsh(string? master = null, string? stream = null)
    {
        master ??= """: > "$cp"; exec cat >/dev/null""";
        stream ??= $"exec cat '{FixturePath("unoq_stream.xaud")}'";
        string script = $$"""
            #!/bin/bash
            cp=""; tty=0
            while [ $# -gt 0 ]; do
              case "$1" in
                -o) case "$2" in ControlPath=*) cp="${2#ControlPath=}";; esac; shift 2;;
                -e) shift 2;;
                -tt) tty=1; shift;;
                -T) shift;;
                -*) echo "fake ssh: unknown option $1" >&2; exit 254;;
                *) break;;
              esac
            done
            dest="$1"; shift
            cmd="$*"
            sock=0; [ -e "$cp" ] && sock=1
            echo "$$ tty=$tty sock=$sock dest=$dest cmd=$cmd" >> '{{Log}}'
            if [ "$cmd" = "{{SshConnection.MasterCommand}}" ]; then
              {{master}}
            fi
            if [ $tty = 1 ]; then
              printf 'Linux unoq\r\nfake@unoq:~$ '
              while IFS= read -r line; do
                printf '%s\r\n' "$line"
                [ "$line" = exit ] && exit 0
                printf 'read %d characters\r\n' "${#line}"
                printf 'fake@unoq:~$ '
              done
              exit 0
            fi
            case "$cmd" in
              flock*) {{stream}};;
              uname) echo Linux; exit 0;;
            esac
            echo "fake ssh: no such command: $cmd" >&2
            exit 127
            """;
        string path = Path.Combine(_dir.FullName, "ssh");
        File.WriteAllText(path, script.Replace("\r\n", "\n"));
        if (!OperatingSystem.IsWindows())
        {
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        }
        return path;
    }

    private sealed record Run(int Pid, bool Terminal, bool Socket, string Destination, string Command);

    private List<Run> Runs()
    {
        if (!File.Exists(Log))
        {
            return [];
        }
        return File.ReadAllLines(Log).Select(line =>
        {
            string[] f = line.Split(' ', 5);
            return new Run(int.Parse(f[0]), f[1] == "tty=1", f[2] == "sock=1", f[3]["dest=".Length..], f[4]["cmd=".Length..]);
        }).ToList();
    }

    /// <summary>Whether the process has ended (and was reaped) within a few seconds.</summary>
    private static async Task<bool> GoneAsync(int pid)
    {
        for (int i = 0; i < 100; i++)
        {
            string stat = $"/proc/{pid}/stat";
            try
            {
                if (!File.Exists(stat) || File.ReadAllText(stat).Split(')')[^1].Trim().StartsWith('Z'))
                {
                    return true;
                }
            }
            catch (IOException)
            {
                return true;
            }
            await Task.Delay(50);
        }
        return false;
    }

    private async Task AllGoneAsync()
    {
        foreach (Run run in Runs())
        {
            (await GoneAsync(run.Pid)).Should().BeTrue($"the fake ssh {run.Pid} ({run.Command}) has ended");
        }
    }

    private static short[] Expected()
    {
        byte[] bytes = File.ReadAllBytes(FixturePath("unoq_decimate3.s16"));
        short[] samples = new short[bytes.Length / 2];
        for (int i = 0; i < samples.Length; i++)
        {
            samples[i] = BinaryPrimitives.ReadInt16LittleEndian(bytes.AsSpan(2 * i));
        }
        return samples;
    }

    [Fact]
    public async Task Stream_through_ssh_is_the_tools_output()
    {
        SshConnection ssh = new(Destination, FakeSsh());
        string control = ssh.ControlPath;
        SshAudioSource source = new(ssh, UnoQ.Stream.Command!);
        List<AudioBlock> blocks = new();
        TaskCompletionSource<string> ended = new(TaskCreationOptions.RunContinuationsAsynchronously);
        using (AudioStreamClient client = new(source))
        {
            client.BlockReceived += b =>
            {
                lock (blocks)
                {
                    blocks.Add(b);
                }
            };
            client.Disconnected += reason => ended.TrySetResult(reason);
            client.Start();
            (await ended.Task.WaitAsync(TimeSpan.FromSeconds(10))).Should().Be("the stream command ended");
            client.Status().Header.Should().Be(new AudioStreamHeader(1, 1, 16000));
        }
        ssh.Dispose();

        List<AudioBlock> first;
        lock (blocks)
        {
            first = blocks.ToList();
        }
        short[] samples = first.SelectMany(b => b.Samples).Take(9590).ToArray();
        short[] expected = Expected();
        samples.Should().HaveCount(expected.Length - 10);
        samples.Should().Equal(expected[..samples.Length], "the tool's samples are features_np.decimate3's");
        first[0].NewStream.Should().BeTrue();
        first.Take(30).Skip(1).Should().OnlyContain(b => !b.NewStream && b.Lost == 0 && !b.Idle);
        first.Take(29).Should().OnlyContain(b => b.Samples.Length == 320);

        List<Run> runs = Runs();
        runs.Where(r => r.Command == SshConnection.MasterCommand).Should().HaveCount(1, "one master for the connection");
        runs.Where(r => r.Command.StartsWith("flock", StringComparison.Ordinal))
            .Should().OnlyContain(r => r.Socket && !r.Terminal && r.Destination == Destination)
            .And.Contain(r => r.Command == "flock -n -F -E 75 /home/arduino python3 -B ~/audiostudio-unoq/audio_stream.py");
        await AllGoneAsync();
        Directory.Exists(Path.GetDirectoryName(control)).Should().BeFalse("the control directory is removed");
    }

    [Fact]
    public async Task Stopping_the_stream_ends_the_ssh_process()
    {
        string stream = $"cat '{FixturePath("unoq_stream.xaud")}'; exec sleep 600";
        using SshConnection ssh = new(Destination, FakeSsh(stream: stream));
        TaskCompletionSource started = new(TaskCreationOptions.RunContinuationsAsynchronously);
        AudioStreamClient client = new(new SshAudioSource(ssh, UnoQ.Stream.Command!));
        client.BlockReceived += _ => started.TrySetResult();
        client.Start();
        await started.Task.WaitAsync(TimeSpan.FromSeconds(10));
        Run run = Runs().Single(r => r.Command.StartsWith("flock", StringComparison.Ordinal));
        client.Dispose();
        (await GoneAsync(run.Pid)).Should().BeTrue("disconnecting ends the stream's ssh process");
        Run master = Runs().Single(r => r.Command == SshConnection.MasterCommand);
        (await GoneAsync(master.Pid)).Should().BeTrue("the master ends with its last user");
    }

    [Fact]
    public async Task Exit_75_says_the_adapter_is_in_use()
    {
        using SshConnection ssh = new(Destination, FakeSsh(stream: "exit 75"));
        TaskCompletionSource<string> ended = new(TaskCreationOptions.RunContinuationsAsynchronously);
        using (AudioStreamClient client = new(new SshAudioSource(ssh, UnoQ.Stream.Command!)))
        {
            client.Disconnected += reason => ended.TrySetResult(reason);
            client.Start();
            (await ended.Task.WaitAsync(TimeSpan.FromSeconds(10))).Should().Be(SshAudioSource.InUseReason);
            client.Status().State.Should().Be(AudioStreamState.Waiting);
        }
        SshAudioSource.InUseReason.Should().Contain("in use on the target");
    }

    [Fact]
    public async Task Stderr_lines_of_the_stream_command_are_raised()
    {
        string stream = "echo 'audio_stream: arecord overrun 1' >&2; exit 1";
        using SshConnection ssh = new(Destination, FakeSsh(stream: stream));
        SshAudioSource source = new(ssh, UnoQ.Stream.Command!);
        TaskCompletionSource<string> line = new(TaskCreationOptions.RunContinuationsAsynchronously);
        TaskCompletionSource<string> ended = new(TaskCreationOptions.RunContinuationsAsynchronously);
        source.ErrorLine += l => line.TrySetResult(l);
        using AudioStreamClient client = new(source);
        client.Disconnected += reason => ended.TrySetResult(reason);
        client.Start();
        (await line.Task.WaitAsync(TimeSpan.FromSeconds(10))).Should().Be("audio_stream: arecord overrun 1");
        (await ended.Task.WaitAsync(TimeSpan.FromSeconds(10)))
            .Should().Be("the stream command ended with exit code 1: audio_stream: arecord overrun 1");
    }

    [Fact]
    public async Task Authentication_failure_is_said_clearly()
    {
        string master = """echo "$dest: Permission denied (publickey,password)." >&2; exit 255""";
        using SshConnection ssh = new(Destination, FakeSsh(master: master));
        TaskCompletionSource<string> ended = new(TaskCreationOptions.RunContinuationsAsynchronously);
        using (AudioStreamClient client = new(new SshAudioSource(ssh, UnoQ.Stream.Command!)))
        {
            client.Disconnected += reason => ended.TrySetResult(reason);
            client.Start();
            string reason = await ended.Task.WaitAsync(TimeSpan.FromSeconds(10));
            reason.Should().StartWith("cannot connect: ssh arduino@unoq.local: arduino@unoq.local: Permission denied")
                .And.Contain("BatchMode=yes").And.Contain("key authentication");
        }
        Func<Task> shell = () => BoardShell.OpenSshAsync(ssh, UnoQ.Prompt);
        (await shell.Should().ThrowAsync<BoardException>()).Which.Message.Should().Contain("Permission denied");
        Runs().Should().NotContain(r => r.Command != SshConnection.MasterCommand, "nothing runs without the connection");
    }

    [Fact]
    public async Task Shell_runs_on_a_terminal_and_queries_on_channels_of_their_own()
    {
        using SshConnection ssh = new(Destination, FakeSsh());
        StringBuilder shown = new();
        int pid;
        using (BoardShell shell = await BoardShell.OpenSshAsync(ssh, UnoQ.Prompt))
        {
            shell.Output += text => { lock (shown) { shown.Append(text); } };
            await shell.SettleAsync();
            string output = await shell.ExecuteAsync("hello there", TimeSpan.FromSeconds(5));
            shell.Prompt.Finished(output).Should().BeTrue();
            shell.Prompt.OutputLines(output, "hello there").Should().Equal("read 11 characters");
            (await shell.QueryAsync("uname", TimeSpan.FromSeconds(5))).Should().Equal("Linux");
            lock (shown)
            {
                shown.ToString().Should().Contain("read 11 characters").And.NotContain("Linux\n");
            }
            pid = Runs().Single(r => r.Terminal).Pid;
        }
        (await GoneAsync(pid)).Should().BeTrue("closing the shell ends its ssh process");
        List<Run> runs = Runs();
        runs.Should().ContainSingle(r => r.Terminal && r.Command.Length == 0 && r.Socket);
        runs.Should().ContainSingle(r => !r.Terminal && r.Command == "uname" && r.Socket);
        runs.Should().ContainSingle(r => r.Command == SshConnection.MasterCommand);
        ssh.Dispose();
        await AllGoneAsync();
    }

    [Fact]
    public async Task A_shell_that_ends_says_why()
    {
        using SshConnection ssh = new(Destination, FakeSsh());
        using BoardShell shell = await BoardShell.OpenSshAsync(ssh, UnoQ.Prompt);
        TaskCompletionSource<string?> closed = new(TaskCreationOptions.RunContinuationsAsynchronously);
        shell.Closed += reason => closed.TrySetResult(reason);
        await shell.SettleAsync();
        await shell.ExecuteAsync("exit", TimeSpan.FromMilliseconds(500));
        (await closed.Task.WaitAsync(TimeSpan.FromSeconds(10))).Should().Be("the session ended");
    }

    [Fact]
    public void Destinations_that_ssh_would_take_as_options_are_refused()
    {
        SshConnection.CheckDestination("arduino@unoq.local").Should().BeNull();
        SshConnection.CheckDestination("-oProxyCommand=x").Should().NotBeNull();
        SshConnection.CheckDestination("a b").Should().NotBeNull();
        SshConnection.CheckDestination("").Should().NotBeNull();
        FluentActions.Invoking(() => new SshConnection("-x", "ssh")).Should().Throw<SshException>();
    }

    [Fact]
    public void The_ssh_program_can_be_given()
    {
        string? before = Environment.GetEnvironmentVariable(SshConnection.ExecutableVariable);
        try
        {
            Environment.SetEnvironmentVariable(SshConnection.ExecutableVariable, null);
            SshConnection.ResolveExecutable("").Should().Be("ssh");
            SshConnection.ResolveExecutable("/opt/ssh").Should().Be("/opt/ssh");
            Environment.SetEnvironmentVariable(SshConnection.ExecutableVariable, "/tmp/fake-ssh");
            SshConnection.ResolveExecutable("/opt/ssh").Should().Be("/tmp/fake-ssh");
        }
        finally
        {
            Environment.SetEnvironmentVariable(SshConnection.ExecutableVariable, before);
        }
    }

    [Fact]
    public async Task A_missing_ssh_program_is_said()
    {
        using SshConnection ssh = new(Destination, Path.Combine(_dir.FullName, "no-such-ssh"));
        Func<Task> acquire = () => ssh.AcquireAsync();
        (await acquire.Should().ThrowAsync<SshException>()).Which.Message.Should().StartWith("Cannot run ");
    }
}
