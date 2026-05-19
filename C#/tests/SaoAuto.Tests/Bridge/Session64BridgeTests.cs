using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// S64 — bridge wiring for the 8 events that landed in S62/S63 plus the
/// EnterGame self-uid carry. Confirms each event reaches the right slot
/// on <see cref="GameState"/> (or correctly no-ops for marker events).
/// </summary>
public class Session64BridgeTests
{
    private static (GameStateManager State, PacketBridge Bridge) NewRig()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        return (state, bridge);
    }

    [Fact]
    public void EnterGameStoresSelfUuid()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new EnterGameEvent(0xDEAD_BEEFUL, 1.0));
        Assert.Equal(0xDEAD_BEEFUL, state.Snapshot.SelfUuid);
        Assert.True(state.Snapshot.PacketActive);
    }

    [Fact]
    public void SkillUseRecordsLastUseTimestamp()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new SkillUseEvent(0x123L, 50301, 12.5));
        bridge.Apply(new SkillUseEvent(0x123L, 60401, 13.5));
        var map = state.Snapshot.SkillLastUseAt;
        Assert.Equal(12.5, map[50301]);
        Assert.Equal(13.5, map[60401]);
    }

    [Fact]
    public void SkillUseOverwritesExistingTimestamp()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new SkillUseEvent(0L, 1, 1.0));
        bridge.Apply(new SkillUseEvent(0L, 1, 2.0));
        Assert.Equal(2.0, state.Snapshot.SkillLastUseAt[1]);
        Assert.Single(state.Snapshot.SkillLastUseAt);
    }

    [Fact]
    public void DungeonDataStoresSceneAndTargets()
    {
        var (state, bridge) = NewRig();
        var targets = new List<DungeonTargetProgress>
        {
            new(100, 5, 0),
            new(200, 10, 1),
        };
        bridge.Apply(new DungeonDataEvent(0x0BAD_F00DL, 3, targets, 0.0));
        var snap = state.Snapshot;
        Assert.Equal(0x0BAD_F00DL, snap.DungeonSceneUuid);
        Assert.Equal(3, snap.DungeonDifficulty);
        Assert.Equal(2, snap.DungeonTargets.Length);
    }

    [Fact]
    public void DungeonDirtyDataUpdatesFlowAndTargets()
    {
        var (state, bridge) = NewRig();
        var targets = new List<DungeonTargetProgress> { new(7, 1, 0) };
        bridge.Apply(new DungeonDirtyDataEvent(2, targets, 0.0));
        Assert.Equal(2, state.Snapshot.DungeonFlowState);
        Assert.Single(state.Snapshot.DungeonTargets);
    }

    [Fact]
    public void DungeonDirtyDataKeepsExistingTargetsWhenEmpty()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new DungeonDataEvent(1L, 1,
            new List<DungeonTargetProgress> { new(1, 1, 0) }, 0.0));
        bridge.Apply(new DungeonDirtyDataEvent(5,
            new List<DungeonTargetProgress>(), 0.0));
        Assert.Equal(5, state.Snapshot.DungeonFlowState);
        Assert.Single(state.Snapshot.DungeonTargets);
    }

    [Fact]
    public void MarkerEventsDoNotMutate()
    {
        var (state, bridge) = NewRig();
        var before = state.Snapshot;
        bridge.Apply(new BuffChangeEvent(1, 2, 0.0));
        bridge.Apply(new SkillEndEvent(99, 0.0));
        bridge.Apply(new SkillStageEndEvent(99, 1, 2, 3, 0.0));
        bridge.Apply(new QteBeginEvent(7, 1, 0.0));
        // EventsApplied stays at 0 because each marker mutator returns false.
        Assert.Equal(0, bridge.EventsApplied);
        Assert.Equal(before.SelfUuid, state.Snapshot.SelfUuid);
    }
}
