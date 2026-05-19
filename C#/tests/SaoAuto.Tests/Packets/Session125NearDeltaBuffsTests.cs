using Google.Protobuf;
using SaoAuto.Core.Packets;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S125 — wire <c>SyncNearDeltaInfo</c> (0x2D) BuffInfos + BuffEffect
/// branches per delta. Same shape as the S123/S124 0x2E BaseDelta path
/// but emitted once per delta uuid. Mirrors Python's
/// <c>_on_sync_near_delta</c> calling <c>_process_aoi_sync_delta</c> at
/// packet_parser.py 4014.
/// </summary>
public class Session125NearDeltaBuffsTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private const long PlayerUuid = (123L << 16) | 640;
    private const long MonsterAUuid = (123L << 16) | 64;
    private const long MonsterBUuid = (456L << 16) | 64;

    private static BuffInfo BI(int baseId, int buffUuid = 0, long createTime = 0,
        int duration = 0, int layer = 0, int count = 0)
        => new()
        {
            BaseId = baseId,
            BuffUuid = buffUuid,
            CreateTime = createTime,
            Duration = duration,
            Layer = layer,
            Count = count,
        };

    private static BuffEffect BE(int type, int buffUuid = 0, long hostUuid = 0)
        => new() { Type = (EBuffEventType)type, BuffUuid = buffUuid, HostUuid = hostUuid };

    private static AoiSyncDelta DeltaWithBuffs(long uuid, params BuffInfo[] buffs)
    {
        var d = new AoiSyncDelta { Uuid = uuid, BuffInfos = new BuffInfoSync() };
        foreach (var b in buffs) d.BuffInfos.BuffInfos.Add(b);
        return d;
    }

    private static AoiSyncDelta DeltaWithEffects(long uuid, params BuffEffect[] effects)
    {
        var d = new AoiSyncDelta { Uuid = uuid, BuffEffect = new BuffEffectSync() };
        foreach (var e in effects) d.BuffEffect.BuffEffects.Add(e);
        return d;
    }

    private static List<ParserEvent> Dispatch(SyncNearDeltaInfo msg, double ts = 1.0)
    {
        var captured = new List<ParserEvent>();
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), ts,
            captured.Add);
        return captured;
    }

    [Fact]
    public void Decoder_PlayerBuffInfos_Emits()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(DeltaWithBuffs(PlayerUuid,
            BI(baseId: 100, buffUuid: 1, duration: 5_000, layer: 1, count: 1)));

        var ev = Assert.Single(Dispatch(msg).OfType<AoiBuffSyncEvent>());
        Assert.Equal(PlayerUuid, ev.Uuid);
        var buff = Assert.Single(ev.Buffs);
        Assert.Equal(100, buff.Id);
        Assert.Equal(5_000, buff.DurationMs);
    }

    [Fact]
    public void Decoder_MonsterBuffInfos_Emits()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(DeltaWithBuffs(MonsterAUuid, BI(baseId: 200, buffUuid: 7)));

        var ev = Assert.Single(Dispatch(msg).OfType<AoiBuffSyncEvent>());
        Assert.Equal(MonsterAUuid, ev.Uuid);
        Assert.Equal(200, Assert.Single(ev.Buffs).Id);
    }

    [Fact]
    public void Decoder_MultipleDeltas_EmitsPerUuid()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(DeltaWithBuffs(MonsterAUuid, BI(baseId: 100)));
        msg.DeltaInfos.Add(DeltaWithBuffs(MonsterBUuid, BI(baseId: 200)));

        var evs = Dispatch(msg).OfType<AoiBuffSyncEvent>().ToList();
        Assert.Equal(2, evs.Count);
        Assert.Contains(evs, e => e.Uuid == MonsterAUuid && e.Buffs[0].Id == 100);
        Assert.Contains(evs, e => e.Uuid == MonsterBUuid && e.Buffs[0].Id == 200);
    }

    [Fact]
    public void Decoder_FiltersBaseIdZero()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(DeltaWithBuffs(MonsterAUuid,
            BI(baseId: 0), BI(baseId: 100), BI(baseId: 0)));

        var ev = Assert.Single(Dispatch(msg).OfType<AoiBuffSyncEvent>());
        Assert.Equal(100, Assert.Single(ev.Buffs).Id);
    }

    [Fact]
    public void Decoder_AllBaseIdZero_NoEvent()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(DeltaWithBuffs(MonsterAUuid, BI(baseId: 0), BI(baseId: 0)));

        Assert.Empty(Dispatch(msg).OfType<AoiBuffSyncEvent>());
    }

    [Fact]
    public void Decoder_BuffEffect_EmitsForMonster()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(DeltaWithEffects(MonsterAUuid,
            BE(BuffEventType.HostDeath, buffUuid: 9)));

        var ev = Assert.Single(Dispatch(msg).OfType<BuffEffectEvent>());
        Assert.Equal(MonsterAUuid, ev.TargetUuid);
        var eff = Assert.Single(ev.Effects);
        Assert.Equal(BuffEventType.HostDeath, eff.Type);
        // be.HostUuid==0 → fallback to delta uuid.
        Assert.Equal(MonsterAUuid, eff.HostUuid);
    }

    [Fact]
    public void Decoder_BuffEffect_PlayerLowMarker_NoEvent()
    {
        // Mirrors S124 0x2E gate: target_is_monster predicate.
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(DeltaWithEffects(PlayerUuid,
            BE(BuffEventType.EnterBreaking)));

        Assert.Empty(Dispatch(msg).OfType<BuffEffectEvent>());
    }

    [Fact]
    public void Decoder_BuffEffect_FiltersNonBossTypes()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(DeltaWithEffects(MonsterAUuid,
            BE(999), BE(BuffEventType.ShieldBroken), BE(7)));

        var ev = Assert.Single(Dispatch(msg).OfType<BuffEffectEvent>());
        Assert.Equal(BuffEventType.ShieldBroken, Assert.Single(ev.Effects).Type);
    }

    [Fact]
    public void Decoder_MixedBranchesPerDelta_EmitsBoth()
    {
        // Same delta carries both BuffInfos AND BuffEffect → both events
        // surface, both keyed to the same uuid.
        var d = new AoiSyncDelta
        {
            Uuid = MonsterAUuid,
            BuffInfos = new BuffInfoSync(),
            BuffEffect = new BuffEffectSync(),
        };
        d.BuffInfos.BuffInfos.Add(BI(baseId: 100));
        d.BuffEffect.BuffEffects.Add(BE(BuffEventType.HostDeath));
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(d);

        var evs = Dispatch(msg);
        var buff = Assert.Single(evs.OfType<AoiBuffSyncEvent>());
        var effect = Assert.Single(evs.OfType<BuffEffectEvent>());
        Assert.Equal(MonsterAUuid, buff.Uuid);
        Assert.Equal(MonsterAUuid, effect.TargetUuid);
    }

    [Fact]
    public void Decoder_NoBuffBranches_OnlyNearDelta()
    {
        // Just the legacy uuid list — no buffs / no effects.
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(new AoiSyncDelta { Uuid = MonsterAUuid });

        var evs = Dispatch(msg);
        Assert.Single(evs.OfType<NearDeltaEvent>());
        Assert.Empty(evs.OfType<AoiBuffSyncEvent>());
        Assert.Empty(evs.OfType<BuffEffectEvent>());
    }
}
