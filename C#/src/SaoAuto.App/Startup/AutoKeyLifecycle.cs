using Microsoft.Extensions.Logging;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Logging;

namespace SaoAuto.App.Startup;

/// <summary>
/// S137 — Best-effort start/stop wrapper around an
/// <see cref="AutoKeyTickHost"/>.
///
/// Mirrors <see cref="RecognitionLifecycle"/>: factory exceptions
/// are logged and swallowed; <see cref="IsActive"/> reflects whether
/// the host was successfully constructed and started. Dispose is
/// idempotent and never throws into the runner.
/// </summary>
public sealed class AutoKeyLifecycle : IDisposable
{
    private readonly AutoKeyTickHost? _host;
    private readonly ILogger _log;
    private bool _disposed;

    private AutoKeyLifecycle(AutoKeyTickHost? host, ILogger log)
    {
        _host = host;
        _log = log;
    }

    public bool IsActive => _host is not null;

    /// <summary>S138 — pass-through status surface for HUD / status bar.</summary>
    public string LastReason => _host?.LastReason ?? "inactive";
    public string LastActiveProfileId => _host?.LastActiveProfileId ?? "";
    public string? LastFiredActionId => _host?.LastFiredActionId;
    public long TickCount => _host?.TickCount ?? 0;

    /// <summary>
    /// S143 — atomic immutable runtime snapshot for background
    /// consumers (telemetry, web bridge). Returns
    /// <see cref="AutoKeyRuntimeSnapshot.Empty"/> when the lifecycle
    /// is inactive, so callers can poll without a null check.
    /// </summary>
    public AutoKeyRuntimeSnapshot SnapshotRuntime()
        => _host?.SnapshotRuntime() ?? AutoKeyRuntimeSnapshot.Empty;

    public static AutoKeyLifecycle Start(
        Func<AutoKeyTickHost> hostFactory,
        ILogger? logger,
        CancellationToken cancellationToken)
    {
        if (hostFactory is null) throw new ArgumentNullException(nameof(hostFactory));
        var log = logger ?? SaoLog.For("auto-key");

        AutoKeyTickHost? host = null;
        try
        {
            host = hostFactory();
            host.StartAsync(cancellationToken).GetAwaiter().GetResult();
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "auto-key tick host failed to start; continuing without it");
            if (host is not null)
            {
                try { host.Dispose(); } catch { /* swallow */ }
                host = null;
            }
        }
        return new AutoKeyLifecycle(host, log);
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
