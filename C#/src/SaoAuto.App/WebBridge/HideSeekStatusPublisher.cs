using System.Text.Json.Nodes;
using SaoAuto.App.Startup;
using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S155 — Bridges <see cref="HideSeekLifecycle"/> → JS via
/// <see cref="BridgeEvents.HideSeekStatus"/>. Subscribes to per-tick
/// snapshots and re-emits when any of the four fields
/// (<c>is_running</c>, <c>tick_count</c>, <c>last_fired_step</c>,
/// <c>last_error</c>) differ from the previously emitted payload.
///
/// Mirrors the dedupe contract in
/// <see cref="GameStatePublisher"/>: identical back-to-back snapshots
/// collapse to one wire emit. <c>Start(emitInitial: true)</c> fires
/// the current snapshot (or the inactive Empty) so a JS subscriber
/// that attaches before the first tick still has a value to render.
/// </summary>
public sealed class HideSeekStatusPublisher : IDisposable
{
    private readonly HideSeekLifecycle _lifecycle;
    private readonly BridgeEventBroadcaster _broadcaster;
    private readonly object _gate = new();
    private IDisposable? _sub;
    private HideSeekRuntimeSnapshot? _lastEmitted;
    private bool _disposed;

    public HideSeekStatusPublisher(
        HideSeekLifecycle lifecycle,
        BridgeEventBroadcaster broadcaster)
    {
        _lifecycle = lifecycle ?? throw new ArgumentNullException(nameof(lifecycle));
        _broadcaster = broadcaster ?? throw new ArgumentNullException(nameof(broadcaster));
    }

    public bool IsActive => _sub is not null;

    public void Start(bool emitInitial = true)
    {
        lock (_gate)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(HideSeekStatusPublisher));
            if (_sub is not null) return;
            _sub = _lifecycle.Subscribe(OnTick);
        }
        if (emitInitial) OnTick(_lifecycle.SnapshotRuntime());
    }

    public void Dispose()
    {
        IDisposable? toDispose;
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            toDispose = _sub;
            _sub = null;
        }
        toDispose?.Dispose();
    }

    private void OnTick(HideSeekRuntimeSnapshot snap)
    {
        lock (_gate)
        {
            if (_lastEmitted is { } prev && prev.Equals(snap)) return;
            _lastEmitted = snap;
        }
        _broadcaster.Emit(BridgeEvents.HideSeekStatus, new JsonObject
        {
            ["is_running"] = snap.IsRunning,
            ["tick_count"] = snap.TickCount,
            ["last_fired_step"] = snap.LastFiredStep,
            ["last_error"] = snap.LastError,
        });
    }
}
