using System.Collections.Immutable;
using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;

namespace SaoAuto.Core.State;

/// <summary>
/// S92 — Snapshot serializer that mirrors Python
/// <c>game_state.GameState.to_dict</c> (game_state.py 188–233) for
/// byte-for-byte parity with the WebBridge JS subscribers.
///
/// Key rounding rules from Python:
/// <list type="bullet">
/// <item><description><c>hp_pct</c>, <c>stamina_pct</c>, <c>boss_hp_est_pct</c>,
///   <c>boss_shield_pct</c>, <c>boss_extinction_pct</c> → 4 decimals.</description></item>
/// <item><description><c>boss_enrage_remaining</c> → 1 decimal.</description></item>
/// </list>
///
/// Both Python's <c>round()</c> and .NET's <c>Math.Round(double, int)</c>
/// default to banker's rounding (away-from-zero ties go to the even neighbor),
/// so the formula matches without an explicit MidpointRounding override.
/// </summary>
public static class StateSnapshotPayload
{
    public static JsonObject ToDict(GameState s)
        => ToDict(s, dps: null);

    /// <summary>
    /// S131 — overload that surfaces a per-attacker DPS rollup as
    /// <c>dps_entries</c> (and <c>dps_active / dps_total_damage / dps_total /
    /// dps_total_heal / dps_hps / dps_duration_seconds / dps_report_reason</c>
    /// scalars). Pass <c>null</c> (the 1-arg overload does this) when no
    /// tracker is available — the keys still ship with empty/zero defaults
    /// so JS subscribers never null-guard.
    /// </summary>
    public static JsonObject ToDict(GameState s, DpsSnapshot? dps)
    {
        if (s is null) throw new ArgumentNullException(nameof(s));

        var obj = new JsonObject
        {
            ["player_name"] = s.PlayerName,
            ["level_base"] = s.LevelBase,
            ["level_extra"] = s.LevelExtra,
            ["season_exp"] = s.SeasonExp,
            ["level_text"] = s.LevelText,
            ["player_id"] = s.PlayerId,
            ["hp_current"] = s.HpCurrent,
            ["hp_max"] = s.HpMax,
            ["hp_pct"] = Math.Round(s.HpPct, 4),
            ["stamina_current"] = s.StaminaCurrent,
            ["stamina_max"] = s.StaminaMax,
            ["stamina_pct"] = Math.Round(s.StaminaPct, 4),
            ["skill_slots"] = SerializeArray(s.SkillSlots),
            ["burst_ready"] = s.BurstReady,
            ["profession_id"] = s.ProfessionId,
            ["profession_name"] = s.ProfessionName,
            ["hp_text"] = s.HpText,
            ["stamina_text"] = s.StaminaText,
            ["recognition_ok"] = s.RecognitionOk,
            ["packet_active"] = s.PacketActive,
            ["capture_ts"] = s.CaptureTimestamp,
            ["boss_raid_active"] = s.BossRaidActive,
            ["boss_raid_phase"] = s.BossRaidPhase,
            ["boss_raid_phase_name"] = s.BossRaidPhaseName,
            ["boss_enrage_remaining"] = Math.Round(s.BossEnrageRemaining, 1),
            ["boss_timer_text"] = s.BossTimerText,
            ["boss_total_damage"] = s.BossTotalDamage,
            ["boss_dps"] = s.BossDps,
            ["boss_hp_est_pct"] = Math.Round(s.BossHpEstPct, 4),
            ["boss_current_hp"] = s.BossCurrentHp,
            ["boss_total_hp"] = s.BossTotalHp,
            ["boss_hp_source"] = (int)s.BossHpSource,
            ["boss_shield_active"] = s.BossShieldActive,
            ["boss_shield_pct"] = Math.Round(s.BossShieldPct, 4),
            ["boss_breaking_stage"] = s.BossBreakingStage,
            ["boss_extinction_pct"] = Math.Round(s.BossExtinctionPct, 4),
            ["boss_in_overdrive"] = s.BossInOverdrive,
            ["boss_invincible"] = s.BossInvincible,
            ["identity_alert_serial"] = s.IdentityAlertSerial,
            ["identity_alert_title"] = s.IdentityAlertTitle,
            ["identity_alert_message"] = s.IdentityAlertMessage,
            ["self_buffs"] = SerializeArray(s.SelfBuffs),
            ["server_time_offset_ms"] = s.ServerTimeOffsetMs,
            // ── S129 narrow-channel extensions (not in Python to_dict;
            // surfaced for the WebView2 HUD's combat-stat / CDR / DPS panels).
            // S126 player identity tail.
            ["fight_point"] = s.FightPoint,
            ["in_combat"] = s.InCombat,
            // S126b combat stats.
            ["attack"] = s.Attack,
            ["magic_attack"] = s.MagicAttack,
            ["defense"] = s.Defense,
            ["magic_defense"] = s.MagicDefense,
            ["crit_rate"] = s.CritRate,
            ["crit_damage"] = s.CritDamage,
            ["attack_speed_pct"] = s.AttackSpeedPct,
            ["cast_speed_pct"] = s.CastSpeedPct,
            ["charge_speed_pct"] = s.ChargeSpeedPct,
            ["heal_power"] = s.HealPower,
            ["dam_inc"] = s.DamInc,
            ["m_dam_inc"] = s.MDamInc,
            ["boss_dam_inc"] = s.BossDamInc,
            // S122 buff-driven CDR scalars (TempAttr ids 100/101/103).
            ["temp_attr_cd_pct"] = s.TempAttrCdPct,
            ["temp_attr_cd_fixed"] = s.TempAttrCdFixed,
            ["temp_attr_cd_accel"] = s.TempAttrCdAccel,
            // S126c equipment / passive CDR (AttrCollection 11750/11760/11960/11980).
            ["attr_skill_cd"] = s.AttrSkillCd,
            ["attr_skill_cd_pct"] = s.AttrSkillCdPct,
            ["attr_cd_accelerate_pct"] = s.AttrCdAcceleratePct,
            ["attr_fight_res_cd_speed"] = s.AttrFightResCdSpeed,
            // S130 — per-monster table (S113 lazy-create + S114-S117 attrs +
            // S123 BuffList + S128 IsDead). Sorted by MaxHp desc so the JS
            // HUD's primary slot is the strongest live mob; dead rows kept
            // (consumer can filter) so per-encounter death tracking works.
            ["monster_table"] = SerializeMonsterTable(s.MonsterDataMap),
            // S131 — DPS rollup from PacketBridge.DpsTracker. When the
            // 1-arg overload runs, defaults to inactive/empty so JS still
            // sees every key. Per-attacker rows include skill breakdown only
            // when the caller passed `SnapshotWithSkills()`.
            ["dps_active"] = dps?.Active ?? false,
            ["dps_total_damage"] = dps?.TotalDamage ?? 0L,
            ["dps_total"] = dps?.Dps ?? 0L,
            ["dps_total_heal"] = dps?.TotalHeal ?? 0L,
            ["dps_hps"] = dps?.Hps ?? 0L,
            ["dps_duration_seconds"] = dps?.DurationSeconds ?? 0.0,
            ["dps_report_reason"] = dps?.ReportReason,
            ["dps_entries"] = SerializeDpsEntries(dps),
        };
        return obj;
    }

