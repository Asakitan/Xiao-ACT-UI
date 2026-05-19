using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S110 — extend <see cref="SyncNearEntitiesDecoder"/> to unpack the
/// monster core attrs (HP / MaxHp / Level) from <c>Entity.Attrs</c> on
/// each appearance and surface them on <see cref="EntityAppearance"/>.
/// Mirrors the HP/MAX_HP/LEVEL slice of Python's
/// <c>_process_monster_attr_collection</c> at
/// <c>packet_parser.py:4259</c>; max-hp estimation, template-id cache,
/// name + breaking-stage land in the follow-up MonsterData session.
/// Bridge stores the attrs on <see cref="EntityTableEntry"/> so the
/// near-entity table can feed monster/boss HP downstream.
/// </summary>
public class Session110EntityAttrTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    // ── Helpers ──

    /// <summary>Build a varint payload for an int (positive only — sufficient
    /// for HP/MaxHp/Level which Python only reads as non-negative).</summary>
    private static ByteString Varint(int value)
    {
        var buf = new List<byte>(5);
        var v = (uint)value;
        while (v >= 0x80)
        {
            buf.Add((byte)(v | 0x80));
            v >>= 7;
        }
        buf.Add((byte)v);
        return ByteString.CopyFrom(buf.ToArray());
    }

    private static Attr A(int id, int value) => new() { Id = id, RawData = Varint(value) };

    // ── Decoder ──

    [Fact]
    public void Decoder_UnpacksHpMaxHpLevel()
    {
        var msg = new SyncNearEntities();
        var ent = new Entity
        {
            Uuid = 4242,
            EntType = EEntityType.Entmonster,
            Attrs = new AttrCollection(),
        };
        ent.Attrs.Attrs.Add(A(11310, 5_000));   // HP
        ent.Attrs.Attrs.Add(A(11320, 8_000));   // MAX_HP
        ent.Attrs.Attrs.Add(A(10000, 42));      // LEVEL
        msg.Appear.Add(ent);

        NearEntitiesEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearEntities, msg.ToByteArray(), 1.0,
            e => { if (e is NearEntitiesEvent n) captured = n; });

        Assert.NotNull(captured);
        var app = Assert.Single(captured!.Appear);
        Assert.Equal(4242, app.Uuid);
        Assert.Equal(5_000, app.CurHp);
        Assert.Equal(8_000, app.MaxHp);
        Assert.Equal(42, app.Level);
    }

    [Fact]
    public void Decoder_NoAttrs_LeavesZeroes()
    {
        var msg = new SyncNearEntities();
        msg.Appear.Add(new Entity { Uuid = 1, EntType = EEntityType.Entchar });

        NearEntitiesEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearEntities, msg.ToByteArray(), 1.0,
            e => { if (e is NearEntitiesEvent n) captured = n; });

        var app = Assert.Single(captured!.Appear);
        Assert.Equal(0, app.CurHp);
        Assert.Equal(0, app.MaxHp);
        Assert.Equal(0, app.Level);
    }

    [Fact]
    public void Decoder_EmptyAttrCollection_LeavesZeroes()
    {
        var msg = new SyncNearEntities();
        msg.Appear.Add(new Entity
        {
            Uuid = 1,
            EntType = EEntityType.Entchar,
            Attrs = new AttrCollection(),
        });

        NearEntitiesEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearEntities, msg.ToByteArray(), 1.0,
            e => { if (e is NearEntitiesEvent n) captured = n; });

        var app = Assert.Single(captured!.Appear);
        Assert.Equal(0, app.CurHp);
    }

    [Fact]
    public void Decoder_UnknownAttrIds_Ignored()
    {
        var msg = new SyncNearEntities();
        var ent = new Entity
        {
            Uuid = 1,
            EntType = EEntityType.Entmonster,
            Attrs = new AttrCollection(),
        };
        ent.Attrs.Attrs.Add(A(99999, 12345));   // unknown id — ignored
        ent.Attrs.Attrs.Add(A(11310, 7));       // HP — kept
        msg.Appear.Add(ent);

        NearEntitiesEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearEntities, msg.ToByteArray(), 1.0,
            e => { if (e is NearEntitiesEvent n) captured = n; });

        var app = Assert.Single(captured!.Appear);
        Assert.Equal(7, app.CurHp);
        Assert.Equal(0, app.MaxHp);
    }

    [Fact]
    public void Decoder_ZeroIdEmptyRaw_Skipped()
    {
        // Mirrors Python `if not raw_data or not attr_id: continue`.
        var msg = new SyncNearEntities();
        var ent = new Entity
        {
            Uuid = 1,
            EntType = EEntityType.Entmonster,
            Attrs = new AttrCollection(),
        };
        ent.Attrs.Attrs.Add(new Attr { Id = 0, RawData = Varint(123) });
        ent.Attrs.Attrs.Add(new Attr { Id = 11310, RawData = ByteString.Empty });
        msg.Appear.Add(ent);

        NearEntitiesEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearEntities, msg.ToByteArray(), 1.0,
            e => { if (e is NearEntitiesEvent n) captured = n; });

        var app = Assert.Single(captured!.Appear);
        Assert.Equal(0, app.CurHp);
    }

    // ── Bridge ──

    [Fact]
    public void Bridge_StoresAttrsOnEntityTableEntry()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var ev = new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(4242, (int)EEntityType.Entmonster)
                {
                    CurHp = 5_000, MaxHp = 8_000, Level = 42,
                },
            },
            Array.Empty<EntityDisappearance>(),
            TimestampSeconds: 1.5);
        bridge.Apply(ev);

        Assert.True(state.Snapshot.NearEntities.TryGetValue(4242, out var row));
        Assert.Equal(5_000, row.CurHp);
        Assert.Equal(8_000, row.MaxHp);
        Assert.Equal(42, row.Level);
        Assert.Equal(1.5, row.FirstSeenSeconds);
    }

    [Fact]
    public void Bridge_FirstSeenWins_AttrsNotOverwritten()
    {
        // Existing first-seen-wins contract must survive S110.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 100, MaxHp = 100, Level = 5,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 50, MaxHp = 200, Level = 6,
                },
            },
            Array.Empty<EntityDisappearance>(), 2.0));

        var row = state.Snapshot.NearEntities[7];
        Assert.Equal(100, row.CurHp);
        Assert.Equal(100, row.MaxHp);
        Assert.Equal(5, row.Level);
        Assert.Equal(1.0, row.FirstSeenSeconds);
    }
}
