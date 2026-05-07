using System.Text.Json;
using System.Text.Json.Nodes;

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
        };
        return obj;
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
