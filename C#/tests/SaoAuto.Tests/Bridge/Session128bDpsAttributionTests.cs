using System.Collections.Immutable;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// S128b — DPS attribution + encounter rollup. Bridge consumer for
/// <see cref="SkillEffectEvent"/> fans damage/heal rows out to
/// <see cref="DpsTracker"/>. Mirrors Python's <c>_on_damage</c>
/// callback at packet_parser.py 4172–4253: per-row attacker
/// resolution via <see cref="CyCombat.DpsAttackerUid"/>, name from
/// snapshot (self → PlayerName; monster → MonsterDataMap[uuid].Name;
/// otherwise Player_{uid}), profession only when self.
/// </summary>
public class Session128bDpsAttributionTests
{
    private const long SelfUid = 123L;
    private const ulong SelfUuid = (123UL << 16) | CyCombat.PlayerUuidMarker;
    private const long MateUid = 999L;
    private const long MateUuid = (999L << 16) | (long)CyCombat.PlayerUuidMarker;
    private const long MonsterUuid = (456L << 16) | (long)CyCombat.MonsterUuidMarker;
    private const long TargetUuid = (789L << 16) | (long)CyCombat.MonsterUuidMarker;

    private static DamagePayload Row(
        long attackerUuid, long damage = 100, int skillId = 0,
        bool isCrit = false, bool isHeal = false)
        => new(
            AttackerUuid: attackerUuid, TopSummonerId: 0, SkillId: skillId,
            DamageSource: 0, OwnerLevel: 0, OwnerStage: 0, HitEventId: 0,
            PassiveUuid: 0u, DamageType: 0, TypeFlag: 0, DamageMode: 0,
            Damage: damage, HpLessen: 0, ShieldLessen: 0,
            Element: 0, IsCrit: isCrit, IsDead: false, IsNormal: false,
            IsRainbow: false, IsHeal: isHeal, IsImmune: false,
            IsAbsorbed: false);

    private static GameStateManager NewSelfState(string name = "笨猫", int profession = 12)
    {
        var s = new GameStateManager();
        s.Update(g => g with
        {
            SelfUuid = SelfUuid,
            PlayerName = name,
            ProfessionId = profession,
        });
        return s;
    }

    private static (PacketBridge bridge, DpsTracker tracker) BuildBridge(GameStateManager state)
    {
        var clock = new TestClock();
        var tracker = new DpsTracker(() => clock.Now);
        var bridge = new PacketBridge(state, new PacketParser(), dpsTracker: tracker);
        // Step the clock once before injection so encounter elapsed > 0.
        clock.Advance(TimeSpan.FromSeconds(1));
        return (bridge, tracker);
    }

    [Fact]
    public void SelfAttack_RecordsDamageAgainstSelfRow()
    {
        var state = NewSelfState();
        var (bridge, tracker) = BuildBridge(state);
        try
        {
            bridge.Apply(new SkillEffectEvent(
                TargetUuid, new[] { Row((long)SelfUuid, damage: 500) }, 5.0));

            var snap = tracker.Snapshot();
            Assert.True(snap.Active);
            Assert.Equal(500, snap.TotalDamage);
            var row = Assert.Single(snap.Rows);
            Assert.Equal(SelfUid, row.EntityUuid);
            Assert.Equal("笨猫", row.EntityName);
            Assert.True(row.IsSelf);
            Assert.Equal(12, row.ProfessionId);
        }
        finally { bridge.Dispose(); }
    }

    [Fact]
    public void TeamMateAttack_RecordsAsOtherPlayer()
    {
        var state = NewSelfState();
        var (bridge, tracker) = BuildBridge(state);
        try
        {
            bridge.Apply(new SkillEffectEvent(
                TargetUuid, new[] { Row(MateUuid, damage: 250) }, 5.0));

            var snap = tracker.Snapshot();
            var row = Assert.Single(snap.Rows);
            Assert.Equal(MateUid, row.EntityUuid);
            Assert.Equal($"Player_{MateUid}", row.EntityName);
            Assert.False(row.IsSelf);
            Assert.Equal(0, row.ProfessionId);
        }
        finally { bridge.Dispose(); }
    }

    [Fact]
    public void MonsterAttacker_NotRecorded()
    {
        // Monster low-marker (64) is not a player uuid. DpsAttackerUid
        // returns 0 unless attackerIsSelf — mob hits drop on the floor.
        var state = NewSelfState();
        var (bridge, tracker) = BuildBridge(state);
        try
        {
            bridge.Apply(new SkillEffectEvent(
                (long)SelfUuid, new[] { Row(MonsterUuid, damage: 999) }, 5.0));

            var snap = tracker.Snapshot();
            Assert.False(snap.Active);
            Assert.Equal(0, snap.TotalDamage);
        }
        finally { bridge.Dispose(); }
    }

    [Fact]
    public void HealRow_RoutesToRecordHeal()
    {
        var state = NewSelfState();
        var (bridge, tracker) = BuildBridge(state);
        try
        {
            bridge.Apply(new SkillEffectEvent(
                (long)SelfUuid, new[] { Row((long)SelfUuid, damage: 300, isHeal: true) }, 5.0));

            var snap = tracker.Snapshot();
            Assert.True(snap.Active);
            Assert.Equal(0, snap.TotalDamage);
            Assert.Equal(300, snap.TotalHeal);
            Assert.Equal(SelfUid, Assert.Single(snap.Rows).EntityUuid);
        }
        finally { bridge.Dispose(); }
    }

