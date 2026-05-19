using Google.Protobuf;
using SaoAuto.Core.Packets;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S127 — wire <c>AoiSyncDelta.SkillEffects</c> (proto field 7) to a
/// new <see cref="SkillEffectEvent"/>. Mirrors Python's
/// <c>_process_skill_effect</c> + the per-row decode/filter slice of
/// <c>_decode_sync_damage_info</c> at packet_parser.py 4077–4151.
/// Decoder-only session: attribution / DpsTracker wiring is S128.
/// </summary>
public class Session127SkillEffectTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private const long PlayerUuid = (123L << 16) | 640;
    private const long MonsterUuid = (456L << 16) | 64;
    private const long MonsterBUuid = (789L << 16) | 64;

    private static SyncDamageInfo D(
        EDamageType type = EDamageType.Normal,
        long value = 0, long lucky = 0, long actual = 0,
        long hpLessen = 0, long shieldLessen = 0,
        long attackerUuid = 0, long topSummonerId = 0,
        int ownerId = 0, int ownerLevel = 0, int ownerStage = 0,
        int hitEventId = 0, uint passiveUuid = 0,
        int typeFlag = 0, EDamageMode damageMode = EDamageMode.Damagenormal,
        EDamageProperty property = EDamageProperty.General,
        EDamageSource source = EDamageSource.Skill,
        bool isNormal = false, bool isRainbow = false, bool isDead = false)
        => new()
        {
            Type = type,
            Value = value, LuckyValue = lucky, ActualValue = actual,
            HpLessenValue = hpLessen, ShieldLessenValue = shieldLessen,
            AttackerUuid = attackerUuid, TopSummonerId = topSummonerId,
            OwnerId = ownerId, OwnerLevel = ownerLevel, OwnerStage = ownerStage,
            HitEventId = hitEventId, PassiveUuid = passiveUuid,
            TypeFlag = typeFlag, DamageMode = damageMode,
            Property = property, DamageSource = source,
            IsNormal = isNormal, IsRainbow = isRainbow, IsDead = isDead,
        };

    private static SkillEffect Eff(params SyncDamageInfo[] dmgs)
    {
        var se = new SkillEffect();
        foreach (var d in dmgs) se.Damages.Add(d);
        return se;
    }

    private static List<ParserEvent> DispatchToMe(AoiSyncToMeDelta inner, double ts = 1.0)
    {
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        var captured = new List<ParserEvent>();
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), ts,
            captured.Add);
        return captured;
    }

    private static List<ParserEvent> DispatchNear(SyncNearDeltaInfo msg, double ts = 1.0)
    {
        var captured = new List<ParserEvent>();
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), ts,
            captured.Add);
        return captured;
    }

    [Fact]
    public void Decoder_ToMeBaseDelta_EmitsSingleDamage()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = MonsterUuid,
            BaseDelta = new AoiSyncDelta
            {
                Uuid = MonsterUuid,
                SkillEffects = Eff(D(
                    type: EDamageType.Normal,
                    value: 12345,
                    attackerUuid: PlayerUuid,
                    ownerId: 410001,
                    ownerLevel: 5,
                    typeFlag: 1,  // crit bit
                    isNormal: true)),
            },
        };
        var ev = Assert.Single(DispatchToMe(inner).OfType<SkillEffectEvent>());
        Assert.Equal(MonsterUuid, ev.TargetUuid);
        var row = Assert.Single(ev.Damages);
        Assert.Equal(PlayerUuid, row.AttackerUuid);
        Assert.Equal(410001, row.SkillId);
        Assert.Equal(5, row.OwnerLevel);
        Assert.Equal(12345, row.Damage);
        Assert.True(row.IsCrit);
        Assert.True(row.IsNormal);
        Assert.False(row.IsHeal);
        Assert.False(row.IsImmune);
    }

    [Fact]
    public void Decoder_NearDelta_EmitsPerDelta()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(new AoiSyncDelta
        {
            Uuid = MonsterUuid,
            SkillEffects = Eff(D(value: 100, attackerUuid: PlayerUuid)),
        });
        msg.DeltaInfos.Add(new AoiSyncDelta
        {
            Uuid = MonsterBUuid,
            SkillEffects = Eff(D(value: 200, attackerUuid: PlayerUuid)),
        });
        var events = DispatchNear(msg).OfType<SkillEffectEvent>().ToList();
        Assert.Equal(2, events.Count);
        Assert.Equal(MonsterUuid, events[0].TargetUuid);
        Assert.Equal(100, events[0].Damages[0].Damage);
        Assert.Equal(MonsterBUuid, events[1].TargetUuid);
        Assert.Equal(200, events[1].Damages[0].Damage);
    }

    [Fact]
    public void Decoder_MissAndFall_AreFiltered()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = MonsterUuid,
            BaseDelta = new AoiSyncDelta
            {
                Uuid = MonsterUuid,
                SkillEffects = Eff(
                    D(type: EDamageType.Miss, value: 999),
                    D(type: EDamageType.Fall, value: 999),
                    D(type: EDamageType.Normal, value: 50)),
            },
        };
        var ev = Assert.Single(DispatchToMe(inner).OfType<SkillEffectEvent>());
        var row = Assert.Single(ev.Damages);
        Assert.Equal(50, row.Damage);
        Assert.Equal((int)EDamageType.Normal, row.DamageType);
    }

    [Fact]
    public void Decoder_ZeroDamage_DroppedUnlessImmuneOrAbsorbed()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = MonsterUuid,
            BaseDelta = new AoiSyncDelta
            {
                Uuid = MonsterUuid,
                SkillEffects = Eff(
                    D(type: EDamageType.Normal),  // dropped: damage=0
                    D(type: EDamageType.Immune),   // kept: invincibility marker
                    D(type: EDamageType.Absorbed)),
            },
        };
        var ev = Assert.Single(DispatchToMe(inner).OfType<SkillEffectEvent>());
        Assert.Equal(2, ev.Damages.Count);
        Assert.True(ev.Damages[0].IsImmune);
        Assert.True(ev.Damages[1].IsAbsorbed);
    }

    [Fact]
    public void Decoder_DamageAmount_PrefersValue_FallsThroughToHpPlusShield()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = MonsterUuid,
            BaseDelta = new AoiSyncDelta
            {
                Uuid = MonsterUuid,
                SkillEffects = Eff(
                    D(value: 100, lucky: 200, actual: 300),  // value wins
                    D(lucky: 50, actual: 60),                // lucky wins
                    D(actual: 70),                           // actual wins
                    D(hpLessen: 10, shieldLessen: 25)),      // sum
            },
        };
        var ev = Assert.Single(DispatchToMe(inner).OfType<SkillEffectEvent>());
        Assert.Equal(4, ev.Damages.Count);
        Assert.Equal(100, ev.Damages[0].Damage);
        Assert.Equal(50, ev.Damages[1].Damage);
        Assert.Equal(70, ev.Damages[2].Damage);
        Assert.Equal(35, ev.Damages[3].Damage);
    }

    [Fact]
    public void Decoder_HealAndCritFlags_Surfaced()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = new AoiSyncDelta
            {
                Uuid = PlayerUuid,
                SkillEffects = Eff(
                    D(type: EDamageType.Heal, value: 500, typeFlag: 1),
                    D(type: EDamageType.Normal, value: 1000, typeFlag: 0)),
            },
        };
        var ev = Assert.Single(DispatchToMe(inner).OfType<SkillEffectEvent>());
        Assert.True(ev.Damages[0].IsHeal);
        Assert.True(ev.Damages[0].IsCrit);
        Assert.False(ev.Damages[1].IsHeal);
        Assert.False(ev.Damages[1].IsCrit);
    }

    [Fact]
    public void Decoder_PlayerUuidTarget_AlsoEmits()
    {
        // S127 is uuid-agnostic at decoder — player-as-target damage
        // (mob hitting self) surfaces just like monster-as-target.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = new AoiSyncDelta
            {
                Uuid = PlayerUuid,
                SkillEffects = Eff(D(value: 800, attackerUuid: MonsterUuid)),
            },
        };
        var ev = Assert.Single(DispatchToMe(inner).OfType<SkillEffectEvent>());
        Assert.Equal(PlayerUuid, ev.TargetUuid);
        Assert.Equal(MonsterUuid, ev.Damages[0].AttackerUuid);
    }

    [Fact]
    public void Decoder_TopSummonerAndPassive_PassThrough()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = MonsterUuid,
            BaseDelta = new AoiSyncDelta
            {
                Uuid = MonsterUuid,
                SkillEffects = Eff(D(
                    value: 250,
                    attackerUuid: 999,
                    topSummonerId: PlayerUuid,
                    passiveUuid: 0xDEADBEEFu,
                    hitEventId: 42,
                    ownerStage: 3)),
            },
        };
        var ev = Assert.Single(DispatchToMe(inner).OfType<SkillEffectEvent>());
        var row = ev.Damages[0];
        Assert.Equal(PlayerUuid, row.TopSummonerId);
        Assert.Equal(999, row.AttackerUuid);
        Assert.Equal(0xDEADBEEFu, row.PassiveUuid);
        Assert.Equal(42, row.HitEventId);
        Assert.Equal(3, row.OwnerStage);
    }

    [Fact]
    public void Decoder_AllRowsFiltered_NoEvent()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = MonsterUuid,
            BaseDelta = new AoiSyncDelta
            {
                Uuid = MonsterUuid,
                SkillEffects = Eff(
                    D(type: EDamageType.Miss),
                    D(type: EDamageType.Fall),
                    D(type: EDamageType.Normal)),  // damage=0 dropped
            },
        };
        Assert.Empty(DispatchToMe(inner).OfType<SkillEffectEvent>());
    }

    [Fact]
    public void Decoder_NoSkillEffects_NoEvent()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = MonsterUuid,
            BaseDelta = new AoiSyncDelta { Uuid = MonsterUuid },
        };
        Assert.Empty(DispatchToMe(inner).OfType<SkillEffectEvent>());
    }

    [Fact]
    public void Decoder_EmptyDamagesList_NoEvent()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = MonsterUuid,
            BaseDelta = new AoiSyncDelta
            {
                Uuid = MonsterUuid,
                SkillEffects = new SkillEffect(),
            },
        };
        Assert.Empty(DispatchToMe(inner).OfType<SkillEffectEvent>());
    }

    [Fact]
    public void Decoder_NearDelta_OnlySomeHaveSkillEffects()
    {
        var msg = new SyncNearDeltaInfo();
        // Attr-only delta — no SkillEffect should surface for it.
        msg.DeltaInfos.Add(new AoiSyncDelta { Uuid = MonsterUuid });
        // Second delta carries damage.
        msg.DeltaInfos.Add(new AoiSyncDelta
        {
            Uuid = MonsterBUuid,
            SkillEffects = Eff(D(value: 333, attackerUuid: PlayerUuid)),
        });
        var events = DispatchNear(msg).OfType<SkillEffectEvent>().ToList();
        var ev = Assert.Single(events);
        Assert.Equal(MonsterBUuid, ev.TargetUuid);
        Assert.Equal(333, ev.Damages[0].Damage);
    }

    [Fact]
    public void Decoder_DamageSourceAndModeAndElement_PassThrough()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = MonsterUuid,
            BaseDelta = new AoiSyncDelta
            {
                Uuid = MonsterUuid,
                SkillEffects = Eff(D(
                    value: 600,
                    source: EDamageSource.Buff,
                    damageMode: EDamageMode.Damagephysical,
                    property: EDamageProperty.Fire,
                    isRainbow: true,
                    isDead: true)),
            },
        };
        var ev = Assert.Single(DispatchToMe(inner).OfType<SkillEffectEvent>());
        var row = ev.Damages[0];
        Assert.Equal((int)EDamageSource.Buff, row.DamageSource);
        Assert.Equal((int)EDamageMode.Damagephysical, row.DamageMode);
        Assert.Equal((int)EDamageProperty.Fire, row.Element);
        Assert.True(row.IsRainbow);
        Assert.True(row.IsDead);
    }
}
