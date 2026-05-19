using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S158 — Binds an <see cref="IWebMessageBus"/> to a
/// <see cref="BridgeHostAdapter"/>. Encapsulates the two glue lines a
/// WebView2 host would otherwise paste at the call site:
///
/// <list type="number">
///   <item>Inbound: bus message → <see cref="BridgeHostAdapter.HandleMessageJson"/>
///   → bus.Post(reply) when the dispatch produced a reply.</item>
///   <item>Outbound: <see cref="BridgeHostAdapter.PostJson"/> →
///   bus.Post.</item>
/// </list>
///
/// Disposing the returned binding unhooks both sides so the host can
/// detach without leaking subscriptions.
/// </summary>
public static class WebViewBridgeBinder
{
    public static IDisposable Bind(
        IWebMessageBus bus,
        BridgeHostAdapter adapter,
        ILogger? logger = null)
    {
        if (bus is null) throw new ArgumentNullException(nameof(bus));
        if (adapter is null) throw new ArgumentNullException(nameof(adapter));
        var log = logger ?? NullLogger.Instance;

        void OnReceived(string json)
        {
            string? reply;
            try { reply = adapter.HandleMessageJson(json); }
            catch (Exception ex)
            {
                log.LogWarning(ex, "[WebViewBridgeBinder] adapter threw on inbound message");
                return;
            }
            if (reply is null) return;
            try { bus.Post(reply); }
            catch (Exception ex)
            {
                log.LogWarning(ex, "[WebViewBridgeBinder] bus.Post threw on reply");
            }
        }

        void OnPostJson(string json)
        {
            try { bus.Post(json); }
            catch (Exception ex)
            {
                log.LogWarning(ex, "[WebViewBridgeBinder] bus.Post threw on event");
            }
        }

        bus.Received += OnReceived;
        adapter.PostJson += OnPostJson;
        return new Binding(bus, adapter, OnReceived, OnPostJson);
    }

    private sealed class Binding : IDisposable
    {
        private IWebMessageBus? _bus;
        private BridgeHostAdapter? _adapter;
        private readonly Action<string> _onReceived;
        private readonly Action<string> _onPostJson;

        public Binding(
            IWebMessageBus bus,
            BridgeHostAdapter adapter,
            Action<string> onReceived,
            Action<string> onPostJson)
        {
            _bus = bus;
            _adapter = adapter;
            _onReceived = onReceived;
            _onPostJson = onPostJson;
        }

        public void Dispose()
        {
            if (_bus is { } b) b.Received -= _onReceived;
            if (_adapter is { } a) a.PostJson -= _onPostJson;
            _bus = null;
            _adapter = null;
        }
    }
}
