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

    // R16: burst-visual virtual actions appended to every profile tick after
    // the declared actions. Mirrors Python's _burst_visual_actions list set
    // by set_burst_actions(...) and read back by _get_burst_virtual_actions()
    // at auto_key_engine.py:929-980. Stored as the typed action spec so the
    // runtime path is identical to declared actions.
    private System.Collections.Immutable.ImmutableArray<AutoKeyActionSpec> _burstActions
        = System.Collections.Immutable.ImmutableArray<AutoKeyActionSpec>.Empty;

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

    /// <summary>R16: install the burst-visual virtual actions for subsequent
    /// ticks. Mirrors Python's <c>set_burst_actions(list)</c> at
    /// auto_key_engine.py:929. Each input pair is normalised: out-of-range
    /// (trigger_slot or action_slot ∉ [1, 9]) entries are dropped, the
    /// generated key is mapped 1:1 with the slot digit, the condition list
    /// pins <c>burst_ready_is=true</c> AND <c>slot_state_is=ready</c> on the
    /// trigger slot, and the fire rules mirror the Python tuning
    /// (tap × 1, hold 80ms, post-delay 200ms, rearm 500ms).</summary>
    public void SetBurstActions(IReadOnlyList<(int TriggerSlot, int ActionSlot)> pairs)
    {
        if (pairs is null || pairs.Count == 0)
        {
            _burstActions = System.Collections.Immutable.ImmutableArray<AutoKeyActionSpec>.Empty;
            return;
        }
        var builder = System.Collections.Immutable.ImmutableArray.CreateBuilder<AutoKeyActionSpec>();
        for (var i = 0; i < pairs.Count; i++)
        {
            var trigger = pairs[i].TriggerSlot;
            var action = pairs[i].ActionSlot;
            if (trigger < 1 || trigger > 9) continue;
            if (action < 1 || action > 9) continue;
            builder.Add(BuildBurstVirtualAction(i, trigger, action));
        }
        _burstActions = builder.ToImmutable();
    }

    /// <summary>R16: read back the installed burst-visual actions. The
    /// returned specs are the same objects evaluated each tick, so consumers
    /// must treat them as read-only.</summary>
    public IReadOnlyList<AutoKeyActionSpec> BurstActions => _burstActions;

    private static AutoKeyActionSpec BuildBurstVirtualAction(
        int index, int triggerSlot, int actionSlot)
    {
        // Slot index → digit key mirrors Python's SLOT_KEY_MAP at line 946.
        var key = ((char)('0' + actionSlot)).ToString();
        var conds = System.Collections.Immutable.ImmutableArray.Create<AutoKeyCondition>(
            new BurstReadyIsCondition(true),
            new SlotStateIsCondition(triggerSlot, "ready"));
        return new AutoKeyActionSpec(
            Id: $"_burst_visual_{index}",
            Label: $"Burst S{triggerSlot}→S{actionSlot}",
            Enabled: true,
            SlotIndex: actionSlot,
            Key: key,
            PressMode: "tap",
            PressCount: 1,
            PressIntervalMs: 0,
            HoldMs: 80,
            ReadyDelayMs: 0,
            MinRearmMs: 500,
            PostDelayMs: 200,
            Conditions: conds);
    }

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
        var actionCount = profile.Actions.Length + _burstActions.Length;
        var reasons = new Dictionary<string, string>(actionCount);
        var cooldowns = new Dictionary<string, int>();
        var readyFor = new Dictionary<string, int>();
        string? fired = null;
        // First pass: declared profile actions.
        fired = EvaluateActions(profile.Actions, ctx, reasons, cooldowns, readyFor, fired);
        // R16: second pass — burst-visual virtual actions appended at the end
        // so a declared action that fires this tick still wins. Matches
        // Python's loop ordering at auto_key_engine.py:760-770.
        if (_burstActions.Length > 0)
        {
            fired = EvaluateActions(_burstActions, ctx, reasons, cooldowns, readyFor, fired);
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

    private string? EvaluateActions(
        System.Collections.Immutable.ImmutableArray<AutoKeyActionSpec> actions,
        AutoKeySpecContext ctx,
        Dictionary<string, string> reasons,
        Dictionary<string, int> cooldowns,
        Dictionary<string, int> readyFor,
        string? fired)
    {
        foreach (var action in actions)
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
