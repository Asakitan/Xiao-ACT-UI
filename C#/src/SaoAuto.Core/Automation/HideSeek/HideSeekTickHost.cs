using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Automation.HideSeek;

/// <summary>
/// S154 — immutable atomic view of one <see cref="HideSeekTickHost"/>
/// state. Safe to hand to background consumers (HUD, web bridge,
/// telemetry) without a snapshot-copy on the receiver side. Mirrors
/// <see cref="AutoKeyRuntimeSnapshot"/> in shape so the HUD layer can
/// treat both subsystems uniformly.
/// </summary>
public readonly record struct HideSeekRuntimeSnapshot(
    bool IsRunning,
    long TickCount,
    int LastFiredStep,
    string LastError)
{
    public static HideSeekRuntimeSnapshot Empty => new(false, 0L, -1, "inactive");
}

/// <summary>
/// S151 — Background tick pump for <see cref="HideSeekStateMachine"/>.
/// Mirrors Python <c>HideSeekEngine._run</c>: a dedicated worker that
/// sleeps <see cref="IntervalMs"/> between ticks, swallowing per-tick
/// exceptions so a single bad capture / decoder call never kills the
/// pump. Pure Core type — capture, input and detector live behind
/// interfaces the state machine already owns.
/// </summary>
public sealed class HideSeekTickHost : IAsyncDisposable, IDisposable
{
    public const int DefaultIntervalMs = 1000;

    private readonly HideSeekStateMachine _machine;
    private readonly ILogger _log;
    private readonly CancellationTokenSource _shutdown = new();
    private CancellationTokenSource? _runCts;
    private Task? _pump;
    private bool _disposed;

    public HideSeekTickHost(
        HideSeekStateMachine machine,
        int intervalMs = DefaultIntervalMs,
        Func<DateTimeOffset>? clock = null,
        ILogger<HideSeekTickHost>? logger = null)
    {
        _machine = machine ?? throw new ArgumentNullException(nameof(machine));
        if (intervalMs < 1) throw new ArgumentOutOfRangeException(nameof(intervalMs), "must be ≥ 1ms");
        IntervalMs = intervalMs;
        Clock = clock ?? (() => DateTimeOffset.UtcNow);
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public int IntervalMs { get; }
    public Func<DateTimeOffset> Clock { get; }
    public long TickCount { get; private set; }
    public int LastFiredStep { get; private set; } = -1;
    public string LastError { get; private set; } = "";
    public bool IsRunning => _pump is { IsCompleted: false };

    /// <summary>
    /// S160 — pause flag. When <c>true</c>, <see cref="TickOnce"/>
    /// short-circuits before touching the state machine: no
    /// <see cref="TickCount"/> bump, no <see cref="Ticked"/> emission.
    /// Used by the toggle-hotkey path so a user can park HideSeek
    /// without tearing the pump down (and so the HUD pill stops
    /// flickering at the tick rate).
    /// </summary>
    public bool Suspended { get; set; }

    /// <summary>
    /// S155 — fires once per <see cref="TickOnce"/> call with the
    /// post-tick snapshot. Subscribers must be cheap and non-throwing
    /// (handler exceptions are swallowed so a bad consumer cannot kill
    /// the pump).
    /// </summary>
    public event Action<HideSeekRuntimeSnapshot>? Ticked;

    /// <summary>S154 — atomic snapshot for HUD / web bridge consumers.</summary>
    public HideSeekRuntimeSnapshot SnapshotRuntime()
        => new(IsRunning, TickCount, LastFiredStep, LastError);

    /// <summary>
    /// Run one tick synchronously. Returns the fired step index or -1.
    /// Catches state-machine exceptions and surfaces them via
    /// <see cref="LastError"/> so the caller can show a UI banner
    /// without crashing the pump.
    /// </summary>
    public int TickOnce()
    {
        if (_disposed) return -1;
        if (Suspended) return -1;
        TickCount++;
        try
        {
            var fired = _machine.Tick(Clock());
            LastFiredStep = fired;
            LastError = "";
            RaiseTicked();
            return fired;
        }
        catch (Exception ex)
        {
            LastError = $"{ex.GetType().Name}: {ex.Message}";
            _log.LogWarning(ex, "[HideSeekTickHost] tick threw");
            RaiseTicked();
            return -1;
        }
    }

    private void RaiseTicked()
    {
        var handler = Ticked;
        if (handler is null) return;
        try { handler(SnapshotRuntime()); }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[HideSeekTickHost] Ticked handler threw");
        }
    }

    public Task StartAsync(CancellationToken cancellationToken)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(HideSeekTickHost));
        if (_pump is not null) return Task.CompletedTask;
        _runCts = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token, cancellationToken);
        _pump = Task.Run(() => PumpAsync(_runCts.Token), CancellationToken.None);
        return Task.CompletedTask;
    }

    public async Task StopAsync()
    {
        _shutdown.Cancel();
        _runCts?.Cancel();
        var t = _pump;
        if (t is null) return;
        try { await t.ConfigureAwait(false); }
        catch (OperationCanceledException) { /* normal */ }
    }

    public void Dispose() => DisposeAsync().AsTask().GetAwaiter().GetResult();

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true;
        _shutdown.Cancel();
        _runCts?.Cancel();
        if (_pump is not null)
        {
            try { await _pump.ConfigureAwait(false); }
            catch (OperationCanceledException) { /* normal */ }
        }
        _runCts?.Dispose();
        _shutdown.Dispose();
    }

    private async Task PumpAsync(CancellationToken token)
    {
        try
        {
            while (!token.IsCancellationRequested)
            {
                await Task.Delay(IntervalMs, token).ConfigureAwait(false);
                if (token.IsCancellationRequested) break;
                TickOnce();
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested)
        {
            // expected on shutdown
        }
    }
}
