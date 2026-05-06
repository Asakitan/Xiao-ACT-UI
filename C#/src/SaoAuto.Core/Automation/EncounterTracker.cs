namespace SaoAuto.Core.Automation;

/// <summary>
/// Stateful encounter window tracker — port of the
/// <c>_encounter_start</c> / <c>_encounter_end</c> /
/// <c>_total_damage</c> / <c>_total_heal</c> bookkeeping inside
/// <c>dps_tracker.py::DpsTracker</c>. Holds nothing about which
/// entities did damage; that lives in <see cref="EntityStats"/>.
///
/// Why a separate type: the C# <see cref="DpsTracker"/> currently
/// derives encounter timing from the <c>_lastEvent</c> /
/// <c>_combatStart</c> pair, which doesn't match Python's
/// "encounter is open until reset" semantics. This class encodes
/// the Python rule (no idle split inside the lifetime, only an
/// explicit reset closes the window) so the snapshot fields
/// (<c>encounter_active</c>, <c>encounter_started_at</c>,
/// <c>encounter_ended_at</c>, <c>elapsed_s</c>) line up bit-exact.
/// </summary>
public sealed class EncounterTracker
{
    /// <summary>Floor used for elapsed time when the encounter has
    /// barely started (Python <c>max(0.001, ...)</c>).</summary>
    public const double MinElapsedSeconds = 0.001;

    private double _start;
    private double _end;
    private long _totalDamage;
    private long _totalHeal;

    public double StartedAt => _start;
    public double EndedAt => _end;
    public long TotalDamage => _totalDamage;
    public long TotalHeal => _totalHeal;

    /// <summary>True iff the window has opened AND at least one
    /// damage or heal event has landed (Python
    /// <c>_has_meaningful_data_locked</c> conjunction).</summary>
    public bool Active => _start > 0.0 && (_totalDamage > 0 || _totalHeal > 0);

    /// <summary>Mirrors Python: encounter elapsed seconds, floored
    /// at <see cref="MinElapsedSeconds"/>, returning the floor when
    /// the window is closed.</summary>
    public double ElapsedSeconds
    {
        get
        {
            if (_start <= 0.0) return MinElapsedSeconds;
            double raw = _end - _start;
            return raw < MinElapsedSeconds ? MinElapsedSeconds : raw;
        }
    }

    /// <summary>Record a damage event at <paramref name="now"/>.
    /// Opens the window on first event, advances <c>_end</c> on
    /// every event. Mirrors the
    /// <c>if not self._encounter_start: self._encounter_start = now</c>
    /// pattern in `dps_tracker.add_combat_event`.</summary>
    public void OnDamage(long amount, double now)
    {
        if (amount <= 0) return;
        if (_start <= 0.0) _start = now;
        _end = now;
        _totalDamage += amount;
    }

    /// <summary>Record a heal event at <paramref name="now"/>. Same
    /// open-on-first / advance-on-every semantics as
    /// <see cref="OnDamage"/>.</summary>
    public void OnHeal(long amount, double now)
    {
        if (amount <= 0) return;
        if (_start <= 0.0) _start = now;
        _end = now;
        _totalHeal += amount;
    }

    /// <summary>Close the window and zero all counters. Python's
    /// <c>reset</c> does the same — explicit only, never on idle.</summary>
    public void Reset()
    {
        _start = 0.0;
        _end = 0.0;
        _totalDamage = 0;
        _totalHeal = 0;
    }

    /// <summary>Project the live values into the per-snapshot dict
    /// fields the GUI panel reads. Mirrors the Python
    /// <c>_build_snapshot_locked</c> "encounter_*" block.</summary>
    public EncounterSnapshot Snapshot(long totalDamageOverride = -1)
    {
        long dmg = totalDamageOverride < 0 ? _totalDamage : totalDamageOverride;
        double elapsed = ElapsedSeconds;
        return new EncounterSnapshot(
            Active: Active,
            StartedAt: _start,
            EndedAt: _end,
            ElapsedSeconds: Math.Round(elapsed, 1),
            TotalDamage: dmg,
            TotalHeal: _totalHeal,
            // Python uses `int(damage / elapsed)` which is truncation.
            Dps: (long)(dmg / elapsed),
            Hps: (long)(_totalHeal / elapsed));
    }
}

/// <summary>Per-tick encounter projection consumed by the DPS panel.
/// Field names mirror the Python snapshot dict keys
/// (<c>encounter_active</c>, <c>encounter_started_at</c>,
/// <c>encounter_ended_at</c>, <c>elapsed_s</c>, <c>total_damage</c>,
/// <c>total_heal</c>, <c>total_dps</c>, <c>total_hps</c>).</summary>
public sealed record EncounterSnapshot(
    bool Active,
    double StartedAt,
    double EndedAt,
    double ElapsedSeconds,
    long TotalDamage,
    long TotalHeal,
    long Dps,
    long Hps);
