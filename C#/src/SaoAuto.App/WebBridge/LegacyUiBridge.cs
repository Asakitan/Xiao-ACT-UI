using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S193 — Stub responder for the pywebview-shim's <c>ui.*</c>
/// commands. Pages built against the Python panel call hit-region /
/// panel-visibility / exit-application APIs; we acknowledge them so
/// the page's promise chains complete, log the payload at debug
/// level for future wiring, and that's it. None of these have a real
/// C# consumer yet (no native click-through, no Tk-style panel
/// visibility flag); a future session can plug each into a real
/// handler without touching the JS shim.
///
/// Reply shape: <c>{ok:bool, command:string}</c>. <c>ui.exit</c>
/// returns immediately and schedules an <c>Application.Shutdown</c>
/// on the dispatcher so the JS promise resolves before the process
/// dies.
/// </summary>
public sealed class LegacyUiBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly ILogger _log;
    private readonly Action? _exitAction;
    private readonly string[] _commands;
    private bool _disposed;

    public LegacyUiBridge(BridgeRouter router, ILogger? logger = null, Action? exitAction = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _log = (ILogger?)logger ?? NullLogger.Instance;
        _exitAction = exitAction;
        _commands = new[]
        {
            BridgeCommands.SetHitRegions,
            BridgeCommands.NotifyHpHitRegionsReady,
            BridgeCommands.ExitApplication,
            BridgeCommands.SetPanelVisible,
        };
        router.Register(BridgeCommands.SetHitRegions, p => Ack("set_hit_regions", p));
        router.Register(BridgeCommands.NotifyHpHitRegionsReady, p => Ack("notify_hp_hit_regions_ready", p));
        router.Register(BridgeCommands.ExitApplication, HandleExit);
        router.Register(BridgeCommands.SetPanelVisible, p => Ack("set_panel_visible", p));
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var name in _commands)
            _router.Unregister(name);
    }

    private JsonObject Ack(string command, JsonObject? payload)
    {
        _log.LogDebug("[LegacyUiBridge] {Command} payload={Payload}", command,
            payload?.ToJsonString() ?? "null");
        return new JsonObject { ["ok"] = true, ["command"] = command };
    }

    private JsonObject HandleExit(JsonObject? payload)
    {
        _log.LogInformation("[LegacyUiBridge] exit requested via bridge");
        _exitAction?.Invoke();
        return new JsonObject { ["ok"] = true, ["command"] = "exit" };
    }
}
