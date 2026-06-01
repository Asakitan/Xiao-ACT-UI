using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using System.Collections.Immutable;

namespace SaoAuto.Core.Bridge;

/// <summary>
/// Per-event-type mutator table — applies a <see cref="ParserEvent"/> to a
/// <see cref="GameState"/> via <see cref="GameStateManager.Update"/>.
/// Mirrors the per-handler `_apply_*` methods in
/// <c>sao_auto/packet_bridge.py</c>; one method per event keeps the
/// switch short and per-event tests trivial.
///
/// Each mutator returns true when it actually changed something. The
/// canonical "did this event matter" signal is used by
/// <see cref="PacketBridge.EventsApplied"/> for stats / smokes.
/// </summary>
public static class StateMutators
{
    /// <summary>
    /// R8 epic: thread-local collector for derived parser events emitted by
    /// scene-aware mutators (ApplyDungeonStart, ApplyEnterScene,
    /// ApplyDungeonData). The PacketBridge wraps each Apply() call by setting
    /// a fresh List here, then re-applying the collected events through
    /// itself so a single packet can fan out into the primary
    /// (DungeonStartEvent) + a derived (SceneChangeEvent) cleanly. Mutators
    /// that want to emit a derived event call <see cref="EmitDerived"/>.
    /// </summary>
    [ThreadStatic] private static List<ParserEvent>? _derivedEvents;

    // MAPSWITCH-06: dungeon target ids the reset-rule helper skips when
    // detecting new_objective / target_completed signals. Mirrors Python's
    // _RESET_IGNORE_TARGETS frozenset at packet_parser.py:110-112 — these
    // are quest-style "watch the player" objectives that fire spuriously
    // and would otherwise trigger a soft restart every few seconds.
    private static readonly HashSet<int> _resetIgnoreTargets = new()
    {
        1301104, 1301105, 1301106, 6521002, 6521003, 1083, 1302101,
    };

    // MAPSWITCH-06 / per-instance: the last seen "active" objective id so a
    // target_completed signal with target_id=0 can be re-attributed back to
    // the most-recent objective. Mirrors Python's
    // `self._active_dungeon_target_id` set inside _apply_dungeon_target_reset_rules.
    [ThreadStatic] private static int _activeDungeonTargetId;

    /// <summary>Begin collecting derived events for the duration of a
    /// single Apply() call; returns the previous collector so callers can
    /// nest cleanly. PacketBridge always pairs Begin/End around Apply().</summary>
    internal static List<ParserEvent>? BeginDerivedCapture(out List<ParserEvent> sink)
    {
        var prev = _derivedEvents;
        sink = new List<ParserEvent>(0);
        _derivedEvents = sink;
        return prev;
    }

    internal static void EndDerivedCapture(List<ParserEvent>? prior)
    {
        _derivedEvents = prior;
    }

    private static void EmitDerived(ParserEvent ev)
    {
        _derivedEvents?.Add(ev);
    }

    /// <summary>
    /// MAPSWITCH-06: scan a dungeon target list for new_objective /
    /// target_completed signals and emit a soft scene restart per match.
    /// Mirrors Python's <c>_apply_dungeon_target_reset_rules</c> at
    /// packet_parser.py:1610-1647. The ignore set drops quest-style watcher
    /// targets that would otherwise spuriously trigger every few seconds.
    /// </summary>
    private static void EvaluateDungeonTargetResetRules(
        IReadOnlyList<DungeonTargetProgress> targets,
        string source,
        double timestampSeconds)
    {
        if (targets is null || targets.Count == 0) return;
        foreach (var t in targets)
        {
            string? reason = null;
            var effectiveId = t.TargetId;
            if (t.Complete == 0 && t.Nums == 0)
            {
                _activeDungeonTargetId = t.TargetId;
                reason = "new_objective";
            }
            else if (t.Complete == 1 && t.Nums > 0)
            {
                if (t.TargetId == 0 && _activeDungeonTargetId != 0)
                {
                    effectiveId = _activeDungeonTargetId;
                }
                reason = "target_completed";
            }
            if (reason is null) continue;
            if (_resetIgnoreTargets.Contains(effectiveId)) continue;
            EmitDerived(new SceneChangeEvent(
                SceneChangeKind.Restart,
                $"dungeon_{reason}:{source}:target={effectiveId}",
                PreserveCombat: true,
                ResetOnNextDamage: true,
                ResetDelaySeconds: 3.0,
                TimestampSeconds: timestampSeconds));
        }
    }

    public static bool Apply(GameStateManager state, ParserEvent ev)
    {
        return ev switch
        {
            IdentityEvent id => ApplyIdentity(state, id),
            HealthEvent hp => ApplyHealth(state, hp),
            StaminaEvent st => ApplyStamina(state, st),
            ServerTimeEvent srv => ApplyServerTime(state, srv),
            ReviveEvent rv => ApplyRevive(state, rv),
            KickOffEvent _ => ApplyKickOff(state),
            EnterGameEvent enter => ApplyEnterGame(state, enter),
            CombatStateEvent cs => ApplyCombatState(state, cs),
            DungeonStartEvent ds => ApplyDungeonStart(state, ds),
            EnterSceneEvent es => ApplyEnterScene(state, es),
            SceneChangeEvent _ or SoftSceneRestartEvent _ => false, // routed by PacketBridge subscribers
            BuffSnapshotEvent buff => ApplyBuffs(state, buff),
            BossHpEvent boss => ApplyBossHp(state, boss),
            DpsEvent dps => ApplyDps(state, dps),
            IdentityAlertEvent alert => ApplyIdentityAlert(state, alert),
            SkillUseEvent skUse => ApplySkillUse(state, skUse),
            DungeonDataEvent dd => ApplyDungeonData(state, dd),
            DungeonDirtyDataEvent ddd => ApplyDungeonDirtyData(state, ddd),
            ContainerSyncEvent csync => ApplyContainerSync(state, csync),
            ContainerDirtyEvent cdirty => ApplyContainerDirty(state, cdirty),
            NearEntitiesEvent ne => ApplyNearEntities(state, ne),
            NearDeltaEvent nd => ApplyNearDelta(state, nd),
            ToMeDeltaEvent me => ApplyToMeDelta(state, me),
            ToMeFightResCdEvent fres => ApplyToMeFightResCd(state, fres),
            TempAttrCdEvent tac => ApplyTempAttrCd(state, tac),
            AoiBuffSyncEvent abs => ApplyAoiBuffSync(state, abs),
            BuffEffectEvent be => ApplyBuffEffect(state, be),
            PlayerAttrEvent pa => ApplyPlayerAttr(state, pa),
            SkillEffectEvent se => ApplySkillEffect(state, se),
            // Marker events that don't mutate state on their own — caller
            // can still observe via Subscribe → no-op here.
            AllMemberReadyEvent _ or CaptainReadyEvent _ => false,
            BuffChangeEvent _ or SkillEndEvent _ or SkillStageEndEvent _ or QteBeginEvent _ => false,
            _ => false,
        };
    }

    private static bool ApplyIdentity(GameStateManager state, IdentityEvent id)
    {
        state.Update(s => s with
        {
            PlayerName = id.PlayerName,
            PlayerId = id.PlayerId,
            LevelBase = id.LevelBase,
            LevelExtra = id.LevelExtra,
            SeasonExp = id.SeasonExp,
            FightPoint = id.FightPoint,
            ProfessionId = id.ProfessionId,
            ProfessionName = id.ProfessionName,
            PacketActive = true,
        });
        return true;
    }

    private static bool ApplyHealth(GameStateManager state, HealthEvent hp)
    {
        // S102: route HP through ApplyPartial so the rollback/clamp/cap
        // pipeline runs; PacketActive flips in the same atomic snapshot.
        var partial = new StatePartial
        {
            HpCurrent = hp.HpCurrent,
            HpMax = hp.HpMax,
            HpPct = hp.HpPct,
        };
        state.ApplyPartial(partial, s => s with { PacketActive = true });
        return true;
    }

