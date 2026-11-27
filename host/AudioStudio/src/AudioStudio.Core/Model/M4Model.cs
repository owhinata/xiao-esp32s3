// SPDX-License-Identifier: MIT
using System.Globalization;

namespace AudioStudio.Core.Model;

/// <summary>
/// The M4 swallow detector of zaitaku-mealpace (<c>analysis/m4_infer.py</c>,
/// <c>analysis/features_np.py</c>): a weights file (<c>.npz</c>) of one to
/// three seeds of a small CNN over 40 log-mel bands, and the mel matrix of
/// its configuration's feature set beside it (<see cref="MelFileNameOf"/>). The arithmetic follows the Python
/// code: float64 up to the log-mel values, which are rounded to float32;
/// normalised in float64 and rounded to float32; the CNN in float32 (sums
/// accumulated in float64 and rounded once); the sigmoid in float64; the
/// seeds' probabilities averaged in float64. Thread-safe: every call has its
/// own buffers.
/// </summary>
public sealed class M4Model
{
    public const int SampleRate = 16000;
    /// <summary>One window: 1.0 s.</summary>
    public const int WindowSamples = 16000;
    /// <summary>Window start to window start: 0.25 s.</summary>
    public const int HopSamples = 4000;
    public const int WindowMs = 1000;
    public const int HopMs = 250;
    public const int FrameLength = 400;
    public const int FrameHop = 160;
    public const int Frames = 1 + (WindowSamples - FrameLength) / FrameHop;
    public const int FftSize = 512;
    public const int Bins = FftSize / 2 + 1;
    public const int Mels = 40;
    public const double Preemphasis = 0.97;
    public const double LogFloor = 1e-10;
    public const int Format = 1;
    /// <summary>The mel matrix of the set mfe40 (40 x 257, float64, 0 to 4000 Hz), looked up in the directory of the weights file.</summary>
    public const string MelFileName = "mel40_htk_0_4000.npy";

    /// <summary>The file name of a CNN set's mel matrix: <c>mel40_htk_&lt;low&gt;_4000.npy</c> (features_np.mel40_path).</summary>
    public static string MelFileNameOf(string set) =>
        string.Create(CultureInfo.InvariantCulture, $"mel40_htk_{M4Config.CnnSets[set]}_4000.npy");

    private static readonly (string Name, int[] Shape)[] s_cnnShapes =
    [
        ("conv1.weight", [8, 1, 3, 3]), ("conv1.bias", [8]),
        ("conv2.weight", [16, 8, 3, 3]), ("conv2.bias", [16]),
        ("conv3.weight", [32, 16, 3, 3]), ("conv3.bias", [32]),
        ("fc.weight", [1, 32]), ("fc.bias", [1]),
    ];
    private static readonly double[] s_hamming = Hamming(FrameLength);

    private readonly double[] _mel;
    private readonly Seed[] _seeds;

    private sealed record Seed(int Id, float[][] Conv, float[][] Bias, float[] Fc, float FcBias, double[] Mean, double[] Std);

    public string Path { get; }
    public string MelPath { get; }
    public M4Config Config { get; }

    private M4Model(string path, string melPath, M4Config config, double[] mel, Seed[] seeds)
    {
        Path = path;
        MelPath = melPath;
        Config = config;
        _mel = mel;
        _seeds = seeds;
    }

