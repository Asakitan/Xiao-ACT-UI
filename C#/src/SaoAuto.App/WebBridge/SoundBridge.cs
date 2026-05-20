using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S193 — Routes the pywebview-shim's <c>sound.play</c> command into
/// the in-process <see cref="ISoundPlayer"/> + <see cref="SoundCatalog"/>.
///
/// Reply shape: <c>{played:bool, name:string, path?:string,
/// error?:string}</c>. <c>played:false</c> when the catalog can't
/// resolve the short name (typo, missing asset) or the player is
/// disabled. The bridge never throws; failures surface in the reply.
///
/// Idempotent. Dispose unregisters the command.
/// </summary>
public sealed class SoundBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly ISoundPlayer _player;
    private readonly SoundCatalog _catalog;
    private bool _disposed;

    public SoundBridge(BridgeRouter router, ISoundPlayer player, SoundCatalog catalog)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _player = player ?? throw new ArgumentNullException(nameof(player));
        _catalog = catalog ?? throw new ArgumentNullException(nameof(catalog));
        router.Register(BridgeCommands.PlaySound, HandlePlay);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _router.Unregister(BridgeCommands.PlaySound);
    }

    private JsonObject HandlePlay(JsonObject? payload)
    {
        var name = payload?["name"]?.GetValue<string>() ?? string.Empty;
        if (string.IsNullOrEmpty(name))
            return new JsonObject { ["played"] = false, ["error"] = "missing_name" };
        var path = _catalog.Resolve(name);
        if (string.IsNullOrEmpty(path))
            return new JsonObject { ["played"] = false, ["name"] = name, ["error"] = "unknown_clip" };
        if (!_player.Enabled)
            return new JsonObject { ["played"] = false, ["name"] = name, ["error"] = "disabled" };
        _player.Play(path);
        return new JsonObject { ["played"] = true, ["name"] = name, ["path"] = path };
    }
}