    private static bool ApplyStamina(GameStateManager state, StaminaEvent st)
    {
        // S102: route stamina through ApplyPartial so the rollback rule
        // (skip-when-explicit-pct, skip-when-stamina_max>0) runs.
        var partial = new StatePartial
        {
            StaminaCurrent = (int)Math.Round(st.StaminaCurrent),
            StaminaMax = (int)Math.Min(int.MaxValue, st.StaminaMax),
            StaminaPct = st.StaminaPct,
        };
        state.ApplyPartial(partial, s => s with { StaminaOffline = st.Offline });
        return true;
    }

    private static bool ApplyServerTime(GameStateManager state, ServerTimeEvent srv)
    {
        state.Update(s => s with { ServerTimeOffsetMs = srv.OffsetMs });
        return true;
    }

    private static bool ApplyRevive(GameStateManager state, ReviveEvent _)
    {
        // S102: HpPct goes through the pct clamp on the way through.
        state.ApplyPartial(
            new StatePartial { HpPct = 1.0 },
            s => s with { InCombat = false });
        return true;
    }

    private static bool ApplyKickOff(GameStateManager state)
    {
        state.Update(s => s with
        {
            PacketActive = false,
            InCombat = false,
            ErrorMsg = "session ended (kicked)",
        });
        return true;
    }

    private static bool ApplyEnterGame(GameStateManager state, EnterGameEvent ev)
    {
        // MSR-8: cross-server / re-login transition. When SelfUuid is rolling
        // to a NEW non-zero value, zero out the per-scene memos (DungeonId,
        // SceneId, SceneKey, SyncContainerCount) so subsequent decoders treat
        // the next scene as "first observation" instead of comparing against
        // stale memos from the prior session. Mirrors Python's reset_scene()
        // + _on_enter_game reset chain at packet_parser.py:1525-1526 / 2566-2571.
        state.Update(s =>
        {
            var crossServer = s.SelfUuid != 0 && s.SelfUuid != ev.SelfUuid;
            return s with
            {
                PacketActive = true,
                ErrorMsg = string.Empty,
                SelfUuid = ev.SelfUuid,
                LastDungeonId = crossServer ? 0 : s.LastDungeonId,
                LastSceneId = crossServer ? 0 : s.LastSceneId,
                LastSceneKey = crossServer ? 0 : s.LastSceneKey,
                SyncContainerCount = crossServer ? 0 : s.SyncContainerCount,
                DungeonSceneUuid = crossServer ? 0 : s.DungeonSceneUuid,
            };
        });
        return true;
    }

    private static bool ApplyCombatState(GameStateManager state, CombatStateEvent cs)
    {
        state.Update(s => s with { InCombat = cs.InCombat });
        return true;
    }

    /// <summary>
    /// MSR-2 / MAPSWITCH-03: NotifyStartPlayingDungeon handling. When the
    /// dungeon id changes from a previously-observed value, this is a soft
    /// scene restart (party finished a different dungeon and pulled the
    /// next one) — preserve combat, defer DPS / boss-bar reset to the next
    /// incoming damage event so the encounter still finalizes cleanly.
    /// Same dungeon id == retry: just clear InCombat for the brief
    /// pre-pull window. Python branches at packet_parser.py:2488-2541.
    /// </summary>
    private static bool ApplyDungeonStart(GameStateManager state, DungeonStartEvent ev)
    {
        SceneChangeEvent? emit = null;
        state.Update(s =>
        {
            var prior = s.LastDungeonId;
            var isSoftRestart = ev.DungeonId != 0 && prior != 0 && prior != ev.DungeonId;
            if (isSoftRestart)
            {
                emit = new SceneChangeEvent(
                    SceneChangeKind.Restart,
                    $"notify_start_playing_dungeon:{prior}->{ev.DungeonId}",
                    PreserveCombat: true,
                    ResetOnNextDamage: true,
                    ResetDelaySeconds: 3.0,
                    TimestampSeconds: ev.TimestampSeconds);
            }
            return s with
            {
                // Match Python: don't flip InCombat off on a soft restart so
                // the prior encounter window stays open until the deferred
                // reset fires. Hard flip only when we don't have prior
                // context yet (first dungeon of the session).
                InCombat = isSoftRestart ? s.InCombat : false,
                LastDungeonId = ev.DungeonId != 0 ? ev.DungeonId : s.LastDungeonId,
            };
        });
        if (emit is not null) EmitDerived(emit);
        return true;
    }

    /// <summary>
    /// MSR-3 / MAPSWITCH-07: EnterScene with extracted SceneBasicId +
    /// PlayerUuid. Latches SelfUuid when missing (covers the boot path where
    /// EnterGame hasn't fired yet) and emits a soft scene transition when
    /// SceneBasicId rolls. Mirrors Python's _on_enter_scene head at
    /// packet_parser.py:2334-2376.
    /// </summary>
    private static bool ApplyEnterScene(GameStateManager state, EnterSceneEvent ev)
    {
        SceneChangeEvent? emit = null;
        state.Update(s =>
        {
            var nextSelf = (s.SelfUuid == 0 && ev.PlayerUuid != 0) ? ev.PlayerUuid : s.SelfUuid;
            var prior = s.LastSceneId;
            if (ev.SceneBasicId != 0 && prior != 0 && prior != ev.SceneBasicId)
            {
                emit = new SceneChangeEvent(
                    SceneChangeKind.Transition,
                    $"enter_scene_basic_id_changed:{prior}->{ev.SceneBasicId}",
                    PreserveCombat: true,
                    ResetOnNextDamage: false,
                    ResetDelaySeconds: 0.0,
                    TimestampSeconds: ev.TimestampSeconds);
            }
            return s with
            {
                SelfUuid = nextSelf,
                LastSceneId = ev.SceneBasicId != 0 ? ev.SceneBasicId : s.LastSceneId,
            };
        });
        if (emit is not null) EmitDerived(emit);
        return true;
    }

    private static bool ApplyBuffs(GameStateManager state, BuffSnapshotEvent buff)
    {
        var entries = buff.Buffs.Select(b => new BuffEntry
        {
            Id = b.Id,
            Uuid = b.Uuid,
            BeginMs = b.BeginMs,
            DurationMs = b.DurationMs,
            Layer = b.Layer,
            Count = b.Count,
            Name = b.Name,
        }).ToImmutableArray();

        state.Update(s => s with
        {
            SelfBuffs = entries,
            ServerTimeOffsetMs = buff.ServerTimeOffsetMs,
        });
        return true;
    }

    private static bool ApplyBossHp(GameStateManager state, BossHpEvent boss)
    {
        state.Update(s => s with
        {
            BossCurrentHp = (int)Math.Min(int.MaxValue, boss.CurrentHp),
            BossTotalHp = (int)Math.Min(int.MaxValue, boss.MaxHp),
            BossHpEstPct = boss.EstimatedPct,
            BossShieldActive = boss.ShieldActive,
            BossShieldPct = boss.ShieldPct,
            BossBreakingStage = boss.BreakingStage,
            BossInOverdrive = boss.InOverdrive,
            BossInvincible = boss.Invincible,
            BossHpSource = BossHpSource.Packet,
            BossHpLastPacketSeconds = boss.TimestampSeconds,
        });
        return true;
    }

    // S120 — projector freshness window. While the last BossHpEvent is
    // newer than this many seconds, the packet path wins and the
    // monster-table projector is a no-op. Mirrors Python's "BossHpEvent
    // always wins; estimate fills gaps" precedence.
    private const double BossPacketFreshnessSeconds = 5.0;

