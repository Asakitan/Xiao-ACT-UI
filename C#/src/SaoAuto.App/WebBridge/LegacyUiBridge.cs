using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S193 — Stub responder for the pywebview-shim's <c>ui.*</c>
/// commands. Pages built against the Python panel call hit-region,
/// context-menu, drag, panel-visibility, menu-action, and exit APIs; we
/// acknowledge them so the page's promise chains complete, log the payload
/// at debug level for future wiring, and route exit aliases to the supplied
/// shutdown action. A future session can plug each non-exit command into a
/// real native handler without touching the JS shim.
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
            BridgeCommands.ToggleMenu,
            BridgeCommands.ContextAction,
            BridgeCommands.MenuAction,
            BridgeCommands.WindowDrag,
            BridgeCommands.SetCtxMenuActive,
            BridgeCommands.ClosePanel,
            BridgeCommands.PanelAction,
        };
        router.Register(BridgeCommands.SetHitRegions, p => Ack("set_hit_regions", p));
        router.Register(BridgeCommands.NotifyHpHitRegionsReady, p => Ack("notify_hp_hit_regions_ready", p));
        router.Register(BridgeCommands.ExitApplication, HandleExit);
        router.Register(BridgeCommands.SetPanelVisible, p => Ack("set_panel_visible", p));
        router.Register(BridgeCommands.ToggleMenu, p => Ack("toggle_menu", p));
        router.Register(BridgeCommands.ContextAction, p => HandleAction("context_action", p));
        router.Register(BridgeCommands.MenuAction, p => HandleAction("menu_action", p));
        router.Register(BridgeCommands.WindowDrag, p => Ack("window_drag", p));
        router.Register(BridgeCommands.SetCtxMenuActive, p => Ack("set_ctx_menu_active", p));
        router.Register(BridgeCommands.ClosePanel, p => Ack("close_panel", p));
        router.Register(BridgeCommands.PanelAction, p => Ack("panel_action", p));
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

    private JsonObject HandleAction(string command, JsonObject? payload)
    {
        var action = payload?["action"]?.GetValue<string>() ?? string.Empty;
        if (string.Equals(action, "exit", StringComparison.Ordinal))
        {
            return HandleExit(payload);
        }
        return Ack(command, payload);
    }
}
