using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S122 — wire <c>SyncToMeDeltaInfo.BaseDelta.TempAttrs</c> to a new
/// <see cref="TempAttrCdEvent"/> + <see cref="GameState.TempAttrCdPct"/>
/// / <c>TempAttrCdFixed</c> / <c>TempAttrCdAccel</c> fields. Mirrors
/// Python's <c>_process_temp_attr_collection</c> at
/// <c>packet_parser.py:4831–4869</c>: cumulative sum of TempAttr ids
/// 100 (pct, /10000), 101 (fixed ms), and 103 (accel, /10000) over the
/// collection. Decoder gates on player low-marker uuid; bridge applies
/// the SelfUuid filter (S70) and the cold-start S109 latching.
/// </summary>
public class Session122TempAttrCdTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private const long PlayerUuid = (123L << 16) | 640;
    private const long OtherPlayerUuid = (456L << 16) | 640;
    private const long MonsterUuid = (123L << 16) | 64;

    private static TempAttr T(int id, int value) => new() { Id = id, Value = value };

    private static AoiSyncDelta Base(long uuid, params TempAttr[] tas)
    {
        var d = new AoiSyncDelta { Uuid = uuid, TempAttrs = new TempAttrCollection() };
        foreach (var ta in tas) d.TempAttrs.Attrs.Add(ta);
        return d;
    }

    private static List<ParserEvent> Dispatch(AoiSyncToMeDelta inner, double ts = 1.0)
    {
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        var captured = new List<ParserEvent>();
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), ts,
            captured.Add);
        return captured;
    }

    [Fact]
    public void Decoder_SumsThreeIds_AcrossCollection()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(PlayerUuid,
                T(100, 1500), T(101, 200), T(103, 300),
                T(100, 500), T(103, 100)),
        };
        var ev = Assert.Single(Dispatch(inner).OfType<TempAttrCdEvent>());
        Assert.Equal(PlayerUuid, ev.Uuid);
        Assert.Equal(2000, ev.CdPct);
        Assert.Equal(200, ev.CdFixed);
        Assert.Equal(400, ev.CdAccel);
    }

    [Fact]
    public void Decoder_IgnoresUnrelatedIds()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(PlayerUuid, T(99, 5_000), T(102, 5_000), T(104, 5_000)),
        };
        var ev = Assert.Single(Dispatch(inner).OfType<TempAttrCdEvent>());
        Assert.Equal(0, ev.CdPct);
        Assert.Equal(0, ev.CdFixed);
        Assert.Equal(0, ev.CdAccel);
    }

    [Fact]
    public void Decoder_NonPlayerUuid_NoEvent()
    {
        // Monster low-marker — TempAttrs not for player; skip per
        // Python's `target_is_player` gate.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(MonsterUuid, T(100, 999)),
        };
        Assert.Empty(Dispatch(inner).OfType<TempAttrCdEvent>());
    }

    [Fact]
    public void Decoder_NoTempAttrs_NoEvent()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = new AoiSyncDelta { Uuid = PlayerUuid }, // no TempAttrs
        };
        Assert.Empty(Dispatch(inner).OfType<TempAttrCdEvent>());
    }

    [Fact]
    public void Decoder_EmptyTempAttrs_EmitsZeros()
    {
        // TempAttrCollection present but with no entries → still emit
        // (zero-value), so the bridge can clear stale CDR scalars when
        // all buffs drop off.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = new AoiSyncDelta { Uuid = PlayerUuid, TempAttrs = new TempAttrCollection() },
        };
        var ev = Assert.Single(Dispatch(inner).OfType<TempAttrCdEvent>());
        Assert.Equal(0, ev.CdPct);
        Assert.Equal(0, ev.CdFixed);
        Assert.Equal(0, ev.CdAccel);
    }

    [Fact]
    public void Bridge_ColdStart_LatchesSelfUuid_AndWritesFields()
    {
        var state = new GameStateManager();
        Assert.Equal(0UL, state.Snapshot.SelfUuid);
        var bridge = new PacketBridge(state, new PacketParser());

        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(PlayerUuid, T(100, 750), T(101, 50), T(103, 250)),
        };
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), 1.0,
            ev => bridge.Apply(ev));

        var s = state.Snapshot;
        Assert.Equal((ulong)PlayerUuid, s.SelfUuid);
        Assert.Equal(750, s.TempAttrCdPct);
        Assert.Equal(50, s.TempAttrCdFixed);
        Assert.Equal(250, s.TempAttrCdAccel);
    }

    [Fact]
    public void Bridge_SelfUuidMismatch_DoesNotWrite()
    {
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid });
        var bridge = new PacketBridge(state, new PacketParser());

        // OtherPlayerUuid is a player low-marker but != SelfUuid → drop.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = OtherPlayerUuid,
            BaseDelta = Base(OtherPlayerUuid, T(100, 999), T(101, 999), T(103, 999)),
        };
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), 1.0,
            ev => bridge.Apply(ev));

        var s = state.Snapshot;
        Assert.Equal(0, s.TempAttrCdPct);
        Assert.Equal(0, s.TempAttrCdFixed);
        Assert.Equal(0, s.TempAttrCdAccel);
    }

    [Fact]
    public void Bridge_NoChange_DoesNotIncrementEventsApplied()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid,
            TempAttrCdPct = 100, TempAttrCdFixed = 200, TempAttrCdAccel = 300,
        });
        var bridge = new PacketBridge(state, new PacketParser());

        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(PlayerUuid, T(100, 100), T(101, 200), T(103, 300)),
        };
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        var before = bridge.EventsApplied;
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), 1.0,
            ev => bridge.Apply(ev));
        var after = bridge.EventsApplied;

        // ToMeDeltaEvent (Uuid=PlayerUuid, no skill cds, hateCount=0) is also
        // emitted — it always Updates (sets PacketActive). So `after` will
        // be > before. But TempAttrCdEvent specifically should be a no-op.
        // Re-deliver only the TempAttrCdEvent in isolation to assert this.
        var only = new TempAttrCdEvent(PlayerUuid, 100, 200, 300, 2.0);
        var beforeIsolated = bridge.EventsApplied;
        bridge.Apply(only);
        Assert.Equal(beforeIsolated, bridge.EventsApplied);
    }

    [Fact]
    public void Bridge_ChangedValues_IncrementEventsApplied()
    {
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid });
        var bridge = new PacketBridge(state, new PacketParser());
        var before = bridge.EventsApplied;
        bridge.Apply(new TempAttrCdEvent(PlayerUuid, 1500, 0, 200, 1.0));
        Assert.Equal(before + 1, bridge.EventsApplied);
        Assert.Equal(1500, state.Snapshot.TempAttrCdPct);
        Assert.Equal(0, state.Snapshot.TempAttrCdFixed);
        Assert.Equal(200, state.Snapshot.TempAttrCdAccel);
    }
}