    /// <summary>
    /// S120 — pick the active boss row from <paramref name="monsters"/>
    /// and return <paramref name="s"/> with <c>Boss*</c> mirrored from it,
    /// when no fresh <see cref="BossHpEvent"/> is currently winning. The
    /// selector prefers the monster currently aggro'd on the local
    /// player (<c>HatedCharId == SelfUuid</c>); fallback is the live
    /// Entmonster with the highest <c>MaxHp</c>. Dead rows
    /// (<c>IsDead</c> or <c>MaxHp &lt;= 0</c>) are filtered out.
    /// Pure: returns <paramref name="s"/> unchanged when nothing should
    /// move (callers can <c>ReferenceEquals</c>-skip).
    /// </summary>
    private static GameState ProjectBossFromMonsters(GameState s, double evTs)
    {
        if (s.BossHpSource == BossHpSource.Packet
            && evTs - s.BossHpLastPacketSeconds < BossPacketFreshnessSeconds)
        {
            return s;
        }
        MonsterData? best = null;
        var selfMatch = s.SelfUuid != 0 ? (long)s.SelfUuid : 0L;
        foreach (var (_, mon) in s.MonsterDataMap)
        {
            if (mon.IsDead || mon.MaxHp <= 0) continue;
            if (best is null) { best = mon; continue; }
            // Aggro on Self always wins.
            var bestSelf = selfMatch != 0 && best.HatedCharId == selfMatch;
            var monSelf = selfMatch != 0 && mon.HatedCharId == selfMatch;
            if (monSelf && !bestSelf) { best = mon; continue; }
            if (bestSelf && !monSelf) continue;
            // Tiebreak: highest MaxHp.
            if (mon.MaxHp > best.MaxHp) best = mon;
        }
        if (best is null)
        {
            // Only clear when we previously projected — never blank a
            // packet-sourced row from the projector path.
            if (s.BossHpSource == BossHpSource.MonsterData)
            {
                return s with
                {
                    BossCurrentHp = 0,
                    BossTotalHp = 0,
                    BossHpEstPct = 0.0,
                    BossShieldActive = false,
                    BossShieldPct = 0.0,
                    BossBreakingStage = -1,
                    BossInOverdrive = false,
                    BossHpSource = BossHpSource.None,
                };
            }
            return s;
        }
        var pct = best.MaxHp > 0 ? (double)best.Hp / best.MaxHp : 0.0;
        var shieldPct = best.ShieldMaxTotal > 0
            ? (double)best.ShieldTotal / best.ShieldMaxTotal
            : 0.0;
        var projected = s with
        {
            BossCurrentHp = best.Hp,
            BossTotalHp = best.MaxHp,
            BossHpEstPct = pct,
            BossShieldActive = best.ShieldActive,
            BossShieldPct = shieldPct,
            BossBreakingStage = best.BreakingStage,
            BossInOverdrive = best.InOverdrive,
            BossHpSource = BossHpSource.MonsterData,
        };
        return projected.Equals(s) ? s : projected;
    }

    private static bool ApplyDps(GameStateManager state, DpsEvent dps)
    {
        state.Update(s => s with
        {
            BossTotalDamage = (int)Math.Min(int.MaxValue, dps.TotalDamage),
            BossDps = (int)Math.Min(int.MaxValue, dps.Dps),
        });
        return true;
    }

    private static bool ApplyIdentityAlert(GameStateManager state, IdentityAlertEvent alert)
    {
        state.Update(s => s with
        {
            IdentityAlertSerial = alert.Serial,
            IdentityAlertTitle = alert.Title,
            IdentityAlertMessage = alert.Message,
        });
        return true;
    }

    private static bool ApplySkillUse(GameStateManager state, SkillUseEvent ev)
    {
        // PROTO-02: also try auto-detecting profession from the observed
        // skill id when SyncContainerData hasn't yet confirmed it. Mirrors
        // Python's `_remember_seen_skill` + `_try_detect_profession` chain
        // at packet_parser.py:1866 / 1832. The seen-set is best-effort
        // observation only; profession + sub-profession get written into
        // GameState when the reverse table matches.
        state.Update(s =>
        {
            var nextLastUse = s.SkillLastUseAt.SetItem(ev.SkillLevelId, ev.TimestampSeconds);
            var nextProfession = s.ProfessionId;
            var nextProfessionName = s.ProfessionName;
            // Only auto-detect when profession is currently unknown.
            if (s.ProfessionId == 0 && ev.SkillLevelId > 0)
            {
                var pid = Automation.SkillObserver.DetectProfession(ev.SkillLevelId);
                if (pid > 0)
                {
                    nextProfession = pid;
                    nextProfessionName = Automation.SkillObserver.ProfessionNames
                        .TryGetValue(pid, out var pname) ? pname : string.Empty;
                }
            }
            return s with
            {
                SkillLastUseAt = nextLastUse,
                ProfessionId = nextProfession,
                ProfessionName = nextProfessionName,
            };
        });
        return true;
    }

    private static bool ApplyDungeonData(GameStateManager state, DungeonDataEvent ev)
    {
        // MSR-4 / MAPSWITCH-04: mid-fight scene-uuid roll signals a soft scene
        // transition (multi-phase boss layer switch, sub-area swap). Combat
        // is preserved across the boundary and no DPS reset arms — but the
        // DPS / boss-bar subscribers need to see the SceneChangeEvent so
        // stale monster targeting / loot rows can be purged. Mirrors Python's
        // _on_sync_dungeon_data at packet_parser.py:2421-2446.
        SceneChangeEvent? emit = null;
        state.Update(s =>
        {
            var prior = s.DungeonSceneUuid;
            if (prior != 0 && ev.SceneUuid != 0 && prior != ev.SceneUuid)
            {
                emit = new SceneChangeEvent(
                    SceneChangeKind.Transition,
                    $"sync_dungeon_scene_uuid_changed:{prior}->{ev.SceneUuid}",
                    PreserveCombat: true,
                    ResetOnNextDamage: false,
                    ResetDelaySeconds: 0.0,
                    TimestampSeconds: ev.TimestampSeconds);
            }
            return s with
            {
                DungeonSceneUuid = ev.SceneUuid,
                DungeonDifficulty = ev.DungeonDifficulty,
                DungeonTargets = ev.Targets.ToImmutableArray(),
            };
        });
        if (emit is not null) EmitDerived(emit);
        // MAPSWITCH-06: per-target reset rule scan. Runs after the snapshot
        // is updated so observers see the new target list before the soft
        // restart fires. Source label mirrors Python's
        // `_apply_dungeon_target_reset_rules(targets, 'sync_dungeon_data')`.
        EvaluateDungeonTargetResetRules(ev.Targets, "sync_dungeon_data", ev.TimestampSeconds);
        return true;
    }

    private static bool ApplyDungeonDirtyData(GameStateManager state, DungeonDirtyDataEvent ev)
    {
        state.Update(s => s with
        {
            DungeonFlowState = ev.FlowState,
            // Only overwrite targets when the dirty buffer carried any —
            // an empty list means "no change", matching Python's
            // _apply_dungeon_target_reset_rules(targets) only-if-truthy.
            DungeonTargets = ev.Targets.Count > 0
                ? ev.Targets.ToImmutableArray()
                : s.DungeonTargets,
        });
        // MAPSWITCH-06: dirty-data path also fires the target reset rules
        // when the dirty buffer carried any targets — mirrors Python's
        // _apply_dungeon_target_reset_rules call inside _on_sync_dungeon_dirty.
        if (ev.Targets.Count > 0)
        {
            EvaluateDungeonTargetResetRules(ev.Targets, "sync_dungeon_dirty_data", ev.TimestampSeconds);
        }
        return true;
    }

