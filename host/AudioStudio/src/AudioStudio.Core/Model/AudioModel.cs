// SPDX-License-Identifier: MIT
using System.Globalization;
using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;

namespace AudioStudio.Core.Model;

/// <summary>A model that does not fit the contract (README, "The model contract").</summary>
public sealed class ModelContractException(string message) : Exception(message);

/// <summary>
/// An ONNX model that classifies a window of 16 kHz audio. The contract:
/// one float32 input of shape [1, N] or [1, 1, N] holding N samples scaled
/// to -1..1 (N from the model, or <c>dynamicSamples</c> when the model
/// leaves it open), and one float32 output of C scores ([1, C]). Scores
/// that are not already probabilities (all in 0..1, summing to 1) go
/// through a softmax. The class names come from <c>&lt;model&gt;.labels.txt</c>
/// beside the model (one per line), else from the metadata key
/// <c>labels</c> (comma-separated), else they are "class 0", "class 1", ...
/// </summary>
public sealed class AudioModel : IDisposable
{
    public const int SampleRate = 16000;

    private readonly InferenceSession _session;
    private readonly string _input, _output;
    private readonly int[] _inputShape;
    private string[] _labels;

    public string Path { get; }
    /// <summary>Samples per window (N).</summary>
    public int WindowSamples { get; }
    /// <summary>Whether N was left open by the model and taken from the setting.</summary>
    public bool DynamicWindow { get; }
    /// <summary>Number of classes; 0 until the first run when the model leaves it open.</summary>
    public int Classes { get; private set; }
    /// <summary>Where the class names came from: the labels file, the metadata, or none.</summary>
    public string LabelSource { get; }

    public IReadOnlyList<string> Labels => _labels;

    /// <param name="dynamicSamples">N for a model whose input length is not fixed.</param>
    public AudioModel(string path, int dynamicSamples = SampleRate)
    {
        Path = path;
        SessionOptions options = new() { IntraOpNumThreads = 1, InterOpNumThreads = 1 };
        // No busy waiting between runs: the model runs a few times a second.
        options.AddSessionConfigEntry("session.intra_op.allow_spinning", "0");
        try
        {
            _session = new InferenceSession(path, options);
        }
        catch (OnnxRuntimeException e)
        {
            throw new ModelContractException($"Cannot load {System.IO.Path.GetFileName(path)}: {e.Message}");
        }
        finally
        {
            options.Dispose();
        }
        try
        {
            if (_session.InputMetadata.Count != 1)
            {
                throw new ModelContractException($"The model has {_session.InputMetadata.Count} inputs; it needs one.");
            }
            (_input, NodeMetadata input) = _session.InputMetadata.First();
            if (input.ElementType != typeof(float))
            {
                throw new ModelContractException($"The input is of {input.ElementType.Name}; it needs float32.");
            }
            int[] dims = input.Dimensions;
            bool shapeOk = dims.Length is 2 or 3 && dims[..^1].All(d => d is 1 or -1 or 0);
            if (!shapeOk)
            {
                throw new ModelContractException($"The input's shape is {Shape(dims)}; it needs [1, N] or [1, 1, N].");
            }
            DynamicWindow = dims[^1] <= 0;
            WindowSamples = DynamicWindow ? dynamicSamples : dims[^1];
            if (WindowSamples < 1)
            {
                throw new ModelContractException($"A window of {WindowSamples} samples.");
            }
            _inputShape = dims.Length == 2 ? [1, WindowSamples] : [1, 1, WindowSamples];

            if (_session.OutputMetadata.Count < 1)
            {
                throw new ModelContractException("The model has no output.");
            }
            (_output, NodeMetadata output) = _session.OutputMetadata.First();
            if (output.ElementType != typeof(float))
            {
                throw new ModelContractException($"The output is of {output.ElementType.Name}; it needs float32.");
            }
            int[] outDims = output.Dimensions;
            if (outDims.Length is < 1 or > 2 || (outDims.Length == 2 && outDims[0] is not (1 or -1 or 0)))
            {
                throw new ModelContractException($"The output's shape is {Shape(outDims)}; it needs [1, C].");
            }
            Classes = Math.Max(0, outDims[^1]);

            (_labels, LabelSource) = ReadLabels(path, _session.ModelMetadata.CustomMetadataMap);
            FitLabels();
        }
        catch
        {
            _session.Dispose();
            throw;
        }
    }