    /// <summary>
    /// Reads the weights file and the mel matrix beside it, with the checks
    /// of <c>_check_arrays</c>: a configuration of a CNN (mfe40 or one of its
    /// variants, <see cref="M4Config.CnnSets"/>), exactly
    /// the keys it needs, each with its shape and dtype, and format 1.
    /// </summary>
    public static M4Model Load(string path)
    {
        string name = System.IO.Path.GetFileName(path);
        Dictionary<string, NpyArray> a;
        try
        {
            a = Npy.ReadNpz(path);
        }
        catch (NpyFormatException e)
        {
            throw new ModelContractException(e.Message);
        }
        if (!a.TryGetValue("config", out NpyArray? configArray) || configArray.Kind != 'U' || configArray.Count != 1)
        {
            throw new ModelContractException($"{name} has no configuration string (\"config\").");
        }
        M4Config config;
        try
        {
            config = M4Config.Parse(configArray.ToStrings()[0]);
        }
        catch (FormatException e)
        {
            throw new ModelContractException($"{name}: {e.Message}");
        }
        if (config.Kind != "cnn" || !M4Config.CnnSets.ContainsKey(config.Set))
        {
            throw new ModelContractException(
                $"{name}: only the configurations of the CNN ({string.Join(", ", M4Config.CnnSets.Keys)}) are supported, not {config.Set} / {config.Kind}.");
        }

        Dictionary<string, (int[] Shape, string Dtype)> want = new()
        {
            ["config"] = ([], "U"),
            ["format"] = ([], "i"),
        };
        foreach (int s in config.Seeds)
        {
            foreach ((string key, int[] shape) in s_cnnShapes)
            {
                want[$"s{s}/{key}"] = (shape, "<f4");
            }
            want[$"s{s}/mean"] = ([Mels], "<f8");
            want[$"s{s}/std"] = ([Mels], "<f8");
        }
        string[] missing = want.Keys.Except(a.Keys).Order(StringComparer.Ordinal).ToArray();
        string[] extra = a.Keys.Except(want.Keys).Order(StringComparer.Ordinal).ToArray();
        if (missing.Length > 0 || extra.Length > 0)
        {
            throw new ModelContractException(
                $"{name}: the keys differ (missing: {string.Join(", ", missing)}; extra: {string.Join(", ", extra)}).");
        }
        foreach ((string key, (int[] shape, string dtype)) in want)
        {
            NpyArray v = a[key];
            if (!v.HasShape(shape))
            {
                throw new ModelContractException(
                    $"{name}: {key} has the shape ({string.Join(", ", v.Shape)}); it needs ({string.Join(", ", shape)}).");
            }
            bool ok = dtype switch
            {
                "U" => v.Kind == 'U',
                "i" => v.Kind == 'i' && v.ToInt64()[0] == Format,
                _ => v.Descr == dtype,
            };
            if (!ok)
            {
                throw new ModelContractException($"{name}: {key} is of {v.Descr}" +
                    (dtype == "i" && v.Kind == 'i' ? $" with the value {v.ToInt64()[0]}; it needs format {Format}." : $"; it needs {dtype}."));
            }
        }

        string melName = MelFileNameOf(config.Set);
        string melPath = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(path))!, melName);
        double[] mel;
        try
        {
            NpyArray m = Npy.ReadFile(melPath);
            if (!m.HasShape(Mels, Bins) || m.Descr != "<f8")
            {
                throw new ModelContractException(
                    $"{melName} is of {m.Descr} ({string.Join(", ", m.Shape)}); it needs <f8 ({Mels}, {Bins}).");
            }
            mel = m.ToFloat64();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            throw new ModelContractException($"The mel matrix {melName} beside {name} cannot be read: {e.Message}");
        }
        catch (NpyFormatException e)
        {
            throw new ModelContractException(e.Message);
        }

        Seed[] seeds = config.Seeds.Select(s => new Seed(
            s,
            [a[$"s{s}/conv1.weight"].ToFloat32(), a[$"s{s}/conv2.weight"].ToFloat32(), a[$"s{s}/conv3.weight"].ToFloat32()],
            [a[$"s{s}/conv1.bias"].ToFloat32(), a[$"s{s}/conv2.bias"].ToFloat32(), a[$"s{s}/conv3.bias"].ToFloat32()],
            a[$"s{s}/fc.weight"].ToFloat32(),
            a[$"s{s}/fc.bias"].ToFloat32()[0],
            a[$"s{s}/mean"].ToFloat64(),
            a[$"s{s}/std"].ToFloat64())).ToArray();
        return new M4Model(path, melPath, config, mel, seeds);
    }

    /// <summary>The seeds of the ensemble, in the configuration's order.</summary>
    public IReadOnlyList<int> Seeds => Config.Seeds;

    // ---- features (features_np.mfe40, features_np.mfe for the variants) ----

    /// <summary>numpy's symmetric Hamming window: 0.54 + 0.46 cos(pi n / (M - 1)), n = 1 - M, 3 - M, ..., M - 1.</summary>
    private static double[] Hamming(int m)
    {
        double[] w = new double[m];
        for (int i = 0; i < m; i++)
        {
            double n = 1 - m + 2 * i;
            w[i] = 0.54 + 0.46 * Math.Cos(Math.PI * n / (m - 1));
        }
        return w;
    }

    /// <summary>The 98 x 40 log-mel values of one window of 16-bit samples (each divided by 32768).</summary>
    public float[] Mfe40(ReadOnlySpan<short> samples)
    {
        double[] x = new double[Math.Min(samples.Length, WindowSamples)];
        for (int i = 0; i < x.Length; i++)
        {
            x[i] = samples[i] / 32768.0;
        }
        return Mfe40(x);
    }

    /// <summary>The 98 x 40 log-mel values of one window of float32 samples (widened to float64, as numpy does).</summary>
    public float[] Mfe40(ReadOnlySpan<float> samples)
    {
        double[] x = new double[Math.Min(samples.Length, WindowSamples)];
        for (int i = 0; i < x.Length; i++)
        {
            x[i] = samples[i];
        }
        return Mfe40(x);
    }

    /// <summary>
    /// The 98 x 40 log-mel values (frame-major) of one window: pre-emphasis
    /// 0.97 over the whole window, frames of 400 every 160 samples, the
    /// symmetric Hamming window, |rfft(512)|^2 / 512, the mel matrix,
    /// log(x + 1e-10), rounded to float32. All zeros for fewer than 16000
    /// samples, as features_np.mfe40. The mel matrix is the one of the
    /// configuration's set (features_np.mfe), so a variant's bands start at
    /// its lower edge.
    /// </summary>
    public float[] Mfe40(ReadOnlySpan<double> samples)
    {
        float[] result = new float[Frames * Mels];
        if (samples.Length < WindowSamples)
        {
            return result;
        }
        double[] y = new double[WindowSamples];
        y[0] = samples[0];
        for (int i = 1; i < WindowSamples; i++)
        {
            y[i] = samples[i] - Preemphasis * samples[i - 1];
        }
        double[] re = new double[FftSize], im = new double[FftSize], power = new double[Bins];
        for (int f = 0; f < Frames; f++)
        {
            Array.Clear(re);
            Array.Clear(im);
            int start = f * FrameHop;
            for (int i = 0; i < FrameLength; i++)
            {
                re[i] = y[start + i] * s_hamming[i];
            }
            Fft512.Transform(re, im);
            for (int k = 0; k < Bins; k++)
            {
                power[k] = (re[k] * re[k] + im[k] * im[k]) / FftSize;
            }
            for (int m = 0; m < Mels; m++)
            {
                double sum = 0;
                int row = m * Bins;
                for (int k = 0; k < Bins; k++)
                {
                    sum += power[k] * _mel[row + k];
                }
                result[f * Mels + m] = (float)Math.Log(sum + LogFloor);
            }
        }
        return result;
    }

    // ---- the CNN (features_np.cnn_forward) and the ensemble (m4_infer.model_prob) -----

    /// <summary>The ensemble's probability for one window's log-mel values: the mean of the seeds' probabilities.</summary>
    public double Probability(float[] mfe)
    {
        if (_seeds.Length == 1)
        {
            return SeedProbability(0, mfe);
        }
        double sum = 0;
        for (int i = 0; i < _seeds.Length; i++)
        {
            sum += SeedProbability(i, mfe);
        }
        return sum / _seeds.Length;
    }

    /// <summary>The probability of one seed (by its index in <see cref="Seeds"/>).</summary>
    public double SeedProbability(int index, float[] mfe)
    {
        if (mfe.Length != Frames * Mels)
        {
            throw new ArgumentException($"{mfe.Length} log-mel values; the model takes {Frames} x {Mels}.");
        }
        Seed s = _seeds[index];
        float[] x = new float[Frames * Mels];
        for (int f = 0; f < Frames; f++)
        {
            for (int m = 0; m < Mels; m++)
            {
                x[f * Mels + m] = (float)((mfe[f * Mels + m] - s.Mean[m]) / s.Std[m]);
            }
        }
        int h = Frames, w = Mels, c = 1;
        int[] channels = [8, 16, 32];
        for (int layer = 0; layer < 3; layer++)
        {
            x = Conv3x3Relu(x, c, h, w, s.Conv[layer], s.Bias[layer], channels[layer]);
            c = channels[layer];
            if (layer < 2)
            {
                x = MaxPool2(x, c, h, w);
                h /= 2;
                w /= 2;
            }
        }
        double dot = 0;
        for (int ch = 0; ch < c; ch++)
        {
            double sum = 0;
            int plane = ch * h * w;
            for (int i = 0; i < h * w; i++)
            {
                sum += x[plane + i];
            }
            float mean = (float)sum / (h * w);
            dot += (double)mean * s.Fc[ch];
        }
        float z = (float)dot + s.FcBias;
        return 1.0 / (1.0 + Math.Exp(-(double)z));
    }

    /// <summary>
    /// 3 x 3 cross-correlation with a padding of 1, bias, ReLU. x and the
    /// result are [channel][row][column]. Each output sums its products in
    /// the order of the weights (input channel, row, column) in float64 and
    /// is rounded to float32 once before the bias is added.
    /// </summary>
    private static float[] Conv3x3Relu(float[] x, int ci, int h, int w, float[] weight, float[] bias, int co)
    {
        int pw = w + 2, plane = (h + 2) * pw;
        double[] padded = new double[ci * plane];
        for (int i = 0; i < ci; i++)
        {
            for (int r = 0; r < h; r++)
            {
                for (int col = 0; col < w; col++)
                {
                    padded[i * plane + (r + 1) * pw + col + 1] = x[(i * h + r) * w + col];
                }
            }
        }
        float[] y = new float[co * h * w];
        double[] acc = new double[h * w];
        for (int o = 0; o < co; o++)
        {
            Array.Clear(acc);
            for (int i = 0; i < ci; i++)
            {
                for (int dr = 0; dr < 3; dr++)
                {
                    for (int dc = 0; dc < 3; dc++)
                    {
                        double k = weight[((o * ci + i) * 3 + dr) * 3 + dc];
                        int origin = i * plane + dr * pw + dc;
                        for (int r = 0; r < h; r++)
                        {
                            ReadOnlySpan<double> src = padded.AsSpan(origin + r * pw, w);
                            Span<double> dst = acc.AsSpan(r * w, w);
                            for (int col = 0; col < dst.Length; col++)
                            {
                                dst[col] += k * src[col];
                            }
                        }
                    }
                }
            }
            for (int n = 0; n < acc.Length; n++)
            {
                float v = (float)acc[n] + bias[o];
                y[o * acc.Length + n] = v > 0 ? v : 0f;
            }
        }
        return y;
    }

    /// <summary>2 x 2 max pooling; an odd last row or column is dropped.</summary>
    private static float[] MaxPool2(float[] x, int c, int h, int w)
    {
        int h2 = h / 2, w2 = w / 2;
        float[] y = new float[c * h2 * w2];
        for (int ch = 0; ch < c; ch++)
        {
            for (int r = 0; r < h2; r++)
            {
                for (int col = 0; col < w2; col++)
                {
                    int i = (ch * h + 2 * r) * w + 2 * col;
                    y[(ch * h2 + r) * w2 + col] = Math.Max(Math.Max(x[i], x[i + 1]), Math.Max(x[i + w], x[i + w + 1]));
                }
            }
        }
        return y;
    }

    public override string ToString() => string.Create(CultureInfo.InvariantCulture, $"{System.IO.Path.GetFileName(Path)}: {Config.Text}");

    /// <summary>A radix-2 complex FFT of 512 points in float64, with exact-as-possible twiddles.</summary>
    private static class Fft512
    {
        private static readonly int[] s_reversed = Reversed();
        private static readonly double[] s_cos = new double[FftSize / 2], s_sin = new double[FftSize / 2];

        static Fft512()
        {
            for (int i = 0; i < FftSize / 2; i++)
            {
                double angle = -2 * Math.PI * i / FftSize;
                s_cos[i] = Math.Cos(angle);
                s_sin[i] = Math.Sin(angle);
            }
        }

        private static int[] Reversed()
        {
            int bits = int.TrailingZeroCount(FftSize);
            int[] r = new int[FftSize];
            for (int i = 0; i < FftSize; i++)
            {
                int v = 0;
                for (int b = 0; b < bits; b++)
                {
                    v |= (i >> b & 1) << (bits - 1 - b);
                }
                r[i] = v;
            }
            return r;
        }

        public static void Transform(double[] re, double[] im)
        {
            for (int i = 0; i < FftSize; i++)
            {
                int j = s_reversed[i];
                if (j > i)
                {
                    (re[i], re[j]) = (re[j], re[i]);
                    (im[i], im[j]) = (im[j], im[i]);
                }
            }
            for (int half = 1; half < FftSize; half <<= 1)
            {
                int step = FftSize / (half * 2);
                for (int start = 0; start < FftSize; start += half * 2)
                {
                    for (int k = 0; k < half; k++)
                    {
                        double c = s_cos[k * step], s = s_sin[k * step];
                        int a = start + k, b = a + half;
                        double tr = re[b] * c - im[b] * s;
                        double ti = re[b] * s + im[b] * c;
                        re[b] = re[a] - tr;
                        im[b] = im[a] - ti;
                        re[a] += tr;
                        im[a] += ti;
                    }
                }
            }
        }
    }
}

