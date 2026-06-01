using System.Globalization;
using System.IO;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Logging;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.App.Startup;

/// <summary>
/// Best-effort start/stop wrapper around the live packet pipeline
/// (`IPacketSource` → <see cref="TcpReassembler"/> →
/// <see cref="PacketParser"/> → <see cref="PacketBridge"/>).
///
/// Mirrors <c>RecognitionLifecycle</c>: startup failures are logged and
/// swallowed so UI/headless modes keep running even when Npcap or the
/// game traffic is unavailable. When active, exposes a DPS snapshot
/// provider that WebBridge can thread into
/// <see cref="StateSnapshotPayload.ToDict(GameState, DpsSnapshot?)"/>.
/// </summary>
public sealed class PacketLifecycle : IDisposable
{
    private readonly PacketRuntimeHost? _host;
    private readonly ILogger _log;
    private bool _disposed;

    private PacketLifecycle(PacketRuntimeHost? host, ILogger log)
    {
        _host = host;
        _log = log;
    }

    public bool IsActive => _host is not null;

    public Func<DpsSnapshot?>? DpsSnapshotProvider =>
        _host is null ? null : _host.SnapshotWithSkills;

    /// <summary>S169 — pass-through for <c>DpsTracker.Reset()</c> so
    /// <see cref="WebBridge.DpsBridge"/> can clear combat state from
    /// HUD. Null when the packet runtime didn't start, which makes
    /// the bridge surface <c>{error:"dps_unavailable"}</c>.</summary>
    public Action? ResetDps =>
        _host is null ? null : _host.ResetDps;

    /// <summary>R8: live DpsTracker so the WebBridge can drive the per-tick
    /// DPS overlay pump (DPS-02/03/07). Null when the packet runtime
    /// didn't start — publisher gracefully degrades to snapshot-only.</summary>
    public DpsTracker? DpsTracker =>
        _host?.DpsTracker;

    /// <summary>R8: live PacketBridge so the App layer can subscribe to
    /// <see cref="Core.Bridge.PacketBridge.SceneChanged"/> and fan
    /// scene-change events through the WebBridge.</summary>
    public PacketBridge? PacketBridge =>
        _host?.Bridge;

    public static PacketLifecycle Start(
        SettingsManager settings,
        GameStateManager states,
        ILogger? logger,
        CancellationToken cancellationToken,
        Func<IPacketSource>? sourceFactory = null)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        if (states is null) throw new ArgumentNullException(nameof(states));

        var log = logger ?? SaoLog.For("packet");
        if (!IsPacketRuntimeRequested(settings))
        {
            log.LogInformation("packet runtime disabled by settings");
            return new PacketLifecycle(null, log);
        }

