using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S115 — extend <c>ExtractMonsterCoreAttrs</c> with break-gauge attrs
/// (BREAKING_STAGE=455, EXTINCTION=441, MAX_EXTINCTION=440,
/// STUNNED=443, MAX_STUNNED=442, IN_OVERDRIVE=444). Refactored to
/// carry the full <see cref="MonsterCoreAttrs"/> struct on
/// <see cref="EntityAppearance.Attrs"/> /
/// <see cref="NearDeltaAttrUpdate.Attrs"/> instead of grafting more
/// init slots onto the events. Bridge mirrors fields onto
/// <see cref="MonsterData"/> with Python's lazy max-estimation:
/// MAX_EXTINCTION skipped when 0; first non-zero EXTINCTION seeds
/// max; subsequent EXTINCTION exceeding max raises max (recovery).
/// Same pair for STUNNED.
/// </summary>
public class Session115BreakGaugeTests
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

    private static AoiSyncDelta Delta(long uuid, params Attr[] attrs)
    {
        var d = new AoiSyncDelta { Uuid = uuid, Attrs = new AttrCollection() };
        foreach (var a in attrs) d.Attrs.Attrs.Add(a);
        return d;
    }

    [Fact]
    public void Decoder_UnpacksBreakGaugeOnDelta()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(Delta(7,
            A(455, Varint(2)),
            A(441, Varint(300)),
            A(440, Varint(500)),
            A(443, Varint(80)),
            A(442, Varint(100)),
            A(444, Varint(1))));

        NearDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 1.0,
            e => { if (e is NearDeltaEvent n) captured = n; });

        Assert.NotNull(captured);
        var u = Assert.Single(captured!.AttrUpdates);
        var a = u.Attrs;
        Assert.Equal(2, a.BreakingStage);
        Assert.True(a.HasBreakingStage);
        Assert.Equal(300, a.Extinction);
        Assert.Equal(500, a.MaxExtinction);
        Assert.Equal(80, a.Stunned);
        Assert.Equal(100, a.MaxStunned);
        Assert.True(a.InOverdrive);
        Assert.True(a.HasInOverdrive);
        Assert.False(a.HasCurHp);
    }

    [Fact]
    public void Decoder_UnpacksBreakGaugeOnAppearance()
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
                    A(440, Varint(1000)),
                    A(441, Varint(750)),
                    A(455, Varint(0)),
                },
            },
        });

        NearEntitiesEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearEntities, msg.ToByteArray(), 1.0,
            e => { if (e is NearEntitiesEvent n) captured = n; });

        Assert.NotNull(captured);
        var app = Assert.Single(captured!.Appear);
        Assert.Equal(50_000, app.CurHp);
        Assert.Equal(0, app.Attrs.BreakingStage);
        Assert.True(app.Attrs.HasBreakingStage);
        Assert.Equal(1000, app.Attrs.MaxExtinction);
        Assert.Equal(750, app.Attrs.Extinction);
    }

    [Fact]
    public void Bridge_AppearanceSeedsBreakGauge()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var attrs = new MonsterCoreAttrs
        {
            CurHp = 50_000, HasCurHp = true,
            BreakingStage = 1, HasBreakingStage = true,
            Extinction = 600, HasExtinction = true,
            MaxExtinction = 1000, HasMaxExtinction = true,
            InOverdrive = true, HasInOverdrive = true,
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
        Assert.Equal(1, mon.BreakingStage);
        Assert.Equal(600, mon.Extinction);
        Assert.Equal(1000, mon.MaxExtinction);
        Assert.True(mon.InOverdrive);
    }

    [Fact]
    public void Bridge_AppearanceWithoutAttrsKeepsLegacyDefaults()
    {
        // Pre-S115 callers (S113 tests) construct EntityAppearance with
        // no Attrs slot — default(MonsterCoreAttrs) has all Has* false.
        // BreakingStage must default to -1 ("not received"), not 0.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, (int)EEntityType.Entmonster) { CurHp = 1 } },
            Array.Empty<EntityDisappearance>(), 1.0));

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(-1, mon.BreakingStage);
        Assert.Equal(0, mon.MaxExtinction);
        Assert.False(mon.InOverdrive);
    }

    [Fact]
    public void Bridge_DeltaSeedsMaxExtinctionFromFirstExtinction()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, (int)EEntityType.Entmonster) { CurHp = 1 } },
            Array.Empty<EntityDisappearance>(), 1.0));
        Assert.Equal(0, state.Snapshot.MonsterDataMap[7].MaxExtinction);

        var attrs = new MonsterCoreAttrs
        {
            Extinction = 750, HasExtinction = true,
        };
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, 0, 0, 0) { Attrs = attrs } },
        });

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(750, mon.Extinction);
        Assert.Equal(750, mon.MaxExtinction);
    }

    [Fact]
    public void Bridge_DeltaRaisesMaxExtinctionOnRecovery()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var seed = new MonsterCoreAttrs
        {
            MaxExtinction = 500, HasMaxExtinction = true,
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
        Assert.Equal(500, state.Snapshot.MonsterDataMap[7].MaxExtinction);

        var attrs = new MonsterCoreAttrs
        {
            Extinction = 800, HasExtinction = true,
        };
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, 0, 0, 0) { Attrs = attrs } },
        });

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(800, mon.Extinction);
        Assert.Equal(800, mon.MaxExtinction);
    }

    [Fact]
    public void Bridge_DeltaInOverdriveFalseEndsRage()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var seed = new MonsterCoreAttrs
        {
            InOverdrive = true, HasInOverdrive = true,
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
        Assert.True(state.Snapshot.MonsterDataMap[7].InOverdrive);

        var ended = new MonsterCoreAttrs
        {
            InOverdrive = false, HasInOverdrive = true,
        };
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, 0, 0, 0) { Attrs = ended } },
        });

        Assert.False(state.Snapshot.MonsterDataMap[7].InOverdrive);
    }

    [Fact]
    public void Bridge_DeltaWithoutBreakGaugeAttrs_CarriesForward()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var seed = new MonsterCoreAttrs
        {
            BreakingStage = 1, HasBreakingStage = true,
            Extinction = 400, HasExtinction = true,
            MaxExtinction = 800, HasMaxExtinction = true,
            Stunned = 50, HasStunned = true,
            MaxStunned = 100, HasMaxStunned = true,
            InOverdrive = true, HasInOverdrive = true,
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
        Assert.Equal(1, mon.BreakingStage);
        Assert.Equal(400, mon.Extinction);
        Assert.Equal(800, mon.MaxExtinction);
        Assert.Equal(50, mon.Stunned);
        Assert.Equal(100, mon.MaxStunned);
        Assert.True(mon.InOverdrive);
    }
}
