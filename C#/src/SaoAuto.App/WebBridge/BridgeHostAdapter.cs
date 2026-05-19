using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S157 — String-in / string-out adapter that sits between a
/// WebView2 host and the in-process bridge. Converts the raw JSON the
/// WebView delivers via <c>WebMessageReceived</c> into a
/// <see cref="BridgeMessage"/>, dispatches through a
/// <see cref="BridgeRouter"/>, and serialises any reply for
/// <c>PostWebMessageAsString</c>.
///
/// Also re-emits every <see cref="BridgeEventBroadcaster.Posted"/>
/// payload as a wire-format JSON string on the <see cref="PostJson"/>
/// event so the host can forward it to the WebView2 without poking the
/// bridge internals.
///
/// Pure adapter — no Microsoft.Web.WebView2 dependency. The actual
/// WebView2 wiring is two glue lines at the host site:
///
/// <code>
/// webView.CoreWebView2.WebMessageReceived += (_, e) =&gt;
///     webView.CoreWebView2.PostWebMessageAsString(
///         adapter.HandleMessageJson(e.WebMessageAsJson) ?? "");
/// adapter.PostJson += s =&gt; webView.CoreWebView2.PostWebMessageAsString(s);
/// </code>
/// </summary>
public sealed class BridgeHostAdapter : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly BridgeEventBroadcaster _broadcaster;
    private readonly ILogger _log;
    private readonly Action<BridgeMessage> _broadcasterHandler;
    private bool _disposed;

    /// <summary>
    /// Fires once per <see cref="BridgeEventBroadcaster.Posted"/>
    /// payload, with the message already serialised to wire JSON.
    /// Subscriber exceptions are swallowed so a misbehaving host
    /// cannot kill event delivery.
    /// </summary>
    public event Action<string>? PostJson;

    public BridgeHostAdapter(
        BridgeRouter router,
        BridgeEventBroadcaster broadcaster,
        ILogger<BridgeHostAdapter>? logger = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _broadcaster = broadcaster ?? throw new ArgumentNullException(nameof(broadcaster));
        _log = (ILogger?)logger ?? NullLogger.Instance;
        _broadcasterHandler = OnBroadcasterPosted;
        _broadcaster.Posted += _broadcasterHandler;
    }

    /// <summary>
    /// Parse <paramref name="json"/>, dispatch, return the reply
    /// envelope as wire JSON — or <c>null</c> when the input was an
    /// event/reply (nothing to send back) or unparseable.
    /// </summary>
    public string? HandleMessageJson(string? json)
    {
        if (_disposed) return null;
        if (string.IsNullOrWhiteSpace(json)) return null;
        var msg = BridgeMessage.TryParse(json!);
        if (msg is null)
        {
            _log.LogWarning("[BridgeHostAdapter] dropping unparseable message");
            return null;
        }
        var reply = _router.Dispatch(msg);
        return reply?.ToWireJson();
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _broadcaster.Posted -= _broadcasterHandler;
    }

    private void OnBroadcasterPosted(BridgeMessage msg)
    {
        var sink = PostJson;
        if (sink is null) return;
        string wire;
        try { wire = msg.ToWireJson(); }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[BridgeHostAdapter] failed to serialise event {Name}", msg.Name);
            return;
        }
        try { sink(wire); }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[BridgeHostAdapter] PostJson subscriber threw on {Name}", msg.Name);
        }
    }
}
