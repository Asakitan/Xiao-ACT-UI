using System.Windows;
using Microsoft.Extensions.Logging;
using SaoAuto.App.Hosting;
using SaoAuto.App.Hotkeys;
using SaoAuto.App.Menu;
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
        // R8: also thread the live DpsTracker + dps_enabled provider into
        // the publisher so the per-tick DPS pump (DPS-02/03/07) can drive
        // show-live / fade-out edges. The packets host owns the tracker;
        // the setting is read from SettingsManager (default true).
        using var webBridge = new WebBridgeLifecycle(
            _states,
            packets.DpsSnapshotProvider,
            packets.DpsTracker,
            () => _settings.Get<bool?>(SettingsKeys.DpsEnabled) ?? true);
        webBridge.Start();
        // R8: scene-change subscriber → state.scene_changed bridge event.
        // Idempotent unsubscribe via using-scope is implicit since the
        // PacketLifecycle outlives this UiRunner.Run scope.
        if (packets.PacketBridge is { } pb)
        {
            pb.SceneChanged += sc =>
            {
                webBridge.Broadcaster.Emit(BridgeEvents.SceneChanged, new System.Text.Json.Nodes.JsonObject
                {
                    ["kind"] = sc.Kind.ToString(),
                    ["reason"] = sc.Reason,
                    ["preserve_combat"] = sc.PreserveCombat,
                    ["reset_on_next_damage"] = sc.ResetOnNextDamage,
                    ["reset_delay_s"] = sc.ResetDelaySeconds,
                    ["timestamp"] = sc.TimestampSeconds,
                });
            };
        }

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
        webBridge.AttachDps(packets.ResetDps, packets.DpsSnapshotProvider, _settings);
        webBridge.AttachHudSettings(_settings);
        // S172 — best-effort updater pipeline. Settings-gated by
        // `update_check_enabled` (default false); when off, the bridge
        // surfaces `{error:"unsupported"}` so the HUD button can grey.
        using var updater = UpdaterLifecycle.Start(_settings, _log);
        webBridge.AttachUpdater(updater);

        // S178 — script-share cloud client for the AutoKey panel.
        // Constructed unconditionally (best-effort; server unreachable just
        // means cloud commands reply `{ok:false}`). Owns its own
        // HttpClient via the default-ctor branch.
        // S180 — base URLs honor `auto_key.server_url` / `boss_raid.server_url`
        // from settings (with the canonical defaults as fallback).
        using var autoKeyCloud = AutoKeyCloudClient.FromSettings(_settings);
        // S182 — pass settings so successful searches persist as
        // `auto_key.last_remote_search` for editor restore on next boot.
        webBridge.AttachAutoKeyCloud(autoKeyCloud, _settings);

        using var bossRaidCloud = BossRaidCloudClient.FromSettings(_settings);
        // S183 — same persistence shape as S182's auto-key.
        webBridge.AttachBossRaidCloud(bossRaidCloud, _settings);

        // S193 — sound playback bridge for the pywebview shim.
        // Catalog points at assets/sounds (deployed by S185); player is
        // the existing WAV implementation. The bridge is still registered
        // when assets are missing so menu sound settings remain persistent.
        using var sounds = new WavSoundPlayer(logger: _log);
        var soundsDir = System.IO.Path.Combine(AppContext.BaseDirectory, "assets", "sounds");
        if (!System.IO.Directory.Exists(soundsDir))
        {
            _log.LogInformation(
                "sounds directory missing at {Dir}; sound settings bridge remains available", soundsDir);
        }
        webBridge.AttachSound(sounds, new SoundCatalog(soundsDir), _settings);
        webBridge.AttachLegacyUi(
            logger: _log,
            exitAction: () =>
            {
                try
                {
                    System.Windows.Application.Current?.Dispatcher.BeginInvoke(
                        new Action(() => System.Windows.Application.Current?.Shutdown(0)));
                }
                catch { /* swallow */ }
            });

        var application = (System.Windows.Application.Current as App) ?? new App();
        application.DispatcherUnhandledException += (_, e) =>
        {
            _log.LogError(e.Exception, "WPF dispatcher unhandled exception");
        };

        // Bug fix (user 2026-06-01, third pass): SAOLinkStart full port.
        // Bracket the splash.ShowDialog() with an explicit ShutdownMode
        // swap. App.xaml declares ShutdownMode="OnLastWindowClose"; the
        // splash is the only live Window when ShowDialog() runs (host
        // chain hasn't built its window yet), so when the splash closes
        // WPF auto-shuts the Application before the host can be shown.
        // OnExplicitShutdown keeps the Application alive across the
        // splash; we restore the previous mode + clear Application.MainWindow
        // afterward so the host window becomes the real MainWindow cleanly.
        // The ~9s 4-phase Storyboard mirrors the Python SAOLinkStart
        // sequence (sao_theme.py:3564 / sao_webview.py:3618). A 10.5s
        // safety timer guarantees the splash dismisses itself even if the
        // animation never raises Completed.
        if (_settings.GetBool("splash_enabled", true))
        {
            var prevShutdownMode = application.ShutdownMode;
            application.ShutdownMode = ShutdownMode.OnExplicitShutdown;
            try
            {
                var splash = new SaoSplashWindow();
                splash.ShowDialog();
            }
            catch (Exception ex)
            {
                _log.LogWarning(ex, "splash failed; continuing to host");
            }
            finally
            {
                application.ShutdownMode = prevShutdownMode;
                application.MainWindow = null;
            }
        }

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

            // S184 — WebView2 control init + bridge wire-up. The window is
            // already constructed (placeholder XAML) but `EnsureCoreWebView2Async`
            // must run on the dispatcher thread after the window is shown.
            // We hook Loaded → init → bind; tear the binding down on close.
            IDisposable? webViewBinding = null;
            if (window is EntityHostWindow ev)
            {
                // S196 — same Python-parity HUD geometry as WebView.
                ev.ApplyHudGeometry(_settings);
            }
            if (window is WebViewHostWindow wv)
            {
                // S196 — apply the Python-parity HUD geometry (75% wide,
                // 500 tall, anchored to monitor bottom at hud_offset_x).
                wv.ApplyHudGeometry(_settings);
                var startUrl = ResolveHudIndexUrl(_log);
                wv.Loaded += async (_, _) =>
                {
                    try
                    {
                        await wv.EnsureWebViewAsync(startUrl);
                        if (wv.MessageBus is not null)
                        {
                            webViewBinding = WebViewBridgeBinder.Bind(
                                wv.MessageBus, webBridge.HostAdapter, _log);
                            _log.LogInformation("WebView2 bridge bound");
                        }
                    }
                    catch (Exception ex)
                    {
                        _log.LogError(ex, "WebView2 initialisation failed");
                    }
                };
                wv.Closed += (_, _) => webViewBinding?.Dispose();
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
                // S191 — Win32 global hotkey service + HideSeek toggle
                // hotkey wiring. ComponentDispatcher hook stays installed
                // for the lifetime of this `Application.Run` so WM_HOTKEY
                // messages route through the service before WPF eats them.
                using var hotkeys = new Win32GlobalHotkeyService();
                System.Windows.Interop.ThreadMessageEventHandler dispatcherHook =
                    (ref System.Windows.Interop.MSG msg, ref bool handled) =>
                {
                    if (handled) return;
                    if (hotkeys.ProcessMessage((uint)msg.message, msg.wParam))
                    {
                        handled = true;
                    }
                };
                System.Windows.Interop.ComponentDispatcher.ThreadFilterMessage += dispatcherHook;
                using var hideSeekHotkey = HideSeekHotkeySetup.TryWire(
                    _settings, hotkeys, hideSeek, _log);
                try
                {
                    return application.Run(window);
                }
                finally
                {
                    System.Windows.Interop.ComponentDispatcher.ThreadFilterMessage -= dispatcherHook;
                }
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

    /// <summary>S184 / S192 — locate the canonical SAO HUD page on disk
    /// so the WebView2 control can load it. <c>hp.html</c> is the
    /// HP/SP/Burst overlay (the main game HUD); falls back to
    /// <c>panel.html</c> (music/piano panel) then <c>about:blank</c>.
    /// Searches a small list of paths relative to the running binary.
    ///
    /// Bug fix (user 2026-06-01, fourth pass): widen the dot-dot ladder to
    /// cover 3/4/5/6/7 levels and add a project-anchored upward walk that
    /// finds a directory named <c>sao_auto</c> containing
    /// <c>web/panel.html</c>. If this resolves to <c>about:blank</c> the
    /// panel buttons literally don't exist in the DOM — which presents
    /// as "buttons don't work" even with a perfectly transparent host.
    /// The hard log line at the end is the source-of-truth signal in
    /// %LOCALAPPDATA%/SaoAuto/logs/saoauto-*.log.</summary>
    public static string? ResolveHudIndexUrl(ILogger log)
    {
        var binDir = AppContext.BaseDirectory;
        string[] names = { "hp.html", "panel.html" };
        var roots = new List<string>
        {
            System.IO.Path.Combine(binDir, "web"),
            System.IO.Path.Combine(binDir, "..", "..", "..", "web"),
            System.IO.Path.Combine(binDir, "..", "..", "..", "..", "web"),
            System.IO.Path.Combine(binDir, "..", "..", "..", "..", "..", "web"),
            System.IO.Path.Combine(binDir, "..", "..", "..", "..", "..", "..", "web"),
            System.IO.Path.Combine(binDir, "..", "..", "..", "..", "..", "..", "..", "web"),
        };

        // Project-anchored upward walk: look for a dir named "sao_auto"
        // that contains web/panel.html. Walks at most 12 levels up to
        // guard against pathological mount points.
        try
        {
            var probe = new System.IO.DirectoryInfo(binDir);
            for (int i = 0; probe is not null && i < 12; i++, probe = probe.Parent)
            {
                var saoAuto = System.IO.Path.Combine(probe.FullName, "sao_auto");
                var probePanel = System.IO.Path.Combine(saoAuto, "web", "panel.html");
                if (System.IO.File.Exists(probePanel))
                {
                    roots.Add(System.IO.Path.Combine(saoAuto, "web"));
                    break;
                }
            }
        }
        catch { /* ignore probe failure */ }

        var candidates = new List<string>(names.Length * roots.Count);
        foreach (var name in names)
            foreach (var root in roots)
                candidates.Add(System.IO.Path.Combine(root, name));
        foreach (var c in candidates)
        {
            try
            {
                var full = System.IO.Path.GetFullPath(c);
                if (System.IO.File.Exists(full))
                {
                    var url = new Uri(full).AbsoluteUri;
                    log.LogInformation("HUD index resolved -> {Path}", full);
                    return url;
                }
            }
            catch { /* ignore malformed candidates */ }
        }
        log.LogWarning("HUD index not found; starting on about:blank — buttons WILL appear dead until web/panel.html is reachable");
        return null;
    }
}
