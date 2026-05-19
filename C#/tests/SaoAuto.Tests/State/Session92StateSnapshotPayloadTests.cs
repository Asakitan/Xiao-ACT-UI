using System.Collections.Immutable;
using System.Text.Json.Nodes;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.State;

/// <summary>
/// S92 — Pins <see cref="StateSnapshotPayload.ToDict"/> against Python
/// <c>game_state.GameState.to_dict</c> rounding rules and key set.
/// </summary>
public class Session92StateSnapshotPayloadTests
{
    [Fact]
    public void HpPct_RoundedTo4dp()
    {
        var s = new GameState { HpPct = 0.123456789 };
        var d = StateSnapshotPayload.ToDict(s);
        Assert.Equal(0.1235, d["hp_pct"]!.GetValue<double>());
    }

    [Fact]
    public void StaminaPct_RoundedTo4dp()
    {
        var s = new GameState { StaminaPct = 0.66666666 };
        var d = StateSnapshotPayload.ToDict(s);
        Assert.Equal(0.6667, d["stamina_pct"]!.GetValue<double>());
    }

    [Fact]
    public void BossEnrageRemaining_RoundedTo1dp()
    {
        var s = new GameState { BossEnrageRemaining = 12.345 };
        var d = StateSnapshotPayload.ToDict(s);
        Assert.Equal(12.3, d["boss_enrage_remaining"]!.GetValue<double>());
    }

    [Fact]
    public void BossPctFields_RoundedTo4dp()
    {
        var s = new GameState
        {
            BossHpEstPct = 0.12345678,
            BossShieldPct = 0.5555556,
            BossExtinctionPct = 0.9999912,
        };
        var d = StateSnapshotPayload.ToDict(s);
        Assert.Equal(0.1235, d["boss_hp_est_pct"]!.GetValue<double>());
        Assert.Equal(0.5556, d["boss_shield_pct"]!.GetValue<double>());
        Assert.Equal(1.0, d["boss_extinction_pct"]!.GetValue<double>());
    }

    [Fact]
    public void BankerRounding_TiesToEven()
    {
        // Python 3 round() defaults to banker's rounding; .NET Math.Round
        // matches. 0.12345 → 0.1234 (even neighbour), 0.12355 → 0.1236.
        var s1 = new GameState { HpPct = 0.12345 };
        var s2 = new GameState { HpPct = 0.12355 };
        Assert.Equal(0.1234, StateSnapshotPayload.ToDict(s1)["hp_pct"]!.GetValue<double>());
        Assert.Equal(0.1236, StateSnapshotPayload.ToDict(s2)["hp_pct"]!.GetValue<double>());
    }

    [Fact]
    public void IdentityAndDerivedFields_PassedThrough()
    {
        var s = new GameState
        {
            PlayerName = "Alice",
            PlayerId = "u-1",
            LevelBase = 42,
            LevelExtra = 5,
            ProfessionName = "雷影剑士",
            HpCurrent = 750, HpMax = 1000,
            StaminaPct = 0.5,
        };
        var d = StateSnapshotPayload.ToDict(s);
        Assert.Equal("Alice", d["player_name"]!.GetValue<string>());
        Assert.Equal("u-1", d["player_id"]!.GetValue<string>());
        Assert.Equal(42, d["level_base"]!.GetValue<int>());
        Assert.Equal("42(+5)", d["level_text"]!.GetValue<string>());
        Assert.Equal("750/1000", d["hp_text"]!.GetValue<string>());
        Assert.Equal("50%", d["stamina_text"]!.GetValue<string>());
        Assert.Equal("雷影剑士", d["profession_name"]!.GetValue<string>());
    }

    [Fact]
    public void EnumBossHpSource_SerializedAsInt()
    {
        var s = new GameState { BossHpSource = BossHpSource.Packet };
        var d = StateSnapshotPayload.ToDict(s);
        Assert.Equal((int)BossHpSource.Packet, d["boss_hp_source"]!.GetValue<int>());
    }

    [Fact]
    public void SkillSlots_SerializedAsArray()
    {
        var s = new GameState
        {
            SkillSlots = ImmutableArray.Create(
                new SkillSlot { Index = 1, State = SkillSlotState.Ready, ChargeCount = 2 },
                new SkillSlot { Index = 2, State = SkillSlotState.Cooldown, RemainingMs = 500 }),
        };
        var arr = StateSnapshotPayload.ToDict(s)["skill_slots"] as JsonArray;
        Assert.NotNull(arr);
        Assert.Equal(2, arr!.Count);
    }

    [Fact]
    public void SelfBuffs_SerializedAsArray()
    {
        var s = new GameState
        {
            SelfBuffs = ImmutableArray.Create(
                new BuffEntry { Id = 100, Name = "Surge", DurationMs = 5000 }),
        };
        var arr = StateSnapshotPayload.ToDict(s)["self_buffs"] as JsonArray;
        Assert.NotNull(arr);
        Assert.Single(arr!);
    }

    [Fact]
    public void EmptyArrays_StillEmitted()
    {
        var s = new GameState();
        var d = StateSnapshotPayload.ToDict(s);
        Assert.NotNull(d["skill_slots"]);
        Assert.NotNull(d["self_buffs"]);
        Assert.Empty((d["skill_slots"] as JsonArray)!);
        Assert.Empty((d["self_buffs"] as JsonArray)!);
    }

    [Fact]
    public void RequiredKeys_AllPresent()
    {
        var s = new GameState();
        var d = StateSnapshotPayload.ToDict(s);
        // Spot-check the full Python key set so a future field addition has a
        // failing test as a forcing function.
        var keys = new[]
        {
            "player_name", "level_base", "level_extra", "season_exp", "level_text",
            "player_id", "hp_current", "hp_max", "hp_pct",
            "stamina_current", "stamina_max", "stamina_pct",
            "skill_slots", "burst_ready", "profession_id", "profession_name",
            "hp_text", "stamina_text", "recognition_ok", "packet_active",
            "capture_ts", "boss_raid_active", "boss_raid_phase",
            "boss_raid_phase_name", "boss_enrage_remaining", "boss_timer_text",
            "boss_total_damage", "boss_dps", "boss_hp_est_pct",
            "boss_current_hp", "boss_total_hp", "boss_hp_source",
            "boss_shield_active", "boss_shield_pct", "boss_breaking_stage",
            "boss_extinction_pct", "boss_in_overdrive", "boss_invincible",
            "identity_alert_serial", "identity_alert_title", "identity_alert_message",
            "self_buffs", "server_time_offset_ms",
        };
        foreach (var k in keys)
        {
            Assert.True(d.ContainsKey(k), $"missing key: {k}");
        }
    }

    [Fact]
    public void NullState_Throws()
    {
        Assert.Throws<ArgumentNullException>(() => StateSnapshotPayload.ToDict(null!));
    }
}
