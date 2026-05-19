using System.Windows;
using Microsoft.Extensions.Logging;
using SaoAuto.App.Hosting;
using SaoAuto.App.Startup;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Logging;
using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.App.Modes;

/// <summary>
/// Drives the WPF UI loop with the same fallback chain as <c>main.run_ui</c>:
/// try each host in <see cref="ModeRouter.BuildChain"/> order, fall back to
/// <see cref="HeadlessRunner"/> if every host throws or reports
/// <see cref="IUiHost.IsAvailable"/> = <c>false</c>.
/// </summary>
public sealed class UiRunner
{
    private readonly SettingsManager _settings;
    private readonly GameStateManager _states;
    private readonly IUiHostFactory _hostFactory;
    private readonly Func<RecognitionBundle> _recognitionBundleFactory;
    private readonly ILogger _log;

    public UiRunner(
        SettingsManager settings,
        GameStateManager states,
        IUiHostFactory? hostFactory = null,
        Func<RecognitionTickHost>? recognitionHostFactory = null,
        ILogger? logger = null)
    {
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _states = states ?? throw new ArgumentNullException(nameof(states));
        _hostFactory = hostFactory ?? new DefaultUiHostFactory();
        _log = logger ?? SaoLog.For("ui");
        _recognitionBundleFactory = recognitionHostFactory is null
            ? (() => RecognitionPipelineBootstrap.BuildBundle(_settings, _states, _log))
            : (() => new RecognitionBundle(recognitionHostFactory(), Locator: null!));
    }

    public int Run(CancellationToken cancellationToken)
    {
        var requested = _settings.NormalizedUiMode;
        var chain = ModeRouter.BuildChain(requested);
        _log.LogInformation("UI mode requested={Requested} chain=[{Chain}]", requested, string.Join(", ", chain));

        GameStateCache.Load(_settings, _states);

        using var packets = PacketLifecycle.Start(
            _settings, _states, _log, cancellationToken);

        // S99 — own the WebView bridge runtime for this UiRunner.Run.
        // The broadcaster has no JS subscriber yet (WebView2 attach
        // pending), but the publisher already streams the typed
        // state.changed payload so a future host attach is one line.
        using var webBridge = new WebBridgeLifecycle(_states, packets.DpsSnapshotProvider);
        webBridge.Start();

        // S156 — share a single AutoKeyProfileService across HUD / future
        // editor by constructing it once and registering its bridge
        // handlers on the WebBridge router.
        using var autoKeyProfile = new AutoKeyProfileLifecycle(_settings, _states);
        webBridge.AttachAutoKeyProfile(autoKeyProfile);
        // S169 — wire the declared-but-unhandled HUD commands. BuffMon
        // toggle + DPS reset/last-report are scoped to the bridge
        // runtime (no per-host state); recognition is attached inside
        // the host loop below, once `recognition` is in scope.
        webBridge.AttachBuffMon(_settings);
        webBridge.AttachDps(packets.ResetDps, packets.DpsSnapshotProvider);
        // S172 — best-effort updater pipeline. Settings-gated by
        // `update_check_enabled` (default false); when off, the bridge
        // surfaces `{error:"unsupported"}` so the HUD button can grey.
        using var updater = UpdaterLifecycle.Start(_settings, _log);
        webBridge.AttachUpdater(updater);

        var application = (System.Windows.Application.Current as App) ?? new App();
        application.DispatcherUnhandledException += (_, e) =>
        {
            _log.LogError(e.Exception, "WPF dispatcher unhandled exception");
        };

        // Honor cancellation by asking the dispatcher to shut down.
        using var ctRegistration = cancellationToken.Register(() =>
        {
            try
            {
                application.Dispatcher.Invoke(() =>
                {
                    application.Shutdown(130);
                });
            }
            catch
            {
                // dispatcher already torn down — fine
            }
        });

        foreach (var modeName in chain)
        {
            if (cancellationToken.IsCancellationRequested) break;
            IUiHost host;
            try
            {
                host = _hostFactory.Create(modeName);
            }
            catch (Exception ex)
            {
                _log.LogError(ex, "host factory failed for {Mode}", modeName);
                continue;
            }

            if (!host.IsAvailable)
            {
                _log.LogWarning("host {Mode} reports not available; trying next", modeName);
                continue;
            }

            Window window;
            try
            {
                window = host.CreateMainWindow();
            }
            catch (Exception ex)
            {
                _log.LogError(ex, "host {Mode} failed to build window; falling through", modeName);
                continue;
            }

            try
            {
                _log.LogInformation("starting {Mode} host window", modeName);
                // S97 — best-effort recognition lifecycle scoped to this
                // host's Application.Run. If the pipeline can't start
                // (no game window, GDI init failure) the UI still runs.
                WindowLocator? sharedLocator = null;
                using var recognition = RecognitionLifecycle.Start(
                    () =>
                    {
                        var bundle = _recognitionBundleFactory();
                        sharedLocator = bundle.Locator;
                        return bundle.Host;
                    },
                    _log, cancellationToken);
                using var autoKey = AutoKeyLifecycle.Start(
                    () => new AutoKeyTickHost(
                        _settings,
                        _states,
                        new AutoKeySpecRuntime(new SendInputKeyDispatcher()),
                        foregroundProbe: sharedLocator is null ? null : GameForegroundProbe.Bind(sharedLocator)),
                    _log,
                    cancellationToken);
                // S153 — HideSeek tick host. Factory throws when the
                // shared locator is unavailable or assets are missing;
                // HideSeekLifecycle catches + logs so startup proceeds.
                using var hideSeek = HideSeekLifecycle.Start(
                    () => HideSeekBootstrap.Build(
                        sharedLocator ?? throw new InvalidOperationException("HideSeek: no shared locator"),
                        ResourcePathResolver.ForCurrentProcess(),
                        log: _log),
                    _log,
                    cancellationToken);
                // S155 — publish per-tick snapshots through the web bridge.
                webBridge.AttachHideSeek(hideSeek);
                // S170 — recognition status + suspend/resume hooks.
                // RecognitionLifecycle.Suspend just sets the tick host's
                // pause flag (S160-style); IsActive flips with it so the
                // HUD toggle reflects reality.
                webBridge.AttachRecognition(
                    () => recognition.IsActive,
                    start: recognition.Resume,
                    stop: recognition.Suspend);
                return application.Run(window);
            }
            catch (Exception ex)
            {
                _log.LogError(ex, "host {Mode} crashed during Application.Run", modeName);
                // Application.Run is one-shot per Application instance; stop trying further hosts here
                // and fall to headless rather than corrupting the loop state.
                break;
            }
        }

        _log.LogWarning("all UI hosts failed; falling back to headless");
        return new HeadlessRunner(_settings, _states, SaoLog.For("headless"))
            .RunAsync(cancellationToken)
            .GetAwaiter()
            .GetResult();
    }
}
