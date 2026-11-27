// SPDX-License-Identifier: MIT
using System.Text.RegularExpressions;
using AudioStudio.Core.Targets;

namespace AudioStudio.Core.Board;

/// <summary>A shell's prompt, as the target profile gives it: a regular expression for the end of the output.</summary>
public sealed class ShellPrompt
{
    private readonly Regex _end;

    public string Pattern { get; }

    /// <exception cref="TargetProfileException">The pattern is not a regular expression.</exception>
    public ShellPrompt(string pattern)
    {
        Pattern = pattern;
        try
        {
            _end = new Regex($"(?:{pattern})$", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
        }
        catch (ArgumentException e)
        {
            throw new TargetProfileException($"shell.prompt: {e.Message}");
        }
    }

    /// <summary>The text ends with the prompt (spaces after it do not count).</summary>
    public bool IsPrompt(string text) => _end.IsMatch(text.TrimEnd());

    /// <summary>Whether a capture ends with the prompt: the command finished within the wait.</summary>
    public bool Finished(string captured) => IsPrompt(captured);

    /// <summary>
    /// The lines a command printed: its output without the echo of the
    /// command and without the prompt.
    /// </summary>
    public List<string> OutputLines(string captured, string line)
    {
        List<string> lines = new();
        foreach (string raw in captured.Split('\n'))
        {
            string text = raw.TrimEnd();
            if (text.Length == 0 || text.Contains(line, StringComparison.Ordinal) || IsPrompt(text))
            {
                continue;
            }
            lines.Add(text);
        }
        return lines;
    }
}
