using System.Collections.Immutable;

namespace SaoAuto.Core.Automation;

/// <summary>
/// S75 — spec-side runtime context. Mirrors the per-tick inputs
/// that <c>auto_key_engine._conditions_match</c> reads off
/// <c>game_state</c> + <c>slot_map</c>: every field condition
/// evaluator needs, no more.
/// </summary>
public readonly record struct AutoKeySpecContext(
    double HpPct,
    double StaminaPct,
    bool BurstReady,
    string ProfessionName,
    string PlayerName,
    bool InCombat,
    ImmutableDictionary<int, SlotReadiness> Slots,
    DateTimeOffset Now)
{
    public static AutoKeySpecContext Empty => new(
        HpPct: 1.0, StaminaPct: 1.0, BurstReady: false,
        ProfessionName: "", PlayerName: "", InCombat: false,
        Slots: ImmutableDictionary<int, SlotReadiness>.Empty,
        Now: DateTimeOffset.UnixEpoch);

    public SlotReadiness Slot(int index)
        => Slots.TryGetValue(index, out var s) ? s : SlotReadiness.NotReady;
}

/// <summary>
/// Pure evaluator for the 8-variant <see cref="AutoKeyCondition"/>
/// ADT. Bit-faithful port of <c>_conditions_match</c>
/// (auto_key_engine.py 754–782): all conditions must pass
/// (logical AND); empty list → trivially true.
/// </summary>
public static class ConditionEvaluator
{
    public static bool Matches(
        IReadOnlyList<AutoKeyCondition> conditions,
        AutoKeySpecContext ctx,
        int fallbackSlotIndex = 0)
    {
        for (var i = 0; i < conditions.Count; i++)
        {
            if (!Matches(conditions[i], ctx, fallbackSlotIndex)) return false;
        }
        return true;
    }

    public static bool Matches(AutoKeyCondition condition, AutoKeySpecContext ctx, int fallbackSlotIndex = 0)
        => condition switch
        {
            HpPctGteCondition c => ctx.HpPct >= c.Value,
            HpPctLteCondition c => ctx.HpPct <= c.Value,
            StaPctGteCondition c => ctx.StaminaPct >= c.Value,
            BurstReadyIsCondition c => ctx.BurstReady == c.Value,
            SlotStateIsCondition c => NormalizedSlotState(ctx.Slot(c.SlotIndex > 0 ? c.SlotIndex : fallbackSlotIndex))
                .Equals(string.IsNullOrEmpty(c.State) ? "ready" : c.State, StringComparison.OrdinalIgnoreCase),
            ProfessionIsCondition c => string.Equals(ctx.ProfessionName, c.Value, StringComparison.Ordinal),
            PlayerNameIsCondition c => string.Equals(ctx.PlayerName, c.Value, StringComparison.Ordinal),
            InCombatIsCondition c => ctx.InCombat == c.Value,
            _ => false,
        };

    /// <summary>
    /// Port of <c>_normalized_slot_state</c> (auto_key_engine.py
    /// 732–738): returns the raw state string when it's one of the
    /// known states, otherwise classifies by IsReady.
    /// </summary>
    public static string NormalizedSlotState(SlotReadiness slot)
    {
        var st = (slot.State ?? string.Empty).ToLowerInvariant();
        if (st is "ready" or "active" or "cooldown" or "unknown" or "insufficient_energy") return st;
        return SlotReadiness.IsReady(slot) ? "ready" : "cooldown";
    }
}

/// <summary>
/// S75 — spec-driven runtime executor. Composes
/// <see cref="ConditionEvaluator"/>, <see cref="AutoKeyReadinessGate"/>
/// (S72), and <see cref="AutoKeyCooldownGate"/> against an
/// <see cref="AutoKeyProfileSpecRecord"/>. Per tick, picks the first
/// enabled action whose slot is ripe (readiness+rearm) and whose
/// conditions all match, dispatches its key sequence, and bumps fire
/// count.
///
/// Mirrors the runtime body of Python's <c>AutoKeyEngine._tick</c>
/// (lines 654–700) but stays pure-functional: caller pumps ticks.
/// </summary>
public sealed class AutoKeySpecRuntime
{
    private readonly IKeyDispatcher _dispatcher;
    private readonly AutoKeyReadinessGate _readiness;
    private readonly AutoKeyCooldownGate _cooldown;

    public AutoKeySpecRuntime(
        IKeyDispatcher dispatcher,
        AutoKeyReadinessGate? readiness = null,
        AutoKeyCooldownGate? cooldown = null)
    {
        _dispatcher = dispatcher ?? throw new ArgumentNullException(nameof(dispatcher));
        _readiness = readiness ?? new AutoKeyReadinessGate();
        _cooldown = cooldown ?? new AutoKeyCooldownGate();
    }

    public long FireCount { get; private set; }

    /// <summary>
    /// Reset both gates — call on profile switch (matches Python's
    /// <c>invalidate()</c> at line 626).
    /// </summary>
    public void InvalidateProfileState()
    {
        _readiness.Reset();
        _cooldown.Reset();
    }

    /// <summary>
    /// Run one tick against <paramref name="profile"/>. Returns the
    /// action id that fired, or null if no action matched. Iterates
    /// actions in declaration order — matches Python (which doesn't
    /// sort by priority for spec actions, only the legacy trigger
    /// path does).
    /// </summary>
    public string? Tick(AutoKeyProfileSpecRecord profile, AutoKeySpecContext ctx)
    {
        ArgumentNullException.ThrowIfNull(profile);
        foreach (var action in profile.Actions)
        {
            if (!action.Enabled) continue;
            var slot = ctx.Slot(action.SlotIndex);

            if (!_readiness.TryFire(action.Id, slot, action.ReadyDelayMs, ctx.Now)) continue;
            if (!_cooldown.TryFire(action.Id, action.MinRearmMs, ctx.Now)) continue;
            if (!ConditionEvaluator.Matches(action.Conditions, ctx, action.SlotIndex)) continue;

            DispatchKey(action);
            FireCount++;
            return action.Id;
        }
        return null;
    }

    private void DispatchKey(AutoKeyActionSpec action)
    {
        var vk = ResolveVk(action.Key);
        if (vk == 0) return;
        var hold = action.PressMode == "hold" ? Math.Max(0, action.HoldMs) : 0;
        var stroke = new KeyStroke(vk, AutoKeyModifiers.None, HoldMs: hold);
        var presses = action.PressMode == "hold" ? 1 : Math.Max(1, action.PressCount);
        for (var i = 0; i < presses; i++) _dispatcher.Dispatch(stroke);
    }

    /// <summary>
    /// Map an action key string (e.g. "1", "F5", "Q") to its
    /// Virtual-Key code. Matches the small map at the top of
    /// auto_key_engine.py (digits 0–9, letters A–Z, F1–F12).
    /// </summary>
    public static int ResolveVk(string key)
    {
        if (string.IsNullOrEmpty(key)) return 0;
        var k = key.Trim().ToUpperInvariant();
        if (k.Length == 1)
        {
            var ch = k[0];
            if (ch is >= '0' and <= '9') return 0x30 + (ch - '0');
            if (ch is >= 'A' and <= 'Z') return ch;
        }
        if (k.Length >= 2 && k[0] == 'F' && int.TryParse(k.AsSpan(1), out var n) && n is >= 1 and <= 12)
            return 0x6F + n;   // 0x70..0x7B
        return 0;
    }
}
