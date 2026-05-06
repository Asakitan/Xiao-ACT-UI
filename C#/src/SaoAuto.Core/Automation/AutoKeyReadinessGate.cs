namespace SaoAuto.Core.Automation;

/// <summary>
/// Snapshot of one skill / ability slot at the moment a tick reads
/// recognition state. Mirrors the dict shape that
/// <c>auto_key_engine._slot_is_ready</c> consumes from
/// <c>game_state.slot_map</c>: any one of (state, active flag,
/// charge_count, remaining_ms, cooldown_pct) signals readiness.
/// </summary>
public readonly record struct SlotReadiness(
    string State,
    bool Active,
    int ChargeCount,
    int RemainingMs,
    double CooldownPct)
{
    /// <summary>Default = no signals → not ready.</summary>
    public static readonly SlotReadiness NotReady = new("", false, 0, 999_999, 1.0);

    /// <summary>Bit-faithful port of <c>_slot_is_ready</c> (lines 740–752).</summary>
    public static bool IsReady(SlotReadiness slot)
    {
        var st = (slot.State ?? string.Empty).ToLowerInvariant();
        if (st is "ready" or "active") return true;
        if (slot.Active) return true;
        if (slot.ChargeCount > 0) return true;
        if (slot.RemainingMs <= 120) return true;
        return slot.CooldownPct <= 0.02;
    }
}

/// <summary>
/// S72 — slot-readiness hysteresis gate. C# port of the
/// <c>_ready_since</c> tracking inside <c>_action_ready</c>
/// (auto_key_engine.py lines 784–800).
///
/// The contract: an action only fires once its slot has been
/// continuously detected as ready for <c>readyDelayMs</c>. If the
/// slot ever drops back to not-ready the timer resets — guards
/// against single-frame flicker in the recognition layer triggering
/// premature key fires (the bug that motivated the hysteresis in
/// Python).
///
/// Independent of <see cref="AutoKeyCooldownGate"/>: cooldown is
/// "have we fired recently?", readiness is "has the slot been
/// stable long enough to warrant firing?". Both must pass.
/// </summary>
public sealed class AutoKeyReadinessGate
{
    private readonly Dictionary<string, DateTimeOffset> _readySince = new();
    private readonly object _gate = new();

    /// <summary>
    /// Returns true if the slot is currently ready AND has been ready
    /// continuously for at least <paramref name="readyDelayMs"/>.
    /// Records the first-ready timestamp on the leading edge.
    /// </summary>
    public bool TryFire(string actionId, SlotReadiness slot, int readyDelayMs, DateTimeOffset now)
    {
        ArgumentException.ThrowIfNullOrEmpty(actionId);
        lock (_gate)
        {
            if (!SlotReadiness.IsReady(slot))
            {
                _readySince.Remove(actionId);
                return false;
            }

            if (!_readySince.TryGetValue(actionId, out var since))
            {
                since = now;
                _readySince[actionId] = since;
            }

            if (readyDelayMs <= 0) return true;
            return (now - since).TotalMilliseconds >= readyDelayMs;
        }
    }

    /// <summary>Inspect without recording; useful for telemetry / tests.</summary>
    public DateTimeOffset? ReadySince(string actionId)
    {
        lock (_gate)
            return _readySince.TryGetValue(actionId, out var since) ? since : null;
    }

    /// <summary>Forget one action — call on profile switch / action edit.</summary>
    public void Forget(string actionId)
    {
        lock (_gate) _readySince.Remove(actionId);
    }

    /// <summary>
    /// Forget all recorded readiness — mirrors Python's
    /// <c>invalidate()</c> on profile switch (line 626–629).
    /// </summary>
    public void Reset()
    {
        lock (_gate) _readySince.Clear();
    }

    public int TrackedCount { get { lock (_gate) return _readySince.Count; } }
}
