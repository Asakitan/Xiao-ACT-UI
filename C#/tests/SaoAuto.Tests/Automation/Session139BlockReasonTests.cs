using System.Collections.Immutable;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S139 — pin the per-action diagnostic stamp on
/// <see cref="AutoKeySpecRuntime.LastBlockReasons"/>. Each gate path
/// must stamp the right reason; fired-then-rest must mark later
/// actions as <c>not-evaluated</c> (matches the first-match-wins
/// semantic of Python's `_tick`).
/// </summary>
public class Session139BlockReasonTests
{
    private sealed class Recorder : IKeyDispatcher
    {
        public void Dispatch(KeyStroke s) { }
    }

    private static DateTimeOffset T(int ms = 0)
        => new DateTimeOffset(2026, 5, 20, 12, 0, 0, TimeSpan.Zero).AddMilliseconds(ms);

    private static SlotReadiness Ready() => new("ready", false, 0, 0, 0.0);
    private static SlotReadiness Cooling() => new("cooldown", false, 0, 999, 1.0);

    private static AutoKeyActionSpec Act(string id, int slot = 1, int minRearm = 0, bool enabled = true, ImmutableArray<AutoKeyCondition>? conds = null)
        => new(
            Id: id, Label: id, Enabled: enabled, SlotIndex: slot,
            Key: "1", PressMode: "tap", PressCount: 1, PressIntervalMs: 0,
            HoldMs: 0, ReadyDelayMs: 0, MinRearmMs: minRearm, PostDelayMs: 0,
            Conditions: conds ?? ImmutableArray<AutoKeyCondition>.Empty);

    private static AutoKeyProfileSpecRecord Profile(params AutoKeyActionSpec[] actions)
        => new("p1", 1, "T", "", 0, "", "local", null, "", "",
               AuthorSnapshot.Empty,
               new AutoKeyEngineConfig(50, false, true),
               actions.ToImmutableArray());

    private static AutoKeySpecContext Ctx(SlotReadiness slot, int slotIndex = 1, DateTimeOffset? now = null)
        => AutoKeySpecContext.Empty with
        {
            Slots = ImmutableDictionary<int, SlotReadiness>.Empty.SetItem(slotIndex, slot),
            Now = now ?? T(0),
        };

    [Fact]
    public void DisabledActionStampsDisabled()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        rt.Tick(Profile(Act("a", enabled: false)), Ctx(Ready()));
        Assert.Equal("disabled", rt.LastBlockReasons["a"]);
    }

    [Fact]
    public void NotReadySlotStampsReadiness()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        rt.Tick(Profile(Act("a")), Ctx(Cooling()));
        Assert.Equal("readiness", rt.LastBlockReasons["a"]);
    }

    [Fact]
    public void CooldownGateStampsCooldown()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        var prof = Profile(Act("a", minRearm: 5_000));
        rt.Tick(prof, Ctx(Ready(), now: T(0)));   // fires
        rt.Tick(prof, Ctx(Ready(), now: T(100))); // cooldown blocked
        Assert.Equal("cooldown", rt.LastBlockReasons["a"]);
    }

    [Fact]
    public void ConditionsFailureStampsConditions()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        var conds = ImmutableArray.Create<AutoKeyCondition>(new HpPctLteCondition(0.1));
        rt.Tick(Profile(Act("a", conds: conds)), Ctx(Ready()) with { HpPct = 1.0 });
        Assert.Equal("conditions", rt.LastBlockReasons["a"]);
    }

    [Fact]
    public void FiredActionStampsFiredLaterActionsNotEvaluated()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        rt.Tick(Profile(Act("a"), Act("b")), Ctx(Ready()));
        Assert.Equal("fired", rt.LastBlockReasons["a"]);
        Assert.Equal("not-evaluated", rt.LastBlockReasons["b"]);
    }

    [Fact]
    public void InvalidateClearsReasons()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        rt.Tick(Profile(Act("a")), Ctx(Ready()));
        Assert.NotEmpty(rt.LastBlockReasons);
        rt.InvalidateProfileState();
        Assert.Empty(rt.LastBlockReasons);
    }
}
