using System.Text.Json.Nodes;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Emits bridge events to JS subscribers. The WebView host wires `Posted`
/// to `CoreWebView2.PostWebMessageAsString`. Tests subscribe directly to
/// the event to assert payload shapes.
/// </summary>
public sealed class BridgeEventBroadcaster
{
    public event Action<BridgeMessage>? Posted;

    public void Emit(string eventName, JsonObject? payload = null)
    {
        if (string.IsNullOrEmpty(eventName)) throw new ArgumentException(null, nameof(eventName));
        var msg = new BridgeMessage(BridgeMessage.TypeEvent, eventName, payload);
        try { Posted?.Invoke(msg); }
        catch { /* swallow */ }
    }

    public void Emit<T>(string eventName, T payload)
    {
        if (payload is null)
        {
            Emit(eventName);
            return;
        }
        var node = System.Text.Json.JsonSerializer.SerializeToNode(payload) as JsonObject;
        Emit(eventName, node);
    }
}
