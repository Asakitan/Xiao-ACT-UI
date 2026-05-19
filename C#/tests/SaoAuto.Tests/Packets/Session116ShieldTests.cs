using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S116 — extend <c>ExtractMonsterCoreAttrs</c> with SHIELD_LIST
/// (id=60050) carrying a repeated <c>ShieldInfo</c> submessage. Sums
/// (value, max_value) across all shields and mirrors onto
/// <see cref="MonsterData"/>.{ShieldTotal, ShieldMaxTotal,
/// ShieldActive}. Mirrors Python's <c>_decode_shield_list</c> at
/// packet_parser.py 4742 — empty payload clears the shield, decode
/// failure force-clears (same "stale shield_active must not survive a
/// corrupt packet" rule).
/// </summary>
public class Session116ShieldTests
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

    private static Attr A(int id, ByteString raw) => new() { Id = id, RawData = raw };

    /// <summary>
    /// Build a SHIELD_LIST raw payload: outer field 1 (length-delimited)
    /// wraps an inner ShieldInfo submessage with field 3 (value) +
    /// field 5 (max_value), both varints. Matches Python's
    /// <c>shield_list { ShieldInfo { value=...; max_value=... } }</c>.
    /// </summary>
    private static ByteString ShieldListRaw(params (int value, int max)[] shields)
    {
        var outer = new List<byte>();
        foreach (var (value, max) in shields)
        {
            var inner = new List<byte>();
            inner.Add(24);
            WriteVarint(inner, (ulong)value);
            inner.Add(40);
            WriteVarint(inner, (ulong)max);

            outer.Add(10);
            WriteVarint(outer, (ulong)inner.Count);
            outer.AddRange(inner);
        }
        return ByteString.CopyFrom(outer.ToArray());
    }

    private static void WriteVarint(List<byte> buf, ulong v)
    {
        while (v >= 0x80) { buf.Add((byte)(v | 0x80)); v >>= 7; }
        buf.Add((byte)v);
    }

    private static AoiSyncDelta Delta(long uuid, params Attr[] attrs)
    {
        var d = new AoiSyncDelta { Uuid = uuid, Attrs = new AttrCollection() };
        foreach (var a in attrs) d.Attrs.Attrs.Add(a);
        return d;
    }

    [Fact]
    public void Decoder_UnpacksShieldOnDelta()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(Delta(7,
            A(60050, ShieldListRaw((400, 1000), (200, 500)))));

        NearDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 1.0,
            e => { if (e is NearDeltaEvent n) captured = n; });

        Assert.NotNull(captured);
        var u = Assert.Single(captured!.AttrUpdates);
        Assert.True(u.Attrs.HasShield);
        Assert.Equal(600, u.Attrs.ShieldTotal);
        Assert.Equal(1500, u.Attrs.ShieldMaxTotal);
    }

    [Fact]
    public void Decoder_UnpacksShieldOnAppearance()
    {
        var msg = new SyncNearEntities();
        msg.Appear.Add(new Entity
        {
            Uuid = 7,
            EntType = EEntityType.Entmonster,
            Attrs = new AttrCollection
            {
                Attrs =
                {
                    A(11310, Varint(50_000)),
                    A(60050, ShieldListRaw((250, 500))),
                },
            },
        });

        NearEntitiesEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearEntities, msg.ToByteArray(), 1.0,
            e => { if (e is NearEntitiesEvent n) captured = n; });

        Assert.NotNull(captured);
        var app = Assert.Single(captured!.Appear);
        Assert.True(app.Attrs.HasShield);
        Assert.Equal(250, app.Attrs.ShieldTotal);
        Assert.Equal(500, app.Attrs.ShieldMaxTotal);
    }

    [Fact]
    public void Bridge_AppearanceSeedsShield()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var attrs = new MonsterCoreAttrs
        {
            CurHp = 50_000, HasCurHp = true,
            ShieldTotal = 350, ShieldMaxTotal = 700, HasShield = true,
        };
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 50_000,
                    Attrs = attrs,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(350, mon.ShieldTotal);
        Assert.Equal(700, mon.ShieldMaxTotal);
        Assert.True(mon.ShieldActive);
    }

    [Fact]
    public void Bridge_DeltaWritesShieldAndFlipsActive()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, (int)EEntityType.Entmonster) { CurHp = 1 } },
            Array.Empty<EntityDisappearance>(), 1.0));
        Assert.False(state.Snapshot.MonsterDataMap[7].ShieldActive);

        var attrs = new MonsterCoreAttrs
        {
            ShieldTotal = 800, ShieldMaxTotal = 1000, HasShield = true,
        };
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, 0, 0, 0) { Attrs = attrs } },
        });

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(800, mon.ShieldTotal);
        Assert.Equal(1000, mon.ShieldMaxTotal);
        Assert.True(mon.ShieldActive);
    }

    [Fact]
    public void Bridge_DeltaShieldZeroEndsActive()
    {
        // HasShield=true with ShieldTotal=0 must clear ShieldActive
        // (server signalling "shield broken" — Python: shield_active = total > 0).
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var seed = new MonsterCoreAttrs
        {
            ShieldTotal = 500, ShieldMaxTotal = 500, HasShield = true,
        };
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 1,
                    Attrs = seed,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));
        Assert.True(state.Snapshot.MonsterDataMap[7].ShieldActive);

        var ended = new MonsterCoreAttrs
        {
            ShieldTotal = 0, ShieldMaxTotal = 0, HasShield = true,
        };
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, 0, 0, 0) { Attrs = ended } },
        });

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(0, mon.ShieldTotal);
        Assert.False(mon.ShieldActive);
    }

    [Fact]
    public void Bridge_DeltaWithoutShieldAttrCarriesForward()
    {
        // Plain HP-only delta must NOT blank the shield slot.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var seed = new MonsterCoreAttrs
        {
            ShieldTotal = 600, ShieldMaxTotal = 800, HasShield = true,
        };
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 100,
                    Attrs = seed,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[]
            {
                new NearDeltaAttrUpdate(7, 50, 0, 0) { HasCurHp = true },
            },
        });

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(50, mon.Hp);
        Assert.Equal(600, mon.ShieldTotal);
        Assert.Equal(800, mon.ShieldMaxTotal);
        Assert.True(mon.ShieldActive);
    }

    [Fact]
    public void Decoder_NoShieldFieldEntries_ClearsShield()
    {
        // Non-empty payload (raw has bytes so the extractor doesn't skip)
        // but no field-1 ShieldInfo entries means "no shields right now".
        var stub = new List<byte> { 16, 1 };
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(Delta(7, A(60050, ByteString.CopyFrom(stub.ToArray()))));

        NearDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 1.0,
            e => { if (e is NearDeltaEvent n) captured = n; });

        Assert.NotNull(captured);
        var u = Assert.Single(captured!.AttrUpdates);
        Assert.True(u.Attrs.HasShield);
        Assert.Equal(0, u.Attrs.ShieldTotal);
        Assert.Equal(0, u.Attrs.ShieldMaxTotal);
    }
}
