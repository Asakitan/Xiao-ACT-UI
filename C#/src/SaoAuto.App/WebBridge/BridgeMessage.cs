using System.Text.Json.Nodes;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Wire envelope shared by JS ↔ C# in the WebView2 bridge. Mirrors the
/// shape Python's <c>SAOWebViewGUI._on_js_message</c> consumes:
/// <c>{ "type": "command|event", "name": "...", "payload": { ... } }</c>.
/// </summary>
public sealed record BridgeMessage(string Type, string Name, JsonObject? Payload)
{
    public const string TypeCommand = "command";
    public const string TypeEvent = "event";
    public const string TypeReply = "reply";

    public string ToWireJson() => new JsonObject
    {
        ["type"] = Type,
        ["name"] = Name,
        ["payload"] = Payload?.DeepClone(),
    }.ToJsonString();

    public static BridgeMessage? TryParse(string json)
    {
        try
        {
            var node = JsonNode.Parse(json) as JsonObject;
            if (node is null) return null;
            var type = node["type"]?.GetValue<string>();
            var name = node["name"]?.GetValue<string>();
            if (string.IsNullOrEmpty(type) || string.IsNullOrEmpty(name)) return null;
            return new BridgeMessage(type!, name!, node["payload"] as JsonObject);
        }
        catch
        {
            return null;
        }
    }
}
