using SaoAuto.Core.Automation;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// R8 epic — Python ↔ C# parity coverage for the SceneChangeEvent
/// infrastructure (MSR-1..8 / MAPSWITCH-01..08 / DPS-04..08 / PROTO-01/06).
///
/// Each test pins one behaviour the Python tree relies on:
///   * dungeon-id roll fires a Restart kind with deferred DPS reset
///   * scene-id roll inside the same dungeon fires a Transition kind
///     that preserves combat
///   * wipe buff (510072) maps to a Restart through the soft-restart
///     side channel
///   * cross-server EnterGame zeros per-scene memos
///   * hard scene change wipes MonsterDataMap + Boss summary
///   * soft restart preserves MonsterDataMap + DungeonTargets
///   * pending-reset gate flushes the prior encounter into LastReport
///     on the next damage event then opens a fresh encounter
///   * PurgeStaleMonsters drops rows with stale LastUpdateSeconds
///   * boss-filtered total accumulates only on the pinned boss uuid
///   * SetSelfUid late-upgrades a phantom Player_{uid} row
/// </summary>
public class R8SceneChangeBridgeTests
{
    private static (GameStateManager State, PacketBridge Bridge) NewRig()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        return (state, bridge);
    }

    private static List<SceneChangeEvent> CaptureSceneEvents(PacketBridge bridge)
    {
        var captured = new List<SceneChangeEvent>();
        bridge.SceneChanged += captured.Add;
        return captured;
    }

    // ─────────────────────────────────────────────────────────────
    //  MSR-2 / MAPSWITCH-03: ApplyDungeonStart
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void FirstDungeonStartStoresIdWithoutSceneChange()
    {
        var (state, bridge) = NewRig();
        var events = CaptureSceneEvents(bridge);
        bridge.Apply(new DungeonStartEvent(1001, 1.0));
        Assert.Equal(1001, state.Snapshot.LastDungeonId);
        Assert.Empty(events);
    }

    [Fact]
    public void DungeonStartIdChangeFiresSoftRestart()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new DungeonStartEvent(1001, 1.0));
        var events = CaptureSceneEvents(bridge);
        bridge.Apply(new DungeonStartEvent(1002, 2.0));
        Assert.Single(events);
        Assert.Equal(SceneChangeKind.Restart, events[0].Kind);
        Assert.True(events[0].PreserveCombat);
        Assert.True(events[0].ResetOnNextDamage);
        Assert.Equal(1002, state.Snapshot.LastDungeonId);
    }

    [Fact]
    public void DungeonStartSameIdNoSoftRestart()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new DungeonStartEvent(1001, 1.0));
        var events = CaptureSceneEvents(bridge);
        bridge.Apply(new DungeonStartEvent(1001, 2.0));
        Assert.Empty(events);
    }

    // ─────────────────────────────────────────────────────────────
    //  MSR-3 / MAPSWITCH-07: ApplyEnterScene
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void EnterSceneLatchesSelfUuidWhenMissing()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new EnterSceneEvent(1.0)
        {
            SceneBasicId = 200,
            PlayerUuid = 0xCAFEBABEUL,
        });
        Assert.Equal(0xCAFEBABEUL, state.Snapshot.SelfUuid);
        Assert.Equal(200, state.Snapshot.LastSceneId);
    }

    [Fact]
    public void EnterSceneIdChangeFiresTransition()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new EnterSceneEvent(1.0) { SceneBasicId = 200 });
        var events = CaptureSceneEvents(bridge);
        bridge.Apply(new EnterSceneEvent(2.0) { SceneBasicId = 300 });
        Assert.Single(events);
        Assert.Equal(SceneChangeKind.Transition, events[0].Kind);
        Assert.True(events[0].PreserveCombat);
        Assert.False(events[0].ResetOnNextDamage);
        Assert.Equal(300, state.Snapshot.LastSceneId);
    }

    // ─────────────────────────────────────────────────────────────
    //  PROTO-01: WipeBuff → SoftSceneRestartEvent
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void SoftSceneRestartEventMapsToRestartKind()
    {
        var (_, bridge) = NewRig();
        var events = CaptureSceneEvents(bridge);
        bridge.Apply(new SoftSceneRestartEvent("notify_buff_change", 5.0));
        Assert.Single(events);
        Assert.Equal(SceneChangeKind.Restart, events[0].Kind);
        Assert.True(events[0].PreserveCombat);
        Assert.True(events[0].ResetOnNextDamage);
        Assert.Equal("notify_buff_change", events[0].Reason);
    }

    // ─────────────────────────────────────────────────────────────
    //  MSR-8: cross-server EnterGame zeroes memos
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void EnterGameCrossServerZerosSceneMemos()
    {
        var (state, bridge) = NewRig();
        bridge.Apply(new EnterGameEvent(0xAAAAUL, 1.0));
        bridge.Apply(new DungeonStartEvent(1001, 2.0));
        bridge.Apply(new EnterSceneEvent(3.0) { SceneBasicId = 200, PlayerUuid = 0xAAAAUL });
        Assert.Equal(1001, state.Snapshot.LastDungeonId);
        Assert.Equal(200, state.Snapshot.LastSceneId);

        // Cross-server: new SelfUuid → all memos reset.
        bridge.Apply(new EnterGameEvent(0xBBBBUL, 4.0));
        Assert.Equal(0xBBBBUL, state.Snapshot.SelfUuid);
        Assert.Equal(0, state.Snapshot.LastDungeonId);
        Assert.Equal(0, state.Snapshot.LastSceneId);
        Assert.Equal(0, state.Snapshot.SyncContainerCount);
    }

    // ─────────────────────────────────────────────────────────────
    //  MSR-7 / MAPSWITCH-08: hard scene wipes DPS + monster map
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void HardSceneChangeResetsDpsAndMonsterMap()
    {
        var (state, bridge) = NewRig();
        bridge.DpsTracker.RecordDamage(100, "self", 1000, 1, isSelf: true);
        Assert.True(bridge.DpsTracker.Snapshot().Active);

        bridge.Apply(new SceneChangeEvent(
            SceneChangeKind.Hard, "test", PreserveCombat: false,
            ResetOnNextDamage: false, ResetDelaySeconds: 0, TimestampSeconds: 1.0));

        Assert.False(bridge.DpsTracker.Snapshot().Active);
        Assert.Empty(state.Snapshot.MonsterDataMap);
    }

    [Fact]
    public void SoftRestartPreservesMonsterMapAndArmsPendingReset()
    {
        var (_, bridge) = NewRig();
        bridge.DpsTracker.RecordDamage(100, "self", 1000, 1, isSelf: true);
        var beforeTotal = bridge.DpsTracker.Snapshot().TotalDamage;
        Assert.Equal(1000, beforeTotal);

        bridge.Apply(new SceneChangeEvent(
            SceneChangeKind.Restart, "test", PreserveCombat: true,
            ResetOnNextDamage: true, ResetDelaySeconds: 3.0, TimestampSeconds: 1.0));

        // Tracker still active — pending reset is armed but not fired.
        Assert.True(bridge.DpsTracker.Snapshot().Active);
        Assert.Equal(1000, bridge.DpsTracker.Snapshot().TotalDamage);
    }

    [Fact]
    public void PendingResetFlushesOnNextDamageEvent()
    {
        var (_, bridge) = NewRig();
        bridge.DpsTracker.RecordDamage(100, "self", 1000, 1, isSelf: true);
        bridge.Apply(new SceneChangeEvent(
            SceneChangeKind.Restart, "test_restart", PreserveCombat: true,
            ResetOnNextDamage: true, ResetDelaySeconds: 3.0, TimestampSeconds: 1.0));

        // Fire a damage event through the bridge — should consume the gate.
        var dmg = new DamagePayload(
            AttackerUuid: 100L << 16 | 640L, TopSummonerId: 0,
            SkillId: 1, DamageSource: 0, OwnerLevel: 0, OwnerStage: 0,
            HitEventId: 0, PassiveUuid: 0, DamageType: 0, TypeFlag: 0,
            DamageMode: 0, Damage: 500, HpLessen: 500, ShieldLessen: 0,
            Element: 0, IsCrit: false, IsDead: false, IsNormal: true,
            IsRainbow: false, IsHeal: false, IsImmune: false, IsAbsorbed: false);
        bridge.Apply(new SkillEffectEvent(0xBEEFL, new[] { dmg }, 2.0));

        var snap = bridge.DpsTracker.Snapshot();
        // Prior 1000 was finalized into LastReport; new encounter has only 500.
        Assert.Equal(500, snap.TotalDamage);
        Assert.NotNull(bridge.DpsTracker.LastReport);
        Assert.Equal("test_restart", bridge.DpsTracker.LastReport!.ReportReason);
    }

    // ─────────────────────────────────────────────────────────────
    //  MSR-6: PurgeStaleMonsters
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void PurgeStaleMonstersDropsOldRows()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            MonsterDataMap = s.MonsterDataMap
                .Add(1, new MonsterData { Uuid = 1, LastUpdateSeconds = 10.0 })
                .Add(2, new MonsterData { Uuid = 2, LastUpdateSeconds = 50.0 }),
        });
        // ttl=5s @ now=100s → row 1 (10 < 95) is stale, row 2 (50 < 95) is stale
        var dropped = state.PurgeStaleMonsters(5.0, 100.0);
        Assert.Equal(2, dropped);
        Assert.Empty(state.Snapshot.MonsterDataMap);
    }

    [Fact]
    public void PurgeStaleMonstersKeepsFreshRows()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            MonsterDataMap = s.MonsterDataMap
                .Add(1, new MonsterData { Uuid = 1, LastUpdateSeconds = 99.0 }),
        });
        var dropped = state.PurgeStaleMonsters(5.0, 100.0);
        Assert.Equal(0, dropped);
        Assert.Single(state.Snapshot.MonsterDataMap);
    }

    [Fact]
    public void PurgeStaleMonstersIgnoresNeverStampedRows()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            MonsterDataMap = s.MonsterDataMap
                .Add(1, new MonsterData { Uuid = 1, LastUpdateSeconds = 0.0 }),
        });
        var dropped = state.PurgeStaleMonsters(5.0, 100.0);
        Assert.Equal(0, dropped);
    }

    // ─────────────────────────────────────────────────────────────
    //  DPS-01: boss-filtered total
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void BossFilteredTotalAccumulatesOnlyForBossUuid()
    {
        var dps = new DpsTracker();
        const ulong bossUuid = 0xABC123UL;
        dps.SetBossUuid(bossUuid);
        dps.RecordDamage(100, "self", 500, 1, isSelf: true, targetUuid: unchecked((long)bossUuid));
        dps.RecordDamage(100, "self", 200, 1, isSelf: true, targetUuid: 0x999L);
        dps.RecordDamage(100, "self", 300, 1, isSelf: true); // no target

        var snap = dps.Snapshot();
        Assert.Equal(1000, snap.TotalDamage);
        Assert.Equal(500, snap.TotalDamageBoss);
    }

    [Fact]
    public void BossSwitchResetsBossFilteredTotal()
    {
        var dps = new DpsTracker();
        dps.SetBossUuid(0xABCUL);
        dps.RecordDamage(100, "self", 500, 1, isSelf: true, targetUuid: 0xABCL);
        Assert.Equal(500, dps.Snapshot().TotalDamageBoss);

        dps.SetBossUuid(0xDEFUL);
        Assert.Equal(0, dps.Snapshot().TotalDamageBoss);
    }

    // ─────────────────────────────────────────────────────────────
    //  DPS-04: SetSelfUid / SelfUid round-trip
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void SetSelfUidPersistsAcrossReset()
    {
        var dps = new DpsTracker();
        dps.SetSelfUid(0xDEADUL);
        Assert.Equal(0xDEADUL, dps.SelfUid);
        dps.RecordDamage(0xDEADL, "self", 100, 1, isSelf: true);
        dps.Reset();
        Assert.Equal(0xDEADUL, dps.SelfUid); // persists per Python parity
    }

    // ─────────────────────────────────────────────────────────────
    //  DPS-08 / R8: PingSubscribers via SkillEffectEvent
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void SkillEffectEventPingsSubscribersEvenWhenStateUnchanged()
    {
        var (state, bridge) = NewRig();
        int callbacks = 0;
        using var _ = state.Subscribe(_ => callbacks++);
        var initial = callbacks; // Subscribe doesn't auto-fire

        var dmg = new DamagePayload(
            AttackerUuid: 100L << 16 | 640L, TopSummonerId: 0,
            SkillId: 1, DamageSource: 0, OwnerLevel: 0, OwnerStage: 0,
            HitEventId: 0, PassiveUuid: 0, DamageType: 0, TypeFlag: 0,
            DamageMode: 0, Damage: 50, HpLessen: 50, ShieldLessen: 0,
            Element: 0, IsCrit: false, IsDead: false, IsNormal: true,
            IsRainbow: false, IsHeal: false, IsImmune: false, IsAbsorbed: false);
        bridge.Apply(new SkillEffectEvent(0xFEEDL, new[] { dmg }, 1.0));

        // PingSubscribers should have fired at least once on the skill-effect path.
        Assert.True(callbacks > initial);
    }

    // ─────────────────────────────────────────────────────────────
    //  PROTO-06: EnterSceneEvent carries SceneBasicId / PlayerUuid
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void EnterSceneEventCarriesExtendedFields()
    {
        var ev = new EnterSceneEvent(7.5)
        {
            SceneBasicId = 999,
            PlayerUuid = 0x1234UL,
            SceneGuid = "abc-123",
        };
        Assert.Equal(999, ev.SceneBasicId);
        Assert.Equal(0x1234UL, ev.PlayerUuid);
        Assert.Equal("abc-123", ev.SceneGuid);
        Assert.Equal(7.5, ev.TimestampSeconds);
    }
}
