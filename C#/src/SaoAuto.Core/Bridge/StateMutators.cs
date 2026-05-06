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
            ToMeDeltaEvent me => ApplyToMeDelta(state, me),
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
        state.Update(s => s with
        {
            HpCurrent = hp.HpCurrent,
            HpMax = hp.HpMax,
            HpPct = hp.HpPct,
            PacketActive = true,
        });
        return true;
    }

    private static bool ApplyStamina(GameStateManager state, StaminaEvent st)
    {
        state.Update(s => s with
        {
            StaminaCurrent = (int)Math.Round(st.StaminaCurrent),
            StaminaMax = (int)Math.Min(int.MaxValue, st.StaminaMax),
            StaminaPct = st.StaminaPct,
            StaminaOffline = st.Offline,
        });
        return true;
    }

    private static bool ApplyServerTime(GameStateManager state, ServerTimeEvent srv)
    {
        state.Update(s => s with { ServerTimeOffsetMs = srv.OffsetMs });
        return true;
    }

    private static bool ApplyRevive(GameStateManager state, ReviveEvent _)
    {
        state.Update(s => s with { InCombat = false, HpPct = 1.0 });
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
        });
        return true;
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
        state.Update(s => s with
        {
            PlayerName = string.IsNullOrEmpty(ev.Name) ? s.PlayerName : ev.Name,
            LevelBase = ev.Level > 0 ? ev.Level : s.LevelBase,
            FightPoint = ev.FightPoint > 0 ? ev.FightPoint : s.FightPoint,
            HpCurrent = cur,
            HpMax = max,
            HpPct = pct,
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
            foreach (var app in ev.Appear)
            {
                if (dict.ContainsKey(app.Uuid)) continue; // first-seen wins
                dict = dict.SetItem(app.Uuid,
                    new EntityTableEntry(app.EntityType, ev.TimestampSeconds));
            }
            foreach (var dis in ev.Disappear)
            {
                if (dict.ContainsKey(dis.Uuid))
                    dict = dict.Remove(dis.Uuid);
            }
            if (ReferenceEquals(dict, s.NearEntities)) return s;
            changed = true;
            return s with { NearEntities = dict };
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
}