        PacketRuntimeHost? host = null;
        try
        {
            var source = sourceFactory?.Invoke() ?? CreateSourceFromSettings(settings, log);
            host = new PacketRuntimeHost(states, source, log);
            TryLoadSkillNames(host.DpsTracker, log);
            host.StartAsync(cancellationToken).GetAwaiter().GetResult();
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "packet runtime failed to start; continuing without it");
            if (host is not null)
            {
                try { host.Dispose(); } catch { /* swallow */ }
                host = null;
            }
        }

        return new PacketLifecycle(host, log);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_host is null) return;
        try { _host.StopAsync().GetAwaiter().GetResult(); } catch { /* swallow */ }
        _host.Dispose();
    }

    internal static bool IsPacketRuntimeRequested(SettingsManager settings)
    {
        var overall = settings.GetString(SettingsKeys.DataSource, "mixed");
        if (string.Equals(overall, "packet", StringComparison.OrdinalIgnoreCase)
            || string.Equals(overall, "mixed", StringComparison.OrdinalIgnoreCase)
            || string.Equals(overall, "hybrid", StringComparison.OrdinalIgnoreCase)
            || string.Equals(overall, "auto", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        var mem = settings.GetString(SettingsKeys.MemDataSource, string.Empty);
        if (string.Equals(mem, "tcp", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        var map = settings.Get<Dictionary<string, string>>(SettingsKeys.DataSourceMap, null);
        return map?.Values.Any(v => string.Equals(v, "packet", StringComparison.OrdinalIgnoreCase)) == true;
    }

    internal static IPacketSource CreateSourceFromSettings(SettingsManager settings, ILogger log)
    {
        var requestedDevice = settings.GetString(SettingsKeys.CaptureDevice, null);
        if (!string.IsNullOrWhiteSpace(requestedDevice))
        {
            log.LogInformation("packet runtime requested adapter={Adapter}", requestedDevice);
        }
        return new SharpPcapPacketSource(deviceName: requestedDevice);
    }

    /// <summary>
    /// Load <c>assets/skill_names.json</c> and push it into the tracker.
    /// Mirrors <c>sao_webview.py:3215-3229</c>: the JSON is a string→string
    /// map keyed by numeric skill ids; non-numeric keys are dropped.
    /// Best-effort: failures are logged at warning level and swallowed so
    /// the packet runtime still starts when the file is missing (e.g. dev
    /// builds without bundled assets).
    /// </summary>
    private static void TryLoadSkillNames(DpsTracker tracker, ILogger log)
    {
        // AppContext.BaseDirectory (not Assembly.Location) keeps this working
        // under AoT / single-file publishes, per the FN-001 plan.
        var path = Path.Combine(AppContext.BaseDirectory, "assets", "skill_names.json");
        if (!File.Exists(path))
        {
            log.LogDebug("skill_names.json not found at {Path}; skipping load", path);
            return;
        }
        try
        {
            var json = File.ReadAllText(path);
            var raw = JsonSerializer.Deserialize<Dictionary<string, string>>(json);
            if (raw is null || raw.Count == 0)
            {
                log.LogInformation("skill_names.json at {Path} contained no entries", path);
                return;
            }
            var map = new Dictionary<long, string>(raw.Count);
            foreach (var kv in raw)
            {
                if (long.TryParse(kv.Key, NumberStyles.Integer, CultureInfo.InvariantCulture, out var id))
                {
                    map[id] = kv.Value;
                }
            }
            tracker.SetSkillNames(map);
            log.LogInformation("Loaded {Count} skill names from {Path}", map.Count, path);
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "Failed to load skill_names.json from {Path}", path);
        }
    }

    internal sealed class PacketRuntimeHost : IDisposable
    {
        private readonly IPacketSource _source;
        private readonly TcpReassembler _reassembler;
        private readonly PacketParser _parser;
        private readonly PacketBridge _bridge;
        private readonly ILogger _log;
        private readonly CancellationTokenSource _shutdown = new();
        private CancellationTokenSource? _runCts;
        private Task? _pump;
        private bool _disposed;

        public PacketRuntimeHost(
            GameStateManager states,
            IPacketSource source,
            ILogger logger,
            PacketParser? parser = null,
            TcpReassembler? reassembler = null,
            MethodDecoderRegistry? registry = null)
        {
            _source = source ?? throw new ArgumentNullException(nameof(source));
            _log = logger ?? throw new ArgumentNullException(nameof(logger));
            _parser = parser ?? new PacketParser();
            _reassembler = reassembler ?? new TcpReassembler();
            _bridge = new PacketBridge(states ?? throw new ArgumentNullException(nameof(states)), _parser, registry);

            _reassembler.GamePacketCaptured += OnGamePacketCaptured;
            _reassembler.ServerChange += OnServerChange;
            // S135: PacketBridge subscribes to parser.NotifyBodyAvailable
            // itself; the lifecycle no longer needs to forward.
        }

        public DpsSnapshot SnapshotWithSkills() => _bridge.DpsTracker.SnapshotWithSkills();

        public void ResetDps() => _bridge.DpsTracker.Reset();

        /// <summary>Internal accessor so <see cref="PacketLifecycle.Start"/>
        /// can feed the skill-name table into <see cref="DpsTracker"/> right
        /// after the bridge is wired (mirrors <c>sao_webview.py:3215-3229</c>
        /// where <c>assets/skill_names.json</c> is pushed via
        /// <c>set_skill_names</c> immediately after <c>DpsTracker()</c>).</summary>
        internal DpsTracker DpsTracker => _bridge.DpsTracker;

        /// <summary>R8: expose the live PacketBridge so the UI / WebBridge
        /// can subscribe to its <see cref="PacketBridge.SceneChanged"/>
        /// observation event.</summary>
        internal PacketBridge Bridge => _bridge;

        public Task StartAsync(CancellationToken cancellationToken)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(PacketRuntimeHost));
            if (_pump is not null) return Task.CompletedTask;

            _runCts = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token, cancellationToken);
            _pump = Task.Run(() => PumpAsync(_runCts.Token), CancellationToken.None);
            return Task.CompletedTask;
        }

        public async Task StopAsync()
        {
            _shutdown.Cancel();
            _runCts?.Cancel();
            if (_pump is null) return;
            try
            {
                await _pump.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                // normal shutdown
            }
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;

            _shutdown.Cancel();
            _runCts?.Cancel();

            _reassembler.ServerChange -= OnServerChange;
            _reassembler.GamePacketCaptured -= OnGamePacketCaptured;

            _bridge.Dispose();
            _source.Dispose();
            _runCts?.Dispose();
            _shutdown.Dispose();
        }

        private async Task PumpAsync(CancellationToken cancellationToken)
        {
            try
            {
                await foreach (var frame in _source.ReadAsync(cancellationToken).ConfigureAwait(false))
                {
                    _reassembler.FeedRawFrame(frame.Bytes.Span, frame.Timestamp);
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                // expected on shutdown
            }
            catch (Exception ex)
            {
                _log.LogWarning(ex, "packet runtime pump failed; continuing without live packets");
            }
        }

        private void OnGamePacketCaptured(CapturedGamePacket packet)
        {
            _parser.FeedGameFrame(packet.Frame.Span, packet.TimestampSeconds);
        }

        private void OnServerChange()
        {
            _parser.Reset();
        }
    }
}
