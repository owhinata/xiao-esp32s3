// SPDX-License-Identifier: MIT
using System.Globalization;
using AudioStudio.Core.Audio;
using AudioStudio.Core.Model;
using FluentAssertions;
using Xunit;
using Xunit.Abstractions;

namespace AudioStudio.Core.Tests;

/// <summary>
/// The spike filter (MainsSpike) and the detector behind it against the
/// values of model/analysis/mains_spike.py and the Python detector
/// (Fixtures/m4_mains_spike.npz, written by tools/make_m4_fixtures.py from a
/// synthetic signal).
/// </summary>
public sealed class MainsSpikeTests(ITestOutputHelper output)
{
    /// <summary>
    /// Output samples: both sides compute in double precision and round to
    /// float at the end, so they differ only where the FIR sums round
    /// differently (an ulp of float at most, about 1e-9 at these levels).
    /// </summary>
    private const double SampleTolerance = 1e-6;
    /// <summary>The probabilities: as the detector's own tests (M4Tests).</summary>
    private const double ProbabilityTolerance = 1e-5;

    private static string Fixture(string name) => Path.Combine(AppContext.BaseDirectory, "Fixtures", name);
    private static readonly Lazy<Dictionary<string, NpyArray>> s_fixture = new(() => Npy.ReadNpz(Fixture("m4_mains_spike.npz")));
    private static readonly Lazy<M4Model> s_model = new(() => M4Model.Load(Path.Combine(AppContext.BaseDirectory, "models", "m4-0038.npz")));

    private void Report(string what, double value) =>
        output.WriteLine(string.Create(CultureInfo.InvariantCulture, $"{what}: {value:E3}"));

    /// <summary>The filter over the signal and Delay zeros after it, as the fixture's output.</summary>
    private static float[] Filtered(short[] signal, double mainsHz)
    {
        MainsSpike filter = new(mainsHz);
        float[] y = new float[signal.Length + MainsSpike.Delay];
        for (int i = 0; i < y.Length; i++)
        {
            y[i] = (float)filter.Step(i < signal.Length ? signal[i] / 32768f : 0f);
        }
        return y;
    }

    private static double MaxDifference(float[] a, float[] b)
    {
        a.Length.Should().Be(b.Length);
        double max = 0;
        for (int i = 0; i < a.Length; i++)
        {
            max = Math.Max(max, Math.Abs((double)a[i] - b[i]));
        }
        return max;
    }

    [Fact]
    public void The_constants_are_those_of_the_Python_code()
    {
        double[] c = s_fixture.Value["constants"].ToFloat64();
        new double[]
        {
            MainsSpike.LowPassHz, MainsSpike.Taps, MainsSpike.EnvelopeTau, MainsSpike.HumQ, MainsSpike.Refractory,
            MainsSpike.PeriodTolerance, MainsSpike.PeriodRate, MainsSpike.Alpha, MainsSpike.QuantStep, MainsSpike.Margin,
            MainsSpike.Limit, MainsSpike.FloorGain2, MainsSpike.FInit, MainsSpike.Warmup, MainsSpike.Spread,
        }.Should().Equal(c);
        MainsSpike.Delay.Should().Be(80);
        MainsSpike.Bins(50).Should().Be(331);
        double[] taps = MainsSpike.LowPassTaps();
        taps.Sum().Should().BeApproximately(1.0, 1e-12);
        for (int k = 0; k < taps.Length; k++)
        {
            taps[k].Should().BeApproximately(taps[^(k + 1)], 1e-15, "the low-pass is symmetric (linear phase)");
        }
    }

    [Theory]
    [InlineData("signal", "output", 50.0)]
    [InlineData("signal60", "output60", 60.0)]
    public void The_filter_gives_the_samples_of_the_Python_front_end(string signal, string expected, double mainsHz)
    {
        short[] x = s_fixture.Value[signal].ToInt16();
        float[] want = s_fixture.Value[expected].ToFloat32();
        double d = MaxDifference(Filtered(x, mainsHz), want);
        Report($"{signal} at {mainsHz} Hz, largest sample difference", d);
        d.Should().BeLessThanOrEqualTo(SampleTolerance);
    }

    [Fact]
    public void The_filter_acts_only_where_the_bursts_are()
    {
        short[] x = s_fixture.Value["signal"].ToInt16();
        float[] y = Filtered(x, 50);
        // Changed by the filter: the output against the input delayed by Delay, per second.
        double Change(int second)
        {
            double sum = 0;
            for (int i = second * 16000; i < (second + 1) * 16000; i++)
            {
                double d = y[i + MainsSpike.Delay] - x[i] / 32768.0;
                sum += d * d;
            }
            return sum;
        }
        Change(6).Should().BeGreaterThan(100 * Change(22), "the bursts run from 3 to 14 s; nothing is limited at 22 s");
    }

    [Fact]
    public void Without_a_high_band_the_filtered_window_is_the_source_window()
    {
        short[] x = new short[3 * 16000];
        for (int i = 0; i < x.Length; i++)
        {
            x[i] = (short)Math.Round(3000 * Math.Sin(2 * Math.PI * 50 * i / 16000.0));
        }
        FileSource source = new("hum.wav", 16000, x);
        SpikeFilteredInput input = new(50);
        input.Restart(0);
        input.Advance(source, 2 * 16000 + MainsSpike.Delay);
        float[] filtered = new float[16000], raw = new float[16000];
        input.Read(16000, filtered);
        source.Read(16000, raw);
        MaxDifference(filtered, raw).Should().BeLessThan(1e-5, "the output is the input delayed by Delay, and Read takes the delay off");
    }

