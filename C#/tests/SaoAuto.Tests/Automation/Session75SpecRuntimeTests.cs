using System.Collections.Immutable;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S75 — spec-to-runtime + condition evaluator parity vs
/// auto_key_engine.py (_conditions_match, _normalized_slot_state,
/// _action_ready, _tick).
/// </summary>
public class Session75SpecRuntimeTests
{
    private sealed class Recorder : IKeyDispatcher
    {
        public readonly List<KeyStroke> Strokes = new();
        public void Dispatch(KeyStroke s) => Strokes.Add(s);
    }

    private static DateTimeOffset T(int ms = 0)
        => new DateTimeOffset(2026, 5, 7, 12, 0, 0, TimeSpan.Zero).AddMilliseconds(ms);

    private static SlotReadiness Ready() => new("ready", false, 0, 0, 0.0);

    private static AutoKeySpecContext Ctx(SlotReadiness slot, int slotIndex = 1, DateTimeOffset? now = null)
        => AutoKeySpecContext.Empty with
        {
            Slots = ImmutableDictionary<int, SlotReadiness>.Empty.SetItem(slotIndex, slot),
            Now = now ?? T(0),
        };

    private static AutoKeyActionSpec Action(
        string id = "a1",
        int slotIndex = 1,
        string key = "1",
        int readyDelayMs = 0,
        int minRearmMs = 0,
        bool enabled = true,
        string pressMode = "tap",
        int pressCount = 1,
        int holdMs = 0,
        params AutoKeyCondition[] conditions)
        => new(id, "L", enabled, slotIndex, key, pressMode, pressCount, 30, holdMs,
            readyDelayMs, minRearmMs, 0, conditions.ToImmutableArray());

    private static AutoKeyProfileSpecRecord Profile(params AutoKeyActionSpec[] actions)
        => AutoKeyProfileSpec.MakeDefaultProfile()
            with { Actions = actions.ToImmutableArray() };

    // ── ConditionEvaluator: 8-variant truth table ───────────────────

    [Fact]
    public void Conditions_AllVariants_PassAndFail()
    {
        var ctx = AutoKeySpecContext.Empty with
        {
            HpPct = 0.6, StaminaPct = 0.5, BurstReady = true,
            ProfessionName = "Bard", PlayerName = "Kirito", InCombat = true,
            Slots = ImmutableDictionary<int, SlotReadiness>.Empty
                .SetItem(2, new SlotReadiness("active", false, 0, 0, 0.0)),
        };
        Assert.True(ConditionEvaluator.Matches(new HpPctGteCondition(0.5), ctx));
        Assert.False(ConditionEvaluator.Matches(new HpPctGteCondition(0.9), ctx));
        Assert.True(ConditionEvaluator.Matches(new HpPctLteCondition(0.7), ctx));
        Assert.False(ConditionEvaluator.Matches(new HpPctLteCondition(0.5), ctx));
        Assert.True(ConditionEvaluator.Matches(new StaPctGteCondition(0.4), ctx));
        Assert.True(ConditionEvaluator.Matches(new BurstReadyIsCondition(true), ctx));
        Assert.False(ConditionEvaluator.Matches(new BurstReadyIsCondition(false), ctx));
        Assert.True(ConditionEvaluator.Matches(new SlotStateIsCondition(2, "active"), ctx));
        Assert.False(ConditionEvaluator.Matches(new SlotStateIsCondition(2, "cooldown"), ctx));
        Assert.True(ConditionEvaluator.Matches(new ProfessionIsCondition("Bard"), ctx));
        Assert.True(ConditionEvaluator.Matches(new PlayerNameIsCondition("Kirito"), ctx));
        Assert.True(ConditionEvaluator.Matches(new InCombatIsCondition(true), ctx));
    }

    [Fact]
    public void Conditions_EmptyList_TriviallyTrue()
    {
        Assert.True(ConditionEvaluator.Matches(
            Array.Empty<AutoKeyCondition>(), AutoKeySpecContext.Empty));
    }

    [Fact]
    public void Conditions_AllMustPass_AndSemantics()
    {
        var ctx = AutoKeySpecContext.Empty with { HpPct = 0.6, BurstReady = true };
        var pass = new AutoKeyCondition[] { new HpPctGteCondition(0.5), new BurstReadyIsCondition(true) };
        var fail = new AutoKeyCondition[] { new HpPctGteCondition(0.5), new BurstReadyIsCondition(false) };
        Assert.True(ConditionEvaluator.Matches(pass, ctx));
        Assert.False(ConditionEvaluator.Matches(fail, ctx));
    }