    private static bool ApplyContainerSync(GameStateManager state, ContainerSyncEvent ev)
    {
        // Mirrors top-level identity assignment in Python's
        // _on_sync_container_data: name/level/fight-point + HP. We
        // overwrite the name only when non-empty so a partial sync
        // doesn't blank an existing player display. HpPct is recomputed
        // in the same shape as ApplyHealth so downstream consumers
        // (HP bar) don't see stale values.
        var cur = (int)Math.Min(int.MaxValue, ev.CurHp);
        var max = (int)Math.Min(int.MaxValue, ev.MaxHp);
        var pct = max > 0 ? Math.Clamp((double)cur / max, 0.0, 1.0) : 1.0;
        // S102: HP + LevelBase go through ApplyPartial; identity fields
        // (PlayerName, FightPoint, PacketActive) ride along in the same
        // atomic snapshot via the extraMutate hook.
        var partial = new StatePartial
        {
            HpCurrent = cur,
            HpMax = max,
            HpPct = pct,
            LevelBase = ev.Level > 0 ? ev.Level : null,
        };
        // MAPSWITCH-05: detect SceneKey changes inside the same MapId before
        // overwriting LastSceneKey. A non-zero prior + non-zero new + diff
        // means the player moved into a different channel / plane / layer
        // mid-fight (multi-phase boss layer switch); emit a soft transition
        // so DPS / BossHP subscribers can decide whether to purge stale
        // monsters / boss rows. Mirrors Python's _notify_soft_scene_transition
        // call at packet_parser.py:3148-3155.
        SceneChangeEvent? sceneEmit = null;
        state.ApplyPartial(partial, s =>
        {
            var prior = s.LastSceneKey;
            if (ev.SceneKey != 0 && prior != 0 && prior != ev.SceneKey)
            {
                sceneEmit = new SceneChangeEvent(
                    SceneChangeKind.Transition,
                    "scene_key_changed",
                    PreserveCombat: true,
                    ResetOnNextDamage: false,
                    ResetDelaySeconds: 0.0,
                    TimestampSeconds: ev.TimestampSeconds);
            }
            return s with
            {
                PlayerName = string.IsNullOrEmpty(ev.Name) ? s.PlayerName : ev.Name,
                FightPoint = ev.FightPoint > 0 ? ev.FightPoint : s.FightPoint,
                PacketActive = true,
                LastSceneKey = ev.SceneKey != 0 ? ev.SceneKey : s.LastSceneKey,
                SyncContainerCount = s.SyncContainerCount + 1,
            };
        });
        if (sceneEmit is not null) EmitDerived(sceneEmit);
        return true;
    }

    private static bool ApplyContainerDirty(GameStateManager state, ContainerDirtyEvent ev)
    {
        // Mirrors per-(field,sub) state writes in Python's _parse_dirty_stream.
        // Only the supported subset reaches this mutator (decoder drops the
        // rest); guards on zero / negative values match Python's
        // "ignore CurHp=0 unless max_hp==0", "ignore MaxHp=0", "level > 0".
        var ch = ev.Change;
        switch ((ch.FieldIndex, ch.SubField))
        {
            case (2, 5): // CharBase.Name
                if (string.IsNullOrEmpty(ch.StringValue)) return false;
                state.Update(s => s with { PlayerName = ch.StringValue });
                return true;
            case (2, 35): // CharBase.FightPoint
                if (ch.IntValue is null or <= 0) return false;
                state.Update(s => s with { FightPoint = (int)Math.Min(int.MaxValue, ch.IntValue.Value) });
                return true;
            case (16, 1): // UserFightAttr.CurHp
            {
                if (ch.IntValue is null) return false;
                var cur = (int)Math.Min(int.MaxValue, ch.IntValue.Value);
                // Python: ignore hp=0 unless max_hp==0 (avoids respawn-flicker).
                if (cur == 0 && state.Snapshot.HpMax > 0) return false;
                state.Update(s => s with
                {
                    HpCurrent = cur,
                    HpPct = s.HpMax > 0 ? Math.Clamp((double)cur / s.HpMax, 0.0, 1.0) : s.HpPct,
                });
                return true;
            }
            case (16, 2): // UserFightAttr.MaxHp
            {
                if (ch.IntValue is null or <= 0) return false;
                var max = (int)Math.Min(int.MaxValue, ch.IntValue.Value);
                state.Update(s => s with
                {
                    HpMax = max,
                    HpPct = max > 0 ? Math.Clamp((double)s.HpCurrent / max, 0.0, 1.0) : s.HpPct,
                });
                return true;
            }
            case (16, 3): // UserFightAttr.OriginEnergy — float interp matches Python default
                // No GameState slot for stamina-from-packet yet; ack and drop.
                return false;
            case (22, 1): // RoleLevel.Level
                if (ch.IntValue is null or <= 0) return false;
                state.Update(s => s with { LevelBase = (int)Math.Min(int.MaxValue, ch.IntValue.Value) });
                return true;
            default:
                return false;
        }
    }

    private static bool ApplyNearEntities(GameStateManager state, NearEntitiesEvent ev)
    {
        // Mirrors Python: appearances add to near_entities (preserve
        // first_seen on dedup); disappearances remove. No-op if both
        // lists are empty so EventsApplied stays accurate.
        if (ev.Appear.Count == 0 && ev.Disappear.Count == 0) return false;
        var changed = false;
        state.Update(s =>
        {
            var dict = s.NearEntities;
            var monsters = s.MonsterDataMap;
            foreach (var app in ev.Appear)
            {
                if (!dict.ContainsKey(app.Uuid))
                {
                    dict = dict.SetItem(app.Uuid,
                        new EntityTableEntry(app.EntityType, ev.TimestampSeconds,
                            CurHp: app.CurHp, MaxHp: app.MaxHp, Level: app.Level));
                }
                // S113: lazy-create MonsterData for Entmonster (=1) only.
                // Mirrors Python's _get_monster (packet_parser.py 1445)
                // first-touch behaviour. Non-monster entities (player,
                // pet, NPC) skip the table.
                if (app.EntityType == 1 && !monsters.ContainsKey(app.Uuid))
                {
                    // S115: seed break-gauge from app.Attrs (struct-on-event)
                    // when the appearance carried any break-gauge attrs.
                    // Mirrors Python's first-touch in _process_monster_attr_collection
                    // — lazy max-extinction estimation runs the same here.
                    var a = app.Attrs;
                    var seedMaxExt = a.HasMaxExtinction ? a.MaxExtinction
                        : (a.HasExtinction && a.Extinction > 0 ? a.Extinction : 0);
                    var seedMaxStun = a.HasMaxStunned ? a.MaxStunned
                        : (a.HasStunned && a.Stunned > 0 ? a.Stunned : 0);
                    monsters = monsters.SetItem(app.Uuid, new MonsterData
                    {
                        Uuid = app.Uuid,
                        Uid = app.Uuid & 0xFFFFFFFFL,
                        Hp = app.CurHp,
                        MaxHp = app.MaxHp,
                        Level = app.Level,
                        Name = app.Name,
                        TemplateId = app.TemplateId,
                        BreakingStage = a.HasBreakingStage ? a.BreakingStage : -1,
                        Extinction = a.Extinction,
                        MaxExtinction = seedMaxExt,
                        Stunned = a.Stunned,
                        MaxStunned = seedMaxStun,
                        InOverdrive = a.InOverdrive,
                        ShieldTotal = a.HasShield ? a.ShieldTotal : 0,
                        ShieldMaxTotal = a.HasShield ? a.ShieldMaxTotal : 0,
                        ShieldActive = a.HasShield && a.ShieldTotal > 0,
                        // S117: extended monster flags / attrs.
                        IsLockStunned = a.HasIsLockStunned && a.IsLockStunned,
                        StopBreakingTicking = a.HasStopBreakingTicking && a.StopBreakingTicking,
                        State = a.HasState ? a.State : 0,
                        DeadType = a.HasDeadType ? a.DeadType : 0,
                        DeadTime = a.HasDeadTime ? a.DeadTime : 0,
                        FirstAttack = a.HasFirstAttack && a.FirstAttack,
                        HatedCharId = a.HasHatedCharId ? a.HatedCharId : 0,
                        HatedCharName = a.HasHatedCharName ? a.HatedCharName : string.Empty,
                        LastUpdateSeconds = ev.TimestampSeconds,
                    });
                }
            }
            foreach (var dis in ev.Disappear)
            {
                if (dict.ContainsKey(dis.Uuid))
                    dict = dict.Remove(dis.Uuid);
                if (monsters.ContainsKey(dis.Uuid))
                    monsters = monsters.Remove(dis.Uuid);
            }
            var entitiesChanged = !ReferenceEquals(dict, s.NearEntities);
            var monstersChanged = !ReferenceEquals(monsters, s.MonsterDataMap);
            if (!entitiesChanged && !monstersChanged) return s;
            changed = true;
            // S120: project Boss* off the new monster table in the same
            // atomic snapshot so JS subscribers can never see a Boss row
            // that disagrees with MonsterDataMap.
            var next = s with { NearEntities = dict, MonsterDataMap = monsters };
            return ProjectBossFromMonsters(next, ev.TimestampSeconds);
        });
        return changed;
    }

