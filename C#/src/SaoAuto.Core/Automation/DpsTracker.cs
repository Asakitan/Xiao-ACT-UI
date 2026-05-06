using System.Collections.Immutable;
using System.Globalization;
using System.Text;

namespace SaoAuto.Core.Automation;

/// <summary>
/// DPS / HPS accumulator ported from <c>dps_tracker.DpsTracker</c>. Tracks
/// per-entity damage and heals, snapshots the live combat at any instant,
/// finalizes on idle, and exposes a per-skill breakdown plus a clipboard-
/// ready report formatter (mirrors the tail of <c>dps_tracker.py</c>:
/// <c>finalize_if_idle</c>, <c>get_entity_detail</c>, <c>format_report</c>).
/// </summary>
public sealed class DpsTracker
{
    private readonly object _gate = new();
    private readonly Dictionary<long, EntityStats> _entities = new();
    private readonly EncounterTracker _encounter = new();
    private DateTimeOffset _lastEvent;
    private readonly Func<DateTimeOffset> _clock;
    private DpsSnapshot? _lastReport;

    public DpsTracker(Func<DateTimeOffset>? clock = null)
    {
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
    }

    public DpsSnapshot? LastReport
    {
        get { lock (_gate) return _lastReport; }
    }

    /// <summary>Convert a wall-clock instant into the monotonic
    /// double-seconds scale that <see cref="EncounterTracker"/> uses
    /// (mirrors Python <c>time.monotonic()</c> resolution).</summary>
    private static double ToSeconds(DateTimeOffset t) =>
        t.UtcTicks / (double)TimeSpan.TicksPerSecond;

    public void RecordDamage(long entityUuid, string entityName, long amount, int professionId, bool isSelf,
        int skillId = 0, string? skillName = null, bool isCrit = false)
    {
        if (amount <= 0) return;
        var now = _clock();
        lock (_gate)
        {
            _encounter.OnDamage(amount, ToSeconds(now));
            _lastEvent = now;
            var stats = GetOrAddLocked(entityUuid, entityName, professionId, isSelf);
            stats.Damage += amount;
            if (skillId != 0 || !string.IsNullOrEmpty(skillName))
            {
                var skill = stats.GetOrAddSkill(skillId, skillName);
                skill.Total += amount;
                skill.Hits++;
                if (isCrit) skill.CritHits++;
                if (amount > skill.MaxHit) skill.MaxHit = amount;
            }
        }
    }

    public void RecordHeal(long entityUuid, string entityName, long amount, int professionId, bool isSelf,
        int skillId = 0, string? skillName = null)
    {
        if (amount <= 0) return;
        var now = _clock();
        lock (_gate)
        {
            _encounter.OnHeal(amount, ToSeconds(now));
            _lastEvent = now;
            var stats = GetOrAddLocked(entityUuid, entityName, professionId, isSelf);
            stats.Heal += amount;
            if (skillId != 0 || !string.IsNullOrEmpty(skillName))
            {
                var skill = stats.GetOrAddSkill(skillId, skillName);
                skill.HealTotal += amount;
                skill.HealHits++;
            }
        }
    }

    public DpsSnapshot Snapshot() => SnapshotInternal(includeSkills: false, reason: null);

    public DpsSnapshot SnapshotWithSkills() => SnapshotInternal(includeSkills: true, reason: null);

    /// <summary>
    /// Finalize the current encounter if no events have arrived for at
    /// least <paramref name="idleThreshold"/>. Stores the result on
    /// <see cref="LastReport"/> and returns the snapshot. Returns null when
    /// there's no active encounter or when the idle threshold hasn't elapsed.
    /// </summary>
    public DpsSnapshot? FinalizeIfIdle(TimeSpan idleThreshold, string reason = "idle_timeout")
    {
        var now = _clock();
        lock (_gate)
        {
            if (!_encounter.Active) return null;
            if ((now - _lastEvent) < idleThreshold) return null;
            var report = SnapshotLocked(includeSkills: true, reason);
            _lastReport = report;
            _entities.Clear();
            _encounter.Reset();
            _lastEvent = default;
            return report;
        }
    }

    public IReadOnlyList<SkillBreakdownRow> SkillBreakdown(long entityUuid)
    {
        lock (_gate)
        {
            if (!_entities.TryGetValue(entityUuid, out var stats))
                return Array.Empty<SkillBreakdownRow>();
            return stats.Skills.Values
                .Select(s => new SkillBreakdownRow(
                    s.SkillId, s.Name, s.Total, s.Hits, s.CritHits,
                    s.Hits > 0 ? (double)s.CritHits / s.Hits : 0,
                    s.MaxHit, s.HealTotal, s.HealHits))
                .OrderByDescending(r => r.Total)
                .ToArray();
        }
    }

    public void Reset()
    {
        lock (_gate)
        {
            _entities.Clear();
            _encounter.Reset();
            _lastEvent = default;
            _lastReport = null;
        }
    }