    [Fact]
    public void SlotStateIs_FallsBackToActionSlotWhenIndexZero()
    {
        var ctx = AutoKeySpecContext.Empty with
        {
            Slots = ImmutableDictionary<int, SlotReadiness>.Empty
                .SetItem(3, new SlotReadiness("ready", false, 0, 0, 0.0)),
        };
        var c = new SlotStateIsCondition(0, "ready");
        Assert.True(ConditionEvaluator.Matches(c, ctx, fallbackSlotIndex: 3));
        Assert.False(ConditionEvaluator.Matches(c, ctx, fallbackSlotIndex: 4));
    }

    [Fact]
    public void NormalizedSlotState_TruthTable()
    {
        Assert.Equal("ready", ConditionEvaluator.NormalizedSlotState(new("ready", false, 0, 0, 1.0)));
        Assert.Equal("active", ConditionEvaluator.NormalizedSlotState(new("ACTIVE", false, 0, 0, 1.0)));
        Assert.Equal("cooldown", ConditionEvaluator.NormalizedSlotState(new("cooldown", false, 0, 999, 1.0)));
        Assert.Equal("unknown", ConditionEvaluator.NormalizedSlotState(new("unknown", false, 0, 0, 1.0)));
        Assert.Equal("insufficient_energy", ConditionEvaluator.NormalizedSlotState(new("insufficient_energy", false, 0, 0, 0.0)));
        // unknown raw → classify by IsReady
        Assert.Equal("ready", ConditionEvaluator.NormalizedSlotState(new("foo", true, 0, 999_999, 1.0)));
        Assert.Equal("cooldown", ConditionEvaluator.NormalizedSlotState(SlotReadiness.NotReady));
    }

    // ── AutoKeySpecContext.Slot ─────────────────────────────────────

    [Fact]
    public void Context_SlotMissing_ReturnsNotReady()
    {
        var ctx = AutoKeySpecContext.Empty;
        Assert.Equal(SlotReadiness.NotReady, ctx.Slot(7));
    }

    // ── AutoKeySpecRuntime.Tick ─────────────────────────────────────

    [Fact]
    public void Tick_FirstMatchingActionFires_DispatchesAndIncrementsCount()
    {
        var rec = new Recorder();
        var rt = new AutoKeySpecRuntime(rec);
        var profile = Profile(Action(id: "burst", key: "Q"));
        var ctx = Ctx(Ready());
        var fired = rt.Tick(profile, ctx);
        Assert.Equal("burst", fired);
        Assert.Equal(1, rt.FireCount);
        Assert.Single(rec.Strokes);
        Assert.Equal('Q', rec.Strokes[0].VirtualKey);   // 'Q' = 0x51
    }

    [Fact]
    public void Tick_SkipsDisabled_AndPicksNextEnabled()
    {
        var rec = new Recorder();
        var rt = new AutoKeySpecRuntime(rec);
        var profile = Profile(
            Action(id: "off", key: "1", enabled: false),
            Action(id: "on", slotIndex: 2, key: "2"));
        var ctx = AutoKeySpecContext.Empty with
        {
            Slots = ImmutableDictionary<int, SlotReadiness>.Empty.SetItem(2, Ready()),
            Now = T(0),
        };
        Assert.Equal("on", rt.Tick(profile, ctx));
    }

    [Fact]
    public void Tick_SlotNotReady_NoFire()
    {
        var rec = new Recorder();
        var rt = new AutoKeySpecRuntime(rec);
        var profile = Profile(Action(key: "1"));
        var ctx = Ctx(SlotReadiness.NotReady);
        Assert.Null(rt.Tick(profile, ctx));
        Assert.Empty(rec.Strokes);
        Assert.Equal(0, rt.FireCount);
    }

    [Fact]
    public void Tick_ConditionFails_NoFire()
    {
        var rec = new Recorder();
        var rt = new AutoKeySpecRuntime(rec);
        var profile = Profile(Action(key: "1", conditions: new HpPctLteCondition(0.1)));
        var ctx = Ctx(Ready()) with { HpPct = 0.9 };
        Assert.Null(rt.Tick(profile, ctx));
    }