    private static bool ApplyNearDelta(GameStateManager state, NearDeltaEvent ev)
    {
        // S111: per-uuid HP/MaxHp/Level updates from SyncNearDeltaInfo.
        // Only mutates rows that already exist in the entity table —
        // delta packets stream live updates for known entities, but the
        // appearance lifecycle is owned by SyncNearEntities (S110). A
        // delta for an unknown uuid is dropped (no auto-create) so the
        // table stays appearance-scoped. Per-attr zero means "not in
        // this packet" → carry forward the existing slot value.
        if (ev.AttrUpdates.Count == 0) return false;
        var changed = false;
        state.Update(s =>
        {
            var dict = s.NearEntities;
            var monsters = s.MonsterDataMap;
            foreach (var upd in ev.AttrUpdates)
            {
                // S112: prefer the explicit presence flag when set; falls
                // back to the legacy "non-zero = present" rule for callers
                // built before S112 (preserves S111 test semantics).
                var hasCurHp = upd.HasCurHp ?? upd.CurHp != 0;
                var hasMaxHp = upd.HasMaxHp ?? upd.MaxHp != 0;
                var hasLevel = upd.HasLevel ?? upd.Level != 0;
                if (dict.TryGetValue(upd.Uuid, out var row))
                {
                    var newRow = row with
                    {
                        CurHp = hasCurHp ? upd.CurHp : row.CurHp,
                        MaxHp = hasMaxHp ? upd.MaxHp : row.MaxHp,
                        Level = hasLevel ? upd.Level : row.Level,
                    };
                    if (!newRow.Equals(row)) dict = dict.SetItem(upd.Uuid, newRow);
                }
                // S113: MonsterData mirror — update Hp/MaxHp/Level on the
                // monster row if one exists (lazy-created in
                // ApplyNearEntities for Entmonster). A real HP=0
                // (HasCurHp=true) flips IsDead so downstream consumers
                // can finally observe deaths.
                if (monsters.TryGetValue(upd.Uuid, out var mon))
                {
                    // S114: Name/TemplateId presence falls back to "non-
                    // empty/non-zero = present" for legacy callers, same
                    // pattern as S112's HasCurHp. Name uses
                    // first-non-empty-wins to match Python — once we have
                    // a name, a delta with empty name shouldn't blank it.
                    var hasName = upd.HasName ?? !string.IsNullOrEmpty(upd.Name);
                    var hasTemplateId = upd.HasTemplateId ?? upd.TemplateId != 0;
                    // S115: break-gauge mirror. Reads from upd.Attrs
                    // (struct-on-event) so the legacy init slots above
                    // stay frozen at 4 positional + 7 init. Python's
                    // lazy-estimation rules:
                    //   * MAX_EXTINCTION: only writes when value > 0 (skip 0s)
                    //   * EXTINCTION: writes anytime; if monster.max_ext==0
                    //     and value>0, seed max_ext = value; if value > old
                    //     max_ext > 0, raise max_ext = value (recovery).
                    //   * Same pair for STUNNED / MAX_STUNNED.
                    //   * IN_OVERDRIVE: writes anytime (real false ends rage).
                    //   * BREAKING_STAGE: writes anytime when present.
                    var a = upd.Attrs;
                    var newMaxExt = mon.MaxExtinction;
                    if (a.HasMaxExtinction && a.MaxExtinction > 0) newMaxExt = a.MaxExtinction;
                    if (a.HasExtinction)
                    {
                        if (newMaxExt == 0 && a.Extinction > 0) newMaxExt = a.Extinction;
                        else if (a.Extinction > newMaxExt && newMaxExt > 0) newMaxExt = a.Extinction;
                    }
                    var newMaxStun = mon.MaxStunned;
                    if (a.HasMaxStunned && a.MaxStunned > 0) newMaxStun = a.MaxStunned;
                    if (a.HasStunned)
                    {
                        if (newMaxStun == 0 && a.Stunned > 0) newMaxStun = a.Stunned;
                        else if (a.Stunned > newMaxStun && newMaxStun > 0) newMaxStun = a.Stunned;
                    }
                    var newMon = mon with
                    {
                        Hp = hasCurHp ? upd.CurHp : mon.Hp,
                        MaxHp = hasMaxHp ? upd.MaxHp : mon.MaxHp,
                        Level = hasLevel ? upd.Level : mon.Level,
                        Name = hasName && !string.IsNullOrEmpty(upd.Name) ? upd.Name : mon.Name,
                        TemplateId = hasTemplateId ? upd.TemplateId : mon.TemplateId,
                        BreakingStage = a.HasBreakingStage ? a.BreakingStage : mon.BreakingStage,
                        Extinction = a.HasExtinction ? a.Extinction : mon.Extinction,
                        MaxExtinction = newMaxExt,
                        Stunned = a.HasStunned ? a.Stunned : mon.Stunned,
                        MaxStunned = newMaxStun,
                        InOverdrive = a.HasInOverdrive ? a.InOverdrive : mon.InOverdrive,
                        ShieldTotal = a.HasShield ? a.ShieldTotal : mon.ShieldTotal,
                        ShieldMaxTotal = a.HasShield ? a.ShieldMaxTotal : mon.ShieldMaxTotal,
                        ShieldActive = a.HasShield ? a.ShieldTotal > 0 : mon.ShieldActive,
                        // S117: extended monster flags. Same carry-forward
                        // pattern as S115 break-gauge: explicit Has* gates a
                        // real overwrite, otherwise keep the seeded value.
                        // HatedCharName preserved when delta omits it (Python
                        // never blanks the aggro display once observed).
                        IsLockStunned = a.HasIsLockStunned ? a.IsLockStunned : mon.IsLockStunned,
                        StopBreakingTicking = a.HasStopBreakingTicking ? a.StopBreakingTicking : mon.StopBreakingTicking,
                        State = a.HasState ? a.State : mon.State,
                        DeadType = a.HasDeadType ? a.DeadType : mon.DeadType,
                        DeadTime = a.HasDeadTime ? a.DeadTime : mon.DeadTime,
                        FirstAttack = a.HasFirstAttack ? a.FirstAttack : mon.FirstAttack,
                        HatedCharId = a.HasHatedCharId ? a.HatedCharId : mon.HatedCharId,
                        HatedCharName = a.HasHatedCharName && !string.IsNullOrEmpty(a.HatedCharName)
                            ? a.HatedCharName : mon.HatedCharName,
                        IsDead = mon.IsDead || (hasCurHp && upd.CurHp == 0),
                        LastUpdateSeconds = ev.TimestampSeconds,
                    };
                    if (!newMon.Equals(mon)) monsters = monsters.SetItem(upd.Uuid, newMon);
                }
            }
            var entitiesChanged = !ReferenceEquals(dict, s.NearEntities);
            var monstersChanged = !ReferenceEquals(monsters, s.MonsterDataMap);
            if (!entitiesChanged && !monstersChanged) return s;
            changed = true;
            var next = s with { NearEntities = dict, MonsterDataMap = monsters };
            return ProjectBossFromMonsters(next, ev.TimestampSeconds);
        });
        return changed;
    }

