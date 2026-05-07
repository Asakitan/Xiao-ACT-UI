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
    private bool _disposed;

    public BridgeEventBroadcaster Broadcaster { get; }

    public WebBridgeLifecycle(GameStateManager states)
    {
        if (states is null) throw new ArgumentNullException(nameof(states));
        Broadcaster = new BridgeEventBroadcaster();
        _publisher = new GameStatePublisher(states, Broadcaster);
    }

    public bool IsActive => _publisher.IsActive;

    public void Start(bool emitInitial = true)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(WebBridgeLifecycle));
        _publisher.Start(emitInitial);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _publisher.Dispose();
    }
}
