using System.Text.Json.Nodes;
using System.Text.Json;
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
            BridgeCommands.SetBossBarMode,
            BridgeCommands.SetDpsFadeTimeout,
            BridgeCommands.SetDataSource,
            BridgeCommands.SetComponentSource,
            BridgeCommands.SaveAutoKeyActions,
        };
        _router.Register(BridgeCommands.SetWatchedSlots, HandleWatchedSlots);
        _router.Register(BridgeCommands.SetBurstEnabled, HandleBurstEnabled);
        _router.Register(BridgeCommands.SetBossBarMode, HandleBossBarMode);
        _router.Register(BridgeCommands.SetDpsFadeTimeout, HandleDpsFadeTimeout);
        _router.Register(BridgeCommands.SetDataSource, HandleDataSource);
        _router.Register(BridgeCommands.SetComponentSource, HandleComponentSource);
        _router.Register(BridgeCommands.SaveAutoKeyActions, HandleSaveAutoKeyActions);
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

    private JsonObject HandleBossBarMode(JsonObject? payload)
    {
        var mode = ReadString(payload?["mode"], "boss_raid").Trim();
        if (mode is not ("always" or "boss_raid" or "off"))
            mode = "boss_raid";

        _settings.Set(SettingsKeys.BossBarMode, mode);
        _settings.Save();
        return new JsonObject
        {
            ["ok"] = true,
            ["mode"] = mode,
        };
    }

    private JsonObject HandleDpsFadeTimeout(JsonObject? payload)
    {
        var node = payload?["seconds"] ?? payload?["timeout"];
        if (!TryGetInt(node, out var rawSeconds))
            return new JsonObject { ["ok"] = false, ["error"] = "bad_payload" };

        var seconds = Math.Max(0, rawSeconds);
        _settings.Set(SettingsKeys.DpsFadeTimeoutSeconds, seconds);
        _settings.Save();
        return new JsonObject
        {
            ["ok"] = true,
            ["timeout"] = seconds,
            ["seconds"] = seconds,
        };
    }

    private JsonObject HandleDataSource(JsonObject? payload)
    {
        var mode = NormalizeDataSourceMode(ReadString(payload?["mode"], "tcp"));
        _settings.Set(SettingsKeys.MemDataSource, mode);
        _settings.Save();
        return new JsonObject
        {
            ["ok"] = true,
            ["mode"] = mode,
            ["live_reconfigured"] = false,
        };
    }

    private static JsonObject HandleComponentSource(JsonObject? payload)
    {
        var component = ReadString(payload?["component"], string.Empty).Trim();
        var mode = ReadString(payload?["mode"], string.Empty).Trim().ToLowerInvariant();
        return new JsonObject
        {
            ["ok"] = true,
            ["component"] = component,
            ["mode"] = mode,
            ["legacy_noop"] = true,
        };
    }

    private JsonObject HandleSaveAutoKeyActions(JsonObject? payload)
    {
        var actions = ParseActions(payload);
        if (actions is null)
            return new JsonObject { ["ok"] = false, ["error"] = "bad_payload" };

        _settings.Set<JsonNode>("autokey_burst_actions", actions.DeepClone());
        _settings.Save();
        return new JsonObject
        {
            ["ok"] = true,
            ["saved_count"] = actions.Count,
            ["live_reconfigured"] = false,
        };
    }

    private static string NormalizeDataSourceMode(string mode)
    {
        var normalized = (mode ?? string.Empty).Trim().ToLowerInvariant();
        return normalized is "tcp" or "memory" or "hybrid" or "auto" ? normalized : "hybrid";
    }

    private static JsonArray? ParseActions(JsonObject? payload)
    {
        if (payload is null) return new JsonArray();
        if (payload["actions"] is JsonArray arr)
            return (JsonArray)arr.DeepClone();
        var json = ReadString(payload["actions_json"], string.Empty);
        if (string.IsNullOrWhiteSpace(json))
            return new JsonArray();
        try
        {
            return JsonNode.Parse(json) as JsonArray;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private static string ReadString(JsonNode? node, string fallback)
    {
        if (node is null) return fallback;
        if (node is JsonValue jsonValue && jsonValue.TryGetValue<string>(out var text))
            return text;
        return node.ToString();
    }

    private static bool TryGetInt(JsonNode? node, out int value)
    {
        value = 0;
        if (node is not JsonValue jsonValue) return false;
        if (jsonValue.TryGetValue<int>(out value)) return true;
        if (jsonValue.TryGetValue<double>(out var dbl))
        {
            value = (int)dbl;
            return true;
        }
        if (jsonValue.TryGetValue<string>(out var text)
            && int.TryParse(text, System.Globalization.NumberStyles.Integer,
                System.Globalization.CultureInfo.InvariantCulture, out value))
        {
            return true;
        }
        return false;
    }
}