    [Fact]
    public void Tick_ReadyDelay_HoldsOffUntilElapsed()
    {
        var rec = new Recorder();
        var readiness = new AutoKeyReadinessGate();
        var rt = new AutoKeySpecRuntime(rec, readiness);
        var profile = Profile(Action(key: "1", readyDelayMs: 100));

        Assert.Null(rt.Tick(profile, Ctx(Ready(), now: T(0))));
        Assert.Null(rt.Tick(profile, Ctx(Ready(), now: T(50))));
        Assert.Equal("a1", rt.Tick(profile, Ctx(Ready(), now: T(120))));
    }

    [Fact]
    public void Tick_CooldownGate_BlocksWithinRearmWindow()
    {
        var rec = new Recorder();
        var rt = new AutoKeySpecRuntime(rec);
        var profile = Profile(Action(key: "1", minRearmMs: 500));

        Assert.Equal("a1", rt.Tick(profile, Ctx(Ready(), now: T(0))));
        Assert.Null(rt.Tick(profile, Ctx(Ready(), now: T(200))));
        Assert.Equal("a1", rt.Tick(profile, Ctx(Ready(), now: T(600))));
        Assert.Equal(2, rt.FireCount);
    }

    [Fact]
    public void Tick_TapPressCount_DispatchesNTimes()
    {
        var rec = new Recorder();
        var rt = new AutoKeySpecRuntime(rec);
        var profile = Profile(Action(key: "1", pressMode: "tap", pressCount: 3));
        rt.Tick(profile, Ctx(Ready()));
        Assert.Equal(3, rec.Strokes.Count);
        Assert.All(rec.Strokes, s => Assert.Equal(0, s.HoldMs));
    }

    [Fact]
    public void Tick_HoldMode_DispatchesOnceWithHoldMs()
    {
        var rec = new Recorder();
        var rt = new AutoKeySpecRuntime(rec);
        var profile = Profile(Action(key: "1", pressMode: "hold", pressCount: 5, holdMs: 250));
        rt.Tick(profile, Ctx(Ready()));
        Assert.Single(rec.Strokes);
        Assert.Equal(250, rec.Strokes[0].HoldMs);
    }

    [Fact]
    public void Tick_UnknownKey_NoDispatchButGatesStillRecord()
    {
        var rec = new Recorder();
        var rt = new AutoKeySpecRuntime(rec);
        var profile = Profile(Action(key: "??"));
        var fired = rt.Tick(profile, Ctx(Ready()));
        Assert.Equal("a1", fired);
        Assert.Empty(rec.Strokes);
        Assert.Equal(1, rt.FireCount);
    }

    [Fact]
    public void InvalidateProfileState_ResetsBothGates()
    {
        var rec = new Recorder();
        var readiness = new AutoKeyReadinessGate();
        var cooldown = new AutoKeyCooldownGate();
        var rt = new AutoKeySpecRuntime(rec, readiness, cooldown);
        var profile = Profile(Action(key: "1", readyDelayMs: 100, minRearmMs: 1000));

        rt.Tick(profile, Ctx(Ready(), now: T(0)));
        rt.Tick(profile, Ctx(Ready(), now: T(150)));   // fires
        Assert.Equal(1, rt.FireCount);

        rt.InvalidateProfileState();

        // After reset, readyDelay must be re-accumulated from scratch
        Assert.Null(rt.Tick(profile, Ctx(Ready(), now: T(160))));
        Assert.Equal("a1", rt.Tick(profile, Ctx(Ready(), now: T(300))));
    }

    // ── ResolveVk ───────────────────────────────────────────────────

    [Fact]
    public void ResolveVk_DigitsLettersFunctionKeys()
    {
        Assert.Equal(0x30, AutoKeySpecRuntime.ResolveVk("0"));
        Assert.Equal(0x39, AutoKeySpecRuntime.ResolveVk("9"));
        Assert.Equal('A', AutoKeySpecRuntime.ResolveVk("a"));
        Assert.Equal('Z', AutoKeySpecRuntime.ResolveVk("z"));
        Assert.Equal(0x70, AutoKeySpecRuntime.ResolveVk("F1"));
        Assert.Equal(0x7B, AutoKeySpecRuntime.ResolveVk("F12"));
        Assert.Equal(0, AutoKeySpecRuntime.ResolveVk(""));
        Assert.Equal(0, AutoKeySpecRuntime.ResolveVk("F13"));
        Assert.Equal(0, AutoKeySpecRuntime.ResolveVk("??"));
    }
}
