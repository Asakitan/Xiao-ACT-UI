using System.Text.Json.Nodes;

namespace SaoAuto.Core.Configuration;

/// <summary>
/// S94 — Read + normalise the <c>watched_skill_slots</c> setting that drives
/// <see cref="State.BurstReadyCalculator"/>. Pure port of Python
/// <c>_sao_cy_uihelpers.normalize_watched_skill_slots</c> (cython source
/// _sao_cy_uihelpers.pyx 78–93) plus the Python read-default convention
/// (<c>[1..9]</c> when the key is missing or null).
///
/// Semantics:
/// <list type="bullet">
/// <item><description>Missing key / null / empty → default [1..9].</description></item>
/// <item><description>Non-integer entries silently skipped.</description></item>
/// <item><description>Out-of-range entries (slot &lt; 1 or &gt; 9) skipped.</description></item>
/// <item><description>Duplicates removed; first-seen order preserved.</description></item>
/// </list>
///
/// Returned list is safe to hand to <c>RecognitionTickHost</c> as the
/// <c>watchedSlots</c> ctor argument.
/// </summary>
public static class WatchedSkillSlotsLoader
{
    private static readonly int[] DefaultSlots = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };

    public static IReadOnlyList<int> Load(SettingsManager settings)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        var raw = settings.Get<JsonArray?>(SettingsKeys.WatchedSkillSlots);
        var normalized = Normalize(raw);
        return normalized.Count == 0 ? DefaultSlots : normalized;
    }

    public static IReadOnlyList<int> Normalize(JsonArray? raw)
    {
        if (raw is null || raw.Count == 0) return Array.Empty<int>();
        var seen = new HashSet<int>();
        var ordered = new List<int>(raw.Count);
        foreach (var node in raw)
        {
            if (node is not JsonValue v) continue;
            if (!TryCoerceInt(v, out var slot)) continue;
            if (slot < 1 || slot > 9) continue;
            if (seen.Add(slot)) ordered.Add(slot);
        }
        return ordered;
    }

    private static bool TryCoerceInt(JsonValue v, out int result)
    {
        if (v.TryGetValue<int>(out result)) return true;
        if (v.TryGetValue<long>(out var l) && l is >= int.MinValue and <= int.MaxValue)
        {
            result = (int)l;
            return true;
        }
        if (v.TryGetValue<double>(out var d) && !double.IsNaN(d) && d is >= int.MinValue and <= int.MaxValue)
        {
            result = (int)d;
            return true;
        }
        if (v.TryGetValue<string>(out var s) && int.TryParse(s, out var parsed))
        {
            result = parsed;
            return true;
        }
        result = 0;
        return false;
    }
}
