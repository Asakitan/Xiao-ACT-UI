using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S123 — wire <c>SyncToMeDeltaInfo.BaseDelta.BuffInfos</c> to a new
/// <see cref="AoiBuffSyncEvent"/>; bridge routes player low-marker
/// uuids into <see cref="GameState.SelfBuffs"/> and non-player uuids
/// into <c>MonsterData.BuffList</c>. Mirrors Python's
/// <c>_decode_buff_info_sync_pb</c> + <c>_process_aoi_sync_delta</c>
/// branch at packet_parser.py 4032–4041.
/// </summary>
public class Session123BuffInfoSyncTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private const long PlayerUuid = (123L << 16) | 640;
    private const long OtherPlayerUuid = (456L << 16) | 640;
    private const long MonsterUuid = (123L << 16) | 64;

    private static BuffInfo BI(int baseId, int buffUuid = 0, long createTime = 0,
        int duration = 0, int layer = 0, int count = 0) => new()
        {
            BaseId = baseId,
            BuffUuid = buffUuid,
            CreateTime = createTime,
            Duration = duration,
            Layer = layer,
            Count = count,
        };

    private static AoiSyncDelta Base(long uuid, params BuffInfo[] buffs)
    {
        var d = new AoiSyncDelta { Uuid = uuid, BuffInfos = new BuffInfoSync() };
        foreach (var bi in buffs) d.BuffInfos.BuffInfos.Add(bi);
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
    public void Decoder_PlayerBuffs_EmitsAllFields()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(PlayerUuid,
                BI(101, buffUuid: 9001, createTime: 50_000, duration: 10_000, layer: 3, count: 1),
                BI(202, buffUuid: 9002, createTime: 51_000, duration: 5_000, layer: 1, count: 2)),
        };
        var ev = Assert.Single(Dispatch(inner).OfType<AoiBuffSyncEvent>());
        Assert.Equal(PlayerUuid, ev.Uuid);
        Assert.Equal(2, ev.Buffs.Count);
        Assert.Equal(101, ev.Buffs[0].Id);
        Assert.Equal(9001, ev.Buffs[0].Uuid);
        Assert.Equal(50_000, ev.Buffs[0].BeginMs);
        Assert.Equal(10_000, ev.Buffs[0].DurationMs);
        Assert.Equal(3, ev.Buffs[0].Layer);
        Assert.Equal(1, ev.Buffs[0].Count);
        Assert.Equal(202, ev.Buffs[1].Id);
    }

    [Fact]
    public void Decoder_MonsterBuffs_AlsoEmits()
    {
        // No player-uuid gate at the decoder — bridge classifies.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(MonsterUuid, BI(700, buffUuid: 1, duration: 2000)),
        };
        var ev = Assert.Single(Dispatch(inner).OfType<AoiBuffSyncEvent>());
        Assert.Equal(MonsterUuid, ev.Uuid);
        Assert.Equal(700, ev.Buffs[0].Id);
    }

    [Fact]
    public void Decoder_FiltersBaseIdZero()
    {
        // Python: `if not base_id: continue`.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(PlayerUuid, BI(0), BI(101), BI(0)),
        };
        var ev = Assert.Single(Dispatch(inner).OfType<AoiBuffSyncEvent>());
        Assert.Single(ev.Buffs);
        Assert.Equal(101, ev.Buffs[0].Id);
    }

    [Fact]
    public void Decoder_AllBaseIdZero_NoEvent()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(PlayerUuid, BI(0), BI(0)),
        };
        Assert.Empty(Dispatch(inner).OfType<AoiBuffSyncEvent>());
    }

    [Fact]
    public void Decoder_NoBuffInfos_NoEvent()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = new AoiSyncDelta { Uuid = PlayerUuid },
        };
        Assert.Empty(Dispatch(inner).OfType<AoiBuffSyncEvent>());
    }

    [Fact]
    public void Bridge_PlayerBuffs_ColdStart_LatchesSelfUuid_AndWritesSelfBuffs()
    {
        var state = new GameStateManager();
        Assert.Equal(0UL, state.Snapshot.SelfUuid);
        var bridge = new PacketBridge(state, new PacketParser());

        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(PlayerUuid,
                BI(101, buffUuid: 9001, createTime: 50_000, duration: 10_000, layer: 3, count: 1)),
        };
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), 1.0,
            ev => bridge.Apply(ev));

        var s = state.Snapshot;
        Assert.Equal((ulong)PlayerUuid, s.SelfUuid);
        var b = Assert.Single(s.SelfBuffs);
        Assert.Equal(101, b.Id);
        Assert.Equal(9001, b.Uuid);
        Assert.Equal(50_000, b.BeginMs);
        Assert.Equal(10_000, b.DurationMs);
        Assert.Equal(3, b.Layer);
    }

    [Fact]
    public void Bridge_PlayerBuffs_SelfUuidMismatch_DoesNotWrite()
    {
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new AoiBuffSyncEvent(OtherPlayerUuid,
            new[] { new BuffPayload(101, 9001, 50_000, 10_000, 3, 1, string.Empty) }, 1.0));

        Assert.Empty(state.Snapshot.SelfBuffs);
    }

    [Fact]
    public void Bridge_PlayerBuffs_DoesNotTouchServerTimeOffset()
    {
        // ApplyBuffs (BuffSnapshotEvent path) writes ServerTimeOffsetMs.
        // The AOI path does NOT carry the offset and must not stomp it.
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid, ServerTimeOffsetMs = 12345.0 });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new AoiBuffSyncEvent(PlayerUuid,
            new[] { new BuffPayload(101, 0, 0, 0, 0, 0, string.Empty) }, 1.0));

        Assert.Equal(12345.0, state.Snapshot.ServerTimeOffsetMs);
        Assert.Single(state.Snapshot.SelfBuffs);
    }

    [Fact]
    public void Bridge_MonsterBuffs_WritesIntoMonsterData()
    {
        var state = new GameStateManager();
        // Seed an existing monster row (mirrors Python's
        // `if uuid in self._monsters` precondition).
        state.Update(s => s with
        {
            MonsterDataMap = s.MonsterDataMap.SetItem(MonsterUuid, new MonsterData
            {
                Uuid = MonsterUuid, Name = "boss", Hp = 100, MaxHp = 100,
            }),
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new AoiBuffSyncEvent(MonsterUuid,
            new[]
            {
                new BuffPayload(700, 1, 1000, 5000, 1, 1, string.Empty),
                new BuffPayload(701, 2, 1100, 3000, 2, 1, string.Empty),
            }, 1.0));

        var md = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.Equal(2, md.BuffList.Length);
        Assert.Equal(700, md.BuffList[0].Id);
        Assert.Equal(701, md.BuffList[1].Id);
        // Other fields untouched.
        Assert.Equal("boss", md.Name);
        Assert.Equal(100, md.Hp);
    }

    [Fact]
    public void Bridge_MonsterBuffs_UnknownUuid_NoOp()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new AoiBuffSyncEvent(MonsterUuid,
            new[] { new BuffPayload(700, 1, 1000, 5000, 1, 1, string.Empty) }, 1.0));

        Assert.Empty(state.Snapshot.MonsterDataMap);
    }

    [Fact]
    public void Bridge_NoChange_DoesNotIncrementEventsApplied()
    {
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid });
        var bridge = new PacketBridge(state, new PacketParser());

        var ev = new AoiBuffSyncEvent(PlayerUuid,
            new[] { new BuffPayload(101, 9001, 50_000, 10_000, 3, 1, string.Empty) }, 1.0);
        bridge.Apply(ev);
        var before = bridge.EventsApplied;
        bridge.Apply(ev);
        Assert.Equal(before, bridge.EventsApplied);
    }
}
