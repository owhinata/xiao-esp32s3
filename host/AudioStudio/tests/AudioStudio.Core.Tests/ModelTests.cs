// SPDX-License-Identifier: MIT
using System.Diagnostics;
using AudioStudio.Core.Audio;
using AudioStudio.Core.Model;
using FluentAssertions;
using Xunit;

namespace AudioStudio.Core.Tests;

/// <summary>The contract, with the model tools/make_test_model.py writes (energy and slope: silence, low, high).</summary>
public sealed class ModelTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("audiostudio-model-").FullName;

    public void Dispose() => Directory.Delete(_dir, recursive: true);

    private string MakeModel(string name, int samples)
    {
        string path = Path.Combine(_dir, name);
        ProcessStartInfo info = new("python3")
        {
            ArgumentList = { "-I", Path.Combine(AppContext.BaseDirectory, "make_test_model.py"), path,
                             "--samples", samples.ToString(System.Globalization.CultureInfo.InvariantCulture) },
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        using Process process = Process.Start(info)!;
        string error = process.StandardError.ReadToEnd();
        process.WaitForExit();
        process.ExitCode.Should().Be(0, error);
        return path;
    }

    private static float[] Tone(double hz, double amplitude, int n) =>
        Enumerable.Range(0, n).Select(i => (float)(amplitude * Math.Sin(2 * Math.PI * hz * i / 16000))).ToArray();

    [Fact]
    public void Reads_the_shape_and_the_labels_and_classifies()
    {
        using AudioModel model = new(MakeModel("fixed.onnx", 16000));
        model.WindowSamples.Should().Be(16000);
        model.DynamicWindow.Should().BeFalse();
        model.Classes.Should().Be(3);
        model.Labels.Should().Equal("silence", "low", "high");
        model.LabelSource.Should().Contain("metadata");

        float[] silence = model.Run(new float[16000]);
        silence.Sum().Should().BeApproximately(1f, 1e-4f, "raw scores go through a softmax");
        Array.IndexOf(silence, silence.Max()).Should().Be(0);
        float[] low = model.Run(Tone(200, 0.3, 16000));
        Array.IndexOf(low, low.Max()).Should().Be(1);
        float[] high = model.Run(Tone(4000, 0.3, 16000));
        Array.IndexOf(high, high.Max()).Should().Be(2);

        Action wrongLength = () => model.Run(new float[100]);
        wrongLength.Should().Throw<ArgumentException>();
    }

    [Fact]
    public void Takes_the_window_from_the_setting_when_the_model_leaves_it_open()
    {
        using AudioModel model = new(MakeModel("dynamic.onnx", 0), dynamicSamples: 4000);
        model.DynamicWindow.Should().BeTrue();
        model.WindowSamples.Should().Be(4000);
        float[] high = model.Run(Tone(5000, 0.5, 4000));
        Array.IndexOf(high, high.Max()).Should().Be(2);
    }

    [Fact]
    public void A_labels_file_beside_the_model_comes_first_and_is_fitted_to_the_classes()
    {
        string path = MakeModel("named.onnx", 1600);
        File.WriteAllLines(AudioModel.LabelsPath(path), ["quiet", "", "hiss", "extra"]);
        AudioModel.LabelsPath(path).Should().EndWith("named.labels.txt");
        using AudioModel model = new(path);
        model.Labels.Should().Equal("quiet", "hiss", "extra");
        model.LabelSource.Should().Be("named.labels.txt");
        File.WriteAllLines(AudioModel.LabelsPath(path), ["only"]);
        using AudioModel fewer = new(path);
        fewer.Labels.Should().Equal("only", "class 1", "class 2");
    }

    [Fact]
    public void Refuses_a_file_that_is_not_a_model()
    {
        string path = Path.Combine(_dir, "bad.onnx");
        File.WriteAllText(path, "not a model");
        Action load = () => _ = new AudioModel(path);
        load.Should().Throw<ModelContractException>().WithMessage("*bad.onnx*");
    }

    [Fact]
    public void Probabilities_are_kept_and_other_scores_go_through_a_softmax()
    {
        float[] p = [0.25f, 0.75f];
        AudioModel.Probabilities(p).Should().BeSameAs(p);
        float[] s = AudioModel.Probabilities([1f, 1f, 1f, 1f]);
        s.Should().OnlyContain(v => Math.Abs(v - 0.25f) < 1e-6f);
        float[] logits = AudioModel.Probabilities([0f, MathF.Log(3f)]);
        logits[1].Should().BeApproximately(0.75f, 1e-6f);
    }

    [Fact]
    public async Task Runs_every_window_of_a_file()
    {
        using AudioModel model = new(MakeModel("file.onnx", 16000));
        // One second of silence, then two of a low tone.
        short[] samples = new short[48000];
        for (int i = 16000; i < samples.Length; i++)
        {
            samples[i] = (short)(10000 * Math.Sin(2 * Math.PI * 200 * i / 16000.0));
        }
        Classifier classifier = new(model, 8000);
        await classifier.AnalyzeAsync(new FileSource("f", 16000, samples), null, CancellationToken.None);
        classifier.Error.Should().BeNull();
        classifier.History.Count.Should().Be(5); // windows ending at 2, 3, 4, 5, 6 × 8000
        classifier.History.At(16000)!.Top.Should().Be(0);
        classifier.History.At(47999)!.End.Should().Be(40000);
        classifier.History.At(48000)!.Top.Should().Be(1);
        classifier.History.Range(30000, 48000).Select(r => r.End).Should().Equal(24000, 32000, 40000, 48000);
    }

    [Fact]
    public async Task On_the_live_stream_takes_the_newest_window_and_counts_the_dropped_ones()
    {
        using AudioModel model = new(MakeModel("live.onnx", 1600));
        Classifier classifier = new(model, 800);
        LiveSource live = new(16000);
        live.Append(new short[1000]);
        classifier.Offer(live);
        classifier.Runs.Should().Be(0, "a whole window is not there yet");
        live.Append(new short[600]);
        classifier.Offer(live);
        await WaitIdle(classifier);
        classifier.Runs.Should().Be(1);
        classifier.History.Latest!.End.Should().Be(1600);
        // Five windows have come meanwhile: the newest is run, four are dropped.
        live.Append(new short[4000]);
        classifier.Offer(live);
        await WaitIdle(classifier);
        classifier.Runs.Should().Be(2);
        classifier.Dropped.Should().Be(4);
        classifier.History.Latest!.End.Should().Be(5600);
    }

    private static async Task WaitIdle(Classifier classifier)
    {
        for (int i = 0; i < 500 && classifier.Busy; i++)
        {
            await Task.Delay(10);
        }
        classifier.Busy.Should().BeFalse();
    }
}
