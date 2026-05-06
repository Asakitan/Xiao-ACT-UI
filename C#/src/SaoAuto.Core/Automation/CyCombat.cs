using System.Collections.Immutable;

namespace SaoAuto.Core.Automation;

/// <summary>
/// Combat-side helpers ported from <c>_sao_cy_combat.pyx</c>: UUID
/// classification (player / monster / self), damage-amount picker,
/// per-skill / per-entity stat accumulators, snapshot builder.
/// Replaces <see cref="DpsTracker"/> for the live aggregator path —
/// the simpler tracker remains for cases where per-skill detail is
/// unnecessary.
/// </summary>
public static class CyCombat
{
    public const ulong PlayerUuidMarker = 640;
    public const ulong MonsterUuidMarker = 64;
    public const ulong MonsterUuidMarker2 = 32832;

    public static bool IsPlayerUuid(ulong uuid) => (uuid & 0xFFFF) == PlayerUuidMarker;

    public static bool IsMonsterUuid(ulong uuid)
    {
        var low = uuid & 0xFFFF;
        return low == MonsterUuidMarker || low == MonsterUuidMarker2;
    }

    public static ulong UuidToUid(ulong uuid) => uuid >> 16;

    /// <summary>
    /// Pick the canonical damage amount from the parallel value fields,
    /// matching Python's preference: <c>value</c> → <c>luckyValue</c> →
    /// <c>actualValue</c> → <c>hpLessen + shieldLessen</c>.
    /// </summary>
    public static long CombatDamageAmount(
        long value, long luckyValue, long actualValue, long hpLessen, long shieldLessen)
    {
        if (value > 0) return value;
        if (luckyValue > 0) return luckyValue;
        if (actualValue > 0) return actualValue;
        if (hpLessen < 0) hpLessen = 0;
        if (shieldLessen < 0) shieldLessen = 0;
        return hpLessen + shieldLessen;
    }

    public static bool TargetIsCombatTarget(ulong targetUuid, bool targetIsPlayer) =>
        !targetIsPlayer && targetUuid > 0;

    public static bool AttackerIsSelf(ulong attackerUuid, ulong currentUuid, ulong currentUid)
    {
        if (attackerUuid == 0) return false;
        if (currentUuid != 0 && attackerUuid == currentUuid) return true;
        if ((attackerUuid & 0xFFFF) != PlayerUuidMarker) return false;
        if (currentUid != 0 && (attackerUuid >> 16) == currentUid) return true;
        return false;
    }

    public static bool DpsTargetIsCombat(
        ulong targetUuid,
        bool hasTargetIsPlayer,
        bool targetIsPlayer,
        bool targetIsMonster,
        bool targetIsCombatTarget)
    {
        if (targetIsCombatTarget || targetIsMonster) return true;
        if (hasTargetIsPlayer && !targetIsPlayer && targetUuid > 0) return true;
        return false;
    }

    public static ulong DpsAttackerUid(ulong attackerUuid, bool attackerIsSelf, ulong selfUid)
    {
        if (attackerUuid > 0 && (attackerUuid & 0xFFFF) == PlayerUuidMarker)
        {
            return attackerUuid >> 16;
        }
        if (attackerIsSelf && selfUid > 0) return selfUid;
        return 0;
    }
}

/// <summary>
/// Per-skill stats. Mirrors <c>CySkillStats</c>: total damage / hits /
/// crit hits / max single hit / heal totals, plus the <see cref="ToDictionary"/>
/// equivalent of the Python `to_dict` output.
/// </summary>
public sealed class SkillStats
{
    public long SkillId { get; }
    public string SkillName { get; set; }
    public long Total { get; private set; }
    public long Hits { get; private set; }
    public long CritHits { get; private set; }
    public long MaxHit { get; private set; }
    public long HealTotal { get; private set; }
    public long HealHits { get; private set; }

    public SkillStats(long skillId, string? skillName = null)
    {
        SkillId = skillId;
        SkillName = string.IsNullOrEmpty(skillName) ? skillId.ToString() : skillName!;
    }

    public void AddDamage(long value, bool isCrit = false)
    {
        Total += value;
        Hits++;
        if (isCrit) CritHits++;
        if (value > MaxHit) MaxHit = value;
    }

    public void AddHeal(long value)
    {
        HealTotal += value;
        HealHits++;
    }

