using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Bridges small HUD/menu settings that legacy pywebview pages write directly.
/// These are real persisted settings, not UI ACKs: menu.html calls them when
/// users change watched skill slots or the Burst alert toggle.
/// </summary>
public sealed class HudSettingsBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly SettingsManager _settings;
    private readonly string[] _commands;
    private bool _disposed;

    public HudSettingsBridge(BridgeRouter router, SettingsManager settings)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _commands = new[]
        {
            BridgeCommands.SetWatchedSlots,
            BridgeCommands.SetBurstEnabled,
        };
        _router.Register(BridgeCommands.SetWatchedSlots, HandleWatchedSlots);
        _router.Register(BridgeCommands.SetBurstEnabled, HandleBurstEnabled);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var command in _commands)
            _router.Unregister(command);
    }

    private JsonObject HandleWatchedSlots(JsonObject? payload)
    {
        var raw = payload?["slots"] as JsonArray;
        var normalized = WatchedSkillSlotsLoader.Normalize(raw).ToArray();
        _settings.Set(SettingsKeys.WatchedSkillSlots, normalized);
        _settings.Save();

        var slots = new JsonArray();
        foreach (var slot in normalized)
            slots.Add(slot);
        return new JsonObject
        {
            ["ok"] = true,
            ["slots"] = slots,
        };
    }

    private JsonObject HandleBurstEnabled(JsonObject? payload)
    {
        bool enabled;
        try
        {
            enabled = payload?["enabled"]?.GetValue<bool>() ?? false;
        }
        catch
        {
            return new JsonObject { ["ok"] = false, ["error"] = "bad_payload" };
        }

        _settings.Set(SettingsKeys.BurstEnabled, enabled);
        _settings.Save();
        return new JsonObject
        {
            ["ok"] = true,
            ["enabled"] = enabled,
        };
    }
}
