// SPDX-License-Identifier: MIT
using System.Globalization;

namespace AudioStudio.Core.Model;

/// <summary>
/// The configuration string of an M4 weights file (its <c>config</c> array),
/// read as <c>parse_config</c> of zaitaku-mealpace's <c>analysis/m4_infer.py</c>
/// reads it: ten <c>key=value</c> pairs in a fixed order, separated by single
/// spaces, with the values that file allows.
/// </summary>
public sealed record M4Config(
    string Text,
    string Set,
    string Kind,
    IReadOnlyList<int> Seeds,
    bool HardNegative,
    string Post,
    double? Threshold,
    double? Enter,
    double? Exit,
    double? CapSeconds,
    double? RefractorySeconds)
{
    public static readonly string[] Keys =
        ["set", "classifier", "seeds", "hard_negative", "post", "threshold", "enter", "exit", "cap_s", "refractory_s"];

    /// <summary>The only refractory period the configuration allows (postprocess.REFRACTORY_S).</summary>
    public const double RefractoryChoice = 0.5;
    /// <summary>The caps of a run the configuration allows (postprocess.CAP_GRID_S).</summary>
    public static readonly double[] CapChoices = [1.5, 2.0, 2.5];

    /// <summary>Parses the string; a <see cref="FormatException"/> says what is wrong.</summary>
    public static M4Config Parse(string text)
    {
        string[][] kv = text.Split(' ').Select(p => p.Split('=', 2)).ToArray();
        if (!kv.Select(k => k[0]).SequenceEqual(Keys) || kv.Any(k => k.Length != 2))
        {
            throw new FormatException($"The M4 configuration is not of the expected form: \"{text}\"");
        }
        Dictionary<string, string> c = kv.ToDictionary(k => k[0], k => k[1]);
        bool lr = c["classifier"] == "lr";
        bool ok =
            ((c["set"], c["classifier"]) is ("a52", "lr") or ("mfe40", "cnn"))
            && (lr ? c["seeds"] == "-" : c["seeds"] is "0" or "0,1,2")
            && (lr ? c["hard_negative"] == "none" : c["hard_negative"] is "none" or "w3.0")
            && c["post"] is "none" or "hyst"
            && (c["cap_s"] == "none" || CapChoices.Any(v => v.ToString("F1", CultureInfo.InvariantCulture) == c["cap_s"]))
            && c["refractory_s"] is "none" or "0.5";
        if (!ok)
        {
            throw new FormatException($"The M4 configuration has a value it does not allow: \"{text}\"");
        }
        double? cap = c["cap_s"] == "none" ? null : Number(c["cap_s"], text);
        double? refractory = c["refractory_s"] == "none" ? null : Number(c["refractory_s"], text);
        double? threshold = null, enter = null, exit = null;
        if (c["post"] == "none")
        {
            if (c["enter"] != "-" || c["exit"] != "-" || cap is not null || refractory is not null)
            {
                throw new FormatException($"The M4 configuration's post-processing values do not fit: \"{text}\"");
            }
            threshold = PostNumber(c["threshold"], text);
        }
        else
        {
            if (c["threshold"] != "-")
            {
                throw new FormatException($"The M4 configuration's post-processing values do not fit: \"{text}\"");
            }
            enter = PostNumber(c["enter"], text);
            exit = PostNumber(c["exit"], text);
        }
        int[] seeds = lr ? [] : c["seeds"].Split(',').Select(s => int.Parse(s, CultureInfo.InvariantCulture)).ToArray();
        return new M4Config(text, c["set"], c["classifier"], seeds, c["hard_negative"] != "none", c["post"],
                            threshold, enter, exit, cap, refractory);
    }

    private static double Number(string value, string text) =>
        double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out double v)
            ? v : throw new FormatException($"Not a number in the M4 configuration: \"{text}\"");

    private static double PostNumber(string value, string text) =>
        double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out double v)
            ? v : throw new FormatException($"The M4 configuration's post-processing values do not fit: \"{text}\"");
}
