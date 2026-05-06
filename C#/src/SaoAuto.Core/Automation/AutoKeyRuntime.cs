using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.Core.State;

namespace SaoAuto.Core.Automation;

/// <summary>
/// Side-effecting layer that turns a <see cref="KeyStroke"/> into
/// real input. Production binds <c>Win32SendInput</c>, tests bind a
/// recording stub.
/// </summary>
public interface IKeyDispatcher
{
    void Dispatch(KeyStroke stroke);
}

/// <summary>
/// Runtime executor for <see cref="AutoKeyProfile"/>s. C# port of
/// the loop in <c>auto_key_engine.py</c> (lines 639–828): every
/// tick it evaluates the active profile's actions in priority order
/// and fires the first one whose trigger + cooldown pass.
///
/// Thin by design: trigger eval is in <see cref="AutoKeyTriggers"/>,
/// cooldown is in <see cref="AutoKeyCooldownGate"/>, key dispatch
/// is delegated to <see cref="IKeyDispatcher"/>. Tests inject a
/// recording dispatcher and call <see cref="Tick"/> directly without
/// running the timer thread.
/// </summary>
public sealed class AutoKeyRuntime : IDisposable
{
    public static readonly TimeSpan DefaultTickInterval = TimeSpan.FromMilliseconds(50);

    private readonly GameStateManager _state;
    private readonly IKeyDispatcher _dispatcher;
    private readonly AutoKeyCooldownGate _gate;
    private readonly Func<DateTimeOffset> _clock;
    private readonly ILogger _log;
    private readonly object _lock = new();
    private AutoKeyProfile? _profile;
    private Func<bool> _enabledGate = () => true;
    private long _ticks;
    private long _fires;
    private CancellationTokenSource? _cts;
    private Task? _pump;
    private bool _disposed;

    public AutoKeyRuntime(
        GameStateManager state,
        IKeyDispatcher dispatcher,
        AutoKeyCooldownGate? gate = null,
        Func<DateTimeOffset>? clock = null,
        ILogger<AutoKeyRuntime>? logger = null)
    {
        _state = state ?? throw new ArgumentNullException(nameof(state));
        _dispatcher = dispatcher ?? throw new ArgumentNullException(nameof(dispatcher));
        _gate = gate ?? new AutoKeyCooldownGate();
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public AutoKeyProfile? Profile { get { lock (_lock) return _profile; } }
    public long TickCount => Interlocked.Read(ref _ticks);
    public long FireCount => Interlocked.Read(ref _fires);

    public void SetProfile(AutoKeyProfile? profile) { lock (_lock) _profile = profile; }

    /// <summary>
    /// Caller-supplied gate (e.g. "game window has focus", "user has not
    /// pressed pause"). Defaults to always-true; matching Python's
    /// <c>_recognition_active</c> + foreground checks.
    /// </summary>
    public void SetEnabledGate(Func<bool> gate)
    {
        ArgumentNullException.ThrowIfNull(gate);
        lock (_lock) _enabledGate = gate;
    }

    /// <summary>
    /// Run one tick. Returns the action id that fired, or null if no
    /// action matched (or the runtime is gated off). Public so tests can
    /// drive it deterministically without spinning a timer.
    /// </summary>
    public string? Tick()
    {
        Interlocked.Increment(ref _ticks);
        AutoKeyProfile? profile;
        Func<bool> gate;
        lock (_lock) { profile = _profile; gate = _enabledGate; }
        if (profile is null || !profile.Enabled) return null;
        if (!gate()) return null;

        var snap = _state.Snapshot;
        var ctx = new AutoKeyContext(
            HpPct: snap.HpPct,
            StaminaPct: snap.StaminaPct,
            BurstReady: snap.BurstReady,
            BossPhase: 0,
            InCombat: snap.InCombat,
            Now: _clock());

        // First-match-wins, mirroring Python's early-return loop.
        foreach (var action in profile.Actions.OrderByDescending(a => a.Priority))
        {
            if (!AutoKeyTriggers.ShouldFire(action.Trigger, ctx)) continue;
            if (!_gate.TryFire(action.Id, action.CooldownMs, ctx.Now)) continue;
            try { _dispatcher.Dispatch(action.KeyStroke); }
            catch (Exception ex) { _log.LogWarning(ex, "[AutoKey] dispatch failed for {Id}", action.Id); continue; }
            Interlocked.Increment(ref _fires);
            return action.Id;
        }
        return null;
    }

    public void Start(TimeSpan? tickInterval = null)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(AutoKeyRuntime));
        if (_pump is { IsCompleted: false }) return;
        var interval = tickInterval ?? DefaultTickInterval;
        _cts = new CancellationTokenSource();
        _pump = Task.Run(() => PumpAsync(interval, _cts.Token));
    }

    public void Stop()
    {
        _cts?.Cancel();
        try { _pump?.Wait(TimeSpan.FromSeconds(1)); } catch { /* swallow */ }
        _cts?.Dispose();
        _cts = null;
        _pump = null;
    }

    private async Task PumpAsync(TimeSpan interval, CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try { Tick(); }
            catch (Exception ex) { _log.LogWarning(ex, "[AutoKey] tick failed"); }
            try { await Task.Delay(interval, ct).ConfigureAwait(false); }
            catch (OperationCanceledException) { return; }
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Stop();
    }
}
