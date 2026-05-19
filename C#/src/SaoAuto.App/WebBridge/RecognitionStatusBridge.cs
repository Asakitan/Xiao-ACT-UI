using System.Text.Json.Nodes;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S168 — Registers <c>recognition.start</c>, <c>recognition.stop</c>,
/// and <c>recognition.status</c> on a <see cref="BridgeRouter"/>,
/// backed by delegates so the bridge stays decoupled from the
/// recognition lifetime. Start/Stop are best-effort: the runner site
/// can wire them to <see cref="Startup.RecognitionLifecycle"/> restart
/// hooks once those exist, but today only <c>status</c> has a real
/// consumer (the HUD checking whether vision is alive).
///
/// Reply shapes:
/// <list type="bullet">
/// <item><c>recognition.status</c> → <c>{active:bool}</c>.</item>
/// <item><c>recognition.start</c> / <c>recognition.stop</c> →
///   <c>{ok:bool}</c> when a hook is supplied, otherwise
///   <c>{error:"unsupported"}</c>. Status reflects the lifecycle's
///   reported state *after* the hook ran.</item>
/// </list>
/// </summary>
public sealed class RecognitionStatusBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly Func<bool> _isActive;
    private readonly Action? _start;
    private readonly Action? _stop;
    private bool _disposed;

    public RecognitionStatusBridge(
        BridgeRouter router,
        Func<bool> isActive,
        Action? start = null,
        Action? stop = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _isActive = isActive ?? throw new ArgumentNullException(nameof(isActive));
        _start = start;
        _stop = stop;
        router.Register(BridgeCommands.RecognitionStatus, _ => HandleStatus());
        router.Register(BridgeCommands.StartRecognition, _ => HandleStart());
        router.Register(BridgeCommands.StopRecognition, _ => HandleStop());
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _router.Unregister(BridgeCommands.RecognitionStatus);
        _router.Unregister(BridgeCommands.StartRecognition);
        _router.Unregister(BridgeCommands.StopRecognition);
    }

    private JsonObject HandleStatus()
        => new() { ["active"] = _isActive() };

    private JsonObject HandleStart()
    {
        if (_start is null)
            return new JsonObject { ["error"] = "unsupported", ["active"] = _isActive() };
        _start();
        return new JsonObject { ["ok"] = true, ["active"] = _isActive() };
    }

    private JsonObject HandleStop()
    {
        if (_stop is null)
            return new JsonObject { ["error"] = "unsupported", ["active"] = _isActive() };
        _stop();
        return new JsonObject { ["ok"] = true, ["active"] = _isActive() };
    }
}
