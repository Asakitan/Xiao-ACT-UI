using System.Windows.Threading;
using Microsoft.Web.WebView2.Core;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S158 — Thin adapter exposing a <see cref="CoreWebView2"/> as an
/// <see cref="IWebMessageBus"/>.
///
/// <para>S185 — <see cref="CoreWebView2"/> is COM-affine: every member
/// must be invoked on the UI/STA thread that owns it. Events posted
/// from background threads (e.g. the state-change broadcaster) are
/// marshalled via the supplied <see cref="Dispatcher"/>; calls already
/// on that dispatcher run synchronously.</para>
///
/// Caller owns the <see cref="CoreWebView2"/> lifetime; this adapter
/// only forwards messages. Dispose unhooks <c>WebMessageReceived</c>.
/// </summary>
public sealed class CoreWebView2MessageBus : IWebMessageBus, IDisposable
{
    private readonly CoreWebView2 _core;
    private readonly Dispatcher _dispatcher;
    private readonly EventHandler<CoreWebView2WebMessageReceivedEventArgs> _onReceived;
    private bool _disposed;

    public event Action<string>? Received;

    public CoreWebView2MessageBus(CoreWebView2 core, Dispatcher? dispatcher = null)
    {
        _core = core ?? throw new ArgumentNullException(nameof(core));
        // CoreWebView2 doesn't expose its owning dispatcher directly;
        // callers wire it from the hosting Window's Dispatcher. Falling
        // back to CurrentDispatcher is correct when constructed on the
        // UI thread (the canonical path).
        _dispatcher = dispatcher ?? Dispatcher.CurrentDispatcher;
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
        if (_dispatcher.CheckAccess())
        {
            _core.PostWebMessageAsString(json);
            return;
        }
        // Fire-and-forget marshal to the UI thread; exceptions inside
        // the dispatched callback surface via Dispatcher.UnhandledException.
        _dispatcher.BeginInvoke(new Action(() =>
        {
            if (_disposed) return;
            try { _core.PostWebMessageAsString(json); }
            catch { /* swallow — host disposed mid-flight */ }
        }));
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _core.WebMessageReceived -= _onReceived;
    }
}
