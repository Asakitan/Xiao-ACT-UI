using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Updater;

public sealed record UpdateClientLoopOptions(
    string BaseUrl,
    string Channel,
    string Target,
    string CurrentVersion,
    string StagingDirectory,
    TimeSpan PollInterval)
{
    public TimeSpan InitialDelay { get; init; } = TimeSpan.Zero;
    public bool AutoDownload { get; init; } = true;
    public bool AutoApply { get; init; } = false;
}

/// <summary>
/// Host-side orchestrator that drives <see cref="UpdaterStateMachine"/>
/// off a polling timer. C# port of the loop body inside
/// <c>sao_updater.UpdateManager</c> — owns the cadence, the manifest
/// cache (no re-stage when the staged version still matches latest),
/// and the deferred-apply gate.
///
/// Thin by design: HTTP work lives in <see cref="HttpUpdateClient"/>,
/// state lives in <see cref="UpdaterStateMachine"/>, file work lives in
/// <see cref="ApplyEngine"/>. The loop is what wires them together.
/// </summary>
public sealed class UpdateClientLoop : IDisposable
{
    private readonly UpdateClientLoopOptions _opts;
    private readonly HttpUpdateClient _client;
    private readonly UpdaterStateMachine _machine;
    private readonly Func<bool> _quiescent;
    private readonly Func<UpdateManifest, string, CancellationToken, Task<bool>> _apply;
    private readonly ILogger _log;
    private readonly object _gate = new();
    private CancellationTokenSource? _cts;
    private Task? _pump;
    private string? _stagedVersion;
    private string? _stagedPath;
    private bool _disposed;

    public UpdateClientLoop(
        UpdateClientLoopOptions options,
        HttpUpdateClient client,
        UpdaterStateMachine machine,
        Func<bool>? quiescent = null,
        Func<UpdateManifest, string, CancellationToken, Task<bool>>? apply = null,
        ILogger<UpdateClientLoop>? logger = null)
    {
        _opts = options ?? throw new ArgumentNullException(nameof(options));
        _client = client ?? throw new ArgumentNullException(nameof(client));
        _machine = machine ?? throw new ArgumentNullException(nameof(machine));
        _quiescent = quiescent ?? (() => true);
        _apply = apply ?? ((_, _, _) => Task.FromResult(false));
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public string? StagedVersion { get { lock (_gate) return _stagedVersion; } }
    public string? StagedPath { get { lock (_gate) return _stagedPath; } }
    public bool IsRunning => _pump is { IsCompleted: false };

    public void Start()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(UpdateClientLoop));
        if (IsRunning) return;
        _cts = new CancellationTokenSource();
        _pump = Task.Run(() => PumpAsync(_cts.Token));
    }

    public void Stop()
    {
        _cts?.Cancel();
        try { _pump?.Wait(TimeSpan.FromSeconds(2)); } catch { /* swallow */ }
        _cts?.Dispose();
        _cts = null;
        _pump = null;
    }

    private async Task PumpAsync(CancellationToken ct)
    {
        if (_opts.InitialDelay > TimeSpan.Zero)
        {
            try { await Task.Delay(_opts.InitialDelay, ct).ConfigureAwait(false); }
            catch (OperationCanceledException) { return; }
        }
        while (!ct.IsCancellationRequested)
        {
            try { await TickAsync(ct).ConfigureAwait(false); }
            catch (OperationCanceledException) { return; }
            catch (Exception ex) { _log.LogWarning(ex, "[UpdateClientLoop] tick failed"); }
            try { await Task.Delay(_opts.PollInterval, ct).ConfigureAwait(false); }
            catch (OperationCanceledException) { return; }
        }
    }

    /// <summary>
    /// Single iteration: check manifest → maybe download → maybe apply.
    /// Public so deterministic tests can drive the loop without a timer.
    /// </summary>
    public async Task TickAsync(CancellationToken ct = default)
    {
        _machine.BeginCheck();
        var latest = await _client.CheckLatestAsync(_opts.BaseUrl, _opts.Channel, _opts.Target, ct)
            .ConfigureAwait(false);
        _machine.CheckCompleted(latest, _opts.CurrentVersion);

        if (latest is null) return;
        if (_machine.Snapshot.Status != UpdaterStatus.UpdateAvailable
            && _machine.Snapshot.Status != UpdaterStatus.StagedReady)
        {
            return;
        }

        if (ShouldSkipDownload(latest))
        {
            // Already staged the same version — restore StagedReady so AutoApply can proceed.
            if (_machine.Snapshot.Status != UpdaterStatus.StagedReady)
            {
                _machine.BeginDownload();
                _machine.DownloadCompleted();
            }
        }
        else if (_opts.AutoDownload)
        {
            await DownloadAsync(latest, ct).ConfigureAwait(false);
        }

        if (_opts.AutoApply
            && _machine.Snapshot.Status == UpdaterStatus.StagedReady
            && _quiescent())
        {
            await ApplyAsync(latest, ct).ConfigureAwait(false);
        }
    }

    private bool ShouldSkipDownload(UpdateManifest latest)
    {
        lock (_gate)
        {
            return _stagedVersion is not null
                && string.Equals(_stagedVersion, latest.Version, StringComparison.OrdinalIgnoreCase)
                && _stagedPath is not null
                && File.Exists(_stagedPath);
        }
    }

    public async Task<bool> DownloadAsync(UpdateManifest manifest, CancellationToken ct = default)
    {
        Directory.CreateDirectory(_opts.StagingDirectory);
        var dest = Path.Combine(_opts.StagingDirectory, $"{manifest.Version}.zip");
        _machine.BeginDownload();
        var progress = new Progress<double>(p => _machine.ReportProgress(p));
        var ok = await _client.DownloadAsync(manifest, dest, progress, ct).ConfigureAwait(false);
        if (!ok)
        {
            _machine.DownloadFailed("download or hash check failed");
            return false;
        }
        lock (_gate)
        {
            _stagedVersion = manifest.Version;
            _stagedPath = dest;
        }
        _machine.DownloadCompleted();
        return true;
    }

    public async Task<bool> ApplyAsync(UpdateManifest manifest, CancellationToken ct = default)
    {
        string? staged;
        lock (_gate) staged = _stagedPath;
        if (staged is null) return false;
        _machine.BeginApply();
        try
        {
            var ok = await _apply(manifest, staged, ct).ConfigureAwait(false);
            if (!ok) _machine.ApplyFailed("apply returned false");
            return ok;
        }
        catch (Exception ex)
        {
            _machine.ApplyFailed(ex.Message);
            return false;
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Stop();
    }
}
