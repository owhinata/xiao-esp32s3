// SPDX-License-Identifier: MIT
using System.Globalization;
using System.IO.Compression;
using System.Text;
using System.Text.Json;
using AudioStudio.Core.Audio;
using AudioStudio.Core.Model;
using FluentAssertions;
using Xunit;
using Xunit.Abstractions;

namespace AudioStudio.Core.Tests;

/// <summary>
/// The M4 detector against the values of its Python code
/// (Fixtures/, written by tools/make_m4_fixtures.py), and the .npy / .npz reader.
/// </summary>
public sealed class M4Tests(ITestOutputHelper output)
{
    private const double MfeTolerance = 1e-4;
    private const double ProbabilityTolerance = 1e-5;

    private static string Fixture(string name) => Path.Combine(AppContext.BaseDirectory, "Fixtures", name);
    private static string ModelPath => Path.Combine(AppContext.BaseDirectory, "models", "m4-0038.npz");

    private static readonly Lazy<M4Model> s_model = new(() => M4Model.Load(ModelPath));

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

    private void Report(string what, double value) =>
        output.WriteLine(string.Create(CultureInfo.InvariantCulture, $"{what}: {value:E3}"));

    // ---- the model file --------------------------------------------------

    [Fact]
    public void Loads_the_weights_and_their_configuration()
    {
        M4Model model = s_model.Value;
        model.Config.Text.Should().Be(
            "set=mfe40 classifier=cnn seeds=0,1,2 hard_negative=none post=hyst threshold=- enter=0.85 exit=0.65 cap_s=none refractory_s=0.5");
        model.Seeds.Should().Equal(0, 1, 2);
        model.Config.Enter.Should().Be(0.85);
        model.Config.Exit.Should().Be(0.65);
        model.Config.CapSeconds.Should().BeNull();
        model.Config.RefractorySeconds.Should().Be(0.5);
        Path.GetFileName(model.MelPath).Should().Be(M4Model.MelFileName);
    }

    /// <summary>A copy of the weights file with some members left out or replaced.</summary>
    private static string Altered(string dir, Func<string, bool> keep, Dictionary<string, byte[]>? add = null)
    {
        string path = Path.Combine(dir, "altered.npz");
        File.Delete(path);
        using (ZipArchive source = ZipFile.OpenRead(ModelPath))
        using (ZipArchive target = ZipFile.Open(path, ZipArchiveMode.Create))
        {
            foreach (ZipArchiveEntry e in source.Entries.Where(e => keep(e.FullName)))
            {
                using System.IO.Stream from = e.Open(), to = target.CreateEntry(e.FullName).Open();
                from.CopyTo(to);
            }
            foreach ((string name, byte[] bytes) in add ?? [])
            {
                using System.IO.Stream to = target.CreateEntry(name).Open();
                to.Write(bytes);
            }
        }
        File.Copy(Path.Combine(AppContext.BaseDirectory, "models", M4Model.MelFileName), Path.Combine(dir, M4Model.MelFileName), true);
        return path;
    }

    private static byte[] NpyBytes(string descr, string shape, byte[] data, string fortran = "False")
    {
        string header = $"{{'descr': '{descr}', 'fortran_order': {fortran}, 'shape': {shape}, }}";
        int total = 10 + header.Length + 1;
        header += new string(' ', (64 - total % 64) % 64) + "\n";
        MemoryStream m = new();
        m.Write([0x93, (byte)'N', (byte)'U', (byte)'M', (byte)'P', (byte)'Y', 1, 0]);
        m.Write(BitConverter.GetBytes((ushort)header.Length));
        m.Write(Encoding.Latin1.GetBytes(header));
        m.Write(data);
        return m.ToArray();
    }

