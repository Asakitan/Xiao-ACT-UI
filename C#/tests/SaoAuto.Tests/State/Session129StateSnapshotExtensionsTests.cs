using SaoAuto.Core.State;

namespace SaoAuto.Tests.State;

/// <summary>
/// S129 — Pins the new <see cref="StateSnapshotPayload.ToDict"/> keys
/// added beyond Python's <c>game_state.GameState.to_dict</c> for the
/// WebView2 HUD's combat-stat / CDR / DPS panels: combat stats (S126b),
/// CD attrs (S122 TempAttr + S126c equipment), FightPoint (S126),
/// InCombat (S128 family). Defaults must serialize as 0 / false (no
/// missing keys) so the JS subscriber never has to null-guard.
/// </summary>
public class Session129StateSnapshotExtensionsTests
{
    [Fact]
    public void NewKeys_AllPresentOnDefaultState()
    {
        var d = StateSnapshotPayload.ToDict(new GameState());
        var keys = new[]
        {
            "fight_point", "in_combat",
            "attack", "magic_attack", "defense", "magic_defense",
            "crit_rate", "crit_damage", "attack_speed_pct", "cast_speed_pct",
            "charge_speed_pct", "heal_power", "dam_inc", "m_dam_inc", "boss_dam_inc",
            "temp_attr_cd_pct", "temp_attr_cd_fixed", "temp_attr_cd_accel",
            "attr_skill_cd", "attr_skill_cd_pct",
            "attr_cd_accelerate_pct", "attr_fight_res_cd_speed",
        };
        foreach (var k in keys)
            Assert.True(d.ContainsKey(k), $"missing key: {k}");
    }

    [Fact]
    public void DefaultValues_AreZeroOrFalse()
    {
        var d = StateSnapshotPayload.ToDict(new GameState());
        Assert.Equal(0, d["fight_point"]!.GetValue<int>());
        Assert.False(d["in_combat"]!.GetValue<bool>());
        Assert.Equal(0, d["attack"]!.GetValue<int>());
        Assert.Equal(0, d["temp_attr_cd_pct"]!.GetValue<int>());
        Assert.Equal(0, d["attr_skill_cd"]!.GetValue<int>());
    }

    [Fact]
    public void S126b_CombatStats_PassedThrough()
    {
        var s = new GameState
        {
            Attack = 5000, MagicAttack = 4500,
            Defense = 1200, MagicDefense = 1100,
            CritRate = 1500, CritDamage = 5000,
            AttackSpeedPct = 800, CastSpeedPct = 900, ChargeSpeedPct = 700,
            HealPower = 200, DamInc = 1000, MDamInc = 950, BossDamInc = 1500,
        };
        var d = StateSnapshotPayload.ToDict(s);
        Assert.Equal(5000, d["attack"]!.GetValue<int>());
        Assert.Equal(4500, d["magic_attack"]!.GetValue<int>());
        Assert.Equal(1200, d["defense"]!.GetValue<int>());
        Assert.Equal(1100, d["magic_defense"]!.GetValue<int>());
        Assert.Equal(1500, d["crit_rate"]!.GetValue<int>());
        Assert.Equal(5000, d["crit_damage"]!.GetValue<int>());
        Assert.Equal(800, d["attack_speed_pct"]!.GetValue<int>());
        Assert.Equal(900, d["cast_speed_pct"]!.GetValue<int>());
        Assert.Equal(700, d["charge_speed_pct"]!.GetValue<int>());
        Assert.Equal(200, d["heal_power"]!.GetValue<int>());
        Assert.Equal(1000, d["dam_inc"]!.GetValue<int>());
        Assert.Equal(950, d["m_dam_inc"]!.GetValue<int>());
        Assert.Equal(1500, d["boss_dam_inc"]!.GetValue<int>());
    }

    [Fact]
    public void S122_And_S126c_CdAttrs_AreSeparateChannels()
    {
        var s = new GameState
        {
            // S122 buff CDR.
            TempAttrCdPct = 500, TempAttrCdFixed = 100, TempAttrCdAccel = 200,
            // S126c equipment CDR.
            AttrSkillCd = 2000, AttrSkillCdPct = 1500,
            AttrCdAcceleratePct = 800, AttrFightResCdSpeed = 1200,
        };
        var d = StateSnapshotPayload.ToDict(s);
        Assert.Equal(500, d["temp_attr_cd_pct"]!.GetValue<int>());
        Assert.Equal(100, d["temp_attr_cd_fixed"]!.GetValue<int>());
        Assert.Equal(200, d["temp_attr_cd_accel"]!.GetValue<int>());
        Assert.Equal(2000, d["attr_skill_cd"]!.GetValue<int>());
        Assert.Equal(1500, d["attr_skill_cd_pct"]!.GetValue<int>());
        Assert.Equal(800, d["attr_cd_accelerate_pct"]!.GetValue<int>());
        Assert.Equal(1200, d["attr_fight_res_cd_speed"]!.GetValue<int>());
    }

    [Fact]
    public void FightPoint_And_InCombat_PassedThrough()
    {
        var s = new GameState { FightPoint = 123456, InCombat = true };
        var d = StateSnapshotPayload.ToDict(s);
        Assert.Equal(123456, d["fight_point"]!.GetValue<int>());
        Assert.True(d["in_combat"]!.GetValue<bool>());
    }

    [Fact]
    public void OriginalPythonKeys_StillIntact()
    {
        // S92 parity must not regress — the new keys land alongside the
        // original Python set, not replacing any of them.
        var d = StateSnapshotPayload.ToDict(new GameState());
        Assert.True(d.ContainsKey("hp_pct"));
        Assert.True(d.ContainsKey("boss_hp_source"));
        Assert.True(d.ContainsKey("self_buffs"));
        Assert.True(d.ContainsKey("server_time_offset_ms"));
    }
}
