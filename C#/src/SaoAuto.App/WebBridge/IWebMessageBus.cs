namespace SaoAuto.App.WebBridge;

/// <summary>
/// S158 — Minimal abstraction over a WebView-style message channel.
/// Lets <see cref="WebViewBridgeBinder"/> connect a
/// <see cref="BridgeHostAdapter"/> to anything that can post / receive
/// JSON strings (real <c>CoreWebView2</c>, in-process fake, test
/// harness).
///
/// The contract is intentionally narrow:
/// <list type="bullet">
///   <item><see cref="Received"/> fires for every inbound JSON string
///   from the page (typically the host raises it in response to
///   <c>WebMessageReceived</c>).</item>
///   <item><see cref="Post"/> sends a JSON string back to the page
///   (typically a wrapper around
///   <c>PostWebMessageAsString</c>).</item>
/// </list>
/// </summary>
public interface IWebMessageBus
{
    event Action<string>? Received;
    void Post(string json);
}
