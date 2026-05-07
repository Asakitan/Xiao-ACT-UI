using System.Collections.Immutable;
using SaoAuto.Core.State;

namespace SaoAuto.Core.Vision;

/// <summary>
/// S93 — Maps recognition-tier <see cref="SkillSlotResult"/> records into
/// the canonical <see cref="SkillSlot"/> snapshot type that
/// <see cref="GameStateManager"/> + <see cref="BurstReadyCalculator"/> consume.
///
/// Recognition-side <c>SkillSlotResult.State</c> is a string from
/// <see cref="SkillStateClassifier"/> (<c>"ready" | "cooldown" |
/// "insufficient_energy" | "unknown"</c>), without an explicit
/// <c>"active"</c> branch — the recognition layer marks active via the
/// <see cref="SkillSlotResult.Active"/> bool. Charge count + remaining-ms
/// only come from the packet path, so this mapper leaves them at their
/// defaults — matches Python <c>recognition.py</c> dict slots which omit
/// those keys (and consequently default-trigger the
/// <c>remaining_ms &lt;= 120</c> branch of <see cref="BurstReadyCalculator"/>;
/// the documented Python parity behaviour, fixed by switching to the
/// packet bridge for accurate burst readiness).
/// </summary>
public static class RecognitionSkillSlotProjector
{
    public static ImmutableArray<SkillSlot> Project(IReadOnlyList<SkillSlotResult>? results)
    {
        if (results is null || results.Count == 0)
        {
            return ImmutableArray<SkillSlot>.Empty;
        }
        var b = ImmutableArray.CreateBuilder<SkillSlot>(results.Count);
        foreach (var r in results)
        {
            b.Add(new SkillSlot
            {
                Index = r.Index,
                State = ParseState(r.State),
                CooldownPct = r.CooldownRatio,
                InsufficientEnergy = r.InsufficientEnergy,
                Active = r.Active,
                ReadyEdge = r.ReadyEdge,
                // ChargeCount + RemainingMs intentionally default — recognition
                // path can't infer these; packet path supplies them.
            });
        }
        return b.ToImmutable();
    }

    public static SkillSlotState ParseState(string? state)
    {
        if (string.IsNullOrEmpty(state)) return SkillSlotState.Unknown;
        return state.Trim().ToLowerInvariant() switch
        {
            "ready" => SkillSlotState.Ready,
            "active" => SkillSlotState.Active,
            "cooldown" => SkillSlotState.Cooldown,
            "insufficient_energy" => SkillSlotState.InsufficientEnergy,
            _ => SkillSlotState.Unknown,
        };
    }
}
