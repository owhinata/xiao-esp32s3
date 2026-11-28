// SPDX-License-Identifier: MIT
using System.Globalization;
using System.Text;

namespace AudioStudio.Core.Marks;

/// <summary>
/// The text of a <c>setup</c> row: what produced the indications from that
/// row on, as one line of space-separated <c>key=value</c> fields in this
/// order:
/// <c>model=&lt;file name|-&gt; spike_filter=&lt;on|off&gt; mains=&lt;Hz&gt;</c>,
/// for example <c>model=m4-0038.npz spike_filter=on mains=50</c>.
/// <c>model</c> is the file name of the model loaded (no directory), <c>-</c>
/// when none is; in it '%', '=', white space and control characters are
/// written as %XX of their UTF-8 bytes, so a value never holds a space.
/// <c>spike_filter</c> is the switch before the M4 detector and <c>mains</c>
/// the mains frequency the filter follows, written also while it is off.
/// </summary>
public static class DetectorSetup
{
    public static string Format(string? modelPath, bool spikeFilter, double mainsHz) =>
        string.Create(CultureInfo.InvariantCulture,
            $"model={(modelPath is null ? "-" : Escape(Path.GetFileName(modelPath)))} spike_filter={(spikeFilter ? "on" : "off")} mains={mainsHz:0.##}");

    private static string Escape(string name)
    {
        StringBuilder text = new();
        Span<byte> bytes = stackalloc byte[4];
        foreach (Rune r in name.EnumerateRunes())
        {
            if (r.Value == '%' || r.Value == '=' || Rune.IsWhiteSpace(r) || Rune.IsControl(r))
            {
                int n = r.EncodeToUtf8(bytes);
                foreach (byte b in bytes[..n])
                {
                    text.Append('%').Append(b.ToString("X2", CultureInfo.InvariantCulture));
                }
            }
            else
            {
                text.Append(r.ToString());
            }
        }
        return text.ToString();
    }
}
