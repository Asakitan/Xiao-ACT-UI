using System.Collections.Immutable;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S140 — per-action cooldown remaining-ms surface. Pins
/// <see cref="AutoKeyCooldownGate.RemainingMs"/> and the runtime-level
/// snapshot <see cref="AutoKeySpecRuntime.LastCooldownRemainingMs"/>
/// that the HUD will consume next to the "cooldown" block reason.
/// </summary>
public class Session140CooldownRemainingTests
{
    private sealed class Recorder : IKeyDispatcher
    {
        public void Dispatch(KeyStroke s) { }
    }

    private static DateTimeOffset T(int ms = 0)
        => new DateTimeOffset(2026, 5, 20, 12, 0, 0, TimeSpan.Zero).AddMilliseconds(ms);

    private static SlotReadiness Ready() => new("ready", false, 0, 0, 0.0);

    private static AutoKeyActionSpec Act(string id, int slot = 1, int minRearm = 1000)
        => new(
            Id: id, Label: id, Enabled: true, SlotIndex: slot,
            Key: "1", PressMode: "tap", PressCount: 1, PressIntervalMs: 0,
            HoldMs: 0, ReadyDelayMs: 0, MinRearmMs: minRearm, PostDelayMs: 0,
            Conditions: ImmutableArray<AutoKeyCondition>.Empty);

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
    public void GateRemainingIsZeroBeforeFirstFire()
    {
        var g = new AutoKeyCooldownGate();
        Assert.Equal(0, g.RemainingMs("a", 1000, T(0)));
    }

    [Fact]
    public void GateRemainingIsZeroWhenCooldownNonPositive()
    {
        var g = new AutoKeyCooldownGate();
        g.TryFire("a", 0, T(0));
        Assert.Equal(0, g.RemainingMs("a", 0, T(0)));
    }

    [Fact]
    public void GateRemainingCountsDownAfterFire()
    {
        var g = new AutoKeyCooldownGate();
        Assert.True(g.TryFire("a", 1000, T(0)));
        Assert.Equal(1000, g.RemainingMs("a", 1000, T(0)));
        Assert.Equal(700, g.RemainingMs("a", 1000, T(300)));
        Assert.Equal(0, g.RemainingMs("a", 1000, T(1000)));
        Assert.Equal(0, g.RemainingMs("a", 1000, T(1500)));
    }

    [Fact]
    public void RuntimeStampsCooldownRemainingForBlockedAction()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        var prof = Profile(Act("a", minRearm: 2_000));
        rt.Tick(prof, Ctx(Ready(), now: T(0)));     // fires
        rt.Tick(prof, Ctx(Ready(), now: T(500)));   // cooldown blocked
        Assert.Equal("cooldown", rt.LastBlockReasons["a"]);
        Assert.True(rt.LastCooldownRemainingMs.TryGetValue("a", out var remain));
        Assert.InRange(remain, 1400, 1500);
    }

    [Fact]
    public void RuntimeOmitsCooldownRemainingWhenNoneCooling()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        rt.Tick(Profile(Act("a", minRearm: 0)), Ctx(Ready()));
        Assert.False(rt.LastCooldownRemainingMs.ContainsKey("a"));
    }

    [Fact]
    public void InvalidateClearsCooldownRemaining()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        var prof = Profile(Act("a", minRearm: 2_000));
        rt.Tick(prof, Ctx(Ready(), now: T(0)));
        rt.Tick(prof, Ctx(Ready(), now: T(100)));
        Assert.NotEmpty(rt.LastCooldownRemainingMs);
        rt.InvalidateProfileState();
        Assert.Empty(rt.LastCooldownRemainingMs);
    }
}
