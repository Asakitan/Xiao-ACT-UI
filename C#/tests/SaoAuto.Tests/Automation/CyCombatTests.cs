using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

public class CyCombatTests
{
    [Fact]
    public void IsPlayerUuidMatchesLowMarker640()
    {
        Assert.True(CyCombat.IsPlayerUuid((100UL << 16) | 640));
        Assert.False(CyCombat.IsPlayerUuid((100UL << 16) | 64));
    }

    [Fact]
    public void IsMonsterUuidAcceptsBothMarkers()
    {
        Assert.True(CyCombat.IsMonsterUuid((10UL << 16) | 64));
        Assert.True(CyCombat.IsMonsterUuid((10UL << 16) | 32832));
        Assert.False(CyCombat.IsMonsterUuid((10UL << 16) | 640));
    }

    [Fact]
    public void UuidToUidShiftsRight16()
    {
        Assert.Equal(123UL, CyCombat.UuidToUid((123UL << 16) | 640));
    }

    [Theory]
    [InlineData(100, 0, 0, 0, 0, 100)]            // value preferred
    [InlineData(0, 50, 0, 0, 0, 50)]              // luckyValue preferred
    [InlineData(0, 0, 25, 0, 0, 25)]              // actualValue preferred
    [InlineData(0, 0, 0, 30, 20, 50)]             // hp + shield fallback
    [InlineData(0, 0, 0, -5, 10, 10)]             // negative hp clamped to 0
    public void CombatDamageAmountFollowsPriority(long v, long lv, long av, long hp, long shield, long expected)
    {
        Assert.Equal(expected, CyCombat.CombatDamageAmount(v, lv, av, hp, shield));
    }

    [Fact]
    public void AttackerIsSelfMatchesByCurrentUuid()
    {
        Assert.True(CyCombat.AttackerIsSelf(attackerUuid: 0xABCD, currentUuid: 0xABCD, currentUid: 0));
    }

    [Fact]
    public void AttackerIsSelfMatchesByUidShift()
    {
        var attacker = (5UL << 16) | 640;
        Assert.True(CyCombat.AttackerIsSelf(attacker, currentUuid: 0, currentUid: 5));
    }

    [Fact]
    public void AttackerIsSelfRejectsNonPlayerLowMarker()
    {
        var attacker = (5UL << 16) | 64;
        Assert.False(CyCombat.AttackerIsSelf(attacker, currentUuid: 0, currentUid: 5));
    }
}

public class SkillStatsTests
{
    [Fact]
    public void AddDamageAccumulatesAndTracksMax()
    {
        var s = new SkillStats(skillId: 100, skillName: "Strike");
        s.AddDamage(50);
        s.AddDamage(120, isCrit: true);
        s.AddDamage(70);
        Assert.Equal(240, s.Total);
        Assert.Equal(3, s.Hits);
        Assert.Equal(1, s.CritHits);
        Assert.Equal(120, s.MaxHit);
    }

    [Fact]
    public void ToDictionaryShapeMatchesPython()
    {
        var s = new SkillStats(7, "Slash");
        s.AddDamage(100, isCrit: true);
        var d = s.ToDictionary();
        Assert.Equal(7L, d["skill_id"]);
        Assert.Equal("Slash", d["skill_name"]);
        Assert.Equal(100L, d["total"]);
        Assert.Equal(1L, d["hits"]);
        Assert.Equal(1.0, d["crit_rate"]);
    }
}

public class EntityStatsTests
{
    [Fact]
    public void AddDamageRecordsTimestampSpan()
    {
        var e = new EntityStats(uid: 100, name: "Self", isSelf: true, createdAt: 1000.0);
        e.AddDamage(skillId: 1, value: 500, timestamp: 1000.0);
        e.AddDamage(skillId: 1, value: 500, timestamp: 1010.0);
        Assert.Equal(1000.0, e.FirstDamageTime);
        Assert.Equal(1010.0, e.LastDamageTime);
        Assert.Equal(10.0, e.GetElapsedSeconds(), 3);
        Assert.Equal(100L, e.Dps); // 1000 / 10
    }

    [Fact]
    public void AddDamageRoutesToPerSkillBuckets()
    {
        var e = new EntityStats(1);
        e.AddDamage(100, 50, skillName: "A", timestamp: 1.0);
        e.AddDamage(101, 70, skillName: "B", timestamp: 2.0);
        Assert.Equal(2, e.Skills.Count);
        Assert.Equal(50, e.Skills[100].Total);
        Assert.Equal(70, e.Skills[101].Total);
    }

