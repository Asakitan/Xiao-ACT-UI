namespace SaoAuto.Overlay;

/// <summary>
/// One skill-slot snapshot for the burst-trigger state machine — typed
/// equivalent of the Python <c>gs.skill_slots</c> dict entries
/// (`index`, `state`, `cooldown_pct`, `ready_edge`).
/// </summary>
public sealed record BurstSlot(
    int Index,
    string State,
    double CooldownPct,
    bool ReadyEdge);

/// <summary>
/// Burst-anchor selection state machine — port of the
/// <c>pick_burst_trigger_slot</c> function in
/// <c>_sao_cy_uihelpers.pyx</c>. Selection priority (highest wins):
///   <list type="number">
///     <item>any slot with <c>ready_edge=true</c></item>
///     <item><paramref name="prevSlot"/> if it's still in a ready-ish state</item>
///     <item>first slot with <c>state == "ready"</c></item>
///     <item>first slot with <c>state == "active"</c></item>
///     <item>first slot with <c>cooldown_pct &lt;= 0.02</c></item>
///   </list>
/// Returns 0 when no candidate qualifies.
/// </summary>
public static class BurstTriggerSelector
{
    private const double LowCooldownThreshold = 0.02;

    public static int Pick(
        IEnumerable<BurstSlot>? slots,
        IEnumerable<int>? watched,
        int prevSlot)
    {
        var watchedSet = new HashSet<int>();
        if (watched is not null)
        {
            foreach (var w in watched)
            {
                if (w > 0) watchedSet.Add(w);
            }
        }
        if (watchedSet.Count == 0) watchedSet.Add(1);

        int edgeSlot = 0;
        int firstReady = 0;
        int firstActive = 0;
        int firstLowCd = 0;
        bool prevStillOk = false;

        if (slots is not null)
        {
            foreach (var slot in slots)
            {
                if (slot is null) continue;
                int idx = slot.Index;
                if (idx <= 0 || !watchedSet.Contains(idx)) continue;

                var state = (slot.State ?? string.Empty).Trim().ToLowerInvariant();
                double cd = slot.CooldownPct;
                bool isReady = state == "ready" || state == "active" || cd <= LowCooldownThreshold;

                if (slot.ReadyEdge && edgeSlot == 0) edgeSlot = idx;
                if (state == "ready" && firstReady == 0) firstReady = idx;
                if (state == "active" && firstActive == 0) firstActive = idx;
                if (cd <= LowCooldownThreshold && firstLowCd == 0) firstLowCd = idx;
                if (idx == prevSlot && isReady) prevStillOk = true;
            }
        }

        if (edgeSlot != 0) return edgeSlot;
        if (prevStillOk && prevSlot != 0) return prevSlot;
        if (firstReady != 0) return firstReady;
        if (firstActive != 0) return firstActive;
        if (firstLowCd != 0) return firstLowCd;
        return 0;
    }
}