    public double CritRate => Hits > 0 ? Math.Round((double)CritHits / Hits, 3) : 0.0;

    public Dictionary<string, object> ToDictionary() => new()
    {
        ["skill_id"] = SkillId,
        ["skill_name"] = SkillName,
        ["total"] = Total,
        ["hits"] = Hits,
        ["crit_hits"] = CritHits,
        ["crit_rate"] = CritRate,
        ["max_hit"] = MaxHit,
        ["heal_total"] = HealTotal,
        ["heal_hits"] = HealHits,
    };
}

/// <summary>Per-entity stats. Mirrors <c>CyEntityStats</c>.</summary>
public sealed class EntityStats
{
    public ulong Uid { get; }
    public string Name { get; set; }
    public string Profession { get; set; }
    public long FightPoint { get; set; }
    public bool IsSelf { get; set; }
    public long DamageTotal { get; private set; }
    public long DamageHits { get; private set; }
    public long DamageCritHits { get; private set; }
    public long HealTotal { get; private set; }
    public long HealHits { get; private set; }
    public long TakenTotal { get; private set; }
    public long TakenHits { get; private set; }
    public double FirstDamageTime { get; private set; }
    public double LastDamageTime { get; private set; }
    public long MaxHit { get; private set; }
    public double CreatedAt { get; }

    public Dictionary<long, SkillStats> Skills { get; } = new();

    public EntityStats(ulong uid, string? name = null, string? profession = null,
                       bool isSelf = false, long fightPoint = 0, double? createdAt = null)
    {
        Uid = uid;
        Name = string.IsNullOrEmpty(name) ? $"Player_{uid}" : name!;
        Profession = profession ?? string.Empty;
        FightPoint = fightPoint;
        IsSelf = isSelf;
        CreatedAt = createdAt ?? UnixSeconds();
    }

    public void AddDamage(long skillId, long value, bool isCrit = false,
                          string? skillName = null, double? timestamp = null)
    {
        var ts = timestamp.HasValue && timestamp.Value > 0 ? timestamp.Value : UnixSeconds();
        DamageTotal += value;
        DamageHits++;
        if (isCrit) DamageCritHits++;
        if (value > MaxHit) MaxHit = value;
        if (FirstDamageTime == 0.0) FirstDamageTime = ts;
        LastDamageTime = ts;
        GetOrCreate(skillId, skillName).AddDamage(value, isCrit);
    }

    public void AddHeal(long skillId, long value, string? skillName = null, double? timestamp = null)
    {
        var ts = timestamp.HasValue && timestamp.Value > 0 ? timestamp.Value : UnixSeconds();
        HealTotal += value;
        HealHits++;
        if (FirstDamageTime == 0.0) FirstDamageTime = ts;
        LastDamageTime = ts;
        GetOrCreate(skillId, skillName).AddHeal(value);
    }

    public void AddTaken(long value)
    {
        TakenTotal += value;
        TakenHits++;
    }

    public double GetElapsedSeconds()
    {
        if (FirstDamageTime > 0 && LastDamageTime > 0)
        {
            var span = LastDamageTime - FirstDamageTime;
            return span < 0.001 ? 0.001 : span;
        }
        return 0.001;
    }

    public long Dps => DamageTotal <= 0 ? 0 : (long)(DamageTotal / GetElapsedSeconds());
    public long Hps => HealTotal <= 0 ? 0 : (long)(HealTotal / GetElapsedSeconds());
    public double CritRate => DamageHits > 0 ? Math.Round((double)DamageCritHits / DamageHits, 3) : 0.0;

    public Dictionary<string, object> ToDictionary(bool includeSkills = false)
    {
        var elapsed = GetElapsedSeconds();
        var dict = new Dictionary<string, object>
        {
            ["uid"] = Uid,
            ["name"] = Name,
            ["profession"] = Profession,
            ["fight_point"] = FightPoint,
            ["is_self"] = IsSelf,
            ["damage_total"] = DamageTotal,
            ["damage_hits"] = DamageHits,
            ["damage_crit_hits"] = DamageCritHits,
            ["crit_rate"] = CritRate,
            ["heal_total"] = HealTotal,
            ["heal_hits"] = HealHits,
            ["taken_total"] = TakenTotal,
            ["taken_hits"] = TakenHits,
            ["dps"] = Dps,
            ["hps"] = Hps,
            ["max_hit"] = MaxHit,
            ["elapsed_s"] = Math.Round(elapsed, 1),
        };
        if (includeSkills)
        {
            var list = Skills.Values.Select(s => s.ToDictionary()).ToList();
            list.Sort((a, b) => Comparer<long>.Default.Compare((long)b["total"], (long)a["total"]));
            dict["skills"] = list;
        }
        return dict;
    }

