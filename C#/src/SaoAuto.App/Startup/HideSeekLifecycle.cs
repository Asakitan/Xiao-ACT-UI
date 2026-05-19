using Microsoft.Extensions.Logging;
using SaoAuto.Core.Automation.HideSeek;
using SaoAuto.Core.Logging;

namespace SaoAuto.App.Startup;

/// <summary>
/// S153 — Best-effort start/stop wrapper around a
/// <see cref="HideSeekTickHost"/>. Mirrors
/// <see cref="AutoKeyLifecycle"/>: factory exceptions are logged and
/// swallowed, <see cref="IsActive"/> reflects whether the host was
/// constructed and started. Dispose is idempotent.
/// </summary>
public sealed class HideSeekLifecycle : IDisposable
{
    private readonly HideSeekTickHost? _host;
    private readonly ILogger _log;
    private bool _disposed;

    private HideSeekLifecycle(HideSeekTickHost? host, ILogger log)
    {
        _host = host;
        _log = log;
    }

    public bool IsActive => _host is not null;
    public long TickCount => _host?.TickCount ?? 0;
    public int LastFiredStep => _host?.LastFiredStep ?? -1;
    public string LastError => _host?.LastError ?? "inactive";

    /// <summary>S154 — atomic snapshot pass-through. Returns
    /// <see cref="HideSeekRuntimeSnapshot.Empty"/> when inactive so
    /// callers can poll without a null check.</summary>
    public HideSeekRuntimeSnapshot SnapshotRuntime()
        => _host?.SnapshotRuntime() ?? HideSeekRuntimeSnapshot.Empty;

    /// <summary>S160 — true when the host exists and is paused.
    /// Inactive lifecycles report <c>false</c>.</summary>
    public bool IsSuspended => _host?.Suspended ?? false;

    /// <summary>S160 — pause tick processing. No-op when inactive.</summary>
    public void Suspend() { if (_host is { } h) h.Suspended = true; }

    /// <summary>S160 — resume tick processing. No-op when inactive.</summary>
    public void Resume() { if (_host is { } h) h.Suspended = false; }

    /// <summary>S160 — flip the suspended flag and return the new value.
    /// Returns <c>false</c> when inactive (nothing to toggle).</summary>
    public bool ToggleSuspend()
    {
        if (_host is null) return false;
        _host.Suspended = !_host.Suspended;
        return _host.Suspended;
    }

    /// <summary>
    /// S155 — subscribe to per-tick snapshots. Returns a disposable
    /// unsubscribe handle. No-op subscription when the lifecycle is
    /// inactive (handler is never invoked).
    /// </summary>
    public IDisposable Subscribe(Action<HideSeekRuntimeSnapshot> handler)
    {
        if (handler is null) throw new ArgumentNullException(nameof(handler));
        if (_host is null) return new NoopUnsubscribe();
        _host.Ticked += handler;
        var host = _host;
        return new DelegateUnsubscribe(() => host.Ticked -= handler);
    }

    private sealed class NoopUnsubscribe : IDisposable
    {
        public void Dispose() { }
    }

    private sealed class DelegateUnsubscribe : IDisposable
    {
        private Action? _drop;
        public DelegateUnsubscribe(Action drop) => _drop = drop;
        public void Dispose()
        {
            var d = Interlocked.Exchange(ref _drop, null);
            d?.Invoke();
        }
    }

    public static HideSeekLifecycle Start(
        Func<HideSeekTickHost> hostFactory,
        ILogger? logger,
        CancellationToken cancellationToken)
    {
        if (hostFactory is null) throw new ArgumentNullException(nameof(hostFactory));
        var log = logger ?? SaoLog.For("hide-seek");

        HideSeekTickHost? host = null;
        try
        {
            host = hostFactory();
            host.StartAsync(cancellationToken).GetAwaiter().GetResult();
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "hide-seek tick host failed to start; continuing without it");
            if (host is not null)
            {
                try { host.Dispose(); } catch { /* swallow */ }
                host = null;
            }
        }
        return new HideSeekLifecycle(host, log);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_host is null) return;
        try { _host.StopAsync().GetAwaiter().GetResult(); } catch { /* swallow on shutdown */ }
        _host.Dispose();
    }
}