    private static bool ApplyToMeDelta(GameStateManager state, ToMeDeltaEvent ev)
    {
        // Mirrors Python's `_on_sync_to_me_delta` skill_cd_map merge:
        //   - drop entries with skill_level_id <= 0
        //   - drop expired entries (begin + duration < server_now_ms),
        //     removing any pre-existing row from the map
        //   - carry-forward charge/sub_ratio/sub_fixed/accel_ratio when
        //     the new packet leaves them at 0 (server-side optimisation)
        // SyncHateIds list is intentionally not mutated — Python decodes
        // its count but never writes anywhere.
        // S70: route only when the packet's uuid matches the local
        // SelfUuid (or when SelfUuid/ev.Uuid is 0 — legacy/cold-start
        // packets predate EnterGame).
        if (ev.SkillCds.Count == 0) return false;
        var self = state.Snapshot.SelfUuid;
        if (self != 0 && ev.Uuid != 0 && (ulong)ev.Uuid != self) return false;
        // S109: auto-confirm SelfUuid when the cold-start path observes
        // a packet whose uuid passes the player low-marker check
        // (mirrors Python `_confirm_self_uid` at packet_parser.py 3888).
        // Once SelfUuid is set, the S70 filter starts gating subsequent
        // packets — so we only flip it once, and only from a clearly
        // player-marker uuid.
        TryConfirmSelfUuid(state, self, ev.Uuid);
        var changed = false;
        state.Update(s =>
        {
            var serverNowMs = (long)Math.Round(
                ev.TimestampSeconds * 1000.0 + s.ServerTimeOffsetMs);
            var dict = s.SkillCdMap;
            foreach (var cd in ev.SkillCds)
            {
                if (cd.SkillLevelId <= 0) continue;
                if (cd.BeginMs + cd.DurationMs < serverNowMs)
                {
                    if (dict.ContainsKey(cd.SkillLevelId))
                        dict = dict.Remove(cd.SkillLevelId);
                    continue;
                }
                var entry = cd;
                if (dict.TryGetValue(cd.SkillLevelId, out var prev))
                {
                    entry = entry with
                    {
                        ChargeCount = entry.ChargeCount != 0 ? entry.ChargeCount : prev.ChargeCount,
                        SubCdRatio = entry.SubCdRatio != 0 ? entry.SubCdRatio : prev.SubCdRatio,
                        SubCdFixed = entry.SubCdFixed != 0 ? entry.SubCdFixed : prev.SubCdFixed,
                        AccelerateCdRatio = entry.AccelerateCdRatio != 0 ? entry.AccelerateCdRatio : prev.AccelerateCdRatio,
                        // SKILL-005: preserve prior anchor when packet doesn't
                        // restamp — keeps VCD extrapolation continuous across
                        // delta packets that only refresh BeginMs/Duration.
                        SkillCdType = entry.SkillCdType != 0 ? entry.SkillCdType : prev.SkillCdType,
                        MaxCharges = entry.MaxCharges > 1 ? entry.MaxCharges : prev.MaxCharges,
                        VcdSpeedRatio = entry.VcdSpeedRatio != 0 ? entry.VcdSpeedRatio : prev.VcdSpeedRatio,
                    };
                    // SKILL-004: dedupe when the new entry matches `prev` on
                    // every meaningful field — packet_parser.py:1991-2002 skips
                    // SetItem so subscribers don't churn on no-op refreshes.
                    if (entry.BeginMs == prev.BeginMs
                        && entry.DurationMs == prev.DurationMs
                        && entry.ValidCdTimeMs == prev.ValidCdTimeMs
                        && entry.ChargeCount == prev.ChargeCount
                        && entry.SkillCdType == prev.SkillCdType
                        && entry.SubCdRatio == prev.SubCdRatio
                        && entry.SubCdFixed == prev.SubCdFixed
                        && entry.AccelerateCdRatio == prev.AccelerateCdRatio)
                    {
                        continue;
                    }
                }
                dict = dict.SetItem(cd.SkillLevelId, entry);
            }
            if (ReferenceEquals(dict, s.SkillCdMap)) return s;
            changed = true;
            return s with { SkillCdMap = dict };
        });
        return changed;
    }

    private static void TryConfirmSelfUuid(GameStateManager state, ulong currentSelf, long evUuid)
    {
        // S109: mirror Python `_confirm_self_uid` (packet_parser.py 3888).
        // Cold-start path: when local SelfUuid is unset and the packet's
        // uuid passes the player low-marker check, latch it as SelfUuid so
        // subsequent S70 filtering can gate misrouted packets.
        if (currentSelf != 0) return;
        if (evUuid == 0) return;
        if (!Automation.CyCombat.IsPlayerUuid((ulong)evUuid)) return;
        state.Update(s => s.SelfUuid == 0 ? s with { SelfUuid = (ulong)evUuid } : s);
    }

    private static bool ApplyToMeFightResCd(GameStateManager state, ToMeFightResCdEvent ev)
    {
        // S108: mirror ApplyToMeDelta's pattern for fight-resource cds.
        // - Same self-uuid filter (S70): drop if SelfUuid known and
        //   ev.Uuid mismatches; pass-through when either side is 0.
        // - Decoder already filtered ResId <= 0, so the inner loop
        //   trusts ResId > 0.
        // - Expire rule mirrors skill cd: when begin + duration <
        //   server_now_ms, drop the row from the map.
        // Carry-forward semantics aren't applicable — FightResCD only
        // carries 4 fields and none has a server-side "0 means
        // unchanged" optimisation in Python (packet_parser.py 3967-3973
        // overwrites every field on each packet).
        if (ev.FightResCds.Count == 0) return false;
        var self = state.Snapshot.SelfUuid;
        if (self != 0 && ev.Uuid != 0 && (ulong)ev.Uuid != self) return false;
        // S109: same auto-confirm hook as ApplyToMeDelta — sibling event
        // path must keep SelfUuid latching consistent.
        TryConfirmSelfUuid(state, self, ev.Uuid);
        var changed = false;
        state.Update(s =>
        {
            var serverNowMs = (long)Math.Round(
                ev.TimestampSeconds * 1000.0 + s.ServerTimeOffsetMs);
            var dict = s.FightResCdMap;
            foreach (var fcd in ev.FightResCds)
            {
                if (fcd.ResId <= 0) continue;
                if (fcd.BeginMs + fcd.DurationMs < serverNowMs)
                {
                    if (dict.ContainsKey(fcd.ResId))
                        dict = dict.Remove(fcd.ResId);
                    continue;
                }
                dict = dict.SetItem(fcd.ResId, fcd);
            }
            if (ReferenceEquals(dict, s.FightResCdMap)) return s;
            changed = true;
            return s with { FightResCdMap = dict };
        });
        return changed;
    }

