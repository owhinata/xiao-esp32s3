// SPDX-License-Identifier: MIT
namespace AudioStudio.Core.Marks;

/// <summary>What became of one indication in the user's notes.</summary>
/// <param name="Indication">The indication mark.</param>
/// <param name="MarkedFalse">The <c>false</c> mark the user attached to it; null for a hit.</param>
/// <param name="Swallow">The <c>swallow</c> mark that is the same swallow; null when there is none.</param>
public sealed record ReviewedIndication(Mark Indication, Mark? MarkedFalse, Mark? Swallow)
{
    public bool IsHit => MarkedFalse is null;
}

/// <summary>The marks of a recording read as the user's notes about the indications.</summary>
/// <param name="Indications">Every indication, in order, with what the notes say of it.</param>
/// <param name="MissedSwallows">The <c>swallow</c> marks with no indication near them.</param>
/// <param name="UnattachedFalse">The <c>false</c> marks with no indication left to refer to.</param>
public sealed record MarkReviewResult(IReadOnlyList<ReviewedIndication> Indications, IReadOnlyList<Mark> MissedSwallows,
                                      IReadOnlyList<Mark> UnattachedFalse)
{
    public int MarkedFalse => Indications.Count(i => !i.IsHit);
    public int Hits => Indications.Count(i => i.IsHit);
}

/// <summary>
/// Reads the marks as the user's notes: an indication counts as a swallow
/// unless the user marked it wrong; the user marks <c>swallow</c> only for
/// a swallow the detector did not indicate.
/// <list type="bullet">
/// <item>A <c>false</c> mark refers to the latest indication that starts at or
/// before it and that no earlier <c>false</c> mark took; without one it is an
/// unattached note.</item>
/// <item>An indication without a <c>false</c> mark is a hit, one with a
/// <c>false</c> mark a false indication.</item>
/// <item>A <c>swallow</c> mark is the same swallow as the nearest indication not
/// marked false that starts from <see cref="SwallowBefore"/> before to
/// <see cref="SwallowAfter"/> after it and that no other <c>swallow</c> mark
/// took (the marks taken in order); otherwise it is a missed swallow.</item>
/// </list>
/// </summary>
public static class MarkReview
{
    /// <summary>How long before a <c>swallow</c> mark an indication of the same swallow may start.</summary>
    public static readonly TimeSpan SwallowBefore = TimeSpan.FromSeconds(2);
    /// <summary>How long after a <c>swallow</c> mark an indication of the same swallow may start.</summary>
    public static readonly TimeSpan SwallowAfter = TimeSpan.FromSeconds(3);

    public static MarkReviewResult Review(IEnumerable<Mark> marks, int sampleRate)
    {
        List<Mark> sorted = MarkFile.Sorted(marks);
        List<Mark> indications = sorted.Where(m => m.Kind == MarkKind.Indication).ToList();
        Mark?[] falseOf = new Mark?[indications.Count];
        Mark?[] swallowOf = new Mark?[indications.Count];
        List<Mark> unattached = new();
        foreach (Mark f in sorted.Where(m => m.Kind == MarkKind.False))
        {
            int i = indications.FindLastIndex(m => m.Sample <= f.Sample);
            while (i >= 0 && falseOf[i] is not null)
            {
                i--;
            }
            if (i >= 0)
            {
                falseOf[i] = f;
            }
            else
            {
                unattached.Add(f);
            }
        }
        long before = (long)Math.Round(SwallowBefore.TotalSeconds * sampleRate);
        long after = (long)Math.Round(SwallowAfter.TotalSeconds * sampleRate);
        List<Mark> missed = new();
        foreach (Mark s in sorted.Where(m => m.Kind == MarkKind.Swallow))
        {
            int best = -1;
            for (int i = 0; i < indications.Count; i++)
            {
                long d = indications[i].Sample - s.Sample;
                if (falseOf[i] is null && swallowOf[i] is null && d >= -before && d <= after &&
                    (best < 0 || Math.Abs(d) < Math.Abs(indications[best].Sample - s.Sample)))
                {
                    best = i;
                }
            }
            if (best >= 0)
            {
                swallowOf[best] = s;
            }
            else
            {
                missed.Add(s);
            }
        }
        List<ReviewedIndication> reviewed = indications
            .Select((m, i) => new ReviewedIndication(m, falseOf[i], swallowOf[i])).ToList();
        return new MarkReviewResult(reviewed, missed, unattached);
    }
}
