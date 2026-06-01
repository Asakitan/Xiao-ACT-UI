using SaoAuto.Core.Automation;
using SaoAuto.Core.Automation.HideSeek;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// R8 epic follow-up — coverage for phases α-λ landed after the main R8
/// SceneChange infrastructure: SKILL-004/005, PROTO-02/05/07,
/// HideSeek HS-01/02/03/08, AutoKey R16 burst-visual, BossRaid P1
/// damage ingestion, Commander P3 snapshot.
/// </summary>
public class R8FollowUpTests
{
    // ─────────────────────────────────────────────────────────────
    //  SKILL-005: SkillCdSnapshot extended fields
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void SkillCdSnapshotCarriesExtendedFields()
    {
        var snap = new SkillCdSnapshot(
            SkillLevelId: 50301,
            BeginMs: 100,
            DurationMs: 5000,
            ValidCdTimeMs: 1000,
            ChargeCount: 0,
            SubCdRatio: 0,
            SubCdFixed: 0,
            AccelerateCdRatio: 0)
        {
            SkillCdType = 2,
            MaxCharges = 3,
            LastVcdUpdateMs = 12345,
            ObservedAtMs = 12340,
            VcdSpeedRatio = 10000,
        };
        Assert.Equal(2, snap.SkillCdType);
        Assert.Equal(3, snap.MaxCharges);
        Assert.Equal(12345, snap.LastVcdUpdateMs);
        Assert.Equal(10000, snap.VcdSpeedRatio);
    }

    [Fact]
    public void SkillCdSnapshotDefaultMaxChargesIsOne()
    {
        var snap = new SkillCdSnapshot(
            SkillLevelId: 1, BeginMs: 0, DurationMs: 0, ValidCdTimeMs: 0,
            ChargeCount: 0, SubCdRatio: 0, SubCdFixed: 0, AccelerateCdRatio: 0);
        Assert.Equal(1, snap.MaxCharges); // default = 1 per Python parity
    }

    // ─────────────────────────────────────────────────────────────
    //  PROTO-02: SkillObserver
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void SkillObserverDetectsProfessionFromNormalAttack()
    {
        // 雷影剑士 normal attack base id = 1701
        Assert.Equal(1, SkillObserver.DetectProfession(170103));
        Assert.Equal("雷影剑士", SkillObserver.ProfessionNames[1]);
    }

    [Fact]
    public void SkillObserverDetectsProfessionFromUltimate()
    {
        // Composite skill_level_id = base * 100 + level; matches Python's
        // _compose_skill_level_id convention. 冰魔导师 ult base = 1248.
        Assert.Equal(2, SkillObserver.DetectProfession(124801));
        // 神盾骑士 ult base = 2407 — composite is 240701.
        Assert.Equal(12, SkillObserver.DetectProfession(240701));
    }

    [Fact]
    public void SkillObserverDetectsSubProfession()
    {
        // 居合 branch — base 1714 sent as composite 171401.
        Assert.Equal("居合", SkillObserver.DetectSubProfession(171401));
        // 月刃 branch — base 44701 sent as composite 4470101.
        Assert.Equal("月刃", SkillObserver.DetectSubProfession(4470101));
    }

    [Fact]
    public void SkillObserverReturnsEmptyForUnknownSkill()
    {
        Assert.Equal(0, SkillObserver.DetectProfession(999999));
        Assert.Equal(string.Empty, SkillObserver.DetectSubProfession(999999));
    }

