using Microsoft.Extensions.Logging;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Logging;
using SaoAuto.Core.Updater;

namespace SaoAuto.App.Startup;

/// <summary>
/// S172 — Best-effort start/stop wrapper around the update client
/// trio (<see cref="HttpUpdateClient"/> + <see cref="UpdateClientLoop"/>
/// + <see cref="UpdaterStateMachine"/>). Mirrors
/// <see cref="PacketLifecycle"/> / <see cref="RecognitionLifecycle"/>:
/// startup failures are logged and swallowed so the UI keeps running
/// even when the update host is unreachable or settings disable the
/// background poll.
///
/// Exposes the three async delegates that <see cref="WebBridge.UpdaterBridge"/>
/// needs (check / download / apply) plus the <see cref="StateMachine"/>
/// itself for event broadcasting. Inactive instances surface
/// <c>null</c> delegates so the bridge replies with
/// <c>{error:"unsupported"}</c>.
/// </summary>
public sealed class UpdaterLifecycle : IDisposable
{
    private readonly UpdateClientLoop? _loop;
    private readonly ILogger _log;
    private readonly SettingsManager? _settings;
    private bool _disposed;

    public UpdaterStateMachine StateMachine { get; }

    private UpdaterLifecycle(UpdateClientLoop? loop, UpdaterStateMachine machine, ILogger log, SettingsManager? settings = null)
    {
        _loop = loop;
        StateMachine = machine;
        _log = log;
        _settings = settings;
    }

    public bool IsActive => _loop is not null;

    /// <summary>S173 — true when the background poll pump is running.
    /// Off unless <c>update_auto_poll</c> is set; HUD-driven manual
    /// triggers still work either way.</summary>
    public bool IsBackgroundPollRunning => _loop is not null && _loop.IsRunning;

    public Func<CancellationToken, Task>? Check =>
        _loop is null ? null : _loop.TickAsync;

    public Func<CancellationToken, Task>? Download =>
        _loop is null ? null : async ct =>
        {
            var latest = StateMachine.Snapshot.Latest;
            if (latest is null) return;
            await _loop.DownloadAsync(latest, ct).ConfigureAwait(false);
        };

    public Func<CancellationToken, Task>? Apply =>
        _loop is null ? null : async ct =>
        {
            var latest = StateMachine.Snapshot.Latest;
            if (latest is null) return;
            await _loop.ApplyAsync(latest, ct).ConfigureAwait(false);
        };

    public Func<CancellationToken, Task<bool>>? Skip =>
        _loop is null || _settings is null ? null : ct =>
        {
            var latest = StateMachine.Snapshot.Latest;
            if (latest is null || string.IsNullOrWhiteSpace(latest.Version))
                return Task.FromResult(false);
            _settings.Set(SettingsKeys.UpdateSkippedVersion, latest.Version);
            _settings.Save();
            StateMachine.CheckCompleted(null, AppVersion.Version);
            return Task.FromResult(true);
        };

    public static UpdaterLifecycle Start(
        SettingsManager settings,
        ILogger? logger = null,
        Func<HttpUpdateClient>? clientFactory = null,
        Func<UpdateClientLoopOptions, HttpUpdateClient, UpdaterStateMachine, UpdateClientLoop>? loopFactory = null,
        Func<UpdateClientLoopOptions>? optionsFactory = null)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        var log = logger ?? SaoLog.For("updater");
        var machine = new UpdaterStateMachine();

        if (!settings.GetBool(SettingsKeys.UpdateCheckEnabled, defaultValue: false))
        {
            log.LogInformation("updater disabled by settings");
            return new UpdaterLifecycle(null, machine, log, settings);
        }

        try
        {
            var options = optionsFactory?.Invoke() ?? BuildDefaultOptions(settings);
            var client = clientFactory?.Invoke() ?? new HttpUpdateClient();
            var loop = loopFactory is null
                ? new UpdateClientLoop(options, client, machine)
                : loopFactory(options, client, machine);
            if (settings.GetBool(SettingsKeys.UpdateAutoPoll, defaultValue: false))
            {
                try { loop.Start(); }
                catch (Exception ex)
                {
                    log.LogWarning(ex, "updater background poll failed to start; manual triggers still work");
                }
            }
            return new UpdaterLifecycle(loop, machine, log, settings);
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "updater pipeline failed to construct; continuing without it");
            return new UpdaterLifecycle(null, machine, log, settings);
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        try { _loop?.Stop(); } catch { /* swallow */ }
        try { _loop?.Dispose(); } catch { /* swallow */ }
    }

    private static UpdateClientLoopOptions BuildDefaultOptions(SettingsManager settings)
    {
        var host = settings.GetString(SettingsKeys.UpdateHost, AppVersion.DefaultUpdateHost) ?? AppVersion.DefaultUpdateHost;
        var staging = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "saoauto-update-staging");
        return new UpdateClientLoopOptions(
            BaseUrl: host,
            Channel: AppVersion.UpdateChannel,
            Target: AppVersion.UpdateTarget,
            CurrentVersion: AppVersion.Version,
            StagingDirectory: staging,
            PollInterval: TimeSpan.FromHours(1))
        {
            SkippedVersionProvider = () => settings.GetString(SettingsKeys.UpdateSkippedVersion, null),
        };
    }
}
