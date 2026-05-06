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

    public BossRaidEngine(Func<DateTimeOffset>? clock = null)
    {
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
    }

    public event Action<RaidPhase>? PhaseEntered;
    public event Action? RaidStarted;
    public event Action? RaidEnded;

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
        }
        if (wasRunning)
        {
            try { RaidEnded?.Invoke(); } catch { /* swallow */ }
        }
    }
}

public sealed record RaidPhase(
    int Index,
    string Name,
    double DurationSeconds,
    string? Reminder = null);
