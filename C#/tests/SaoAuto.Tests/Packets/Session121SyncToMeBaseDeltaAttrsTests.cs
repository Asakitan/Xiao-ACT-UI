using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S121 — wire <c>SyncToMeDeltaInfo</c>'s <c>AoiSyncToMeDelta.BaseDelta</c>
/// path. Mirrors Python's <c>_on_sync_to_me_delta</c> at
/// <c>packet_parser.py:3979–3981</c>:
/// <c>if di.HasField('BaseDelta'): self._process_aoi_sync_delta(di.BaseDelta)</c>.
/// BaseDelta carries the AOI delta for surrounding entities riding the
/// "to me" packet; when the BaseDelta uuid is a non-player (monster low
/// marker mismatch), surface core attrs as a sibling
/// <see cref="NearDeltaEvent"/> so the bridge can mirror HP / break /
/// shield / aggro into an existing <see cref="MonsterData"/> row.
/// Player-uuid BaseDeltas are skipped (they belong to the unported
/// <c>_process_attr_collection</c> path).
/// </summary>
public class Session121SyncToMeBaseDeltaAttrsTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private const long PlayerUuid = (123L << 16) | 640;
    private const long MonsterUuid = (123L << 16) | 64;

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

    private static List<ParserEvent> Dispatch(AoiSyncToMeDelta inner, double ts = 1.0)
    {
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        var captured = new List<ParserEvent>();
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), ts,
            captured.Add);
        return captured;
    }

    [Fact]
    public void BaseDelta_MonsterUuid_WithAttrs_EmitsNearDelta()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Delta(MonsterUuid, A(11310, 4_500), A(11320, 9_000), A(10000, 42)),
        };
        var evs = Dispatch(inner);
        var nd = Assert.Single(evs.OfType<NearDeltaEvent>());
        Assert.Equal(MonsterUuid, Assert.Single(nd.DeltaUuids));
        var u = Assert.Single(nd.AttrUpdates);
        Assert.Equal(MonsterUuid, u.Uuid);
        Assert.Equal(4_500, u.CurHp);
        Assert.Equal(9_000, u.MaxHp);
        Assert.Equal(42, u.Level);
        Assert.True(u.HasCurHp);
        Assert.True(u.HasMaxHp);
        Assert.True(u.HasLevel);
    }

    [Fact]
    public void BaseDelta_PlayerUuid_DoesNotEmitNearDelta()
    {
        // Player low-marker (640) → belongs to the unported
        // _process_attr_collection path; must not write into MonsterDataMap.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Delta(PlayerUuid, A(11310, 1_000), A(11320, 1_000)),
        };
        var evs = Dispatch(inner);
        Assert.Empty(evs.OfType<NearDeltaEvent>());
    }

    [Fact]
    public void BaseDelta_MissingAttrs_NoNearDelta()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = new AoiSyncDelta { Uuid = MonsterUuid }, // no Attrs
        };
        var evs = Dispatch(inner);
        Assert.Empty(evs.OfType<NearDeltaEvent>());
    }

    [Fact]
    public void BaseDelta_EmptyAttrs_NoNearDelta()
    {
        // AttrCollection present but no recognised ids → bAttrs.Any false.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Delta(MonsterUuid, A(99999, 1)),
        };
        var evs = Dispatch(inner);
        Assert.Empty(evs.OfType<NearDeltaEvent>());
    }

    [Fact]
    public void BaseDelta_ZeroUuid_NoNearDelta()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Delta(0, A(11310, 1_000)),
        };
        var evs = Dispatch(inner);
        Assert.Empty(evs.OfType<NearDeltaEvent>());
    }

    [Fact]
    public void NoBaseDelta_NoNearDelta()
    {
        var inner = new AoiSyncToMeDelta { Uuid = PlayerUuid };
        var evs = Dispatch(inner);
        Assert.Empty(evs.OfType<NearDeltaEvent>());
        // Primary ToMeDeltaEvent still emitted.
        Assert.Single(evs.OfType<ToMeDeltaEvent>());
    }

    [Fact]
    public void BaseDelta_WithBreakGaugeAndShield_PropagatesAllAttrs()
    {
        // 455 = BREAKING_STAGE, 441 = EXTINCTION, 444 = IN_OVERDRIVE.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Delta(MonsterUuid,
                A(11310, 800), A(11320, 1_000),
                A(455, 2), A(441, 50), A(444, 1)),
        };
        var evs = Dispatch(inner);
        var nd = Assert.Single(evs.OfType<NearDeltaEvent>());
        var u = Assert.Single(nd.AttrUpdates);
        Assert.True(u.Attrs.HasBreakingStage);
        Assert.Equal(2, u.Attrs.BreakingStage);
        Assert.True(u.Attrs.HasExtinction);
        Assert.Equal(50, u.Attrs.Extinction);
        Assert.True(u.Attrs.HasInOverdrive);
        Assert.True(u.Attrs.InOverdrive);
    }

    [Fact]
    public void Bridge_BaseDeltaHpUpdate_FlowsIntoMonsterDataMap()
    {
        // End-to-end: pre-seed an Entmonster appearance via NearEntitiesEvent,
        // then deliver a SyncToMeDeltaInfo whose BaseDelta carries an HP
        // delta for the same uuid. The bridge ApplyNearDelta should mirror
        // it into MonsterDataMap.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(MonsterUuid, (int)EEntityType.Entmonster)
                {
                    CurHp = 9_000, MaxHp = 9_000,
                    Attrs = new MonsterCoreAttrs
                    {
                        CurHp = 9_000, HasCurHp = true,
                        MaxHp = 9_000, HasMaxHp = true,
                    },
                },
            },
            Array.Empty<EntityDisappearance>(),
            1.0));
        Assert.Equal(9_000, state.Snapshot.MonsterDataMap[MonsterUuid].Hp);

        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Delta(MonsterUuid, A(11310, 3_500)),
        };
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), 2.0,
            ev => bridge.Apply(ev));

        Assert.Equal(3_500, state.Snapshot.MonsterDataMap[MonsterUuid].Hp);
    }
}
