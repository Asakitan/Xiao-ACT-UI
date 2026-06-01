using System.Collections.Immutable;

namespace SaoAuto.Core.Automation;

/// <summary>
/// BossRaid phase state machine ported from <c>boss_raid_engine.py</c>.
/// Phases run in sequence; an enrage countdown ticks while the engine is
/// active. The engine fires `PhaseEntered` / `EnrageImminent` events that
/// the overlay subscribes to.
/// </summary>
public sealed class BossRaidEngine
{
    private readonly object _gate = new();
    private readonly Func<DateTimeOffset> _clock;
    private ImmutableArray<RaidPhase> _phases = ImmutableArray<RaidPhase>.Empty;
    private int _phaseIndex = -1;
    private DateTimeOffset _phaseStart;
    private double _enrageSeconds;
    private DateTimeOffset _enrageDeadline;
    private bool _running;

    // P1 / BR-1 — damage accumulation + entity table. Mirrors Python's
    // boss_raid_engine.py:756-838 OnDamageEvent + entity-list tracking.
    // Keyed by uuid; each entry pins (name, role, damage_dealt, hit_count,
    // first_seen, last_seen, hp, max_hp, shield_*, breaking_stage).
    private long _totalDamage;
    private long _bossUuid;
    private bool _bossManuallySet;
    private bool _bossInvincible;
    private int _immuneStreak;
    private DateTimeOffset _immuneWindowStart;
    private readonly Dictionary<long, BossRaidEntity> _entities = new();
    private readonly List<long> _entityOrder = new();

    public BossRaidEngine(Func<DateTimeOffset>? clock = null)
    {
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
    }

    public event Action<RaidPhase>? PhaseEntered;
    public event Action? RaidStarted;
    public event Action? RaidEnded;
    /// <summary>P1 / BR-3 — fired when the engine flips into / out of
    /// invincible state. Subscribers (auto-key gating, alert routing)
    /// use this to pause / resume.</summary>
    public event Action<bool>? InvincibleChanged;
    /// <summary>P1 / BR-4 — fired every time the running snapshot changes
    /// in a way the HUD should observe (damage / entity table / boss
    /// switch). Drives the GameState push so the HP overlay can update.</summary>
    public event Action? StateChanged;

    public long TotalDamage { get { lock (_gate) return _totalDamage; } }
    public long BossUuid { get { lock (_gate) return _bossUuid; } }
    public bool BossInvincible { get { lock (_gate) return _bossInvincible; } }
    public IReadOnlyList<BossRaidEntity> Entities
    {
        get
        {
            lock (_gate)
            {
                var snap = new BossRaidEntity[_entityOrder.Count];
                for (int i = 0; i < _entityOrder.Count; i++)
                {
                    snap[i] = _entities[_entityOrder[i]];
                }
                return snap;
            }
        }
    }

    public bool Running { get { lock (_gate) return _running; } }

    public RaidPhase? CurrentPhase
    {
        get
        {
            lock (_gate)
            {
                if (!_running || _phaseIndex < 0 || _phaseIndex >= _phases.Length) return null;
                return _phases[_phaseIndex];
            }
        }
    }

    public double EnrageRemainingSeconds
    {
        get
        {
            lock (_gate)
            {
                if (!_running || _enrageSeconds <= 0) return 0;
                var remaining = (_enrageDeadline - _clock()).TotalSeconds;
                return Math.Max(0, remaining);
            }
        }
    }

    public void Start(IEnumerable<RaidPhase> phases, double enrageSeconds = 0)
    {
        var arr = phases?.ToImmutableArray() ?? ImmutableArray<RaidPhase>.Empty;
        if (arr.IsEmpty) throw new ArgumentException("at least one phase required", nameof(phases));

        RaidPhase entered;
        lock (_gate)
        {
            _phases = arr;
            _phaseIndex = 0;
            _phaseStart = _clock();
            _enrageSeconds = enrageSeconds;
            _enrageDeadline = enrageSeconds > 0 ? _phaseStart.AddSeconds(enrageSeconds) : default;
            _running = true;
            entered = arr[0];
        }
        try { RaidStarted?.Invoke(); } catch { /* swallow */ }
        try { PhaseEntered?.Invoke(entered); } catch { /* swallow */ }
    }

    public void NextPhase()
    {
        RaidPhase? entered = null;
        bool ended = false;
        lock (_gate)
        {
            if (!_running) return;
            _phaseIndex++;
            if (_phaseIndex >= _phases.Length)
            {
                _running = false;
                ended = true;
            }
            else
            {
                _phaseStart = _clock();
                entered = _phases[_phaseIndex];
            }
        }
        if (entered is { } phase)
        {
            try { PhaseEntered?.Invoke(phase); } catch { /* swallow */ }
        }
        if (ended)
        {
            try { RaidEnded?.Invoke(); } catch { /* swallow */ }
        }
    }

    public void Stop()
    {
        bool wasRunning;
        lock (_gate)
        {
            wasRunning = _running;
            _running = false;
            _phaseIndex = -1;
            // P1: clear per-encounter state so a subsequent Start opens fresh.
            _totalDamage = 0;
            _bossUuid = 0;
            _bossManuallySet = false;
            _bossInvincible = false;
            _immuneStreak = 0;
            _entities.Clear();
            _entityOrder.Clear();
        }
        if (wasRunning)
        {
            try { RaidEnded?.Invoke(); } catch { /* swallow */ }
        }
    }

