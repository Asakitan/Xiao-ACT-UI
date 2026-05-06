using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Automation;

/// <summary>
/// Wires <see cref="BossRaidEngine"/> phase transitions to keystroke
/// dispatch through <see cref="IKeyDispatcher"/>. C# port of
/// <c>boss_autokey_linkage.py</c>.
///
/// The Python original keys off alert-message strings (substring
/// match into "→ P2"); the C# port pivots on the typed
/// <see cref="RaidPhase"/> the engine already exposes — same
/// behavior, no string parsing needed.
///
/// Each rule pairs a phase predicate with a <see cref="KeyStroke"/>
/// and a per-rule cooldown. Cooldowns are enforced by the shared
/// <see cref="AutoKeyCooldownGate"/> so rules participate in the
/// same global rearm policy as the auto-key runtime.
/// </summary>
public sealed class BossAutoKeyLinkage : IDisposable
{
    public sealed record Rule(
        string Id,
        Func<RaidPhase, bool> Match,
        KeyStroke KeyStroke,
        int CooldownMs);

    private readonly BossRaidEngine _engine;
    private readonly IKeyDispatcher _dispatcher;
    private readonly AutoKeyCooldownGate _gate;
    private readonly Func<DateTimeOffset> _clock;
    private readonly ILogger _log;
    private readonly object _lock = new();
    private List<Rule> _rules = new();
    private long _fires;
    private bool _disposed;

    public BossAutoKeyLinkage(
        BossRaidEngine engine,
        IKeyDispatcher dispatcher,
        AutoKeyCooldownGate? gate = null,
        Func<DateTimeOffset>? clock = null,
        ILogger<BossAutoKeyLinkage>? logger = null)
    {
        _engine = engine ?? throw new ArgumentNullException(nameof(engine));
        _dispatcher = dispatcher ?? throw new ArgumentNullException(nameof(dispatcher));
        _gate = gate ?? new AutoKeyCooldownGate();
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _log = (ILogger?)logger ?? NullLogger.Instance;
        _engine.PhaseEntered += OnPhaseEntered;
    }

    public long FireCount => Interlocked.Read(ref _fires);

    public void SetRules(IEnumerable<Rule> rules)
    {
        ArgumentNullException.ThrowIfNull(rules);
        var list = rules.ToList();
        lock (_lock) _rules = list;
    }

    /// <summary>
    /// Apply rules against an explicit phase. Public so tests don't have
    /// to drive the whole engine; production calls it through the
    /// `PhaseEntered` event.
    /// </summary>
    public string? OnPhase(RaidPhase phase)
    {
        ArgumentNullException.ThrowIfNull(phase);
        List<Rule> snapshot;
        lock (_lock) snapshot = _rules.ToList();
        var now = _clock();
        foreach (var rule in snapshot)
        {
            bool matched;
            try { matched = rule.Match(phase); }
            catch (Exception ex) { _log.LogWarning(ex, "[BossLinkage] match threw for {Id}", rule.Id); continue; }
            if (!matched) continue;
            if (!_gate.TryFire(rule.Id, rule.CooldownMs, now)) continue;
            try { _dispatcher.Dispatch(rule.KeyStroke); }
            catch (Exception ex) { _log.LogWarning(ex, "[BossLinkage] dispatch failed for {Id}", rule.Id); continue; }
            Interlocked.Increment(ref _fires);
            return rule.Id;
        }
        return null;
    }

    private void OnPhaseEntered(RaidPhase phase) => OnPhase(phase);

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _engine.PhaseEntered -= OnPhaseEntered;
    }
}