    private static bool ApplyTempAttrCd(GameStateManager state, TempAttrCdEvent ev)
    {
        // S122 — buff-driven CD modifiers (cd_pct/cd_fixed/cd_accel).
        // Self-only: apply S70's SelfUuid filter — drop when SelfUuid is
        // known and ev.Uuid mismatches; pass-through when either is 0.
        // S109 latching applies (TryConfirmSelfUuid) so a cold-start
        // packet for a player low-marker uuid sticks SelfUuid.
        var self = state.Snapshot.SelfUuid;
        if (self != 0 && ev.Uuid != 0 && (ulong)ev.Uuid != self) return false;
        TryConfirmSelfUuid(state, self, ev.Uuid);
        var changed = false;
        state.Update(s =>
        {
            if (s.TempAttrCdPct == ev.CdPct
                && s.TempAttrCdFixed == ev.CdFixed
                && s.TempAttrCdAccel == ev.CdAccel)
                return s;
            changed = true;
            return s with
            {
                TempAttrCdPct = ev.CdPct,
                TempAttrCdFixed = ev.CdFixed,
                TempAttrCdAccel = ev.CdAccel,
            };
        });
        return changed;
    }

    private static bool ApplyAoiBuffSync(GameStateManager state, AoiBuffSyncEvent ev)
    {
        // S123 — sustained-buff resync. Mirrors Python's
        // `_process_aoi_sync_delta` BuffInfos branch (packet_parser.py
        // 4032–4041): player low-marker writes self_buff list,
        // non-player writes monster_data[uuid].buff_list.
        // - Player path: same SelfUuid filter + S109 latching as
        //   ApplyTempAttrCd. Replaces the SelfBuffs list wholesale —
        //   periodic resync is authoritative over the incremental
        //   NotifyBuffChange (id-only) signal. ServerTimeOffsetMs is
        //   left untouched (decoder doesn't have it on the AOI path;
        //   only ApplyBuffs/SyncContainerData carry the offset).
        // - Monster path: only updates rows already in MonsterDataMap
        //   (mirrors Python's `if uuid in self._monsters`). Drops the
        //   event when the monster row hasn't been created yet — the
        //   next NearEntities Appear will lazy-create the row, after
        //   which subsequent BuffInfos resyncs land.
        if (ev.Uuid == 0) return false;
        var entries = ev.Buffs.Select(b => new BuffEntry
        {
            Id = b.Id,
            Uuid = b.Uuid,
            BeginMs = b.BeginMs,
            DurationMs = b.DurationMs,
            Layer = b.Layer,
            Count = b.Count,
            Name = b.Name,
        }).ToImmutableArray();

        if (Automation.CyCombat.IsPlayerUuid((ulong)ev.Uuid))
        {
            var self = state.Snapshot.SelfUuid;
            if (self != 0 && (ulong)ev.Uuid != self) return false;
            TryConfirmSelfUuid(state, self, ev.Uuid);
            var changed = false;
            state.Update(s =>
            {
                if (s.SelfBuffs.SequenceEqual(entries)) return s;
                changed = true;
                return s with { SelfBuffs = entries };
            });
            return changed;
        }
        else
        {
            var changed = false;
            state.Update(s =>
            {
                if (!s.MonsterDataMap.TryGetValue(ev.Uuid, out var md)) return s;
                if (md.BuffList.SequenceEqual(entries)) return s;
                changed = true;
                return s with
                {
                    // MSR-6: stamp LastUpdateSeconds so PurgeStaleMonsters
                    // sees this row as fresh after a buff resync. Otherwise
                    // a transition-purge mid-fight could drop the boss row
                    // when the only recent activity was a buff refresh.
                    MonsterDataMap = s.MonsterDataMap.SetItem(
                        ev.Uuid, md with
                        {
                            BuffList = entries,
                            LastUpdateSeconds = ev.TimestampSeconds,
                        }),
                };
            });
            return changed;
        }
    }

    private static bool ApplyBuffEffect(GameStateManager state, BuffEffectEvent ev)
    {
        // S124 — boss-side one-shot buff event log. Mirrors Python's
        // `_process_buff_effect_sync` at packet_parser.py 4789–4823.
        // Only mutates state for the three event types Python's
        // explicit branches handle (HostDeath / ShieldBroken /
        // EnterBreaking); other boss-relevant types (BodyPartDead /
        // BodyPartStateChange / SuperArmorBroken / IntoFractureState)
        // ride the event bus untouched so a future overlay subscriber
        // can react. Drops the row entirely when the monster isn't in
        // MonsterDataMap (mirrors `monster = self._monsters.get(host_uuid);
        // if monster:` at 4803–4804). Also fires the boss-event
        // observation surface even when the row is missing — matches
        // Python's `_notify_boss_event` call OUTSIDE the `if monster`
        // block (4823); the C# observation surface today is the
        // event itself reaching this mutator (subscribers can fan
        // out from PacketBridge.OnEvent).
        if (ev.TargetUuid == 0 || ev.Effects.Count == 0) return false;
        var changed = false;
        state.Update(s =>
        {
            if (!s.MonsterDataMap.TryGetValue(ev.TargetUuid, out var md)) return s;
            var newMd = md;
            foreach (var eff in ev.Effects)
            {
                switch (eff.Type)
                {
                    case BuffEventType.EnterBreaking:
                        newMd = newMd with { BreakingStage = 0, Extinction = 0 };
                        break;
                    case BuffEventType.ShieldBroken:
                        newMd = newMd with
                        {
                            ShieldActive = false,
                            ShieldTotal = 0,
                        };
                        break;
                    case BuffEventType.HostDeath:
                        newMd = newMd with { IsDead = true, Hp = 0 };
                        break;
                }
            }
            if (ReferenceEquals(newMd, md)) return s;
            changed = true;
            return s with
            {
                // MSR-6: stamp on death / break / shield-broken events too —
                // a boss that just died is the most-recent activity even if
                // no HP delta arrived; we don't want PurgeStaleMonsters to
                // sweep it on the same transition.
                MonsterDataMap = s.MonsterDataMap.SetItem(
                    ev.TargetUuid,
                    newMd with { LastUpdateSeconds = ev.TimestampSeconds }),
            };
        });
        return changed;
    }

