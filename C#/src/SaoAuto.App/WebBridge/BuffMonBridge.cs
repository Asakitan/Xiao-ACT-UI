using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S166 — Registers <c>buffmon.set_enabled</c> / <c>buffmon.get_enabled</c>
/// against a <see cref="BridgeRouter"/>, backed by
/// <see cref="SettingsKeys.BuffMonEnabled"/> on a shared
/// <see cref="SettingsManager"/>. The flag survives restarts because
/// every Set writes through to disk; the BuffMon runtime can read the
/// same key on its next tick to honour the user's preference. Dispose
/// unregisters both commands, restoring the router to its pre-attach
/// state.
///
/// Reply shapes:
/// <list type="bullet">
/// <item><c>buffmon.set_enabled</c> ← <c>{enabled:bool}</c> → <c>{enabled:bool, changed:bool}</c></item>
/// <item><c>buffmon.get_enabled</c> ← (no payload) → <c>{enabled:bool}</c></item>
/// </list>
/// </summary>
public sealed class BuffMonBridge : IDisposable
{
    private readonly SettingsManager _settings;
    private readonly BridgeRouter _router;
    private bool _disposed;

    public BuffMonBridge(SettingsManager settings, BridgeRouter router)
    {
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _router = router ?? throw new ArgumentNullException(nameof(router));
        router.Register(BridgeCommands.SetBuffMonEnabled, HandleSet);
        router.Register(BridgeCommands.GetBuffMonEnabled, _ => HandleGet());
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _router.Unregister(BridgeCommands.SetBuffMonEnabled);
        _router.Unregister(BridgeCommands.GetBuffMonEnabled);
    }

    private JsonObject HandleSet(JsonObject? payload)
    {
        var requested = payload?["enabled"]?.GetValue<bool>() ?? false;
        var before = _settings.GetBool(SettingsKeys.BuffMonEnabled);
        if (before != requested)
        {
            _settings.Set(SettingsKeys.BuffMonEnabled, requested);
            _settings.Save();
        }
        return new JsonObject
        {
            ["enabled"] = requested,
            ["changed"] = before != requested,
        };
    }

    private JsonObject HandleGet()
        => new() { ["enabled"] = _settings.GetBool(SettingsKeys.BuffMonEnabled) };
}