    private SkillStats GetOrCreate(long skillId, string? skillName)
    {
        if (!Skills.TryGetValue(skillId, out var s))
        {
            s = new SkillStats(skillId, skillName);
            Skills[skillId] = s;
        }
        else if (!string.IsNullOrEmpty(skillName) && s.SkillName != skillName)
        {
            s.SkillName = skillName!;
        }
        return s;
    }

    private static double UnixSeconds() =>
        DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0;
}

/// <summary>
/// Build snapshot equivalent to Python's <c>build_entity_snapshot</c>:
/// drop idle freshly-created entities, sort by damage desc, fill
/// percentages, backfill fight_point from a name→fight_point cache.
/// </summary>
public static class CombatSnapshot
{
    /// <summary>
    /// Filter, serialize, sort and percent-fill per-entity entries. Mirrors
    /// <c>build_entity_snapshot</c>: drops freshly-created idle entities
    /// after <paramref name="idleRemoveSeconds"/>, sorts by damage_total
    /// desc, fills <c>damage_pct</c>/<c>bar_pct</c> rounded to 3 decimals,
    /// backfills <c>fight_point</c> from <paramref name="playerCache"/>
    /// (keyed by the uid as a string).
    /// </summary>
    public static List<Dictionary<string, object>> Build(
        IDictionary<ulong, EntityStats> entities,
        double now,
        double idleRemoveSeconds,
        bool includeSkills,
        IDictionary<string, long>? playerCache,
        long totalDamage)
    {
        var serialized = new List<Dictionary<string, object>>();
        foreach (var e in entities.Values)
        {
            // Python predicate: keep if damage>0 OR heal>0 OR is_self OR still inside the idle window.
            bool keep = e.DamageTotal > 0
                        || e.HealTotal > 0
                        || e.IsSelf
                        || (now - e.CreatedAt) < idleRemoveSeconds;
            if (!keep) continue;
            serialized.Add(e.ToDictionary(includeSkills));
        }

        serialized.Sort((a, b) => Comparer<long>.Default.Compare(
            (long)b["damage_total"], (long)a["damage_total"]));

        long denomTotal = totalDamage > 0 ? totalDamage : 1;
        long maxDamage = serialized.Count > 0 ? (long)serialized[0]["damage_total"] : 0;
        long denomMax = maxDamage > 0 ? maxDamage : 1;

        foreach (var d in serialized)
        {
            var dmg = (long)d["damage_total"];
            d["damage_pct"] = Math.Round((double)dmg / denomTotal, 3);
            d["bar_pct"] = Math.Round((double)dmg / denomMax, 3);
            // Backfill fight_point from cache when the entity reports zero.
            // Python keys the cache by str(uid).
            long currentFp = d["fight_point"] is long fpLong ? fpLong : 0;
            if (currentFp == 0 && playerCache is { } pc)
            {
                var key = ((ulong)d["uid"]).ToString();
                if (pc.TryGetValue(key, out var cachedFp) && cachedFp > 0)
                {
                    d["fight_point"] = cachedFp;
                }
            }
        }
        return serialized;
    }

    /// <summary>
    /// Classify a damage amount against the three big-hit thresholds.
    /// Returns <c>(false, "")</c> when below <paramref name="bigThreshold"/>;
    /// otherwise <c>(true, tier)</c> with tier in {"impact", "mega", "starburst"}.
    /// Mirrors <c>classify_big_hit_tier</c>.
    /// </summary>
    public static (bool Emit, string Tier) ClassifyBigHitTier(
        long damage, long bigThreshold, long megaThreshold, long starburstThreshold)
    {
        if (damage < bigThreshold) return (false, string.Empty);
        if (damage >= starburstThreshold) return (true, "starburst");
        if (damage >= megaThreshold) return (true, "mega");
        return (true, "impact");
    }
}
