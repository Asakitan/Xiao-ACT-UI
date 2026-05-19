using System.Collections.Immutable;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S128 — bridge consumer for <see cref="SkillEffectEvent"/>. Mirrors
/// the monster-side state mutations of Python's
/// <c>_process_skill_effect</c> at packet_parser.py 4194–4247:
/// damage proves alive (revive), shield-lessen depletion, no HP
/// burn-down (deferred to AttrCollection deltas). DpsTracker /
/// EncounterTracker rollup is deferred to S128b.
/// </summary>
public class Session128SkillEffectBridgeTests
{
    private const long PlayerUuid = (123L << 16) | 640;
    private const long MonsterUuid = (456L << 16) | 64;
    private const long BossUuid = (789L << 16) | 64;

    private static DamagePayload Row(
        long damage = 100, long shieldLessen = 0, bool isHeal = false,
        bool isDead = false, long attackerUuid = PlayerUuid)
        => new(
            AttackerUuid: attackerUuid, TopSummonerId: 0, SkillId: 0,
            DamageSource: 0, OwnerLevel: 0, OwnerStage: 0, HitEventId: 0,
            PassiveUuid: 0u, DamageType: 0, TypeFlag: 0, DamageMode: 0,
            Damage: damage, HpLessen: 0, ShieldLessen: shieldLessen,
            Element: 0, IsCrit: false, IsDead: isDead, IsNormal: false,
            IsRainbow: false, IsHeal: isHeal, IsImmune: false,
            IsAbsorbed: false);

    private static GameStateManager StateWithMonster(MonsterData md)
    {
        var s = new GameStateManager();
        s.Update(g => g with
        {
            MonsterDataMap = ImmutableDictionary<long, MonsterData>.Empty.SetItem(md.Uuid, md),
        });
        return s;
    }

    private static SkillEffectEvent Event(long targetUuid, params DamagePayload[] rows)
        => new(targetUuid, rows, 5.0);

    [Fact]
    public void Apply_RevivesDeadMonster_OnDamageRow()
    {
        var state = StateWithMonster(new MonsterData
        {
            Uuid = MonsterUuid, Hp = 0, MaxHp = 1000, IsDead = true,
        });
        var changed = StateMutators.Apply(state, Event(MonsterUuid, Row(damage: 250)));

        Assert.True(changed);
        var mon = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.False(mon.IsDead);
        Assert.Equal(1000, mon.Hp);
        Assert.Equal(5.0, mon.LastUpdateSeconds);
    }

    [Fact]
    public void Apply_AliveMonster_DamageNoOp()
    {
        var state = StateWithMonster(new MonsterData
        {
            Uuid = MonsterUuid, Hp = 800, MaxHp = 1000, IsDead = false,
        });
        var changed = StateMutators.Apply(state, Event(MonsterUuid, Row(damage: 250)));

        Assert.False(changed);
        var mon = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.Equal(800, mon.Hp);
    }

    [Fact]
    public void Apply_RowWithIsDeadTrue_DoesNotRevive()
    {
        // Row carrying is_dead=true confirms the death — the bridge
        // must not flip IsDead back to false.
        var state = StateWithMonster(new MonsterData
        {
            Uuid = MonsterUuid, Hp = 0, MaxHp = 1000, IsDead = true,
        });
        var changed = StateMutators.Apply(state, Event(MonsterUuid,
            Row(damage: 250, isDead: true)));

        Assert.False(changed);
        Assert.True(state.Snapshot.MonsterDataMap[MonsterUuid].IsDead);
    }

    [Fact]
    public void Apply_ShieldDepleted_FlipsActiveOff()
    {
        var state = StateWithMonster(new MonsterData
        {
            Uuid = MonsterUuid, Hp = 800, MaxHp = 1000,
            ShieldActive = true, ShieldTotal = 100, ShieldMaxTotal = 500,
        });
        var changed = StateMutators.Apply(state, Event(MonsterUuid,
            Row(damage: 0, shieldLessen: 120)));

        Assert.True(changed);
        var mon = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.Equal(0, mon.ShieldTotal);
        Assert.False(mon.ShieldActive);
    }

    [Fact]
    public void Apply_ShieldPartial_KeepsActive()
    {
        var state = StateWithMonster(new MonsterData
        {
            Uuid = MonsterUuid, Hp = 800, MaxHp = 1000,
            ShieldActive = true, ShieldTotal = 500, ShieldMaxTotal = 500,
        });
        var changed = StateMutators.Apply(state, Event(MonsterUuid,
            Row(damage: 0, shieldLessen: 200)));

        Assert.True(changed);
        var mon = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.Equal(300, mon.ShieldTotal);
        Assert.True(mon.ShieldActive);
    }