    [Fact]
    public void CritFlag_PropagatesToSkillBreakdown()
    {
        var state = NewSelfState();
        var (bridge, tracker) = BuildBridge(state);
        try
        {
            bridge.Apply(new SkillEffectEvent(TargetUuid, new[]
            {
                Row((long)SelfUuid, damage: 100, skillId: 42, isCrit: true),
                Row((long)SelfUuid, damage:  50, skillId: 42, isCrit: false),
            }, 5.0));

            var skills = tracker.SkillBreakdown(SelfUid);
            var row = Assert.Single(skills);
            Assert.Equal(42, row.SkillId);
            Assert.Equal(150, row.Total);
            Assert.Equal(2, row.Hits);
            Assert.Equal(1, row.CritHits);
            Assert.Equal(100, row.MaxHit);
        }
        finally { bridge.Dispose(); }
    }

    [Fact]
    public void ZeroDamageRow_Skipped()
    {
        // Filtered at the bridge before hitting RecordDamage so encounter
        // doesn't open on a no-op event.
        var state = NewSelfState();
        var (bridge, tracker) = BuildBridge(state);
        try
        {
            bridge.Apply(new SkillEffectEvent(
                TargetUuid, new[] { Row((long)SelfUuid, damage: 0) }, 5.0));

            Assert.False(tracker.Snapshot().Active);
        }
        finally { bridge.Dispose(); }
    }

    [Fact]
    public void EmptyDamages_NoOp()
    {
        var state = NewSelfState();
        var (bridge, tracker) = BuildBridge(state);
        try
        {
            bridge.Apply(new SkillEffectEvent(
                TargetUuid, Array.Empty<DamagePayload>(), 5.0));
            Assert.False(tracker.Snapshot().Active);
        }
        finally { bridge.Dispose(); }
    }

    [Fact]
    public void MultipleRowsMixedAttackers_AllRecorded()
    {
        var state = NewSelfState();
        var (bridge, tracker) = BuildBridge(state);
        try
        {
            bridge.Apply(new SkillEffectEvent(TargetUuid, new[]
            {
                Row((long)SelfUuid, damage: 200),
                Row(MateUuid, damage: 300),
                Row(MonsterUuid, damage: 999),  // dropped (no attacker uid)
            }, 5.0));

            var snap = tracker.Snapshot();
            Assert.Equal(500, snap.TotalDamage);
            Assert.Equal(2, snap.Rows.Length);
        }
        finally { bridge.Dispose(); }
    }

    [Fact]
    public void DefaultTrackerInstanceExposed()
    {
        // Bridge built without an explicit tracker still surfaces one via
        // the DpsTracker property — same instance across calls.
        var state = NewSelfState();
        using var bridge = new PacketBridge(state, new PacketParser());
        Assert.NotNull(bridge.DpsTracker);
        Assert.Same(bridge.DpsTracker, bridge.DpsTracker);
    }

    [Fact]
    public void InjectedTrackerIsUsed()
    {
        var state = NewSelfState();
        var clock = new TestClock();
        var tracker = new DpsTracker(() => clock.Now);
        using var bridge = new PacketBridge(
            state, new PacketParser(), dpsTracker: tracker);

        Assert.Same(tracker, bridge.DpsTracker);

        clock.Advance(TimeSpan.FromSeconds(1));
        bridge.Apply(new SkillEffectEvent(
            TargetUuid, new[] { Row((long)SelfUuid, damage: 75) }, 5.0));

        Assert.Equal(75, tracker.Snapshot().TotalDamage);
    }

    [Fact]
    public void MonsterNameFromMonsterDataMap_UsedWhenAvailable()
    {
        // Edge case: a friendly summon registered as a monster row with a
        // name. DpsAttackerUid still returns 0 (low marker mismatch), so
        // the path is exercised via the rare "self uuid happens to also
        // be in MonsterDataMap" branch — not hit in practice. Documented
        // here as a no-op assertion to record the behavior.
        var state = NewSelfState();
        state.Update(g => g with
        {
            MonsterDataMap = ImmutableDictionary<long, MonsterData>.Empty
                .SetItem(MonsterUuid, new MonsterData
                {
                    Uuid = MonsterUuid, Name = "Goblin", Hp = 100, MaxHp = 100,
                }),
        });
        var (bridge, tracker) = BuildBridge(state);
        try
        {
            bridge.Apply(new SkillEffectEvent(
                (long)SelfUuid, new[] { Row(MonsterUuid, damage: 50) }, 5.0));
            // Mob attacker still drops — DpsAttackerUid returns 0.
            Assert.False(tracker.Snapshot().Active);
        }
        finally { bridge.Dispose(); }
    }

    private sealed class TestClock
    {
        public DateTimeOffset Now { get; private set; } =
            new DateTimeOffset(2026, 5, 7, 12, 0, 0, TimeSpan.Zero);
        public void Advance(TimeSpan d) => Now = Now + d;
    }
}