/// <summary>The result of one window of the M4 detector.</summary>
/// <param name="K">The window's number: it starts <c>K * 4000</c> samples after the start of its stretch.</param>
/// <param name="Probability">The ensemble's probability; NaN for a window that was not valid (not computed).</param>
/// <param name="Valid">False when samples of the window were lost.</param>
/// <param name="Positive">The post-processing's output: a window of a swallow indication.</param>
/// <param name="Led">The device's LED for this window: 1 yellow (running), 2 green (swallow indication).</param>
public readonly record struct M4Step(long K, double Probability, bool Valid, bool Positive, int Led);

/// <summary>
/// The M4 detector one window at a time, as <c>Detector.step</c> of
/// zaitaku-mealpace's <c>analysis/m4_infer.py</c>: window k starts at
/// k x 0.25 s; its probability goes through <see cref="PostStepper"/> (with
/// the configuration's enter, exit, cap and refractory period, at
/// t = k x 0.25 s; or the threshold when the configuration has no
/// post-processing) and <see cref="LedStepper"/> (at k x 250 ms). A window
/// that is not valid is not computed and is negative.
/// </summary>
public sealed class M4Detector
{
    private readonly M4Model _model;
    private readonly PostStepper? _post;
    private readonly LedStepper _led = new();

    /// <param name="firstWindow">The number of the first window stepped (the windows before it are not seen).</param>
    public M4Detector(M4Model model, long firstWindow = 0)
    {
        _model = model;
        M4Config c = model.Config;
        _post = c.Post == "none" ? null : new PostStepper(c.Enter!.Value, c.Exit!.Value, c.CapSeconds, c.RefractorySeconds);
        Next = firstWindow;
    }

    /// <summary>The number of the next window.</summary>
    public long Next { get; private set; }

    /// <summary>Steps one window of 16000 samples in -1..1 (16-bit samples divided by 32768).</summary>
    public M4Step Step(ReadOnlySpan<float> window, bool valid = true)
    {
        if (window.Length != M4Model.WindowSamples)
        {
            throw new ArgumentException($"A window of {window.Length} samples; the model takes {M4Model.WindowSamples}.");
        }
        return Advance(valid ? _model.Probability(_model.Mfe40(window)) : double.NaN, valid);
    }

    /// <summary>Steps a window that is not valid, without its samples.</summary>
    public M4Step StepInvalid() => Advance(double.NaN, false);

    /// <summary>Steps a window whose probability was computed elsewhere (NaN when not valid).</summary>
    internal M4Step Advance(double p, bool valid)
    {
        long k = Next;
        bool positive = _post is null
            ? valid && p >= _model.Config.Threshold!.Value
            : _post.Step(p, valid, k * PostStepper.HopSeconds);
        int led = _led.Step(k * M4Model.HopMs, positive);
        Next = k + 1;
        return new M4Step(k, p, valid, positive, led);
    }
}
