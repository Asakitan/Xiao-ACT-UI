using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// S67 — bridge wiring for ContainerDirtyEvent. Verifies each
/// (field_index, sub_field) tuple lands in the right GameState slot
/// and Python's guards (ignore CurHp=0 unless MaxHp=0, ignore MaxHp=0,
/// ignore Level<=0) are mirrored.
/// </summary>
public class Session67BridgeTests
{
    private static (GameStateManager State, PacketBridge Bridge) NewRig()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        return (state, bridge);
    }

    private static ContainerDirtyEvent Make(int field, int sub,
        string? str = null, long? i = null, float? f = null)
        => new(new ContainerDirtyChange(field, sub, str, i, f), 0.0);

    [Fact]
    public void ContainerDirty_NameUpdatesPlayerName()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(2, 5, str: "Lisbeth"));
        Assert.Equal("Lisbeth", state.Snapshot.PlayerName);
    }

    [Fact]
    public void ContainerDirty_EmptyNameDoesNotMutate()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(2, 5, str: "Sinon"));
        bridge.Apply(Make(2, 5, str: string.Empty));
        Assert.Equal("Sinon", state.Snapshot.PlayerName);
    }

    [Fact]
    public void ContainerDirty_FightPointWritesValue()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(2, 35, i: 88888));
        Assert.Equal(88888, state.Snapshot.FightPoint);
    }

    [Fact]
    public void ContainerDirty_CurHpUpdatesHpAndPct()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(16, 2, i: 1000));   // MaxHp first
        bridge.Apply(Make(16, 1, i: 250));    // CurHp
        var snap = state.Snapshot;
        Assert.Equal(250, snap.HpCurrent);
        Assert.Equal(1000, snap.HpMax);
        Assert.Equal(0.25, snap.HpPct, 6);
    }

    [Fact]
    public void ContainerDirty_CurHpZeroIgnoredWhenMaxHpKnown()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(16, 2, i: 500));
        bridge.Apply(Make(16, 1, i: 200));
        bridge.Apply(Make(16, 1, i: 0));      // ignored
        Assert.Equal(200, state.Snapshot.HpCurrent);
    }

    [Fact]
    public void ContainerDirty_MaxHpZeroIgnored()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(16, 2, i: 500));
        bridge.Apply(Make(16, 2, i: 0));      // ignored
        Assert.Equal(500, state.Snapshot.HpMax);
    }

    [Fact]
    public void ContainerDirty_LevelUpdatesLevelBase()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(22, 1, i: 88));
        Assert.Equal(88, state.Snapshot.LevelBase);
    }

    [Fact]
    public void ContainerDirty_LevelZeroIgnored()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(22, 1, i: 30));
        bridge.Apply(Make(22, 1, i: 0));      // ignored
        Assert.Equal(30, state.Snapshot.LevelBase);
    }

    [Fact]
    public void ContainerDirty_OriginEnergyAcksAsNoOp()
    {
        var (state, bridge) = NewRig();
        var before = bridge.EventsApplied;
        bridge.Apply(Make(16, 3, i: 50, f: 50f));
        Assert.Equal(before, bridge.EventsApplied);   // no slot yet
    }
}
