using Microsoft.Web.WebView2.Core;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S158 — Thin adapter exposing a <see cref="CoreWebView2"/> as an
/// <see cref="IWebMessageBus"/>. Trivial glue — not covered by unit
/// tests because it pokes WebView2 directly; behaviour matches the
/// two-line example in <see cref="BridgeHostAdapter"/>'s remarks.
///
/// Caller owns the <see cref="CoreWebView2"/> lifetime; this adapter
/// only forwards messages. Dispose unhooks <c>WebMessageReceived</c>.
/// </summary>
public sealed class CoreWebView2MessageBus : IWebMessageBus, IDisposable
{
    private readonly CoreWebView2 _core;
    private readonly EventHandler<CoreWebView2WebMessageReceivedEventArgs> _onReceived;
    private bool _disposed;

    public event Action<string>? Received;

    public CoreWebView2MessageBus(CoreWebView2 core)
    {
        _core = core ?? throw new ArgumentNullException(nameof(core));
        _onReceived = (_, e) =>
        {
            // WebMessageAsJson is the raw JSON the page passed to
            // window.chrome.webview.postMessage; pipe straight through.
            var sink = Received;
            if (sink is null) return;
            try { sink(e.WebMessageAsJson); }
            catch { /* swallow — binder logs subscriber failures */ }
        };
        _core.WebMessageReceived += _onReceived;
    }

    public void Post(string json)
    {
        if (_disposed) return;
        _core.PostWebMessageAsString(json);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _core.WebMessageReceived -= _onReceived;
    }
}
