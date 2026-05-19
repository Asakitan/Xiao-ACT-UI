using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// S70 — self-uuid routing on ToMeDeltaEvent. Verifies that
/// SyncToMeDeltaInfo packets only mutate SkillCdMap when the
/// packet's uuid matches the local SelfUuid (or either side is 0
/// for legacy / cold-start packets).
/// </summary>
public class Session70BridgeTests
{
    private static (GameStateManager State, PacketBridge Bridge) NewRig(ulong selfUuid = 0)
    {
        var state = new GameStateManager();
        if (selfUuid != 0)
            state.Update(s => s with { SelfUuid = selfUuid });
        var bridge = new PacketBridge(state, new PacketParser());
        return (state, bridge);
    }

    private static SkillCdSnapshot Cd(int id, long begin, int dur)
        => new(id, begin, dur, 0, 0, 0, 0, 0);

    private static ToMeDeltaEvent Make(long uuid, double ts = 1000.0)
        => new(uuid, 0, new[] { Cd(101, 1_000_000, 5_000) }, ts);

    [Fact]
    public void Self_Set_AndUuidMatches_Applies()
    {
        var (state, bridge) = NewRig(selfUuid: 12345UL);
        bridge.Apply(Make(uuid: 12345L));
        Assert.True(state.Snapshot.SkillCdMap.ContainsKey(101));
    }

    [Fact]
    public void Self_Set_AndUuidMismatches_Drops()
    {
        var (state, bridge) = NewRig(selfUuid: 12345UL);
        var before = bridge.EventsApplied;
        bridge.Apply(Make(uuid: 99999L));
        Assert.Empty(state.Snapshot.SkillCdMap);
        Assert.Equal(before, bridge.EventsApplied);
    }

    [Fact]
    public void Self_Zero_AlwaysApplies()
    {
        // Cold start before EnterGame: SelfUuid = 0 → accept any uuid.
        var (state, bridge) = NewRig(selfUuid: 0);
        bridge.Apply(Make(uuid: 99999L));
        Assert.True(state.Snapshot.SkillCdMap.ContainsKey(101));
    }

    [Fact]
    public void EvUuid_Zero_AlwaysApplies()
    {
        // Legacy server packet (no uuid field): always accept.
        var (state, bridge) = NewRig(selfUuid: 12345UL);
        bridge.Apply(Make(uuid: 0L));
        Assert.True(state.Snapshot.SkillCdMap.ContainsKey(101));
    }
}
