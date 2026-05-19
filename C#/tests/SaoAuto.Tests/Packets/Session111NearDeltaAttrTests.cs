using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S111 — wire <c>SyncNearDeltaInfo</c> attrs to the bridge so live
/// HP / MaxHp / Level updates flow into the existing
/// <see cref="GameState.NearEntities"/> rows. Mirrors the
/// <c>_process_monster_attr_collection</c> call from Python's
/// <c>SyncNearDeltaInfo</c> handler at <c>packet_parser.py:4021</c>.
/// Deliberately update-only: a delta for a uuid not in the table is
/// dropped (the table is appearance-scoped; lifecycle stays owned by
/// SyncNearEntities). Zero-valued per-attr slots carry forward the
/// existing row value (= attr not present in this packet).
/// </summary>
public class Session111NearDeltaAttrTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private static ByteString Varint(int value)
    {
        var buf = new List<byte>(5);
        var v = (uint)value;
        while (v >= 0x80) { buf.Add((byte)(v | 0x80)); v >>= 7; }
        buf.Add((byte)v);
        return ByteString.CopyFrom(buf.ToArray());
    }

    private static Attr A(int id, int value) => new() { Id = id, RawData = Varint(value) };

    private static AoiSyncDelta Delta(long uuid, params Attr[] attrs)
    {
        var d = new AoiSyncDelta { Uuid = uuid, Attrs = new AttrCollection() };
        foreach (var a in attrs) d.Attrs.Attrs.Add(a);
        return d;
    }

    // ── Decoder ──

    [Fact]
    public void Decoder_PopulatesAttrUpdates()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(Delta(7, A(11310, 5_000), A(11320, 8_000), A(10000, 12)));
        msg.DeltaInfos.Add(Delta(8)); // no attrs
        msg.DeltaInfos.Add(Delta(9, A(11310, 100))); // partial

        NearDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 1.0,
            e => { if (e is NearDeltaEvent n) captured = n; });

        Assert.NotNull(captured);
        Assert.Equal(3, captured!.DeltaUuids.Count);
        // Only deltas with non-zero attr extraction surface in AttrUpdates.
        Assert.Equal(2, captured.AttrUpdates.Count);
        var a = Assert.Single(captured.AttrUpdates, u => u.Uuid == 7);
        Assert.Equal(5_000, a.CurHp);
        Assert.Equal(8_000, a.MaxHp);
        Assert.Equal(12, a.Level);
        var b = Assert.Single(captured.AttrUpdates, u => u.Uuid == 9);
        Assert.Equal(100, b.CurHp);
        Assert.Equal(0, b.MaxHp);
    }

    [Fact]
    public void Decoder_SkipsZeroUuid()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(Delta(0, A(11310, 100)));

        NearDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 1.0,
            e => { if (e is NearDeltaEvent n) captured = n; });

        Assert.NotNull(captured);
        Assert.Empty(captured!.AttrUpdates);
    }

    // ── Bridge ──

    [Fact]
    public void Bridge_DeltaUpdatesExistingRow()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        // Seed via appearance.
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 5_000, MaxHp = 8_000, Level = 12,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        // Delta drops HP — must override the appearance-time value
        // (relaxes first-seen-wins for the delta path).
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, CurHp: 3_500, MaxHp: 0, Level: 0) },
        });

        var row = state.Snapshot.NearEntities[7];
        Assert.Equal(3_500, row.CurHp);
        // Zero-slots carry forward.
        Assert.Equal(8_000, row.MaxHp);
        Assert.Equal(12, row.Level);
    }

    [Fact]
    public void Bridge_DeltaForUnknownUuid_NoOp()
    {
        // Update-only: delta for a uuid not in the table is dropped.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearDeltaEvent(new[] { 999L }, 1.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(999, 100, 200, 5) },
        });
        Assert.Empty(state.Snapshot.NearEntities);
    }

    [Fact]
    public void Bridge_EmptyAttrUpdates_NoOp()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        // Seed.
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, (int)EEntityType.Entmonster) { CurHp = 100 } },
            Array.Empty<EntityDisappearance>(), 1.0));
        // Delta with only uuids, no attrs.
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0));
        Assert.Equal(100, state.Snapshot.NearEntities[7].CurHp);
    }

    [Fact]
    public void Bridge_AttrUpdateNoChange_DropsWithoutMutation()
    {
        // When the delta repeats the existing row's values verbatim,
        // ReferenceEquals on the dict short-circuits the snapshot swap.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 100, MaxHp = 200, Level = 5,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        var beforeSnap = state.Snapshot;
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, 100, 200, 5) },
        });
        var afterSnap = state.Snapshot;
        // Snapshot may swap (other extraMutate paths could), but the
        // entity table reference is unchanged because no row mutated.
        Assert.Same(beforeSnap.NearEntities, afterSnap.NearEntities);
    }

    [Fact]
    public void Bridge_MultipleDeltasInOnePacket()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(1, (int)EEntityType.Entmonster) { CurHp = 100, MaxHp = 100 },
                new EntityAppearance(2, (int)EEntityType.Entmonster) { CurHp = 200, MaxHp = 200 },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        bridge.Apply(new NearDeltaEvent(new[] { 1L, 2L }, 2.0)
        {
            AttrUpdates = new[]
            {
                new NearDeltaAttrUpdate(1, 80, 0, 0),
                new NearDeltaAttrUpdate(2, 150, 0, 0),
            },
        });

        Assert.Equal(80, state.Snapshot.NearEntities[1].CurHp);
        Assert.Equal(150, state.Snapshot.NearEntities[2].CurHp);
    }
}