    [Fact]
    public async Task A_file_with_the_filter_gives_the_windows_of_the_Python_detector()
    {
        Dictionary<string, NpyArray> f = s_fixture.Value;
        short[] signal = f["signal"].ToInt16();
        double[] prob = f["prob"].ToFloat64(), rawProb = f["raw_prob"].ToFloat64();
        long[] positive = f["positive"].ToInt64(), led = f["led"].ToInt64();

        M4Runner on = new(s_model.Value, 50), off = new(s_model.Value);
        on.SpikeFilterMainsHz.Should().Be(50);
        off.SpikeFilterMainsHz.Should().BeNull();
        await on.AnalyzeAsync(new FileSource("spikes.wav", 16000, signal), null, CancellationToken.None);
        await off.AnalyzeAsync(new FileSource("spikes.wav", 16000, signal), null, CancellationToken.None);
        List<M4Window> withFilter = on.History.Range(long.MinValue / 2, long.MaxValue / 2);
        List<M4Window> without = off.History.Range(long.MinValue / 2, long.MaxValue / 2);
        withFilter.Should().HaveCount(prob.Length);
        withFilter.Select(w => w.Start).Should().Equal(without.Select(w => w.Start), "the windows keep the source's positions");
        double worst = 0, worstRaw = 0;
        for (int k = 0; k < prob.Length; k++)
        {
            withFilter[k].Start.Should().Be(4000L * k);
            worst = Math.Max(worst, Math.Abs(withFilter[k].Step.Probability - prob[k]));
            worstRaw = Math.Max(worstRaw, Math.Abs(without[k].Step.Probability - rawProb[k]));
            withFilter[k].Step.Positive.Should().Be(positive[k] == 1, $"window {k}");
            withFilter[k].Step.Led.Should().Be((int)led[k], $"window {k}");
        }
        Report("probability with the filter, largest difference", worst);
        Report("probability without the filter, largest difference", worstRaw);
        worst.Should().BeLessThanOrEqualTo(ProbabilityTolerance);
        worstRaw.Should().BeLessThanOrEqualTo(ProbabilityTolerance);
        positive.Should().Contain(1, "the swallow-like bursts pass the filter");
        rawProb.Count(p => p >= 0.85).Should().BeGreaterThan(prob.Count(p => p >= 0.85) + 20, "the filter removes what the bursts raise");
        on.History.Indications.Should().BeLessThan(off.History.Indications);
    }

    [Fact]
    public async Task The_live_stream_with_the_filter_gives_the_file_s_windows_and_keeps_its_samples()
    {
        short[] signal = s_fixture.Value["signal"].ToInt16();
        M4Runner file = new(s_model.Value, 50);
        await file.AnalyzeAsync(new FileSource("spikes.wav", 16000, signal), null, CancellationToken.None);
        List<M4Window> expected = file.History.Range(long.MinValue / 2, long.MaxValue / 2);

        LiveSource live = new(16000);
        M4Runner runner = new(s_model.Value, 50);
        // One window and the filter's delay: the runner starts at window 0, and the filter at sample 0.
        int first = 16000 + MainsSpike.Delay;
        live.Append(signal.AsSpan(0, first));
        runner.Offer(live);
        await runner.WhenIdle();
        for (int i = first; i < signal.Length; i += 256)
        {
            live.Append(signal.AsSpan(i, Math.Min(256, signal.Length - i)));
            runner.Offer(live);
        }
        await runner.WhenIdle();
        runner.Offer(live);
        await runner.WhenIdle();

        List<M4Window> windows = runner.History.Range(long.MinValue / 2, long.MaxValue / 2);
        // The last file window reads zeros past the end; live it waits for samples that do not come.
        windows.Should().HaveCount(expected.Count - 1);
        for (int k = 0; k < windows.Count; k++)
        {
            windows[k].Start.Should().Be(expected[k].Start);
            windows[k].Step.Should().Be(expected[k].Step, $"window {k}");
        }
        runner.Lagged.Should().Be(0);

        float[] samples = new float[signal.Length];
        live.Read(0, samples);
        samples.Should().Equal(signal.Select(s => s / 32768f), "the filter only reads the stream: what is shown and recorded is unchanged");
    }

    [Fact]
    public async Task On_the_live_stream_the_filter_starts_before_the_first_window()
    {
        short[] signal = s_fixture.Value["signal"].ToInt16();
        LiveSource live = new(16000);
        live.Append(signal.AsSpan(0, 10 * 16000));
        M4Runner runner = new(s_model.Value, 50);
        runner.Offer(live);
        await runner.WhenIdle();
        M4Window w = runner.History.Range(long.MinValue / 2, long.MaxValue / 2).Single();
        // The newest window whose end and the filter's delay are in: (160000 - 16000 - 80) / 4000 = 35.
        w.Step.K.Should().Be(35);
        w.Start.Should().Be(140_000);
        w.Step.Valid.Should().BeTrue();
    }

    [Fact]
    public void The_mains_frequency_must_be_positive()
    {
        Action zero = () => _ = new MainsSpike(0);
        zero.Should().Throw<ArgumentOutOfRangeException>();
        new MainsSpike(60).MainsHz.Should().Be(60);
        MainsSpike.Bins(60).Should().Be(276);
    }
}
