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
    private readonly Dictionary<long, string> _skillNames = new();
    private readonly EncounterTracker _encounter = new();
    private DateTimeOffset _lastEvent;
    private DateTimeOffset _lastDamageTime;
    private bool _dirty;
    private readonly Func<DateTimeOffset> _clock;
    private DpsSnapshot? _lastReport;

    // DPS-01 / DPS-05: boss-filtered total. Mirrors Python's
    // `_boss_uuid` + `_total_damage_boss` (dps_tracker.py:148-151, 304-305).
    // When SetBossUuid is called and an incoming RecordDamage's targetUuid
    // matches, damage also accumulates into _totalDamageBoss so the panel
    // can render boss-only stats. Zero until SetBossUuid lands.
    private ulong _bossUuid;
    private long _totalDamageBoss;

    // DPS-04 / DPS-06: self uid for FN-009-style late-upgrade of phantom
    // Player_{uid} rows. Stored as ulong so 0 is the unset sentinel
    // matching SelfUuid >> 16 semantics in PacketBridge.
    private ulong _selfUid;

    public DpsTracker(Func<DateTimeOffset>? clock = null)
    {
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
    }

    public DpsSnapshot? LastReport
    {
        get { lock (_gate) return _lastReport; }
    }

    /// <summary>Wall-clock instant of the last RecordDamage call. Mirrors
    /// Python <c>self._last_damage_time</c> which is only bumped on the damage
    /// path (dps_tracker.py:306) — heals do NOT advance this.</summary>
    public DateTimeOffset LastDamageTime
    {
        get { lock (_gate) return _lastDamageTime; }
    }

    /// <summary>Read-and-clear dirty bit. Mirrors Python
    /// <c>DpsTracker.is_dirty</c> (dps_tracker.py:596-601): returns the
    /// current dirty flag and resets it to false under the lock.</summary>
    public bool IsDirty()
    {
        lock (_gate)
        {
            var d = _dirty;
            _dirty = false;
            return d;
        }
    }

    /// <summary>Force the next snapshot consumer to rebuild. Mirrors
    /// Python <c>invalidate_snapshot_cache</c> (dps_tracker.py:574-576);
    /// we have no snapshot cache yet, so we just raise the dirty bit so
    /// poll-style consumers see a change.</summary>
    public void InvalidateSnapshotCache()
    {
        lock (_gate) { _dirty = true; }
    }

    /// <summary>Convert a wall-clock instant into the monotonic
    /// double-seconds scale that <see cref="EncounterTracker"/> uses
    /// (mirrors Python <c>time.monotonic()</c> resolution).</summary>
    private static double ToSeconds(DateTimeOffset t) =>
        t.UtcTicks / (double)TimeSpan.TicksPerSecond;

    public void RecordDamage(long entityUuid, string entityName, long amount, int professionId, bool isSelf,
        int skillId = 0, string? skillName = null, bool isCrit = false, int skillKey = 0,
        long targetUuid = 0)
    {
        if (amount <= 0) return;
        var now = _clock();
        lock (_gate)
        {
            _encounter.OnDamage(amount, ToSeconds(now));
            _lastEvent = now;
            _lastDamageTime = now;
            _dirty = true;
            // DPS-01: boss-filtered accumulation. Optional argument keeps the
            // existing call-sites source-compatible. Mirrors Python
            // dps_tracker.py:304-305 (`if self._boss_uuid and target_uuid ==
            // self._boss_uuid: self._total_damage_boss += damage`).
            if (_bossUuid != 0 && targetUuid != 0 && (ulong)targetUuid == _bossUuid)
            {
                _totalDamageBoss += amount;
            }
            var stats = GetOrAddLocked(entityUuid, entityName, professionId, isSelf);
            stats.Damage += amount;
            if (skillId != 0 || !string.IsNullOrEmpty(skillName) || skillKey != 0)
            {
                // FN-002: mirror dps_tracker.py:279-283 — when the caller hands
                // us a skillName use it as-is, otherwise resolve through the
                // _skillNames table (skill_key → skill_id → numeric fallback).
                var name = skillName ?? ResolveSkillNameLocked(skillKey, skillId);
                var skill = stats.GetOrAddSkill(skillId, name);
                skill.Total += amount;
                skill.Hits++;
                if (isCrit) skill.CritHits++;
                if (amount > skill.MaxHit) skill.MaxHit = amount;
            }
        }
    }

    public void RecordHeal(long entityUuid, string entityName, long amount, int professionId, bool isSelf,
        int skillId = 0, string? skillName = null, int skillKey = 0)
    {
        if (amount <= 0) return;
        var now = _clock();
        lock (_gate)
        {
            _encounter.OnHeal(amount, ToSeconds(now));
            _lastEvent = now;
            _dirty = true;
            var stats = GetOrAddLocked(entityUuid, entityName, professionId, isSelf);
            stats.Heal += amount;
            if (skillId != 0 || !string.IsNullOrEmpty(skillName) || skillKey != 0)
            {
                // FN-002: mirror dps_tracker.py:279-283 — see RecordDamage above.
                var name = skillName ?? ResolveSkillNameLocked(skillKey, skillId);
                var skill = stats.GetOrAddSkill(skillId, name);
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
            _lastDamageTime = default;
            _dirty = false;
            _lastReport = null;
            _totalDamageBoss = 0;
            // Boss uuid + self uid persist across encounter resets — they're
            // session-level identity, not per-encounter state. Mirrors Python
            // dps_tracker.py:441-447 reset() which keeps _boss_uuid + _self_uid.
        }
    }

    /// <summary>
    /// DPS-01: pin the current boss uuid so subsequent RecordDamage calls
    /// with matching <c>targetUuid</c> also accumulate into
    /// <see cref="DpsSnapshot.TotalDamageBoss"/>. Mirrors Python
    /// <c>set_boss_uuid</c> (dps_tracker.py:148-151). Pass 0 to clear.
    /// </summary>
    public void SetBossUuid(ulong uuid)
    {
        lock (_gate)
        {
            if (_bossUuid == uuid) return;
            _bossUuid = uuid;
            // Boss switch resets the boss-filtered total so the new boss
            // starts at 0; otherwise we'd display a phantom carry-over from
            // the previous boss. Python's set_boss_uuid does the same.
            _totalDamageBoss = 0;
        }
    }

    /// <summary>
    /// DPS-04: pin the local-player uid so first-damage events with
    /// <c>isSelf</c> derived from <see cref="SelfUid"/> avoid the
    /// "Player_{uid} phantom row" symptom. Mirrors Python's
    /// <c>set_self_uid</c> (dps_tracker.py:144-146). Pass 0 to clear.
    /// </summary>
    public void SetSelfUid(ulong uid)
    {
        lock (_gate) { _selfUid = uid; }
    }

    /// <summary>Current boss uuid; 0 when no boss is pinned.</summary>
    public ulong BossUuid
    {
        get { lock (_gate) return _bossUuid; }
    }

    /// <summary>Current self uid; 0 when not yet confirmed.</summary>
    public ulong SelfUid
    {
        get { lock (_gate) return _selfUid; }
    }

    /// <summary>
    /// R8 / DPS-02: combined poll for the overlay pump. Mirrors Python
    /// <c>dps_tracker.poll_overlay_state</c> (dps_tracker.py:603-677):
    /// finalize on idle first, then return the canonical visibility flags
    /// the panel needs (<paramref name="result"/>.HasLive,
    /// <see cref="DpsPollResult.ShouldFadeOut"/>) along with the live
    /// snapshot. Caller is the per-tick overlay pump (DPS-03); it gates
    /// SetVisible+FadeIn / FadeOut on the booleans below.
    /// </summary>
    public DpsPollResult PollOverlayState(TimeSpan idleTimeout, bool includeSkills = false)
    {
        // Finalize first so an idle-timed-out encounter rolls into LastReport
        // before we read has_live (matches Python's order at line 626-631).
        var finalized = FinalizeIfIdle(idleTimeout, reason: "idle_timeout");
        var now = _clock();
        lock (_gate)
        {
            if (finalized is not null)
            {
                var finalizedDirty = _dirty;
                _dirty = false;
                return new DpsPollResult(
                    finalized,
                    _lastReport,
                    HasLive: false,
                    HasReport: true,
                    ShouldFadeOut: true,
                    Dirty: finalizedDirty,
                    _lastDamageTime);
            }

            var snap = SnapshotLocked(includeSkills, reason: null);
            var lastDmg = _lastDamageTime;
            var sinceLastDamage = lastDmg == default ? TimeSpan.MaxValue : (now - lastDmg);
            var totalDamage = snap.TotalDamage;
            var hasLive = _encounter.Active
                && totalDamage > 0
                && sinceLastDamage < idleTimeout;
            var shouldFadeOut = _encounter.Active
                && totalDamage > 0
                && sinceLastDamage >= idleTimeout;
            var dirty = _dirty;
            _dirty = false;
            return new DpsPollResult(
                snap,
                _lastReport,
                hasLive,
                _lastReport is not null,
                shouldFadeOut,
                dirty,
                lastDmg);
        }
    }

    /// <summary>
    /// Merge a numeric-id → display-name table into the tracker. Mirrors
    /// <c>dps_tracker.set_skill_names</c> (dps_tracker.py:236-238) — the
    /// loader in <c>sao_webview.py:3215-3229</c> passes the contents of
    /// <c>assets/skill_names.json</c> through this on startup. Existing
    /// entries are overwritten so callers can re-feed a fresh map.
    /// </summary>
    public void SetSkillNames(IReadOnlyDictionary<long, string> names)
    {
        if (names is null) return;
        lock (_gate)
        {
            foreach (var kv in names)
            {
                _skillNames[kv.Key] = kv.Value;
            }
        }
    }

    /// <summary>
    /// Resolve a skill key / id pair to a display name. Mirrors
    /// <c>dps_tracker.py:279-283</c>
    /// (<c>_skill_names.get(skill_key) or _skill_names.get(skill_id) or str(skill_id or skill_key)</c>)
    /// plus the base-id strip from <c>packet_bridge.py:70-87</c>
    /// (<c>skill_id // 100</c>) so skill_level_ids of the form
    /// <c>skill_id * 100 + level</c> still resolve to the base skill's name.
    /// </summary>
    internal string ResolveSkillName(long skillKey, long skillId)
    {
        lock (_gate)
        {
            return ResolveSkillNameLocked(skillKey, skillId);
        }
    }

    /// <summary>No-lock body of <see cref="ResolveSkillName"/>. Caller must
    /// already hold <c>_gate</c> — used by <see cref="RecordDamage"/> and
    /// <see cref="RecordHeal"/> which lock once for the whole event so we
    /// avoid reentering the same monitor (and the latency hit).</summary>
    private string ResolveSkillNameLocked(long skillKey, long skillId)
    {
        if (skillKey != 0 && _skillNames.TryGetValue(skillKey, out var n)) return n;
        if (skillId != 0 && _skillNames.TryGetValue(skillId, out var n2)) return n2;
        if (skillId >= 100 && _skillNames.TryGetValue(skillId / 100, out var n3)) return n3;
        var fallback = skillId > 0 ? skillId : skillKey;
        return fallback.ToString(CultureInfo.InvariantCulture);
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
        else if (isSelf && !stats.IsSelf)
        {
            // FN-009: late-upgrade self flag — mirrors Python
            // dps_tracker.py:349-350 `_get_or_create` which sets
            // entity.is_self=True on subsequent matching calls. Fixes
            // the "first damage stuck on Player_{uid} record" symptom
            // when EnterGame / SelfUuid confirmation lands after the
            // first damage event has already created the row.
            stats.IsSelf = true;
            if (!string.IsNullOrEmpty(name)
                && stats.EntityName.StartsWith("Player_", StringComparison.Ordinal))
            {
                stats.EntityName = name;
            }
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
            reason)
        {
            TotalDamageBoss = _totalDamageBoss,
        };
    }

    private sealed class EntityStats
    {
        public long EntityUuid { get; }
        public string EntityName { get; set; }
        public int ProfessionId { get; }
        public bool IsSelf { get; set; }
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
            else if (string.IsNullOrEmpty(s.Name) && !string.IsNullOrEmpty(name))
            {
                // FN-002: late-arrival name upgrade — only fill when the
                // existing slot has no name yet, never overwrite a name we
                // already accepted. Mirrors Python _get_or_create_skill in
                // _sao_cy_combat.pyx:392-400 (creates with the supplied
                // name, then leaves it alone on subsequent hits).
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
    string? ReportReason = null)
{
    /// <summary>DPS-01: boss-filtered total damage. Mirrors Python's
    /// <c>total_damage_boss</c> at dps_tracker.py:483 — accumulated only on
    /// RecordDamage rows whose <c>targetUuid</c> matches the pinned
    /// <see cref="DpsTracker.BossUuid"/>. Zero when no boss is pinned or
    /// no boss-targeted damage has landed in the current encounter.</summary>
    public long TotalDamageBoss { get; init; }
}

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

/// <summary>
/// R8 / DPS-02: structured result of <see cref="DpsTracker.PollOverlayState"/>.
/// Mirrors the dict Python returns from poll_overlay_state at
/// dps_tracker.py:603-677: <c>snapshot</c> + the canonical visibility
/// flags the overlay pump uses to drive show-live edge / fade-out edge.
/// </summary>
public sealed record DpsPollResult(
    DpsSnapshot Snapshot,
    DpsSnapshot? LastReport,
    bool HasLive,
    bool HasReport,
    bool ShouldFadeOut,
    bool Dirty,
    DateTimeOffset LastDamageTime);
