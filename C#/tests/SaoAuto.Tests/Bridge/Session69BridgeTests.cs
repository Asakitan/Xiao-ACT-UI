using System.Collections.Immutable;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// S69 — bridge wiring for ToMeDeltaEvent.SkillCds → GameState.SkillCdMap.
/// Mirrors Python `_on_sync_to_me_delta`: skip skill_id<=0, drop expired
/// (begin+dur < server_now_ms), carry forward charge/sub/accel.
/// </summary>
public class Session69BridgeTests
{
    private static (GameStateManager State, PacketBridge Bridge) NewRig()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        return (state, bridge);
    }

    private static SkillCdSnapshot Cd(int id, long begin, int dur, int vcd = 0,
        int charge = 0, int subRatio = 0, long subFixed = 0, int accel = 0)
        => new(id, begin, dur, vcd, charge, subRatio, subFixed, accel);

    private static ToMeDeltaEvent Make(IEnumerable<SkillCdSnapshot> cds, double ts = 1.0,
        int hate = 0, long uuid = 1L)
        => new(uuid, hate, cds.ToList(), ts);

    [Fact]
    public void SkillCd_AddsEntryToMap()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(new[] { Cd(101, begin: 1_000_000, dur: 5_000, vcd: 1_200) }, ts: 1000.0));
        var snap = state.Snapshot;
        Assert.True(snap.SkillCdMap.ContainsKey(101));
        var e = snap.SkillCdMap[101];
        Assert.Equal(1_000_000L, e.BeginMs);
        Assert.Equal(5_000, e.DurationMs);
        Assert.Equal(1_200, e.ValidCdTimeMs);
    }

    [Fact]
    public void SkillCd_SkipsZeroOrNegativeId()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(new[] { Cd(0, 1_000_000, 5_000), Cd(-3, 1_000_000, 5_000) }, ts: 1000.0));
        Assert.Empty(state.Snapshot.SkillCdMap);
    }

    [Fact]
    public void SkillCd_ExpiredEntryRemovesPrior()
    {
        var (state, bridge) = NewRig();
        // ts=1000 → server_now_ms ~= 1_000_000. CD at 500_000+100 = 500_100 < 1_000_000 → expired.
        bridge.Apply(Make(new[] { Cd(101, 999_000, 5_000) }, ts: 1000.0)); // alive
        Assert.True(state.Snapshot.SkillCdMap.ContainsKey(101));
        bridge.Apply(Make(new[] { Cd(101, 500_000, 100) }, ts: 1000.0));    // expired
        Assert.False(state.Snapshot.SkillCdMap.ContainsKey(101));
    }

    [Fact]
    public void SkillCd_CarriesForwardChargeAndSubAccelWhenZero()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(new[] { Cd(101, 1_000_000, 5_000, charge: 3, subRatio: 50, subFixed: 200, accel: 25) }, ts: 1000.0));
        // Next packet leaves charge/sub/accel at 0 — should retain prior values.
        bridge.Apply(Make(new[] { Cd(101, 1_000_500, 5_000, vcd: 800) }, ts: 1000.5));
        var e = state.Snapshot.SkillCdMap[101];
        Assert.Equal(800, e.ValidCdTimeMs);
        Assert.Equal(3, e.ChargeCount);
        Assert.Equal(50, e.SubCdRatio);
        Assert.Equal(200, e.SubCdFixed);
        Assert.Equal(25, e.AccelerateCdRatio);
    }

    [Fact]
    public void SkillCd_NewNonZeroOverridesCarryForward()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(new[] { Cd(101, 1_000_000, 5_000, charge: 3) }, ts: 1000.0));
        bridge.Apply(Make(new[] { Cd(101, 1_000_500, 5_000, charge: 1) }, ts: 1000.5));
        Assert.Equal(1, state.Snapshot.SkillCdMap[101].ChargeCount);
    }

    [Fact]
    public void EmptyCdList_DoesNotBumpEventsApplied()
    {
        var (state, bridge) = NewRig();
        var before = bridge.EventsApplied;
        bridge.Apply(Make(Array.Empty<SkillCdSnapshot>(), ts: 1.0));
        Assert.Equal(before, bridge.EventsApplied);
    }

    [Fact]
    public void HateIdCount_DoesNotMutate_Standalone()
    {
        // Hate list is decoded but not stored — packet with only hate
        // should produce no state delta (no CD entries).
        var (state, bridge) = NewRig();
        var before = bridge.EventsApplied;
        bridge.Apply(Make(Array.Empty<SkillCdSnapshot>(), ts: 1.0, hate: 5));
        Assert.Equal(before, bridge.EventsApplied);
        Assert.Empty(state.Snapshot.SkillCdMap);
    }
}