    private static bool ApplyPlayerAttr(GameStateManager state, PlayerAttrEvent ev)
    {
        // S126 — player-self AttrCollection mirror. Mirrors the
        // identity + HP + profession slice of Python's
        // `_process_attr_collection` at packet_parser.py 4894–5050.
        // Self-only: same SelfUuid filter + S109 latching as
        // ApplyTempAttrCd / ApplyAoiBuffSync. Per-field guards mirror
        // Python's `if value > 0` clauses (Name=non-empty,
        // Level/FightPoint/MaxHp/ProfessionId > 0, RankLevel >= 0).
        // HP=0 is the special case from Python 4951–4955: ignore a
        // transient zero unless MaxHp is also 0 (cold-start), so a
        // mid-combat AOI delta with HP=0 doesn't false-flag death.
        var self = state.Snapshot.SelfUuid;
        if (self != 0 && ev.Uuid != 0 && (ulong)ev.Uuid != self) return false;
        TryConfirmSelfUuid(state, self, ev.Uuid);
        var changed = false;
        state.Update(s =>
        {
            var newState = s;
            if (ev.HasName && !string.IsNullOrEmpty(ev.Name) && newState.PlayerName != ev.Name)
                newState = newState with { PlayerName = ev.Name };
            if (ev.HasLevel && ev.Level > 0 && newState.LevelBase != ev.Level)
                newState = newState with { LevelBase = ev.Level };
            if (ev.HasFightPoint && ev.FightPoint > 0 && newState.FightPoint != ev.FightPoint)
                newState = newState with { FightPoint = ev.FightPoint };
            if (ev.HasMaxHp && ev.MaxHp > 0 && newState.HpMax != ev.MaxHp)
                newState = newState with { HpMax = ev.MaxHp };
            if (ev.HasHp)
            {
                // Python: ignore HP=0 unless MaxHp is also 0 (cold-start).
                // Use the candidate MaxHp (post-update above) for the check.
                if (ev.Hp > 0 || newState.HpMax == 0)
                {
                    if (newState.HpCurrent != ev.Hp)
                        newState = newState with { HpCurrent = ev.Hp };
                }
            }
            if (ev.HasProfessionId && ev.ProfessionId > 0
                && newState.ProfessionId != ev.ProfessionId)
                newState = newState with { ProfessionId = ev.ProfessionId };
            // S126b — combat stats. ATTACK / M_ATTACK / DEFENSE /
            // M_DEFENSE use `> 0` (zero is meaningless for raw
            // damage/defense). Percent stats use `>= 0` so a buff
            // expiry that drops the slot back to baseline can land.
            if (ev.HasAttack && ev.Attack > 0 && newState.Attack != ev.Attack)
                newState = newState with { Attack = ev.Attack };
            if (ev.HasMagicAttack && ev.MagicAttack > 0 && newState.MagicAttack != ev.MagicAttack)
                newState = newState with { MagicAttack = ev.MagicAttack };
            if (ev.HasDefense && ev.Defense > 0 && newState.Defense != ev.Defense)
                newState = newState with { Defense = ev.Defense };
            if (ev.HasMagicDefense && ev.MagicDefense > 0 && newState.MagicDefense != ev.MagicDefense)
                newState = newState with { MagicDefense = ev.MagicDefense };
            if (ev.HasCritRate && ev.CritRate >= 0 && newState.CritRate != ev.CritRate)
                newState = newState with { CritRate = ev.CritRate };
            if (ev.HasCritDamage && ev.CritDamage >= 0 && newState.CritDamage != ev.CritDamage)
                newState = newState with { CritDamage = ev.CritDamage };
            if (ev.HasAttackSpeedPct && ev.AttackSpeedPct >= 0 && newState.AttackSpeedPct != ev.AttackSpeedPct)
                newState = newState with { AttackSpeedPct = ev.AttackSpeedPct };
            if (ev.HasCastSpeedPct && ev.CastSpeedPct >= 0 && newState.CastSpeedPct != ev.CastSpeedPct)
                newState = newState with { CastSpeedPct = ev.CastSpeedPct };
            if (ev.HasChargeSpeedPct && ev.ChargeSpeedPct >= 0 && newState.ChargeSpeedPct != ev.ChargeSpeedPct)
                newState = newState with { ChargeSpeedPct = ev.ChargeSpeedPct };
            if (ev.HasHealPower && ev.HealPower >= 0 && newState.HealPower != ev.HealPower)
                newState = newState with { HealPower = ev.HealPower };
            if (ev.HasDamInc && ev.DamInc >= 0 && newState.DamInc != ev.DamInc)
                newState = newState with { DamInc = ev.DamInc };
            if (ev.HasMDamInc && ev.MDamInc >= 0 && newState.MDamInc != ev.MDamInc)
                newState = newState with { MDamInc = ev.MDamInc };
            if (ev.HasBossDamInc && ev.BossDamInc >= 0 && newState.BossDamInc != ev.BossDamInc)
                newState = newState with { BossDamInc = ev.BossDamInc };
            // S126c — CD-related player attrs. Python guards: SkillCd /
            // SkillCdPct / CdAcceleratePct use `>= 0` (zero is a real
            // baseline — no equipment CDR); FightResCdSpeed uses `> 0`
            // (matches packet_parser.py 5054–5057 explicit `> 0` gate).
            if (ev.HasAttrSkillCd && ev.AttrSkillCd >= 0 && newState.AttrSkillCd != ev.AttrSkillCd)
                newState = newState with { AttrSkillCd = ev.AttrSkillCd };
            if (ev.HasAttrSkillCdPct && ev.AttrSkillCdPct >= 0 && newState.AttrSkillCdPct != ev.AttrSkillCdPct)
                newState = newState with { AttrSkillCdPct = ev.AttrSkillCdPct };
            if (ev.HasAttrCdAcceleratePct && ev.AttrCdAcceleratePct >= 0
                && newState.AttrCdAcceleratePct != ev.AttrCdAcceleratePct)
                newState = newState with { AttrCdAcceleratePct = ev.AttrCdAcceleratePct };
            if (ev.HasAttrFightResCdSpeed && ev.AttrFightResCdSpeed > 0
                && newState.AttrFightResCdSpeed != ev.AttrFightResCdSpeed)
                newState = newState with { AttrFightResCdSpeed = ev.AttrFightResCdSpeed };
            // Recompute HpPct when either HP field actually changed.
            if (newState.HpMax > 0
                && (newState.HpCurrent != s.HpCurrent || newState.HpMax != s.HpMax))
            {
                newState = newState with
                {
                    HpPct = Math.Clamp((double)newState.HpCurrent / newState.HpMax, 0.0, 1.0),
                };
            }
            if (ReferenceEquals(newState, s)) return s;
            changed = true;
            return newState;
        });
        return changed;
    }

    private static bool ApplySkillEffect(GameStateManager state, SkillEffectEvent ev)
    {
        // S128 — monster-side state mutations on damage. Mirrors Python's
        // `_process_skill_effect` post-decode at packet_parser.py
        // 4194–4247:
        //   * Damage proves alive → revive a monster the local table
        //     thinks is dead (only when the row itself doesn't claim
        //     `is_dead`; if Hp==0 + MaxHp>0, restore Hp = MaxHp).
        //   * Aggregate `shield_lessen` across rows → subtract from
        //     monster.ShieldTotal; flip ShieldActive=false at zero.
        // HP burn-down is NOT applied here — Python relies on the
        // subsequent NearDelta/AttrCollection HP delta to authoritative-
        // -update Hp; mirroring that here would double-count against
        // S111/S113 mutators.
        // DpsTracker / EncounterTracker rollup deferred to S128b
        // (needs PacketBridge constructor change to inject a tracker
        // + name resolution from MonsterDataMap.Name; flagged).
        if (ev.TargetUuid == 0 || ev.Damages.Count == 0) return false;
        var changed = false;
        state.Update(s =>
        {
            if (!s.MonsterDataMap.TryGetValue(ev.TargetUuid, out var mon)) return s;
            long shieldBurn = 0;
            var reviveBecauseAlive = false;
            foreach (var row in ev.Damages)
            {
                // Heal rows aren't damage on the monster — skip.
                if (row.IsHeal) continue;
                // Damage-proves-alive: a damage row landing on a known-
                // dead monster, where the row itself doesn't claim
                // is_dead, means the server still considers it alive.
                if (mon.IsDead && !row.IsDead && row.Damage > 0)
                    reviveBecauseAlive = true;
                if (row.ShieldLessen > 0) shieldBurn += row.ShieldLessen;
            }
            var newMon = mon;
            if (reviveBecauseAlive)
            {
                var hp = newMon.Hp == 0 && newMon.MaxHp > 0 ? newMon.MaxHp : newMon.Hp;
                newMon = newMon with { IsDead = false, Hp = hp };
            }
            if (shieldBurn > 0 && newMon.ShieldActive)
            {
                var remain = (int)Math.Max(0, newMon.ShieldTotal - shieldBurn);
                newMon = newMon with
                {
                    ShieldTotal = remain,
                    ShieldActive = remain > 0,
                };
            }
            if (newMon.Equals(mon)) return s;
            newMon = newMon with { LastUpdateSeconds = ev.TimestampSeconds };
            changed = true;
            var next = s with
            {
                MonsterDataMap = s.MonsterDataMap.SetItem(ev.TargetUuid, newMon),
            };
            return ProjectBossFromMonsters(next, ev.TimestampSeconds);
        });
        return changed;
    }
}
