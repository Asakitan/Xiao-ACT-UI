using Microsoft.Extensions.Logging;
using SaoAuto.App.Startup;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S99 — Bundles the WebView-bridge runtime: a single
/// <see cref="BridgeEventBroadcaster"/> shared by command/event
/// emitters, plus a <see cref="GameStatePublisher"/> that streams
/// <c>state.changed</c> events whenever the
/// <see cref="GameStateManager"/> mutates.
///
/// The host (WebView2 once wired) attaches its
/// <c>CoreWebView2.PostWebMessageAsString</c> to
/// <see cref="BridgeEventBroadcaster.Posted"/>; for now the broadcaster
/// is the single composition point so we can ship the publisher with
/// a real lifetime owner.
///
/// <see cref="Start"/> fires an initial snapshot so the JS side has a
/// payload to render at attach. <see cref="Dispose"/> tears the
/// publisher down idempotently. The broadcaster has no managed
/// resources to dispose.
/// </summary>
public sealed class WebBridgeLifecycle : IDisposable
{
    private readonly GameStatePublisher _publisher;
    private readonly DpsTracker? _dpsTracker;
    private HideSeekStatusPublisher? _hideSeekPublisher;
    private AutoKeyProfileBridge? _autoKeyProfileBridge;
    private BuffMonBridge? _buffMonBridge;
    private DpsBridge? _dpsBridge;
    private HudSettingsBridge? _hudSettingsBridge;
    private RecognitionStatusBridge? _recognitionBridge;
    private UpdaterBridge? _updaterBridge;
    private AutoKeyCloudBridge? _autoKeyCloudBridge;
    private BossRaidCloudBridge? _bossRaidCloudBridge;
    private SoundBridge? _soundBridge;
    private LegacyUiBridge? _legacyUiBridge;
    private ActBridge? _actBridge;
    private bool _disposed;

    public BridgeEventBroadcaster Broadcaster { get; }
    /// <summary>S156 — single router owned by the bridge runtime.
    /// Empty until something registers (e.g.
    /// <see cref="AttachAutoKeyProfile"/>).</summary>
    public BridgeRouter Router { get; } = new();
    /// <summary>S157 — string-in/string-out adapter for a WebView2
    /// host. Subscribe to <see cref="BridgeHostAdapter.PostJson"/> and
    /// route its output to <c>CoreWebView2.PostWebMessageAsString</c>.
    /// Feed inbound messages via
    /// <see cref="BridgeHostAdapter.HandleMessageJson"/>.</summary>
    public BridgeHostAdapter HostAdapter { get; }

    public WebBridgeLifecycle(
        GameStateManager states,
        Func<DpsSnapshot?>? dpsSnapshotProvider = null,
        DpsTracker? dpsTracker = null,
        Func<bool>? dpsEnabledProvider = null)
    {
        if (states is null) throw new ArgumentNullException(nameof(states));
        Broadcaster = new BridgeEventBroadcaster();
        _dpsTracker = dpsTracker;
        // R8 / DPS-02/03: pass the live DpsTracker into the publisher so it
        // can drive show-live / fade-out edges via the per-tick pump. When
        // null, the publisher falls back to the snapshot-only behaviour
        // shipped before R8 (no edge, no fade).
        _publisher = new GameStatePublisher(
            states, Broadcaster, dpsSnapshotProvider, dpsTracker, dpsEnabledProvider);
        HostAdapter = new BridgeHostAdapter(Router, Broadcaster);
    }

    public bool IsActive => _publisher.IsActive;