    [Fact]
    public void Refuses_weights_files_that_do_not_fit()
    {
        string dir = Directory.CreateTempSubdirectory("audiostudio-m4-").FullName;
        try
        {
            Action missing = () => M4Model.Load(Altered(dir, n => n != "s1/std.npy"));
            missing.Should().Throw<ModelContractException>().WithMessage("*missing: s1/std*");

            Action extra = () => M4Model.Load(Altered(dir, _ => true,
                new() { ["s3/mean.npy"] = NpyBytes("<f8", "(40,)", new byte[320]) }));
            extra.Should().Throw<ModelContractException>().WithMessage("*extra: s3/mean*");

            Action dtype = () => M4Model.Load(Altered(dir, n => n != "s0/mean.npy",
                new() { ["s0/mean.npy"] = NpyBytes("<f4", "(40,)", new byte[160]) }));
            dtype.Should().Throw<ModelContractException>().WithMessage("*s0/mean is of <f4*");

            Action shape = () => M4Model.Load(Altered(dir, n => n != "s2/fc.bias.npy",
                new() { ["s2/fc.bias.npy"] = NpyBytes("<f4", "(2,)", new byte[8]) }));
            shape.Should().Throw<ModelContractException>().WithMessage("*s2/fc.bias has the shape (2)*");

            Action format = () => M4Model.Load(Altered(dir, n => n != "format.npy",
                new() { ["format.npy"] = NpyBytes("<i8", "()", BitConverter.GetBytes(2L)) }));
            format.Should().Throw<ModelContractException>().WithMessage("*format*value 2*");

            Action noConfig = () => M4Model.Load(Altered(dir, n => n != "config.npy"));
            noConfig.Should().Throw<ModelContractException>().WithMessage("*config*");

            string path = Altered(dir, _ => true);
            File.Delete(Path.Combine(dir, M4Model.MelFileName));
            Action noMel = () => M4Model.Load(path);
            noMel.Should().Throw<ModelContractException>().WithMessage($"*{M4Model.MelFileName}*");
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public void The_reader_refuses_what_it_does_not_take()
    {
        Action read(byte[] bytes) => () => Npy.Read(new MemoryStream(bytes));
        read([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]).Should().Throw<NpyFormatException>().WithMessage("*not a .npy*");
        read(NpyBytes("|O", "(1,)", new byte[8])).Should().Throw<NpyFormatException>().WithMessage("*|O*");
        read(NpyBytes(">f4", "(1,)", new byte[4])).Should().Throw<NpyFormatException>().WithMessage("*>f4*");
        read(NpyBytes("<f4", "(2, 2)", new byte[16], "True")).Should().Throw<NpyFormatException>().WithMessage("*Fortran*");
        read(NpyBytes("<f8", "(4,)", new byte[16])).Should().Throw<NpyFormatException>().WithMessage("*ends early*");

        NpyArray a = Npy.Read(new MemoryStream(NpyBytes("<f4", "(2, 2)", [0, 0, 128, 63, 0, 0, 0, 64, 0, 0, 64, 64, 0, 0, 128, 64])));
        a.Shape.Should().Equal(2, 2);
        a.ToFloat32().Should().Equal(1f, 2f, 3f, 4f);
        Action wrongType = () => a.ToFloat64();
        wrongType.Should().Throw<NpyFormatException>();
    }

    [Fact]
    public void Reads_compressed_npz_files_of_numpy()
    {
        Dictionary<string, NpyArray> w = Npy.ReadNpz(Fixture("m4_windows.npz"));
        w["names"].ToStrings()[0].Should().Be("zeros");
        w["windows"].Descr.Should().Be("<i2");
        w["windows"].Shape[1].Should().Be(16000);
        Npy.ReadNpz(Fixture("m4_post.npz"))["c0/valid"].ToBool().Should().AllBeEquivalentTo(true);
    }

    // ---- the numbers against the Python code -------------------------------

    [Fact]
    public void The_reference_signal_gives_the_values_of_the_Python_code()
    {
        Dictionary<string, NpyArray> f = Npy.ReadNpz(Fixture("m4_reference.npz"));
        M4Model model = s_model.Value;
        float[] mfe = model.Mfe40(f["signal"].ToFloat64());
        double d = MaxDifference(mfe, f["mfe40"].ToFloat32());
        Report("reference mfe40, largest difference", d);
        d.Should().BeLessThanOrEqualTo(MfeTolerance);

        double[] seeds = f["seed_prob"].ToFloat64();
        for (int i = 0; i < seeds.Length; i++)
        {
            Math.Abs(model.SeedProbability(i, mfe) - seeds[i]).Should().BeLessThanOrEqualTo(ProbabilityTolerance);
        }
        double p = model.Probability(mfe);
        Report("reference probability, difference", Math.Abs(p - f["prob"].ToFloat64()[0]));
        p.Should().BeApproximately(f["prob"].ToFloat64()[0], ProbabilityTolerance);
    }

    [Fact]
    public void Synthetic_windows_give_the_values_of_the_Python_code()
    {
        Dictionary<string, NpyArray> f = Npy.ReadNpz(Fixture("m4_windows.npz"));
        M4Model model = s_model.Value;
        string[] names = f["names"].ToStrings();
        short[] windows = f["windows"].ToInt16();
        float[] mfeAll = f["mfe40"].ToFloat32();
        double[] prob = f["prob"].ToFloat64();
        double[] seedProb = f["seed_prob"].ToFloat64();
        int n = M4Model.Frames * M4Model.Mels, seeds = model.Seeds.Count;
        double worstMfe = 0, worstP = 0;
        for (int w = 0; w < names.Length; w++)
        {
            float[] mfe = model.Mfe40(windows.AsSpan(w * M4Model.WindowSamples, M4Model.WindowSamples));
            double d = MaxDifference(mfe, mfeAll[(w * n)..((w + 1) * n)]);
            double p = model.Probability(mfe);
            double dp = Math.Abs(p - prob[w]);
            output.WriteLine(string.Create(CultureInfo.InvariantCulture,
                $"{names[w],-32} p {prob[w]:G6}  mfe40 diff {d:E2}  p diff {dp:E2}"));
            d.Should().BeLessThanOrEqualTo(MfeTolerance, names[w]);
            dp.Should().BeLessThanOrEqualTo(ProbabilityTolerance, names[w]);
            for (int s = 0; s < seeds; s++)
            {
                Math.Abs(model.SeedProbability(s, mfe) - seedProb[w * seeds + s]).Should().BeLessThanOrEqualTo(ProbabilityTolerance, names[w]);
            }
            worstMfe = Math.Max(worstMfe, d);
            worstP = Math.Max(worstP, dp);
        }
        Report("synthetic windows mfe40, largest difference", worstMfe);
        Report("synthetic windows probability, largest difference", worstP);
    }

    [Fact]
    public void A_stream_gives_the_windows_of_the_Python_detector()
    {
        Dictionary<string, NpyArray> f = Npy.ReadNpz(Fixture("m4_stream.npz"));
        short[] signal = f["signal"].ToInt16();
        double[] prob = f["prob"].ToFloat64();
        long[] positive = f["positive"].ToInt64(), led = f["led"].ToInt64();
        bool[] invalid = f["invalid"].ToBool();
        long[] positiveInvalid = f["positive_invalid"].ToInt64(), ledInvalid = f["led_invalid"].ToInt64();

        M4Detector all = new(s_model.Value), gaps = new(s_model.Value);
        float[] window = new float[M4Model.WindowSamples];
        double worst = 0, stepTime = 0;
        int windows = 1 + (signal.Length - M4Model.WindowSamples) / M4Model.HopSamples;
        windows.Should().Be(prob.Length);
        positive.Should().Contain(1, "the stream has swallow-like bursts");
        for (int k = 0; k < windows; k++)
        {
            for (int i = 0; i < window.Length; i++)
            {
                window[i] = signal[k * M4Model.HopSamples + i] / 32768f;
            }
            long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
            M4Step s = all.Step(window);
            stepTime += System.Diagnostics.Stopwatch.GetElapsedTime(t0).TotalMilliseconds;
            s.K.Should().Be(k);
            worst = Math.Max(worst, Math.Abs(s.Probability - prob[k]));
            s.Positive.Should().Be(positive[k] == 1, $"window {k}");
            s.Led.Should().Be((int)led[k], $"window {k}");

            M4Step g = invalid[k] ? gaps.StepInvalid() : gaps.Step(window);
            g.Valid.Should().Be(!invalid[k]);
            g.Positive.Should().Be(positiveInvalid[k] == 1, $"window {k} with gaps");
            g.Led.Should().Be((int)ledInvalid[k], $"window {k} with gaps");
        }
        Report("stream probability, largest difference", worst);
        output.WriteLine(string.Create(CultureInfo.InvariantCulture, $"{stepTime / windows:F2} ms per window (one thread)"));
        worst.Should().BeLessThanOrEqualTo(ProbabilityTolerance);
    }

    [Fact]
    public void PostStepper_gives_the_outputs_of_the_Python_PostStepper()
    {
        Dictionary<string, NpyArray> f = Npy.ReadNpz(Fixture("m4_post.npz"));
        string[] names = f["names"].ToStrings();
        for (int c = 0; c < names.Length; c++)
        {
            double[] settings = f[$"c{c}/settings"].ToFloat64();
            double[] prob = f[$"c{c}/prob"].ToFloat64();
            bool[] valid = f[$"c{c}/valid"].ToBool(), expected = f[$"c{c}/positive"].ToBool();
            PostStepper post = new(settings[0], settings[1],
                                   double.IsNaN(settings[2]) ? null : settings[2],
                                   double.IsNaN(settings[3]) ? null : settings[3]);
            bool[] got = prob.Select((p, k) => post.Step(p, valid[k], k * PostStepper.HopSeconds)).ToArray();
            got.Should().Equal(expected, names[c]);
        }
    }

    [Fact]
    public void PostStepper_refuses_what_the_Python_refuses()
    {
        Action badExit = () => _ = new PostStepper(0.6, 0.7);
        badExit.Should().Throw<ArgumentException>();
        Action badCap = () => _ = new PostStepper(0.85, 0.65, 1.3);
        badCap.Should().Throw<ArgumentException>();
        Action badRefractory = () => _ = new PostStepper(0.85, 0.65, null, 1.0);
        badRefractory.Should().Throw<ArgumentException>();
        PostStepper post = new(0.85, 0.65);
        post.Step(0.9, true, 0);
        Action offGrid = () => post.Step(0.9, true, 0.5);
        offGrid.Should().Throw<ArgumentException>();
    }

    [Fact]
    public void LedStepper_gives_the_outputs_of_the_Python_LedStepper()
    {
        Dictionary<string, NpyArray> f = Npy.ReadNpz(Fixture("m4_post.npz"));
        for (int c = 0; f.ContainsKey($"led{c}/led"); c++)
        {
            long[] w = f[$"led{c}/window_t_ms"].ToInt64(), p = f[$"led{c}/positive"].ToInt64(), led = f[$"led{c}/led"].ToInt64();
            LedStepper stepper = new();
            w.Select((t, i) => (long)stepper.Step(t, p[i] != 0)).Should().Equal(led, $"case {c}");
        }
        LedStepper s = new();
        s.Step(250, false);
        Action back = () => s.Step(250, false);
        back.Should().Throw<ArgumentException>();
    }

    [Fact]
    public void Configurations_are_read_as_the_Python_reads_them()
    {
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(Fixture("m4_configs.json")));
        foreach (JsonElement row in doc.RootElement.EnumerateArray())
        {
            string text = row.GetProperty("text").GetString()!;
            if (!row.GetProperty("ok").GetBoolean())
            {
                Action parse = () => M4Config.Parse(text);
                parse.Should().Throw<FormatException>(text);
                continue;
            }
            M4Config c = M4Config.Parse(text);
            static double? Num(JsonElement e) => e.ValueKind == JsonValueKind.Null ? null : e.GetDouble();
            c.Set.Should().Be(row.GetProperty("set").GetString(), text);
            c.Kind.Should().Be(row.GetProperty("kind").GetString(), text);
            c.Seeds.Should().Equal(row.GetProperty("seeds").EnumerateArray().Select(e => e.GetInt32()), text);
            c.HardNegative.Should().Be(row.GetProperty("hard_negative").GetBoolean(), text);
            c.Post.Should().Be(row.GetProperty("post").GetString(), text);
            c.Threshold.Should().Be(Num(row.GetProperty("threshold")), text);
            c.Enter.Should().Be(Num(row.GetProperty("enter")), text);
            c.Exit.Should().Be(Num(row.GetProperty("exit")), text);
            c.CapSeconds.Should().Be(Num(row.GetProperty("cap_s")), text);
            c.RefractorySeconds.Should().Be(Num(row.GetProperty("refractory_s")), text);
        }
    }

    // ---- the experimental variants (a higher lower edge of the mel bands) ----

    private static readonly Dictionary<string, Lazy<M4Model>> s_variants = new()
    {
        ["mfe40f200"] = new(() => M4Model.Load(Path.Combine(AppContext.BaseDirectory, "models", "m4-f200.npz"))),
        ["mfe40f500"] = new(() => M4Model.Load(Path.Combine(AppContext.BaseDirectory, "models", "m4-f500.npz"))),
    };

    [Theory]
    [InlineData("mfe40f200", 200)]
    [InlineData("mfe40f500", 500)]
    public void A_variant_model_loads_with_the_mel_matrix_of_its_set(string set, int low)
    {
        M4Model model = s_variants[set].Value;
        model.Config.Set.Should().Be(set);
        model.Config.MelLowHz.Should().Be(low);
        model.Config.Text.Should().Be(s_model.Value.Config.Text.Replace("set=mfe40 ", $"set={set} "),
            "only the feature set differs from the recorded configuration");
        Path.GetFileName(model.MelPath).Should().Be($"mel40_htk_{low}_4000.npy");
        M4Model.MelFileNameOf("mfe40").Should().Be(M4Model.MelFileName);

        Dictionary<string, NpyArray> f = Npy.ReadNpz(Fixture($"m4_variant_{set}.npz"));
        f["config"].ToStrings()[0].Should().Be(model.Config.Text);
        double[] signal = Npy.ReadNpz(Fixture("m4_reference.npz"))["signal"].ToFloat64();
        float[] mfe = model.Mfe40(signal);
        MaxDifference(mfe, s_model.Value.Mfe40(signal)).Should().BeGreaterThan(0.1, "the bands are not those of mfe40");
    }

    [Theory]
    [InlineData("mfe40f200")]
    [InlineData("mfe40f500")]
    public void A_variant_gives_the_values_of_the_Python_code(string set)
    {
        M4Model model = s_variants[set].Value;
        Dictionary<string, NpyArray> f = Npy.ReadNpz(Fixture($"m4_variant_{set}.npz"));

        float[] mfe = model.Mfe40(Npy.ReadNpz(Fixture("m4_reference.npz"))["signal"].ToFloat64());
        double d = MaxDifference(mfe, f["ref_mfe"].ToFloat32());
        Report($"{set} reference features, largest difference", d);
        d.Should().BeLessThanOrEqualTo(MfeTolerance);
        double[] refSeeds = f["ref_seed_prob"].ToFloat64();
        for (int i = 0; i < refSeeds.Length; i++)
        {
            Math.Abs(model.SeedProbability(i, mfe) - refSeeds[i]).Should().BeLessThanOrEqualTo(ProbabilityTolerance);
        }
        model.Probability(mfe).Should().BeApproximately(f["ref_prob"].ToFloat64()[0], ProbabilityTolerance);

        Dictionary<string, NpyArray> w = Npy.ReadNpz(Fixture("m4_windows.npz"));
        string[] names = w["names"].ToStrings();
        short[] windows = w["windows"].ToInt16();
        float[] mfeAll = f["win_mfe"].ToFloat32();
        double[] prob = f["win_prob"].ToFloat64(), seedProb = f["win_seed_prob"].ToFloat64();
        int n = M4Model.Frames * M4Model.Mels, seeds = model.Seeds.Count;
        double worstMfe = 0, worstP = 0;
        for (int k = 0; k < names.Length; k++)
        {
            float[] m = model.Mfe40(windows.AsSpan(k * M4Model.WindowSamples, M4Model.WindowSamples));
            double dm = MaxDifference(m, mfeAll[(k * n)..((k + 1) * n)]);
            double dp = Math.Abs(model.Probability(m) - prob[k]);
            dm.Should().BeLessThanOrEqualTo(MfeTolerance, names[k]);
            dp.Should().BeLessThanOrEqualTo(ProbabilityTolerance, names[k]);
            for (int s = 0; s < seeds; s++)
            {
                Math.Abs(model.SeedProbability(s, m) - seedProb[k * seeds + s]).Should().BeLessThanOrEqualTo(ProbabilityTolerance, names[k]);
            }
            worstMfe = Math.Max(worstMfe, dm);
            worstP = Math.Max(worstP, dp);
        }
        Report($"{set} synthetic windows features, largest difference", worstMfe);
        Report($"{set} synthetic windows probability, largest difference", worstP);

        short[] stream = StreamSignal();
        double[] streamProb = f["stream_prob"].ToFloat64();
        long[] positive = f["stream_positive"].ToInt64(), led = f["stream_led"].ToInt64();
        M4Detector detector = new(model);
        float[] window = new float[M4Model.WindowSamples];
        double worst = 0;
        for (int k = 0; k < streamProb.Length; k++)
        {
            for (int i = 0; i < window.Length; i++)
            {
                window[i] = stream[k * M4Model.HopSamples + i] / 32768f;
            }
            M4Step s = detector.Step(window);
            worst = Math.Max(worst, Math.Abs(s.Probability - streamProb[k]));
            s.Positive.Should().Be(positive[k] == 1, $"window {k}");
            s.Led.Should().Be((int)led[k], $"window {k}");
        }
        Report($"{set} stream probability, largest difference", worst);
        worst.Should().BeLessThanOrEqualTo(ProbabilityTolerance);
    }

    [Fact]
    public void A_variant_without_its_mel_matrix_is_refused()
    {
        string dir = Directory.CreateTempSubdirectory("audiostudio-m4-").FullName;
        try
        {
            string path = Path.Combine(dir, "m4-f200.npz");
            File.Copy(s_variants["mfe40f200"].Value.Path, path);
            // the mfe40 matrix beside it is not the one the set needs
            File.Copy(Path.Combine(AppContext.BaseDirectory, "models", M4Model.MelFileName), Path.Combine(dir, M4Model.MelFileName));
            Action load = () => M4Model.Load(path);
            load.Should().Throw<ModelContractException>().WithMessage("*mel40_htk_200_4000.npy*");
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    // ---- the runner: windows, gaps and new streams -------------------------

    private static short[] StreamSignal() => Npy.ReadNpz(Fixture("m4_stream.npz"))["signal"].ToInt16();

    private static async Task Drain(M4Runner runner, ISampleSource source)
    {
        runner.Offer(source);
        await runner.WhenIdle();
        runner.Offer(source);
        await runner.WhenIdle();
    }

    [Fact]
    public async Task A_file_is_run_window_by_window_from_its_start()
    {
        Dictionary<string, NpyArray> f = Npy.ReadNpz(Fixture("m4_stream.npz"));
        short[] signal = f["signal"].ToInt16();
        long[] positive = f["positive"].ToInt64(), led = f["led"].ToInt64();
        M4Runner runner = new(s_model.Value);
        long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
        await runner.AnalyzeAsync(new FileSource("stream.wav", 16000, signal), null, CancellationToken.None);
        double seconds = System.Diagnostics.Stopwatch.GetElapsedTime(t0).TotalSeconds;
        List<M4Window> windows = runner.History.Range(long.MinValue / 2, long.MaxValue / 2);
        windows.Should().HaveCount(positive.Length);
        for (int k = 0; k < windows.Count; k++)
        {
            windows[k].Start.Should().Be(4000L * k);
            windows[k].Step.K.Should().Be(k);
            windows[k].Step.Positive.Should().Be(positive[k] == 1);
            windows[k].Step.Led.Should().Be((int)led[k]);
        }
        int runs = positive.Select((p, k) => p == 1 && (k == 0 || positive[k - 1] == 0)).Count(r => r);
        runner.History.Indications.Should().Be(runs);
        output.WriteLine(string.Create(CultureInfo.InvariantCulture,
            $"file of {windows.Count} windows in {seconds:F2} s ({runner.MeanMilliseconds:F2} ms per window, run in parallel)"));
    }

    [Fact]
    public async Task Live_windows_that_overlap_lost_samples_are_not_valid()
    {
        short[] signal = StreamSignal();
        LiveSource live = new(16000);
        M4Runner runner = new(s_model.Value);
        // 6 s, a gap of 1000 samples, then the rest; in blocks of 256 as the board sends them.
        int cut = 6 * 16000;
        for (int i = 0; i < cut; i += 256)
        {
            live.Append(signal.AsSpan(i, Math.Min(256, cut - i)));
            if (i == 0)
            {
                await Drain(runner, live);
            }
        }
        live.AppendGap(1000);
        for (int i = cut; i < signal.Length - 1000; i += 4096)
        {
            live.Append(signal.AsSpan(i, Math.Min(4096, signal.Length - 1000 - i)));
            runner.Offer(live);
        }
        await Drain(runner, live);

        List<M4Window> windows = runner.History.Range(long.MinValue / 2, long.MaxValue / 2);
        windows.Should().HaveCount(1 + (signal.Length - 16000) / 4000, "every window is run, none is dropped");
        windows.Select(w => w.Step.K).Should().Equal(Enumerable.Range(0, windows.Count).Select(k => (long)k));
        foreach (M4Window w in windows)
        {
            bool overlaps = w.Start < cut + 1000 && w.End > cut;
            w.Step.Valid.Should().Be(!overlaps, $"window {w.Step.K}");
            double.IsNaN(w.Step.Probability).Should().Be(overlaps);
        }
        windows.Count(w => !w.Step.Valid).Should().Be(4, "a gap of 1000 samples touches the four windows that start 0 to 3 hops before it ends");

        // The same as the detector stepped by hand with these windows invalid.
        M4Detector detector = new(s_model.Value);
        float[] buffer = new float[16000];
        foreach (M4Window w in windows)
        {
            live.Read(w.Start, buffer);
            M4Step expected = w.Step.Valid ? detector.Step(buffer) : detector.StepInvalid();
            w.Step.Should().Be(expected);
        }
        runner.Lagged.Should().Be(0);
        runner.Backlog.Should().Be(0);
    }

    [Fact]
    public async Task A_new_stream_starts_the_detector_afresh()
    {
        short[] signal = StreamSignal();
        LiveSource live = new(16000);
        M4Runner runner = new(s_model.Value);
        live.Append(signal.AsSpan(0, 50_000));
        await Drain(runner, live);
        live.MarkNewStream();
        live.Append(signal.AsSpan(50_000, 60_000));
        await Drain(runner, live);

        List<M4Window> windows = runner.History.Range(long.MinValue / 2, long.MaxValue / 2);
        List<M4Window> before = windows.Where(w => w.Start < 50_000).ToList();
        List<M4Window> after = windows.Where(w => w.Start >= 50_000).ToList();
        // The runner starts at the newest complete window; the next one would reach past the new stream.
        before.Select(w => w.Start).Should().Equal(32_000);
        after.Select(w => w.Start).Should().Equal(Enumerable.Range(0, 12).Select(k => 50_000L + 4000L * k));
        after.Select(w => w.Step.K).Should().Equal(Enumerable.Range(0, 12).Select(k => (long)k));
    }

    [Fact]
    public async Task The_live_runner_starts_at_the_newest_complete_window()
    {
        short[] signal = StreamSignal();
        LiveSource live = new(16000);
        live.Append(signal.AsSpan(0, 5 * 16000 + 1000));
        M4Runner runner = new(s_model.Value);
        await Drain(runner, live);
        M4Window first = runner.History.Range(long.MinValue / 2, long.MaxValue / 2)[0];
        first.Step.K.Should().Be(16);
        first.Start.Should().Be(64_000);
        runner.History.Count.Should().Be(1);
    }
}
