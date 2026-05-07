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
            DungeonStartEvent _ => ApplyDungeonStart(state),
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
        state.Update(s => s with
        {
            PacketActive = true,
            ErrorMsg = string.Empty,
            SelfUuid = ev.SelfUuid,
        });
        return true;
    }

    private static bool ApplyCombatState(GameStateManager state, CombatStateEvent cs)
    {
        state.Update(s => s with { InCombat = cs.InCombat });
        return true;
    }

    private static bool ApplyDungeonStart(GameStateManager state)
    {
        state.Update(s => s with { InCombat = false });
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
        // Mirrors Python: player.skill_last_use_at[skill_level_id] = ts.
        state.Update(s => s with
        {
            SkillLastUseAt = s.SkillLastUseAt.SetItem(ev.SkillLevelId, ev.TimestampSeconds),
        });
        return true;
    }

    private static bool ApplyDungeonData(GameStateManager state, DungeonDataEvent ev)
    {
        state.Update(s => s with
        {
            DungeonSceneUuid = ev.SceneUuid,
            DungeonDifficulty = ev.DungeonDifficulty,
            DungeonTargets = ev.Targets.ToImmutableArray(),
        });
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
        state.ApplyPartial(partial, s => s with
        {
            PlayerName = string.IsNullOrEmpty(ev.Name) ? s.PlayerName : ev.Name,
            FightPoint = ev.FightPoint > 0 ? ev.FightPoint : s.FightPoint,
            PacketActive = true,
        });
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
                    };
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
                    MonsterDataMap = s.MonsterDataMap.SetItem(
                        ev.Uuid, md with { BuffList = entries }),
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
                MonsterDataMap = s.MonsterDataMap.SetItem(ev.TargetUuid, newMd),
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