    /// <summary>
    /// P1 / BR-1: ingest a damage event from <see cref="Bridge.PacketBridge"/>.
    /// Mirrors Python's <c>on_damage_event</c> at boss_raid_engine.py:756-838:
    /// auto-detects the boss (first monster attacked), tracks per-entity
    /// damage, detects invincibility via 3-in-5s immune streak, accumulates
    /// the global total. No-op when the engine isn't running.
    /// </summary>
    public void OnDamageEvent(
        long targetUuid,
        long damage,
        bool targetIsMonster,
        bool attackerIsSelf,
        bool isImmune,
        bool isAbsorbed,
        bool isHeal,
        string targetName = "")
    {
        bool fireInvincibleChanged = false;
        bool newInvincible = false;
        bool changed = false;
        lock (_gate)
        {
            if (!_running) return;
            if (!attackerIsSelf || !targetIsMonster || targetUuid == 0) return;
            var now = _clock();

            // Track entity row — first one becomes boss unless user pinned.
            if (!_entities.TryGetValue(targetUuid, out var ent))
            {
                var role = "unknown";
                if (!_bossManuallySet)
                {
                    role = _bossUuid == 0 ? "boss" : "enemy";
                }
                ent = new BossRaidEntity(
                    Uuid: targetUuid,
                    Name: targetName ?? string.Empty,
                    Role: role,
                    DamageDealt: 0,
                    HitCount: 0,
                    FirstSeen: now,
                    LastSeen: now);
                _entities[targetUuid] = ent;
                _entityOrder.Add(targetUuid);
                changed = true;
            }
            if (_bossUuid == 0 && !_bossManuallySet)
            {
                _bossUuid = targetUuid;
                if (_entities.TryGetValue(targetUuid, out var bossEnt))
                {
                    _entities[targetUuid] = bossEnt with { Role = "boss" };
                }
                changed = true;
            }

            var damageClamped = damage > 0 ? damage : 0;
            ent = _entities[targetUuid];
            var updated = ent with { LastSeen = now };
            if (!isImmune && !isAbsorbed && !isHeal && damageClamped > 0)
            {
                updated = updated with
                {
                    DamageDealt = ent.DamageDealt + damageClamped,
                    HitCount = ent.HitCount + 1,
                };
            }
            if (string.IsNullOrEmpty(updated.Name) && !string.IsNullOrEmpty(targetName))
            {
                updated = updated with { Name = targetName };
            }
            _entities[targetUuid] = updated;

            if (isImmune || isAbsorbed)
            {
                if (_immuneStreak == 0) _immuneWindowStart = now;
                _immuneStreak++;
                if (_immuneStreak >= 3 && (now - _immuneWindowStart).TotalSeconds < 5.0)
                {
                    if (!_bossInvincible)
                    {
                        _bossInvincible = true;
                        fireInvincibleChanged = true;
                        newInvincible = true;
                    }
                }
            }
            else
            {
                if (_bossInvincible)
                {
                    _bossInvincible = false;
                    fireInvincibleChanged = true;
                    newInvincible = false;
                }
                _immuneStreak = 0;
                if (!isHeal && damageClamped > 0)
                {
                    _totalDamage += damageClamped;
                    changed = true;
                }
            }
        }
        if (fireInvincibleChanged)
        {
            try { InvincibleChanged?.Invoke(newInvincible); } catch { /* swallow */ }
        }
        if (changed)
        {
            try { StateChanged?.Invoke(); } catch { /* swallow */ }
        }
    }

    /// <summary>P1 / BR-3: pin the boss uuid manually; the auto-detect path
    /// is skipped once this is called. Use 0 to clear.</summary>
    public void SetBossUuid(long uuid)
    {
        bool changed;
        lock (_gate)
        {
            _bossManuallySet = uuid != 0;
            if (_bossUuid == uuid) { changed = false; }
            else
            {
                _bossUuid = uuid;
                if (uuid != 0 && _entities.TryGetValue(uuid, out var ent))
                {
                    _entities[uuid] = ent with { Role = "boss" };
                }
                changed = true;
            }
        }
        if (changed) try { StateChanged?.Invoke(); } catch { /* swallow */ }
    }
}

public sealed record RaidPhase(
    int Index,
    string Name,
    double DurationSeconds,
    string? Reminder = null);

/// <summary>
/// P1 / BR-1 — per-entity row tracked by <see cref="BossRaidEngine"/>.
/// Mirrors Python's per-uuid dict at boss_raid_engine.py:787-798.
/// </summary>
public sealed record BossRaidEntity(
    long Uuid,
    string Name,
    string Role,
    long DamageDealt,
    int HitCount,
    DateTimeOffset FirstSeen,
    DateTimeOffset LastSeen)
{
    public int Hp { get; init; }
    public int MaxHp { get; init; }
    public bool ShieldActive { get; init; }
    public double ShieldPct { get; init; }
    public int BreakingStage { get; init; }
    public double ExtinctionPct { get; init; }
    public bool InOverdrive { get; init; }
};