    public void Start(bool emitInitial = true)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _publisher.Start(emitInitial);
        // S190 — register the snapshot pull command. Returns the same
        // payload shape `state.changed` events emit; HUD pages call it
        // on first load to paint immediately instead of waiting for the
        // next change.
        Router.Register(BridgeCommands.StateSnapshot, _ => _publisher.SnapshotPayload());
    }

    /// <summary>
    /// S155 — attach a <see cref="HideSeekLifecycle"/> so per-tick
    /// snapshots flow out as <see cref="BridgeEvents.HideSeekStatus"/>.
    /// Idempotent: a second call replaces the previous attachment.
    /// </summary>
    public void AttachHideSeek(HideSeekLifecycle lifecycle, bool emitInitial = true)
    {
        if (lifecycle is null) throw new ArgumentNullException(nameof(lifecycle));
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _hideSeekPublisher?.Dispose();
        _hideSeekPublisher = new HideSeekStatusPublisher(lifecycle, Broadcaster);
        _hideSeekPublisher.Start(emitInitial);
    }

    /// <summary>
    /// S156 — attach an <see cref="AutoKeyProfileLifecycle"/> so its
    /// service registers profile-CRUD handlers on <see cref="Router"/>.
    /// Idempotent: a second call replaces the previous attachment.
    /// </summary>
    public void AttachAutoKeyProfile(AutoKeyProfileLifecycle lifecycle)
    {
        if (lifecycle is null) throw new ArgumentNullException(nameof(lifecycle));
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _autoKeyProfileBridge?.Dispose();
        _autoKeyProfileBridge = new AutoKeyProfileBridge(lifecycle.Service, Router);
    }

    /// <summary>
    /// S169 — attach a <see cref="BuffMonBridge"/> so HUD can read/write
    /// the <c>buffmon_enabled</c> setting. Idempotent.
    /// </summary>
    public void AttachBuffMon(SettingsManager settings)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _buffMonBridge?.Dispose();
        _buffMonBridge = new BuffMonBridge(settings, Router);
    }

    /// <summary>
    /// S169 — attach a <see cref="DpsBridge"/> backed by the packet
    /// lifecycle's reset action + snapshot provider. Either delegate
    /// may be null when the packet runtime didn't start; the bridge
    /// then returns <c>{error:"dps_unavailable"}</c>. Idempotent.
    /// </summary>
    public void AttachDps(
        Action? reset,
        Func<DpsSnapshot?>? lastReport,
        SettingsManager? settings = null,
        DpsTracker? tracker = null)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _dpsBridge?.Dispose();
        _dpsBridge = new DpsBridge(Router, reset, lastReport, settings, tracker ?? _dpsTracker);
    }

    /// <summary>S197 — attach real handlers for menu-backed HUD settings
    /// such as watched skill slots and Burst alert enablement.</summary>
    public void AttachHudSettings(SettingsManager settings)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _hudSettingsBridge?.Dispose();
        _hudSettingsBridge = new HudSettingsBridge(Router, settings);
    }

    /// <summary>
    /// S169 — attach a <see cref="RecognitionStatusBridge"/>. The
    /// <paramref name="start"/> / <paramref name="stop"/> hooks stay
    /// optional until <see cref="RecognitionLifecycle"/> grows a real
    /// suspend/restart path. Idempotent.
    /// </summary>
    public void AttachRecognition(
        Func<bool> isActive,
        Action? start = null,
        Action? stop = null)
    {
        if (isActive is null) throw new ArgumentNullException(nameof(isActive));
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _recognitionBridge?.Dispose();
        _recognitionBridge = new RecognitionStatusBridge(Router, isActive, start, stop);
    }

    /// <summary>
    /// S172 — attach an <see cref="UpdaterLifecycle"/>. Registers
    /// <c>updater.check</c> / <c>updater.download</c> / <c>updater.apply</c>
    /// on <see cref="Router"/> and rebroadcasts the
    /// <see cref="Core.Updater.UpdaterStateMachine.StateChanged"/> event as
    /// <see cref="BridgeEvents.UpdaterStatus"/>. Idempotent.
    /// </summary>
    public void AttachUpdater(UpdaterLifecycle lifecycle)
    {
        if (lifecycle is null) throw new ArgumentNullException(nameof(lifecycle));
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _updaterBridge?.Dispose();
        _updaterBridge = new UpdaterBridge(
            Router,
            lifecycle.StateMachine,
            Broadcaster,
            lifecycle.Check,
            lifecycle.Download,
            lifecycle.Apply);
    }

    /// <summary>
    /// S178 — attach an <see cref="AutoKeyCloudClient"/> so the four
    /// <c>autokey.cloud.*</c> commands route through <see cref="Router"/>.
    /// The caller owns the client's lifetime (a shared
    /// <see cref="HttpClient"/> typically outlives this lifecycle).
    /// Idempotent: a second call replaces the previous attachment.
    /// </summary>
    public void AttachAutoKeyCloud(AutoKeyCloudClient client, SettingsManager? settings = null)
    {
        if (client is null) throw new ArgumentNullException(nameof(client));
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _autoKeyCloudBridge?.Dispose();
        _autoKeyCloudBridge = new AutoKeyCloudBridge(Router, client, settings);
    }

    /// <summary>
    /// S179 — attach a <see cref="BossRaidCloudClient"/> so the four
    /// <c>bossraid.cloud.*</c> commands route through <see cref="Router"/>.
    /// The caller owns the client's lifetime. Idempotent.
    /// </summary>
    public void AttachBossRaidCloud(BossRaidCloudClient client, SettingsManager? settings = null)
    {
        if (client is null) throw new ArgumentNullException(nameof(client));
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _bossRaidCloudBridge?.Dispose();
        _bossRaidCloudBridge = new BossRaidCloudBridge(Router, client, settings);
    }

    /// <summary>S193 — attach the sound playback bridge so the
    /// pywebview-shim's <c>sound.play</c> calls route into the
    /// in-process <see cref="ISoundPlayer"/>. Idempotent.</summary>
    public void AttachSound(ISoundPlayer player, SoundCatalog catalog)
    {
        if (player is null) throw new ArgumentNullException(nameof(player));
        if (catalog is null) throw new ArgumentNullException(nameof(catalog));
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _soundBridge?.Dispose();
        _soundBridge = new SoundBridge(Router, player, catalog);
    }

    /// <summary>S193 — attach the legacy UI command stub so
    /// pywebview-shim's <c>ui.*</c> calls don't fail. Idempotent.</summary>
    public void AttachLegacyUi(Microsoft.Extensions.Logging.ILogger? logger = null, Action? exitAction = null)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _legacyUiBridge?.Dispose();
        _legacyUiBridge = new LegacyUiBridge(Router, logger, exitAction);
    }

    /// <summary>S194 — attach ACT platform command handlers for shared
    /// WebView panels. When <paramref name="handler"/> is null, commands
    /// return structured <c>{error:"act_unavailable"}</c> instead of
    /// falling through as unknown commands. Idempotent.</summary>
    public void AttachAct(ActBridge.ActCommandHandler? handler = null)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _actBridge?.Dispose();
        _actBridge = new ActBridge(Router, handler);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _actBridge?.Dispose();
        _legacyUiBridge?.Dispose();
        _soundBridge?.Dispose();
        _bossRaidCloudBridge?.Dispose();
        _autoKeyCloudBridge?.Dispose();
        _updaterBridge?.Dispose();
        _recognitionBridge?.Dispose();
        _hudSettingsBridge?.Dispose();
        _dpsBridge?.Dispose();
        _buffMonBridge?.Dispose();
        _autoKeyProfileBridge?.Dispose();
        _hideSeekPublisher?.Dispose();
        HostAdapter.Dispose();
        _publisher.Dispose();
    }
}
