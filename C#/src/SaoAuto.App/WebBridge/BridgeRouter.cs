using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Maps incoming <see cref="BridgeMessage"/> commands to handlers and
/// produces optional reply payloads. The WebView2 host wires this to the
/// `WebMessageReceived` event; tests can drive it directly with synthetic
/// messages.
/// </summary>
public sealed class BridgeRouter
{
    public delegate JsonObject? CommandHandler(JsonObject? payload);

    private readonly Dictionary<string, CommandHandler> _handlers = new(StringComparer.Ordinal);
    private readonly ILogger _log;
    private readonly object _gate = new();

    public BridgeRouter(ILogger<BridgeRouter>? logger = null)
    {
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public void Register(string commandName, CommandHandler handler)
    {
        if (string.IsNullOrEmpty(commandName)) throw new ArgumentException(null, nameof(commandName));
        if (handler is null) throw new ArgumentNullException(nameof(handler));
        lock (_gate) _handlers[commandName] = handler;
    }

    public bool Unregister(string commandName)
    {
        lock (_gate) return _handlers.Remove(commandName);
    }

    /// <summary>
    /// Dispatch <paramref name="message"/>. For commands, calls the registered
    /// handler and returns a reply envelope; for events / replies, returns null.
    /// Unknown commands return a reply with an `error` payload.
    /// </summary>
    public BridgeMessage? Dispatch(BridgeMessage message)
    {
        if (message.Type != BridgeMessage.TypeCommand) return null;

        CommandHandler? handler;
        lock (_gate) _handlers.TryGetValue(message.Name, out handler);

        if (handler is null)
        {
            _log.LogWarning("[Bridge] unknown command: {Name}", message.Name);
            return new BridgeMessage(
                BridgeMessage.TypeReply,
                message.Name,
                new JsonObject { ["error"] = "unknown_command" });
        }

        try
        {
            var reply = handler(message.Payload);
            return new BridgeMessage(BridgeMessage.TypeReply, message.Name, reply);
        }
        catch (Exception ex)
        {
            _log.LogError(ex, "[Bridge] handler threw for {Name}", message.Name);
            return new BridgeMessage(
                BridgeMessage.TypeReply,
                message.Name,
                new JsonObject { ["error"] = "handler_exception", ["message"] = ex.Message });
        }
    }

    public IReadOnlyCollection<string> RegisteredCommands
    {
        get { lock (_gate) return _handlers.Keys.ToArray(); }
    }
}