    private static string Shape(int[] dims) =>
        "[" + string.Join(", ", dims.Select(d => d > 0 ? d.ToString(CultureInfo.InvariantCulture) : "?")) + "]";

    /// <summary>The labels file beside the model: <c>name.onnx</c> has <c>name.labels.txt</c>.</summary>
    public static string LabelsPath(string modelPath) =>
        System.IO.Path.ChangeExtension(modelPath, null) + ".labels.txt";

    private static (string[] Labels, string Source) ReadLabels(string path, Dictionary<string, string> metadata)
    {
        string file = LabelsPath(path);
        if (File.Exists(file))
        {
            string[] lines = File.ReadAllLines(file).Select(l => l.Trim()).Where(l => l.Length > 0).ToArray();
            return (lines, System.IO.Path.GetFileName(file));
        }
        if (metadata.TryGetValue("labels", out string? text))
        {
            return (text.Split(',').Select(l => l.Trim()).ToArray(), "metadata \"labels\"");
        }
        return ([], "none");
    }

    /// <summary>One name per class: missing names are made up, extra ones dropped.</summary>
    private void FitLabels()
    {
        if (Classes == 0)
        {
            return;
        }
        _labels = Enumerable.Range(0, Classes)
            .Select(i => i < _labels.Length && _labels[i].Length > 0 ? _labels[i] : $"class {i}")
            .ToArray();
    }

    /// <summary>The class probabilities for one window of <see cref="WindowSamples"/> samples in -1..1.</summary>
    public float[] Run(ReadOnlySpan<float> window)
    {
        if (window.Length != WindowSamples)
        {
            throw new ArgumentException($"A window of {window.Length} samples; the model takes {WindowSamples}.");
        }
        DenseTensor<float> tensor = new(window.ToArray(), _inputShape);
        float[] scores;
        try
        {
            using IDisposableReadOnlyCollection<DisposableNamedOnnxValue> results =
                _session.Run([NamedOnnxValue.CreateFromTensor(_input, tensor)], [_output]);
            scores = results[0].AsTensor<float>().ToArray();
        }
        catch (OnnxRuntimeException e)
        {
            throw new ModelContractException($"The model failed: {e.Message}");
        }
        if (scores.Length == 0)
        {
            throw new ModelContractException("The model gave no scores.");
        }
        if (Classes == 0)
        {
            Classes = scores.Length;
            FitLabels();
        }
        else if (scores.Length != Classes)
        {
            throw new ModelContractException($"The model gave {scores.Length} scores for {Classes} classes.");
        }
        return Probabilities(scores);
    }

    /// <summary>The scores as they are if they are probabilities already, else their softmax.</summary>
    public static float[] Probabilities(float[] scores)
    {
        double sum = 0;
        bool inRange = true;
        foreach (float s in scores)
        {
            sum += s;
            inRange &= s is >= 0 and <= 1;
        }
        if (inRange && Math.Abs(sum - 1) < 1e-3)
        {
            return scores;
        }
        float max = scores.Max();
        double total = 0;
        float[] p = new float[scores.Length];
        for (int i = 0; i < p.Length; i++)
        {
            p[i] = MathF.Exp(scores[i] - max);
            total += p[i];
        }
        for (int i = 0; i < p.Length; i++)
        {
            p[i] = (float)(p[i] / total);
        }
        return p;
    }

    public void Dispose() => _session.Dispose();
}
