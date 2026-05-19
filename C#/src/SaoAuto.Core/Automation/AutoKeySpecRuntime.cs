using System.Collections.Immutable;

namespace SaoAuto.Core.Automation;

/// <summary>
/// S141 — immutable, atomically-captured view of one
/// <see cref="AutoKeySpecRuntime"/> tick. Safe to hand to background
/// consumers (telemetry, web bridge) without snapshot-copy on the
/// receiver side.
/// </summary>
public readonly record struct AutoKeyRuntimeSnapshot(
    ImmutableDictionary<string, string> BlockReasons,
    ImmutableDictionary<string, int> CooldownRemainingMs,
    ImmutableDictionary<string, int> ReadyForMs,
    long FireCount)
{
    public static AutoKeyRuntimeSnapshot Empty => new(
        ImmutableDictionary<string, string>.Empty,
        ImmutableDictionary<string, int>.Empty,
        ImmutableDictionary<string, int>.Empty,
        0L);
}

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
    /// S139 — per-action diagnostic snapshot from the most recent
    /// <see cref="Tick"/>. Keys are action ids; values are one of
    /// <c>disabled</c>, <c>readiness</c>, <c>cooldown</c>,
    /// <c>conditions</c>, <c>fired</c>, <c>not-evaluated</c>.
    /// Mirrors the per-action reason strings Python's
    /// <c>AutoKeyEngine</c> stamps on the legacy debug overlay.
    /// </summary>
    public IReadOnlyDictionary<string, string> LastBlockReasons => _lastBlock;
    private readonly Dictionary<string, string> _lastBlock = new();

    /// <summary>
    /// S140 — per-action cooldown remaining (ms) from the most recent
    /// <see cref="Tick"/>. Populated for every action with a positive
    /// <c>MinRearmMs</c> that has fired at least once during this
    /// runtime's lifetime. Lets the HUD show a countdown next to the
    /// "cooldown" entry on <see cref="LastBlockReasons"/>.
    /// </summary>
    public IReadOnlyDictionary<string, int> LastCooldownRemainingMs => _lastCooldownRemaining;
    private readonly Dictionary<string, int> _lastCooldownRemaining = new();

    /// <summary>
    /// S142 — per-action "ready for" duration (ms) from the most
    /// recent <see cref="Tick"/>. Present for every action whose slot
    /// is currently ready and has a recorded leading-edge timestamp
    /// in the readiness gate. Lets the HUD show a "ready 320ms"
    /// counter beside the action; useful when ReadyDelayMs is non-zero
    /// to visualise the hysteresis wait.
    /// </summary>
    public IReadOnlyDictionary<string, int> LastReadyForMs => _lastReadyFor;
    private readonly Dictionary<string, int> _lastReadyFor = new();

    /// <summary>
    /// S141 — atomic snapshot of the live per-tick diagnostics.
    /// Returns immutable copies of <see cref="LastBlockReasons"/> and
    /// <see cref="LastCooldownRemainingMs"/> taken under a single lock,
    /// so background consumers (telemetry, web bridge) get a
    /// consistent view that won't tear with an in-flight
    /// <see cref="Tick"/>. The live `IReadOnlyDictionary` properties
    /// remain unchanged for cheap UI-thread reads where consistency
    /// isn't critical.
    /// </summary>
    public AutoKeyRuntimeSnapshot Snapshot()
    {
        lock (_snapshotGate)
        {
            return new AutoKeyRuntimeSnapshot(
                BlockReasons: _lastBlock.ToImmutableDictionary(),
                CooldownRemainingMs: _lastCooldownRemaining.ToImmutableDictionary(),
                ReadyForMs: _lastReadyFor.ToImmutableDictionary(),
                FireCount: FireCount);
        }
    }
    private readonly object _snapshotGate = new();

    /// <summary>
    /// Reset both gates — call on profile switch (matches Python's
    /// <c>invalidate()</c> at line 626).
    /// </summary>
    public void InvalidateProfileState()
    {
        _readiness.Reset();
        _cooldown.Reset();
        lock (_snapshotGate)
        {
            _lastBlock.Clear();
            _lastCooldownRemaining.Clear();
            _lastReadyFor.Clear();
        }
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
        var reasons = new Dictionary<string, string>(profile.Actions.Length);
        var cooldowns = new Dictionary<string, int>();
        var readyFor = new Dictionary<string, int>();
        string? fired = null;
        foreach (var action in profile.Actions)
        {
            if (action.MinRearmMs > 0)
            {
                var remain = _cooldown.RemainingMs(action.Id, action.MinRearmMs, ctx.Now);
                if (remain > 0) cooldowns[action.Id] = remain;
            }
            if (!action.Enabled) { reasons[action.Id] = "disabled"; continue; }
            if (fired is not null) { reasons[action.Id] = "not-evaluated"; continue; }
            var slot = ctx.Slot(action.SlotIndex);

            if (!_readiness.TryFire(action.Id, slot, action.ReadyDelayMs, ctx.Now))
            { reasons[action.Id] = "readiness"; continue; }
            // gate just confirmed (or recorded) leading-edge ready;
            // snapshot the "ready for" duration for the HUD.
            readyFor[action.Id] = _readiness.ReadyForMs(action.Id, ctx.Now);
            if (!_cooldown.TryFire(action.Id, action.MinRearmMs, ctx.Now))
            { reasons[action.Id] = "cooldown"; continue; }
            if (!ConditionEvaluator.Matches(action.Conditions, ctx, action.SlotIndex))
            { reasons[action.Id] = "conditions"; continue; }

            DispatchKey(action);
            FireCount++;
            reasons[action.Id] = "fired";
            fired = action.Id;
        }
        lock (_snapshotGate)
        {
            _lastBlock.Clear();
            foreach (var kv in reasons) _lastBlock[kv.Key] = kv.Value;
            _lastCooldownRemaining.Clear();
            foreach (var kv in cooldowns) _lastCooldownRemaining[kv.Key] = kv.Value;
            _lastReadyFor.Clear();
            foreach (var kv in readyFor) _lastReadyFor[kv.Key] = kv.Value;
        }
        return fired;
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
