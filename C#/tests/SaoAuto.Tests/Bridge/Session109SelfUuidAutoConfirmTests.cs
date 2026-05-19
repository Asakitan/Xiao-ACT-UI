using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// S109 — bridge auto-confirms <see cref="GameState.SelfUuid"/> from a
/// player-marker uuid carried by ToMeDelta / ToMeFightResCd packets when
/// no SelfUuid has been set yet (cold-start before EnterGame). Mirrors
/// Python's <c>_confirm_self_uid</c> at packet_parser.py 3888.
///
/// Rules:
///   - Cold-start (SelfUuid == 0) + uuid passes IsPlayerUuid → latch.
///   - Already-set SelfUuid → never overwritten by this path.
///   - Packet uuid == 0 → no-op.
///   - Packet uuid fails IsPlayerUuid (e.g. monster low-marker) → no-op.
///   - Sibling FightResCd path runs the same hook in lockstep.
/// </summary>
public class Session109SelfUuidAutoConfirmTests
{
    // Player low-marker = 640 (CyCombat.PlayerUuidMarker).
    private const long PlayerUuid = (123L << 16) | 640;
    private const long PlayerUuid2 = (456L << 16) | 640;
    private const long MonsterUuid = (123L << 16) | 64;

    private static (GameStateManager State, PacketBridge Bridge) NewRig(ulong selfUuid = 0)
    {
        var state = new GameStateManager();
        if (selfUuid != 0)
            state.Update(s => s with { SelfUuid = selfUuid });
        var bridge = new PacketBridge(state, new PacketParser());
        return (state, bridge);
    }

    private static SkillCdSnapshot SkCd(int id, long begin = 1_000_000, int dur = 5_000)
        => new(id, begin, dur, 0, 0, 0, 0, 0);

    private static FightResCdSnapshot FrCd(int id, long begin = 1_000_000, int dur = 5_000)
        => new(id, begin, dur, 0);

    [Fact]
    public void ColdStart_PlayerMarkerUuid_LatchesSelfUuid()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new ToMeDeltaEvent(PlayerUuid, 0,
            new[] { SkCd(50) }.ToList(), TimestampSeconds: 1000.0));
        Assert.Equal((ulong)PlayerUuid, state.Snapshot.SelfUuid);
    }

    [Fact]
    public void AlreadySet_NotOverwritten()
    {
        var (state, bridge) = NewRig(selfUuid: (ulong)PlayerUuid);
        bridge.Apply(new ToMeDeltaEvent(PlayerUuid2, 0,
            new[] { SkCd(50) }.ToList(), TimestampSeconds: 1000.0));
        // S70 filter drops the packet entirely; SelfUuid stays as-is.
        Assert.Equal((ulong)PlayerUuid, state.Snapshot.SelfUuid);
    }

    [Fact]
    public void PacketUuidZero_NoOp()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new ToMeDeltaEvent(0L, 0,
            new[] { SkCd(50) }.ToList(), TimestampSeconds: 1000.0));
        Assert.Equal(0UL, state.Snapshot.SelfUuid);
    }

    [Fact]
    public void NonPlayerMarkerUuid_NoOp()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new ToMeDeltaEvent(MonsterUuid, 0,
            new[] { SkCd(50) }.ToList(), TimestampSeconds: 1000.0));
        Assert.Equal(0UL, state.Snapshot.SelfUuid);
    }

    [Fact]
    public void FightResCdSibling_AlsoLatches()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new ToMeFightResCdEvent(PlayerUuid,
            new[] { FrCd(50) }.ToList(), TimestampSeconds: 1000.0));
        Assert.Equal((ulong)PlayerUuid, state.Snapshot.SelfUuid);
    }
}
