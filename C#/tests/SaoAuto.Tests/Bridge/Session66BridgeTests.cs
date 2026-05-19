using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// S66 — bridge wiring for ContainerSyncEvent. Verifies the top-level
/// identity + HP slots are written to GameState.
/// </summary>
public class Session66BridgeTests
{
    private static (GameStateManager State, PacketBridge Bridge) NewRig()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        return (state, bridge);
    }

    [Fact]
    public void ContainerSyncStoresIdentityAndHp()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new ContainerSyncEvent(
            CharId: 100L, Name: "Kirito", Level: 60, FightPoint: 22222,
            CurHp: 1500, MaxHp: 5000, Energy: 80f, TimestampSeconds: 3.0));
        var snap = state.Snapshot;
        Assert.Equal("Kirito", snap.PlayerName);
        Assert.Equal(60, snap.LevelBase);
        Assert.Equal(22222, snap.FightPoint);
        Assert.Equal(1500, snap.HpCurrent);
        Assert.Equal(5000, snap.HpMax);
        Assert.Equal(0.3, snap.HpPct, 6);
        Assert.True(snap.PacketActive);
    }

    [Fact]
    public void ContainerSyncEmptyNameKeepsExisting()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new ContainerSyncEvent(1L, "Asuna", 50, 100, 100, 100, 0f, 0.0));
        bridge.Apply(new ContainerSyncEvent(1L, string.Empty, 0, 0, 200, 400, 0f, 0.0));
        var snap = state.Snapshot;
        Assert.Equal("Asuna", snap.PlayerName);
        Assert.Equal(50, snap.LevelBase);
        Assert.Equal(100, snap.FightPoint);
        Assert.Equal(200, snap.HpCurrent);
        Assert.Equal(400, snap.HpMax);
        Assert.Equal(0.5, snap.HpPct, 6);
    }

    [Fact]
    public void ContainerSyncMaxHpZeroKeepsPctOne()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new ContainerSyncEvent(1L, "X", 1, 1, 0, 0, 0f, 0.0));
        Assert.Equal(1.0, state.Snapshot.HpPct);
    }
}