    [Fact]
    public void ToDictionaryWithSkillsSortsBySkillTotalDesc()
    {
        var e = new EntityStats(1);
        e.AddDamage(100, 50, timestamp: 1);
        e.AddDamage(101, 200, timestamp: 2);
        e.AddDamage(100, 30, timestamp: 3);
        var d = e.ToDictionary(includeSkills: true);
        var skills = (List<Dictionary<string, object>>)d["skills"];
        Assert.Equal(101L, skills[0]["skill_id"]);
        Assert.Equal(100L, skills[1]["skill_id"]);
    }
}

public class CombatSnapshotTests
{
    [Fact]
    public void DropsIdleZeroEntities()
    {
        var dict = new Dictionary<ulong, EntityStats>
        {
            [1] = new(1, "ActiveSelf", isSelf: true, createdAt: 0),
            [2] = new(2, "IdleZero", createdAt: 0),
        };
        dict[1].AddDamage(100, 1000, timestamp: 5);

        var list = CombatSnapshot.Build(dict, now: 60.0, idleRemoveSeconds: 30,
            includeSkills: false, playerCache: null, totalDamage: 1000);
        Assert.Single(list);
        Assert.Equal("ActiveSelf", list[0]["name"]);
    }

    [Fact]
    public void SortsByDamageTotalDesc()
    {
        var dict = new Dictionary<ulong, EntityStats>
        {
            [1] = new(1, "Low", createdAt: 0),
            [2] = new(2, "High", createdAt: 0),
        };
        dict[1].AddDamage(1, 100, timestamp: 1);
        dict[2].AddDamage(1, 500, timestamp: 1);
        var list = CombatSnapshot.Build(dict, 1, idleRemoveSeconds: 30, false, null, 600);
        Assert.Equal("High", list[0]["name"]);
        Assert.Equal("Low", list[1]["name"]);
    }

    [Fact]
    public void FillsPercentages()
    {
        var dict = new Dictionary<ulong, EntityStats>
        {
            [1] = new(1, "A", createdAt: 0),
            [2] = new(2, "B", createdAt: 0),
        };
        dict[1].AddDamage(1, 250, timestamp: 1);
        dict[2].AddDamage(1, 750, timestamp: 1);
        var list = CombatSnapshot.Build(dict, 1, 30, false, null, 1000);
        // B: damage_pct = 750/1000 = 0.75; bar_pct = 750/750 = 1.0
        Assert.Equal(0.75, list[0]["damage_pct"]);
        Assert.Equal(1.0, list[0]["bar_pct"]);
        Assert.Equal(0.25, list[1]["damage_pct"]);
        // 250/750 rounded to 3 decimals (Python parity)
        Assert.Equal(0.333, list[1]["bar_pct"]);
    }

    [Fact]
    public void BackfillsFightPointFromCacheKeyedByUid()
    {
        var dict = new Dictionary<ulong, EntityStats>
        {
            [42] = new(42, "Hero", createdAt: 0, fightPoint: 0),
        };
        dict[42].AddDamage(1, 100, timestamp: 1);
        var cache = new Dictionary<string, long> { ["42"] = 9999 };
        var list = CombatSnapshot.Build(dict, 1, 30, false, cache, 100);
        Assert.Equal(9999L, list[0]["fight_point"]);
    }

    [Fact]
    public void KeepsIdleSelfEvenWithoutDamage()
    {
        var dict = new Dictionary<ulong, EntityStats>
        {
            [1] = new(1, "Self", isSelf: true, createdAt: 0),
        };
        var list = CombatSnapshot.Build(dict, now: 999.0, idleRemoveSeconds: 1,
            includeSkills: false, playerCache: null, totalDamage: 0);
        Assert.Single(list);
    }

    [Theory]
    [InlineData(50, 100, 200, 500, false, "")]
    [InlineData(150, 100, 200, 500, true, "impact")]
    [InlineData(250, 100, 200, 500, true, "mega")]
    [InlineData(600, 100, 200, 500, true, "starburst")]
    [InlineData(500, 100, 200, 500, true, "starburst")] // boundary
    [InlineData(200, 100, 200, 500, true, "mega")]      // boundary
    [InlineData(100, 100, 200, 500, true, "impact")]    // boundary
    public void ClassifyBigHitTierMatchesPython(
        long damage, long big, long mega, long starburst, bool emit, string tier)
    {
        var (e, t) = CombatSnapshot.ClassifyBigHitTier(damage, big, mega, starburst);
        Assert.Equal(emit, e);
        Assert.Equal(tier, t);
    }
}