    /// <summary>
    /// Format a snapshot as a multi-line clipboard-ready report. Mirrors
    /// <c>dps_tracker.format_report</c>; produces a header + per-entity
    /// rows with damage / DPS / share %.
    /// </summary>
    public static string FormatReport(DpsSnapshot snap)
    {
        ArgumentNullException.ThrowIfNull(snap);
        var sb = new StringBuilder();
        var c = CultureInfo.InvariantCulture;
        sb.AppendLine($"DPS Report — duration {snap.DurationSeconds.ToString("F1", c)}s");
        sb.AppendLine($"Total damage: {snap.TotalDamage.ToString("N0", c)} ({snap.Dps.ToString("N0", c)} DPS)");
        if (snap.TotalHeal > 0)
        {
            sb.AppendLine($"Total heal:   {snap.TotalHeal.ToString("N0", c)} ({snap.Hps.ToString("N0", c)} HPS)");
        }
        if (!string.IsNullOrEmpty(snap.ReportReason))
        {
            sb.AppendLine($"Reason:       {snap.ReportReason}");
        }
        sb.AppendLine(new string('-', 60));
        var total = Math.Max(snap.TotalDamage, 1);
        foreach (var row in snap.Rows)
        {
            var pct = (double)row.Damage / total * 100.0;
            var marker = row.IsSelf ? "*" : " ";
            sb.AppendLine($"{marker} {row.EntityName,-20} {row.Damage,12:N0} {row.Dps,10:N0}/s {pct,6:F1}%");
        }
        return sb.ToString().TrimEnd();
    }

    private EntityStats GetOrAddLocked(long uuid, string name, int profession, bool isSelf)
    {
        if (!_entities.TryGetValue(uuid, out var stats))
        {
            stats = new EntityStats(uuid, name, profession, isSelf);
            _entities[uuid] = stats;
        }
        return stats;
    }

    private DpsSnapshot SnapshotInternal(bool includeSkills, string? reason)
    {
        lock (_gate) return SnapshotLocked(includeSkills, reason);
    }

    private DpsSnapshot SnapshotLocked(bool includeSkills, string? reason)
    {
        if (!_encounter.Active)
        {
            return new DpsSnapshot(false, 0, 0, 0, 0, 0, ImmutableArray<DpsEntitySnapshot>.Empty, reason);
        }
        var elapsed = _encounter.ElapsedSeconds;

        long totalDmg = 0, totalHeal = 0;
        var rows = ImmutableArray.CreateBuilder<DpsEntitySnapshot>();
        foreach (var s in _entities.Values)
        {
            totalDmg += s.Damage;
            totalHeal += s.Heal;
            ImmutableArray<SkillBreakdownRow> skills = ImmutableArray<SkillBreakdownRow>.Empty;
            if (includeSkills && s.Skills.Count > 0)
            {
                skills = s.Skills.Values
                    .Select(sk => new SkillBreakdownRow(
                        sk.SkillId, sk.Name, sk.Total, sk.Hits, sk.CritHits,
                        sk.Hits > 0 ? (double)sk.CritHits / sk.Hits : 0,
                        sk.MaxHit, sk.HealTotal, sk.HealHits))
                    .OrderByDescending(r => r.Total)
                    .ToImmutableArray();
            }
            rows.Add(new DpsEntitySnapshot(
                s.EntityUuid, s.EntityName, s.ProfessionId, s.IsSelf,
                s.Damage, (long)Math.Round(s.Damage / elapsed),
                s.Heal, (long)Math.Round(s.Heal / elapsed),
                skills));
        }
        rows.Sort((a, b) => b.Damage.CompareTo(a.Damage));

        return new DpsSnapshot(
            true,
            totalDmg,
            (long)Math.Round(totalDmg / elapsed),
            totalHeal,
            (long)Math.Round(totalHeal / elapsed),
            elapsed,
            rows.ToImmutable(),
            reason);
    }

    private sealed class EntityStats
    {
        public long EntityUuid { get; }
        public string EntityName { get; set; }
        public int ProfessionId { get; }
        public bool IsSelf { get; }
        public long Damage { get; set; }
        public long Heal { get; set; }
        public Dictionary<int, SkillStats> Skills { get; } = new();

        public EntityStats(long uuid, string name, int profession, bool isSelf)
        {
            EntityUuid = uuid;
            EntityName = name;
            ProfessionId = profession;
            IsSelf = isSelf;
        }

        public SkillStats GetOrAddSkill(int skillId, string? name)
        {
            if (!Skills.TryGetValue(skillId, out var s))
            {
                s = new SkillStats(skillId, name ?? string.Empty);
                Skills[skillId] = s;
            }
            else if (!string.IsNullOrEmpty(name) && s.Name != name)
            {
                s.Name = name;
            }
            return s;
        }
    }

    private sealed class SkillStats
    {
        public int SkillId { get; }
        public string Name { get; set; }
        public long Total { get; set; }
        public int Hits { get; set; }
        public int CritHits { get; set; }
        public long MaxHit { get; set; }
        public long HealTotal { get; set; }
        public int HealHits { get; set; }
        public SkillStats(int id, string name) { SkillId = id; Name = name; }
    }
}

public sealed record DpsSnapshot(
    bool Active,
    long TotalDamage,
    long Dps,
    long TotalHeal,
    long Hps,
    double DurationSeconds,
    ImmutableArray<DpsEntitySnapshot> Rows,
    string? ReportReason = null);

public sealed record DpsEntitySnapshot(
    long EntityUuid,
    string EntityName,
    int ProfessionId,
    bool IsSelf,
    long Damage,
    long Dps,
    long Heal,
    long Hps,
    ImmutableArray<SkillBreakdownRow> Skills = default);

public sealed record SkillBreakdownRow(
    int SkillId,
    string Name,
    long Total,
    int Hits,
    int CritHits,
    double CritRate,
    long MaxHit,
    long HealTotal,
    int HealHits);
