using System.Collections.Immutable;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S142 — pins the readiness-gate "ready for ms" inspector and the
/// runtime-level <see cref="AutoKeySpecRuntime.LastReadyForMs"/>
/// snapshot. Completes the triad (reasons / cooldown / readiness)
/// the HUD needs alongside <see cref="AutoKeyRuntimeSnapshot"/>.
/// </summary>
public class Session142ReadyForMsTests
{
    private sealed class Recorder : IKeyDispatcher
    {
        public void Dispatch(KeyStroke s) { }
    }

    private static DateTimeOffset T(int ms = 0)
        => new DateTimeOffset(2026, 5, 20, 12, 0, 0, TimeSpan.Zero).AddMilliseconds(ms);

    private static SlotReadiness Ready() => new("ready", false, 0, 0, 0.0);
    private static SlotReadiness Cooling() => new("cooldown", false, 0, 999, 1.0);

    private static AutoKeyActionSpec Act(string id, int readyDelay = 0, int minRearm = 0)
        => new(
            Id: id, Label: id, Enabled: true, SlotIndex: 1,
            Key: "1", PressMode: "tap", PressCount: 1, PressIntervalMs: 0,
            HoldMs: 0, ReadyDelayMs: readyDelay, MinRearmMs: minRearm, PostDelayMs: 0,
            Conditions: ImmutableArray<AutoKeyCondition>.Empty);

    private static AutoKeyProfileSpecRecord Profile(params AutoKeyActionSpec[] actions)
        => new("p1", 1, "T", "", 0, "", "local", null, "", "",
               AuthorSnapshot.Empty,
               new AutoKeyEngineConfig(50, false, true),
               actions.ToImmutableArray());

    private static AutoKeySpecContext Ctx(SlotReadiness slot, DateTimeOffset? now = null)
        => AutoKeySpecContext.Empty with
        {
            Slots = ImmutableDictionary<int, SlotReadiness>.Empty.SetItem(1, slot),
            Now = now ?? T(0),
        };

    [Fact]
    public void GateReadyForIsZeroWhenNotTracked()
    {
        var g = new AutoKeyReadinessGate();
        Assert.Equal(0, g.ReadyForMs("a", T(0)));
    }

    [Fact]
    public void GateReadyForCountsFromLeadingEdge()
    {
        var g = new AutoKeyReadinessGate();
        g.TryFire("a", new SlotReadiness("ready", false, 0, 0, 0.0), readyDelayMs: 5_000, now: T(0));
        Assert.Equal(0, g.ReadyForMs("a", T(0)));
        Assert.Equal(300, g.ReadyForMs("a", T(300)));
        Assert.Equal(1234, g.ReadyForMs("a", T(1234)));
    }

    [Fact]
    public void RuntimeStampsReadyForOnFire()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        rt.Tick(Profile(Act("a")), Ctx(Ready(), now: T(0)));
        Assert.True(rt.LastReadyForMs.ContainsKey("a"));
        Assert.Equal(0, rt.LastReadyForMs["a"]);
    }

    [Fact]
    public void RuntimeOmitsReadyForWhenSlotNotReady()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        rt.Tick(Profile(Act("a")), Ctx(Cooling()));
        Assert.False(rt.LastReadyForMs.ContainsKey("a"));
        Assert.Equal("readiness", rt.LastBlockReasons["a"]);
    }

    [Fact]
    public void SnapshotIncludesReadyForMs()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        rt.Tick(Profile(Act("a")), Ctx(Ready(), now: T(0)));
        var snap = rt.Snapshot();
        Assert.True(snap.ReadyForMs.ContainsKey("a"));
    }

    [Fact]
    public void InvalidateClearsReadyFor()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        rt.Tick(Profile(Act("a")), Ctx(Ready()));
        Assert.NotEmpty(rt.LastReadyForMs);
        rt.InvalidateProfileState();
        Assert.Empty(rt.LastReadyForMs);
    }
}