    private static JsonArray SerializeMonsterTable(
        ImmutableDictionary<long, MonsterData> map)
    {
        var arr = new JsonArray();
        foreach (var m in map.Values
            .OrderByDescending(m => m.MaxHp)
            .ThenBy(m => m.Uuid))
        {
            arr.Add(new JsonObject
            {
                ["uuid"] = m.Uuid,
                ["uid"] = m.Uid,
                ["name"] = m.Name,
                ["template_id"] = m.TemplateId,
                ["hp"] = m.Hp,
                ["max_hp"] = m.MaxHp,
                ["level"] = m.Level,
                ["breaking_stage"] = m.BreakingStage,
                ["extinction"] = m.Extinction,
                ["max_extinction"] = m.MaxExtinction,
                ["stunned"] = m.Stunned,
                ["max_stunned"] = m.MaxStunned,
                ["in_overdrive"] = m.InOverdrive,
                ["shield_active"] = m.ShieldActive,
                ["shield_total"] = m.ShieldTotal,
                ["shield_max_total"] = m.ShieldMaxTotal,
                ["is_dead"] = m.IsDead,
                ["is_lock_stunned"] = m.IsLockStunned,
                ["stop_breaking_ticking"] = m.StopBreakingTicking,
                ["state"] = m.State,
                ["dead_type"] = m.DeadType,
                ["dead_time"] = m.DeadTime,
                ["first_attack"] = m.FirstAttack,
                ["hated_char_id"] = m.HatedCharId,
                ["hated_char_name"] = m.HatedCharName,
                ["buff_list"] = SerializeArray(m.BuffList),
                ["last_update_seconds"] = m.LastUpdateSeconds,
            });
        }
        return arr;
    }

    private static JsonArray SerializeDpsEntries(DpsSnapshot? dps)
    {
        var arr = new JsonArray();
        if (dps is null || dps.Rows.IsDefaultOrEmpty) return arr;

        var totalDamage = dps.TotalDamage;
        foreach (var row in dps.Rows)
        {
            arr.Add(new JsonObject
            {
                ["uid"] = row.EntityUuid,
                ["name"] = row.EntityName,
                ["profession_id"] = row.ProfessionId,
                ["is_self"] = row.IsSelf,
                ["damage_total"] = row.Damage,
                ["heal_total"] = row.Heal,
                ["dps"] = row.Dps,
                ["hps"] = row.Hps,
                ["damage_pct"] = totalDamage > 0
                    ? Math.Round((double)row.Damage / totalDamage, 3)
                    : 0.0,
                ["skills"] = SerializeSkillBreakdown(row.Skills),
            });
        }
        return arr;
    }

    private static JsonArray SerializeSkillBreakdown(
        ImmutableArray<SkillBreakdownRow> skills)
    {
        var arr = new JsonArray();
        if (skills.IsDefaultOrEmpty) return arr;

        foreach (var skill in skills)
        {
            arr.Add(new JsonObject
            {
                ["skill_id"] = skill.SkillId,
                ["name"] = skill.Name,
                ["total"] = skill.Total,
                ["hits"] = skill.Hits,
                ["crit_hits"] = skill.CritHits,
                ["crit_rate"] = skill.CritRate,
                ["max_hit"] = skill.MaxHit,
                ["heal_total"] = skill.HealTotal,
                ["heal_hits"] = skill.HealHits,
            });
        }
        return arr;
    }

    private static JsonArray SerializeArray<T>(IEnumerable<T> items)
    {
        var arr = new JsonArray();
        foreach (var item in items)
        {
            var node = JsonSerializer.SerializeToNode(item);
            arr.Add(node);
        }
        return arr;
    }
}
