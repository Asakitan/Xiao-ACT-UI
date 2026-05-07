namespace SaoAuto.Core.State;

/// <summary>
/// S87 — pure port of Python <c>game_state.compute_burst_ready</c>.
/// Returns true when every watched skill slot is "ready" and at least one
/// of the watched slots was actually present in the live snapshot.
///
/// A slot counts as ready when ANY of:
///   • <see cref="SkillSlot.State"/> is Ready or Active
///   • <see cref="SkillSlot.Active"/> is true
///   • <see cref="SkillSlot.ChargeCount"/> &gt; 0
///   • <see cref="SkillSlot.RemainingMs"/> &lt;= 120
///   • <see cref="SkillSlot.CooldownPct"/> &lt;= 0.02
/// </summary>
public static class BurstReadyCalculator
{
    public static bool Compute(
        IEnumerable<SkillSlot>? skillSlots,
        IEnumerable<int>? watchedSlots)
    {
        if (skillSlots is null || watchedSlots is null) return false;

        var watched = new HashSet<int>();
        foreach (var idx in watchedSlots)
        {
            if (idx > 0) watched.Add(idx);
        }
        if (watched.Count == 0) return false;

        var matched = new List<SkillSlot>();
        foreach (var slot in skillSlots)
        {
            if (slot is null) continue;
            if (watched.Contains(slot.Index)) matched.Add(slot);
        }
        if (matched.Count == 0) return false;

        foreach (var slot in matched)
        {
            if (!IsReady(slot)) return false;
        }
        return true;
    }

    private static bool IsReady(SkillSlot slot)
    {
        if (slot.State is SkillSlotState.Ready or SkillSlotState.Active) return true;
        if (slot.Active) return true;
        if (slot.ChargeCount > 0) return true;
        if (slot.RemainingMs <= 120) return true;
        return slot.CooldownPct <= 0.02;
    }
}
