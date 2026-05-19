using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// S68 — bridge wiring for NearEntitiesEvent. Verifies appearances add
/// rows to GameState.NearEntities (with FirstSeen preserved on dedup)
/// and disappearances remove them.
/// </summary>
public class Session68BridgeTests
{
    private static (GameStateManager State, PacketBridge Bridge) NewRig()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        return (state, bridge);
    }

    private static NearEntitiesEvent Make(
        IEnumerable<(long Uuid, int Type)>? appear = null,
        IEnumerable<(long Uuid, int Reason)>? disappear = null,
        double ts = 0.0)
    {
        var apps = (appear ?? Array.Empty<(long, int)>())
            .Select(a => new EntityAppearance(a.Uuid, a.Type)).ToList();
        var dis = (disappear ?? Array.Empty<(long, int)>())
            .Select(d => new EntityDisappearance(d.Uuid, d.Reason)).ToList();
        return new NearEntitiesEvent(apps, dis, ts);
    }

    [Fact]
    public void Appear_AddsEntryWithTimestamp()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(appear: new[] { (123L, 7) }, ts: 42.5));
        var snap = state.Snapshot;
        Assert.True(snap.NearEntities.ContainsKey(123L));
        Assert.Equal(7, snap.NearEntities[123L].EntityType);
        Assert.Equal(42.5, snap.NearEntities[123L].FirstSeenSeconds, 6);
    }

    [Fact]
    public void Disappear_RemovesEntry()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(appear: new[] { (123L, 7) }, ts: 1.0));
        bridge.Apply(Make(disappear: new[] { (123L, 0) }, ts: 2.0));
        Assert.False(state.Snapshot.NearEntities.ContainsKey(123L));
    }

    [Fact]
    public void Appear_OnExistingPreservesFirstSeen()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(appear: new[] { (123L, 7) }, ts: 1.0));
        bridge.Apply(Make(appear: new[] { (123L, 7) }, ts: 9.0));
        Assert.Equal(1.0, state.Snapshot.NearEntities[123L].FirstSeenSeconds, 6);
    }

    [Fact]
    public void AppearAndDisappear_InSameEvent_BothApply()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(Make(appear: new[] { (1L, 7), (2L, 8) }, ts: 1.0));
        bridge.Apply(Make(
            appear: new[] { (3L, 9) },
            disappear: new[] { (1L, 0) },
            ts: 2.0));
        var snap = state.Snapshot;
        Assert.False(snap.NearEntities.ContainsKey(1L));
        Assert.True(snap.NearEntities.ContainsKey(2L));
        Assert.True(snap.NearEntities.ContainsKey(3L));
        Assert.Equal(2, snap.NearEntities.Count(kv => kv.Key == 2L || kv.Key == 3L));
    }

    [Fact]
    public void EmptyEvent_DoesNotBumpEventsApplied()
    {
        var (state, bridge) = NewRig();
        var before = bridge.EventsApplied;
        bridge.Apply(Make());
        Assert.Equal(before, bridge.EventsApplied);
        Assert.Empty(state.Snapshot.NearEntities);
    }

    [Fact]
    public void Disappear_OfUnknownUuid_IsNoOp()
    {
        var (state, bridge) = NewRig();
        var before = bridge.EventsApplied;
        bridge.Apply(Make(disappear: new[] { (999L, 0) }, ts: 1.0));
        Assert.Equal(before, bridge.EventsApplied);
        Assert.Empty(state.Snapshot.NearEntities);
    }
}
