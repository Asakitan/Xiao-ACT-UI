using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// S108 — bridge wiring for <see cref="ToMeFightResCdEvent"/> →
/// <see cref="GameState.FightResCdMap"/>. Mirrors S69's
/// <c>SkillCdMap</c> pattern but for resource cooldowns: skip
/// <c>ResId &lt;= 0</c>, drop expired rows where
/// <c>begin + duration &lt; server_now_ms</c>, no carry-forward
/// (FightResCD has no "0 means unchanged" optimisation).
/// Self-uuid filter (S70 parity): drop when local SelfUuid is
/// known and the packet uuid mismatches; pass-through when either
/// side is 0 (cold-start / legacy).
/// </summary>
public class Session108FightResCdBridgeTests
{
    private static (GameStateManager State, PacketBridge Bridge) NewRig(ulong selfUuid = 0)
    {
        var state = new GameStateManager();
        if (selfUuid != 0)
            state.Update(s => s with { SelfUuid = selfUuid });
        var bridge = new PacketBridge(state, new PacketParser());
        return (state, bridge);
    }

    private static FightResCdSnapshot Cd(int resId, long begin, int dur, int vcd = 0)
        => new(resId, begin, dur, vcd);

    private static ToMeFightResCdEvent Make(IEnumerable<FightResCdSnapshot> cds,
        double ts = 1.0, long uuid = 1L)
        => new(uuid, cds.ToList(), ts);

    [Fact]
    public void EmptyList_DoesNotMutate()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(Array.Empty<FightResCdSnapshot>()));
        Assert.Empty(state.Snapshot.FightResCdMap);
    }

    [Fact]
    public void OneRow_AddsEntryToMap()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(new[] { Cd(50, begin: 1_000_000, dur: 5_000, vcd: 1_200) },
            ts: 1000.0));
        var snap = state.Snapshot;
        Assert.True(snap.FightResCdMap.TryGetValue(50, out var row));
        Assert.Equal(50, row!.ResId);
        Assert.Equal(1_000_000L, row.BeginMs);
        Assert.Equal(5_000, row.DurationMs);
        Assert.Equal(1_200, row.ValidCdTimeMs);
    }

    [Fact]
    public void ResIdNonPositive_Skipped()
    {
        // The decoder already filters this, but the mutator runs the
        // same guard for defence-in-depth (other event sources could
        // build the event directly).
        var (state, bridge) = NewRig();
        bridge.Apply(Make(new[] { Cd(0, begin: 1_000_000, dur: 5_000) }));
        Assert.Empty(state.Snapshot.FightResCdMap);
        bridge.Apply(Make(new[] { Cd(-3, begin: 1_000_000, dur: 5_000) }));
        Assert.Empty(state.Snapshot.FightResCdMap);
    }

    [Fact]
    public void ExpiredRow_RemovedFromMap()
    {
        var (state, bridge) = NewRig();
        // Seed at ts=0 with a long-running cooldown.
        bridge.Apply(Make(new[] { Cd(50, begin: 0, dur: 10_000) }, ts: 5.0));
        Assert.True(state.Snapshot.FightResCdMap.ContainsKey(50));

        // Same id arrives later with begin+dur < server_now_ms (expired).
        bridge.Apply(Make(new[] { Cd(50, begin: 1_000, dur: 100) }, ts: 100.0));
        Assert.False(state.Snapshot.FightResCdMap.ContainsKey(50));
    }

    [Fact]
    public void MultipleRows_AllStored()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(new[]
        {
            Cd(10, begin: 1_000_000, dur: 3_000),
            Cd(20, begin: 1_000_500, dur: 4_000),
            Cd(30, begin: 1_002_000, dur: 5_000),
        }, ts: 1000.0));
        Assert.Equal(3, state.Snapshot.FightResCdMap.Count);
    }

    [Fact]
    public void SelfUuidMismatch_Drops()
    {
        var (state, bridge) = NewRig(selfUuid: 99UL);
        bridge.Apply(Make(new[] { Cd(50, 1_000_000, 5_000) }, uuid: 12345L));
        Assert.Empty(state.Snapshot.FightResCdMap);
    }

    [Fact]
    public void SelfUuidMatch_Applies()
    {
        var (state, bridge) = NewRig(selfUuid: 12345UL);
        bridge.Apply(Make(new[] { Cd(50, 1_000_000, 5_000) }, uuid: 12345L));
        Assert.True(state.Snapshot.FightResCdMap.ContainsKey(50));
    }

    [Fact]
    public void SelfUuidUnknown_PassesThrough()
    {
        // Cold-start: SelfUuid==0 means we trust whatever the packet says.
        var (state, bridge) = NewRig();
        bridge.Apply(Make(new[] { Cd(50, 1_000_000, 5_000) }, uuid: 12345L));
        Assert.True(state.Snapshot.FightResCdMap.ContainsKey(50));
    }
}