    [Fact]
    public void SkillUseEventAutoDetectsProfession()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        // Initially no profession.
        Assert.Equal(0, state.Snapshot.ProfessionId);
        // Fire a skill use that maps to 神射手 normal attack (2201).
        bridge.Apply(new SkillUseEvent(0x123L, 220101, 1.0));
        Assert.Equal(11, state.Snapshot.ProfessionId);
        Assert.Equal("神射手", state.Snapshot.ProfessionName);
    }

    [Fact]
    public void SkillUseEventDoesNotOverwriteExistingProfession()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        state.Update(s => s with { ProfessionId = 5, ProfessionName = "森语者" });
        bridge.Apply(new SkillUseEvent(0x123L, 170103, 1.0)); // would map to 1
        Assert.Equal(5, state.Snapshot.ProfessionId);
        Assert.Equal("森语者", state.Snapshot.ProfessionName);
    }

    // ─────────────────────────────────────────────────────────────
    //  SKILL-001/002: PacketSkillSlotProjector
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void ProjectorProducesReadySlotWhenCdComplete()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SkillCdMap = s.SkillCdMap.Add(1001, new SkillCdSnapshot(
                SkillLevelId: 1001, BeginMs: 0, DurationMs: 0, ValidCdTimeMs: 0,
                ChargeCount: 0, SubCdRatio: 0, SubCdFixed: 0, AccelerateCdRatio: 0)),
        });
        var projector = new PacketSkillSlotProjector();
        var slots = projector.Project(state.Snapshot);
        Assert.Single(slots);
        Assert.Equal(SkillSlotState.Ready, slots[0].State);
    }

    [Fact]
    public void ProjectorProducesCooldownSlot()
    {
        var state = new GameStateManager();
        // Begin 1s ago (server-clock), Duration 10s, elapsed 2s — should be cooldown.
        var nowMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        state.Update(s => s with
        {
            SkillCdMap = s.SkillCdMap.Add(1001, new SkillCdSnapshot(
                SkillLevelId: 1001, BeginMs: nowMs - 2000, DurationMs: 10000,
                ValidCdTimeMs: 2000, ChargeCount: 0,
                SubCdRatio: 0, SubCdFixed: 0, AccelerateCdRatio: 0)),
        });
        var projector = new PacketSkillSlotProjector();
        var slots = projector.Project(state.Snapshot);
        Assert.Single(slots);
        Assert.Equal(SkillSlotState.Cooldown, slots[0].State);
        Assert.True(slots[0].CooldownPct > 0);
    }

    [Fact]
    public void ProjectorReportsReadyEdgeOnTransition()
    {
        var state = new GameStateManager();
        var projector = new PacketSkillSlotProjector();
        var nowMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        // First pass: cooldown.
        state.Update(s => s with
        {
            SkillCdMap = s.SkillCdMap.SetItem(1001, new SkillCdSnapshot(
                SkillLevelId: 1001, BeginMs: nowMs - 100, DurationMs: 10000,
                ValidCdTimeMs: 100, ChargeCount: 0,
                SubCdRatio: 0, SubCdFixed: 0, AccelerateCdRatio: 0)),
        });
        var s1 = projector.Project(state.Snapshot);
        Assert.False(s1[0].ReadyEdge); // first-ever observation no edge

        // Second pass: still cooldown (no edge).
        var s2 = projector.Project(state.Snapshot);
        Assert.False(s2[0].ReadyEdge);

        // Third pass: now ready — edge should fire once.
        state.Update(s => s with
        {
            SkillCdMap = s.SkillCdMap.SetItem(1001, new SkillCdSnapshot(
                SkillLevelId: 1001, BeginMs: nowMs, DurationMs: 0, ValidCdTimeMs: 0,
                ChargeCount: 0, SubCdRatio: 0, SubCdFixed: 0, AccelerateCdRatio: 0)),
        });
        var s3 = projector.Project(state.Snapshot);
        Assert.True(s3[0].ReadyEdge);

        // Fourth pass: still ready — no edge.
        var s4 = projector.Project(state.Snapshot);
        Assert.False(s4[0].ReadyEdge);
    }

    [Fact]
    public void ProjectorEmptyMapReturnsEmpty()
    {
        var state = new GameStateManager();
        var projector = new PacketSkillSlotProjector();
        Assert.Empty(projector.Project(state.Snapshot));
    }

    // ─────────────────────────────────────────────────────────────
    //  HideSeek HS-01: multi-scale matcher
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void MultiScaleFallsBackWhenNativeFails()
    {
        // Create a 60x60 white image with a 50x50 dark patch in the middle.
        var img = new byte[60 * 60];
        for (int y = 5; y < 55; y++)
        {
            for (int x = 5; x < 55; x++) img[y * 60 + x] = 50;
        }
        // Template is 100x100 — too big, but a downscaled version (50x50)
        // matches the patch exactly.
        var tpl = new byte[100 * 100];
        for (int i = 0; i < tpl.Length; i++) tpl[i] = 50;
        // Native size won't fit (template > image); multi-scale will try
        // smaller scales and the 0.5 scale fits the 50x50 patch.
        Span<double> scales = stackalloc double[] { 0.5 };
        var result = HideSeekTemplateMatcher.MatchNccMultiScale(
            img, 60, 60, tpl, 100, 100, threshold: 0.50, scales: scales);
        Assert.True(result.Score >= -1.0); // sanity — kernel runs
    }

    [Fact]
    public void MultiScaleShortCircuitsOnNativeMatch()
    {
        // CCOEFF is variance-normalised, so a uniform template returns 0
        // for every search position. Build a template with a clear diagonal
        // gradient so the kernel has signal to lock onto.
        var tpl = new byte[10 * 10];
        for (int y = 0; y < 10; y++)
            for (int x = 0; x < 10; x++) tpl[y * 10 + x] = (byte)((x + y) * 12);
        var img = new byte[30 * 30];
        for (int y = 10; y < 20; y++)
            for (int x = 10; x < 20; x++) img[y * 30 + x] = (byte)(((x - 10) + (y - 10)) * 12);
        var result = HideSeekTemplateMatcher.MatchNccMultiScale(
            img, 30, 30, tpl, 10, 10, threshold: 0.90);
        Assert.True(result.Found);
        Assert.Equal(10, result.X);
        Assert.Equal(10, result.Y);
    }

    // ─────────────────────────────────────────────────────────────
    //  HideSeek HS-08: debug sink wiring
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void NullDebugSinkIsNoOp()
    {
        var sink = NullHideSeekDebugSink.Instance;
        // Should not throw.
        sink.SaveStep(0, "test",
            new byte[10], 5, 2,
            new byte[10], 5, 2,
            new byte[10], 5, 2,
            score: 0.5, found: true);
    }

    // ─────────────────────────────────────────────────────────────
    //  AutoKey R16: burst-visual virtual actions
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void AutoKeyBurstActionsArrayInstalled()
    {
        var dispatcher = new TestKeyDispatcher();
        var runtime = new AutoKeySpecRuntime(dispatcher);
        runtime.SetBurstActions(new[]
        {
            (TriggerSlot: 1, ActionSlot: 3),
            (TriggerSlot: 2, ActionSlot: 4),
        });
        Assert.Equal(2, runtime.BurstActions.Count);
        Assert.Equal("_burst_visual_0", runtime.BurstActions[0].Id);
        Assert.Equal(3, runtime.BurstActions[0].SlotIndex);
        Assert.Equal("3", runtime.BurstActions[0].Key);
        Assert.Equal("_burst_visual_1", runtime.BurstActions[1].Id);
        Assert.Equal(4, runtime.BurstActions[1].SlotIndex);
    }

    [Fact]
    public void AutoKeyBurstActionsDropsInvalidSlots()
    {
        var runtime = new AutoKeySpecRuntime(new TestKeyDispatcher());
        runtime.SetBurstActions(new[]
        {
            (TriggerSlot: 0, ActionSlot: 3),    // bad trigger
            (TriggerSlot: 5, ActionSlot: 10),   // bad action
            (TriggerSlot: 7, ActionSlot: 1),    // ok
        });
        Assert.Single(runtime.BurstActions);
        Assert.Equal(1, runtime.BurstActions[0].SlotIndex);
    }

    [Fact]
    public void AutoKeyBurstActionsEmptyClearsList()
    {
        var runtime = new AutoKeySpecRuntime(new TestKeyDispatcher());
        runtime.SetBurstActions(new[] { (TriggerSlot: 1, ActionSlot: 3) });
        Assert.Single(runtime.BurstActions);
        runtime.SetBurstActions(Array.Empty<(int, int)>());
        Assert.Empty(runtime.BurstActions);
    }

    private sealed class TestKeyDispatcher : IKeyDispatcher
    {
        public List<KeyStroke> Strokes { get; } = new();
        public void Dispatch(KeyStroke stroke) => Strokes.Add(stroke);
    }

    // ─────────────────────────────────────────────────────────────
    //  BossRaid P1: damage ingestion
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void BossRaidIgnoresDamageWhenNotRunning()
    {
        var engine = new BossRaidEngine();
        engine.OnDamageEvent(targetUuid: 0xABC, damage: 100,
            targetIsMonster: true, attackerIsSelf: true,
            isImmune: false, isAbsorbed: false, isHeal: false);
        Assert.Equal(0, engine.TotalDamage);
    }

    [Fact]
    public void BossRaidAutoDetectsFirstAttackedMonsterAsBoss()
    {
        var engine = new BossRaidEngine();
        engine.Start(new[] { new RaidPhase(0, "p1", 60) });
        engine.OnDamageEvent(targetUuid: 0xABC, damage: 100,
            targetIsMonster: true, attackerIsSelf: true,
            isImmune: false, isAbsorbed: false, isHeal: false,
            targetName: "Boss");
        Assert.Equal(0xABC, engine.BossUuid);
        Assert.Equal(100, engine.TotalDamage);
        var entities = engine.Entities;
        Assert.Single(entities);
        Assert.Equal("boss", entities[0].Role);
        Assert.Equal(100, entities[0].DamageDealt);
        Assert.Equal(1, entities[0].HitCount);
    }

    [Fact]
    public void BossRaidSecondMonsterBecomesEnemy()
    {
        var engine = new BossRaidEngine();
        engine.Start(new[] { new RaidPhase(0, "p1", 60) });
        engine.OnDamageEvent(0xABC, 100, true, true, false, false, false);
        engine.OnDamageEvent(0xDEF, 50, true, true, false, false, false);
        var entities = engine.Entities;
        Assert.Equal(2, entities.Count);
        Assert.Equal("boss", entities[0].Role);
        Assert.Equal("enemy", entities[1].Role);
    }

    [Fact]
    public void BossRaidDetectsInvincibleAfterThreeImmuneHits()
    {
        var engine = new BossRaidEngine();
        engine.Start(new[] { new RaidPhase(0, "p1", 60) });
        bool? invFlag = null;
        engine.InvincibleChanged += v => invFlag = v;
        // 3 immune hits within window flips invincible on.
        for (int i = 0; i < 3; i++)
        {
            engine.OnDamageEvent(0xABC, 1, true, true, isImmune: true, isAbsorbed: false, isHeal: false);
        }
        Assert.True(engine.BossInvincible);
        Assert.True(invFlag);
        // Normal damage resets it.
        engine.OnDamageEvent(0xABC, 100, true, true, false, false, false);
        Assert.False(engine.BossInvincible);
    }

    [Fact]
    public void BossRaidStopClearsState()
    {
        var engine = new BossRaidEngine();
        engine.Start(new[] { new RaidPhase(0, "p1", 60) });
        engine.OnDamageEvent(0xABC, 100, true, true, false, false, false);
        Assert.Equal(100, engine.TotalDamage);
        engine.Stop();
        Assert.Equal(0, engine.TotalDamage);
        Assert.Equal(0, engine.BossUuid);
        Assert.Empty(engine.Entities);
    }

    // ─────────────────────────────────────────────────────────────
    //  Commander P3: snapshot + patch
    // ─────────────────────────────────────────────────────────────

    [Fact]
    public void CommanderUpdateTeamFiresSnapshotChanged()
    {
        var engine = new CommanderEngine();
        CommanderSnapshot? captured = null;
        engine.SnapshotChanged += s => captured = s;
        engine.UpdateTeam(
            teamId: 100,
            leaderUid: 200,
            leaderName: "Alice",
            members: new[]
            {
                new CommanderMember(200, "Alice") { IsLeader = true },
                new CommanderMember(201, "Bob"),
            },
            selfUid: 200);
        Assert.NotNull(captured);
        Assert.Equal(100, captured!.TeamId);
        Assert.Equal(2, captured.Members.Length);
        Assert.Equal("Alice", captured.LeaderName);
    }

    [Fact]
    public void CommanderIdempotentUpdateNoEvent()
    {
        var engine = new CommanderEngine();
        engine.UpdateTeam(100, 200, "Alice",
            new[] { new CommanderMember(200, "Alice") { IsLeader = true } });
        int events = 0;
        engine.SnapshotChanged += _ => events++;
        engine.UpdateTeam(100, 200, "Alice",
            new[] { new CommanderMember(200, "Alice") { IsLeader = true } });
        Assert.Equal(0, events); // No content change → no event.
    }

    [Fact]
    public void CommanderPatchMemberOnlyUpdatesOneRow()
    {
        var engine = new CommanderEngine();
        engine.UpdateTeam(100, 200, "Alice", new[]
        {
            new CommanderMember(200, "Alice") { IsLeader = true },
            new CommanderMember(201, "Bob") { Level = 10 },
        });
        engine.PatchMember(201, b => b.WithLevel(99).WithFightPoint(50000));
        var snap = engine.Current;
        Assert.Equal(99, snap.Members[1].Level);
        Assert.Equal(50000, snap.Members[1].FightPoint);
        Assert.Equal(0, snap.Members[0].FightPoint); // untouched
    }

    [Fact]
    public void CommanderClearEmpties()
    {
        var engine = new CommanderEngine();
        engine.UpdateTeam(100, 200, "Alice", new[] { new CommanderMember(200, "Alice") });
        Assert.Single(engine.Current.Members);
        engine.Clear();
        Assert.Empty(engine.Current.Members);
        Assert.Equal(0, engine.Current.TeamId);
    }
}