    [Fact]
    public void Apply_NoMonsterRow_NoMutation()
    {
        var state = new GameStateManager();
        var changed = StateMutators.Apply(state, Event(MonsterUuid, Row()));

        Assert.False(changed);
        Assert.False(state.Snapshot.MonsterDataMap.ContainsKey(MonsterUuid));
    }

    [Fact]
    public void Apply_HealRow_DoesNotReviveOrBurn()
    {
        // Heal targeting a dead monster row (rare but possible) must
        // not flip IsDead — heal rows are payload-side only.
        var state = StateWithMonster(new MonsterData
        {
            Uuid = MonsterUuid, Hp = 0, MaxHp = 1000, IsDead = true,
            ShieldActive = true, ShieldTotal = 100,
        });
        var changed = StateMutators.Apply(state, Event(MonsterUuid,
            Row(damage: 500, shieldLessen: 50, isHeal: true)));

        Assert.False(changed);
        var mon = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.True(mon.IsDead);
        Assert.Equal(100, mon.ShieldTotal);
    }

    [Fact]
    public void Apply_MultipleRows_AggregateShield()
    {
        var state = StateWithMonster(new MonsterData
        {
            Uuid = MonsterUuid, Hp = 800, MaxHp = 1000,
            ShieldActive = true, ShieldTotal = 200,
        });
        var changed = StateMutators.Apply(state, Event(MonsterUuid,
            Row(damage: 0, shieldLessen: 50),
            Row(damage: 0, shieldLessen: 30)));

        Assert.True(changed);
        Assert.Equal(120, state.Snapshot.MonsterDataMap[MonsterUuid].ShieldTotal);
    }

    [Fact]
    public void Apply_RevivedMonster_ProjectsBossRow()
    {
        // S120 boss projection should pick up the revived monster as
        // the boss row in the same atomic snapshot.
        var state = StateWithMonster(new MonsterData
        {
            Uuid = BossUuid, Hp = 0, MaxHp = 50000, IsDead = true,
        });
        StateMutators.Apply(state, Event(BossUuid, Row(damage: 100)));

        var s = state.Snapshot;
        Assert.Equal(BossHpSource.MonsterData, s.BossHpSource);
        Assert.Equal(50000, s.BossCurrentHp);
        Assert.Equal(50000, s.BossTotalHp);
    }

    [Fact]
    public void Apply_AliveNoShield_ReturnsFalse()
    {
        var state = StateWithMonster(new MonsterData
        {
            Uuid = MonsterUuid, Hp = 800, MaxHp = 1000, IsDead = false,
            ShieldActive = false,
        });
        var changed = StateMutators.Apply(state, Event(MonsterUuid,
            Row(damage: 250)));

        Assert.False(changed);
    }

    [Fact]
    public void Apply_ShieldAlreadyInactive_ShieldLessenIgnored()
    {
        // ShieldActive=false → shield-lessen rows must not write.
        // Mirrors Python's `if monster.shield_active` gate.
        var state = StateWithMonster(new MonsterData
        {
            Uuid = MonsterUuid, Hp = 800, MaxHp = 1000,
            ShieldActive = false, ShieldTotal = 100,
        });
        var changed = StateMutators.Apply(state, Event(MonsterUuid,
            Row(damage: 0, shieldLessen: 50)));

        Assert.False(changed);
        Assert.Equal(100, state.Snapshot.MonsterDataMap[MonsterUuid].ShieldTotal);
    }

    [Fact]
    public void Apply_EmptyDamages_NoOp()
    {
        var state = StateWithMonster(new MonsterData
        {
            Uuid = MonsterUuid, Hp = 0, MaxHp = 1000, IsDead = true,
        });
        var changed = StateMutators.Apply(state,
            new SkillEffectEvent(MonsterUuid, Array.Empty<DamagePayload>(), 5.0));

        Assert.False(changed);
        Assert.True(state.Snapshot.MonsterDataMap[MonsterUuid].IsDead);
    }

    [Fact]
    public void Apply_ZeroTargetUuid_NoOp()
    {
        var state = new GameStateManager();
        var changed = StateMutators.Apply(state, Event(0, Row()));

        Assert.False(changed);
    }

    [Fact]
    public void Apply_ReviveAndShield_BothInOneEvent()
    {
        // Single SkillEffect with one damage row carrying both a non-
        // zero damage (revive trigger) and a shield-lessen — both
        // mutations must land in the same atomic snapshot.
        var state = StateWithMonster(new MonsterData
        {
            Uuid = MonsterUuid, Hp = 0, MaxHp = 2000, IsDead = true,
            ShieldActive = true, ShieldTotal = 300,
        });
        var changed = StateMutators.Apply(state, Event(MonsterUuid,
            Row(damage: 100, shieldLessen: 100)));

        Assert.True(changed);
        var mon = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.False(mon.IsDead);
        Assert.Equal(2000, mon.Hp);
        Assert.Equal(200, mon.ShieldTotal);
        Assert.True(mon.ShieldActive);
    }
}
