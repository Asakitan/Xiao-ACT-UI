using Microsoft.Extensions.Logging;
using SaoAuto.Core.Logging;
using SaoAuto.Core.Vision;

namespace SaoAuto.App.Startup;

/// <summary>
/// S97 — Best-effort start/stop wrapper around a
/// <see cref="RecognitionTickHost"/>.
///
/// Both <c>HeadlessRunner</c> and <c>UiRunner</c> need the same
/// "build it, start it, swallow startup failures, stop+dispose
/// later" choreography. Centralising it here keeps the runners
/// straight-line and gives us a single place to test the
/// lifecycle semantics.
///
/// Construction never throws — a factory exception is logged as a
/// warning and the lifecycle stays inactive (<see cref="IsActive"/>
/// = <c>false</c>). <see cref="Dispose"/> is idempotent and
/// swallows shutdown noise so it never changes a runner's exit
/// code.
/// </summary>
public sealed class RecognitionLifecycle : IDisposable
{
    private readonly RecognitionTickHost? _host;
    private readonly ILogger _log;
    private bool _disposed;

    private RecognitionLifecycle(RecognitionTickHost? host, ILogger log)
    {
        _host = host;
        _log = log;
    }

    public bool IsActive => _host is not null;

    public static RecognitionLifecycle Start(
        Func<RecognitionTickHost> hostFactory,
        ILogger? logger,
        CancellationToken cancellationToken)
    {
        if (hostFactory is null) throw new ArgumentNullException(nameof(hostFactory));
        var log = logger ?? SaoLog.For("recognition");

        RecognitionTickHost? host = null;
        try
        {
            host = hostFactory();
            host.StartAsync(cancellationToken).GetAwaiter().GetResult();
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "recognition pipeline failed to start; continuing without it");
            if (host is not null)
            {
                try { host.Dispose(); } catch { /* swallow */ }
                host = null;
            }
        }
        return new RecognitionLifecycle(host, log);
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
