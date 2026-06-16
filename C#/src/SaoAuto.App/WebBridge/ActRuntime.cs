using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Minimal native ACT backend for the WebView2 host. It mirrors Python 5.0.0's
/// ACT command envelope with safe empty states until the full plugin/event
/// runtime is ported into C#.
/// </summary>
public sealed class ActRuntime
{
    private readonly SettingsManager _settings;
    private readonly GameStateManager? _states;

    public ActRuntime(SettingsManager settings, GameStateManager? states = null)
    {
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _states = states;
    }

    public JsonObject Handle(string commandName, JsonObject? payload)
        => commandName switch
        {
            BridgeCommands.ActSourcesHealth => SourcesHealth(),
            BridgeCommands.ActSourcesDiagnose => SourcesDiagnose(),
            BridgeCommands.ActSourcesCopy => CopyPayload(commandName, SourcesDiagnose()),

            BridgeCommands.ActPluginsStatus => PluginStatus(),
            BridgeCommands.ActPluginsList => PluginStatus(),
            BridgeCommands.ActPluginsHotkeys => PluginHotkeys(),
            BridgeCommands.ActPluginsUiPanels => PluginUiPanels(),
            BridgeCommands.ActPluginsRenderUiPanel => RenderUiPanel(payload),
            BridgeCommands.ActPluginsInvokeUiAction => InvokeUiAction(payload),
            BridgeCommands.ActPluginsEnable or
            BridgeCommands.ActPluginsDisable or
            BridgeCommands.ActPluginsReload or
            BridgeCommands.ActPluginsPin or
            BridgeCommands.ActPluginsImportDialog or
            BridgeCommands.ActPluginsImport or
            BridgeCommands.ActPluginsUninstall or
            BridgeCommands.ActPluginsSetHotkey => Unsupported(commandName, payload, "ACT plugin management is not implemented in the C# host yet."),

            BridgeCommands.ActRenderSurfaces => RenderSurfaces(),
            BridgeCommands.ActRenderOverlays => RenderOverlays(payload),
            BridgeCommands.ActRenderApplyHooks => RenderApplyHooks(payload),

            BridgeCommands.ActMiniParseStatus => MiniParseStatus(payload),
            BridgeCommands.ActMiniParsePreview => MiniParseStatus(payload),
            BridgeCommands.ActMiniParseCopy => CopyPayload(commandName, MiniParseStatus(payload)),
            BridgeCommands.ActReportStatus => EmptyStatus(commandName, "report", new JsonObject { ["reports"] = new JsonArray() }),
            BridgeCommands.ActReportExport => Unsupported(commandName, payload, "ACT report export is not implemented in the C# host yet."),
            BridgeCommands.ActReportCopy => CopyPayload(commandName, EmptyStatus(commandName, "report")),

            BridgeCommands.ActTimelineStatus => EmptyStatus(commandName, "timeline", new JsonObject
            {
                ["events"] = new JsonArray(),
                ["playing"] = false,
                ["speed"] = 1.0,
            }),
            BridgeCommands.ActTimelinePlay or
            BridgeCommands.ActTimelinePause or
            BridgeCommands.ActTimelineStep or
            BridgeCommands.ActTimelineSeek or
            BridgeCommands.ActTimelineSpeed or
            BridgeCommands.ActTimelineFilter => EmptyStatus(commandName, "timeline"),

            BridgeCommands.ActTriggersStatus => EmptyStatus(commandName, "triggers", new JsonObject { ["rules"] = new JsonArray() }),
            BridgeCommands.ActTriggersEnable or
            BridgeCommands.ActTriggersDisable or
            BridgeCommands.ActTriggersReload or
            BridgeCommands.ActTriggersTest => Unsupported(commandName, payload, "ACT trigger runtime is not implemented in the C# host yet."),

            BridgeCommands.ActHistoryStatus or
            BridgeCommands.ActHistoryLoad => EmptyStatus(commandName, "history", new JsonObject { ["entries"] = new JsonArray() }),
            BridgeCommands.ActHistoryDelete or
            BridgeCommands.ActHistoryClear => EmptyStatus(commandName, "history"),

            BridgeCommands.ActOfflineImportStatus => EmptyStatus(commandName, "offline_import", new JsonObject { ["files"] = new JsonArray() }),
            BridgeCommands.ActOfflineImportChooseFile or
            BridgeCommands.ActOfflineImportImport => Unsupported(commandName, payload, "ACT offline import is not implemented in the C# host yet."),

            BridgeCommands.ActSelectiveParsingStatus => EmptyStatus(commandName, "selective_parsing", new JsonObject { ["policy"] = new JsonObject() }),
            BridgeCommands.ActSelectiveParsingUpdate or
            BridgeCommands.ActSelectiveParsingClear => EmptyStatus(commandName, "selective_parsing"),

            BridgeCommands.ActAggregateStatus => EmptyStatus(commandName, "aggregate", new JsonObject { ["rows"] = new JsonArray() }),
            BridgeCommands.ActMemScopeStatus or
            BridgeCommands.ActMemScopeSearchStatus => EmptyStatus(commandName, "mem_scope", new JsonObject { ["results"] = new JsonArray() }),
            BridgeCommands.ActMemScopeSearch or
            BridgeCommands.ActMemScopeNarrow or
            BridgeCommands.ActMemScopeCancel or
            BridgeCommands.ActMemScopeAttrMap => Unsupported(commandName, payload, "ACT memory scope is not implemented in the C# host yet."),

            BridgeCommands.ActActionLogStatus or
            BridgeCommands.ActActionLogSearch or
            BridgeCommands.ActActionLogFilter => EmptyStatus(commandName, "action_log", new JsonObject { ["events"] = new JsonArray() }),
            BridgeCommands.ActActionLogJumpToTime or
            BridgeCommands.ActActionLogCopy => EmptyStatus(commandName, "action_log"),

            BridgeCommands.ActDeathRecapStatus => EmptyStatus(commandName, "death_recap", new JsonObject { ["events"] = new JsonArray() }),
            BridgeCommands.ActDeathRecapCopy => CopyPayload(commandName, EmptyStatus(commandName, "death_recap")),

            BridgeCommands.ActGraphStatus or
            BridgeCommands.ActGraphSelectMetric or
            BridgeCommands.ActGraphZoom or
            BridgeCommands.ActGraphFilter => EmptyStatus(commandName, "graph", new JsonObject { ["series"] = new JsonArray() }),
            BridgeCommands.ActGraphExport => Unsupported(commandName, payload, "ACT graph export is not implemented in the C# host yet."),

            BridgeCommands.ActCombatantStatus or
            BridgeCommands.ActCombatantFilter => EmptyStatus(commandName, "combatant", new JsonObject { ["combatants"] = new JsonArray() }),
            BridgeCommands.ActCombatantFocusTarget or
            BridgeCommands.ActCombatantBack => EmptyStatus(commandName, "combatant"),

            BridgeCommands.ActSkillStatus or
            BridgeCommands.ActSkillFilter => EmptyStatus(commandName, "skill", new JsonObject { ["skills"] = new JsonArray() }),
            BridgeCommands.ActSkillCopy or
            BridgeCommands.ActSkillBack or
            BridgeCommands.ActSkillOpen => EmptyStatus(commandName, "skill"),

            _ => Unsupported(commandName, payload, "Unknown ACT command for the C# native runtime."),
        };

    private JsonObject SourcesHealth() => new()
    {
        ["ok"] = true,
        ["source"] = "csharp-native",
        ["sources"] = new JsonArray
        {
            new JsonObject
            {
                ["id"] = "csharp_webview",
                ["title"] = "C# WebView2 host",
                ["status"] = "ok",
            },
            new JsonObject
            {
                ["id"] = "plugin_runtime",
                ["title"] = "ACT plugin runtime",
                ["status"] = "empty",
                ["message"] = "Full ACT plugin execution is not implemented in the C# host yet.",
            },
        },
        ["warnings"] = new JsonArray(),
    };

    private JsonObject SourcesDiagnose() => new()
    {
        ["ok"] = true,
        ["source"] = "csharp-native",
        ["diagnostics"] = new JsonArray(),
        ["settings_path"] = _settings.Path,
        ["identity"] = Identity(),
        ["message"] = "C# ACT runtime is attached with safe empty states; full plugin execution is pending.",
    };

    private JsonObject PluginStatus() => new()
    {
        ["ok"] = true,
        ["source"] = "csharp-native",
        ["plugins"] = new JsonArray(),
        ["plugin_count"] = 0,
        ["active_count"] = 0,
        ["loaded_count"] = 0,
        ["enabled_count"] = 0,
        ["ui_panels"] = new JsonArray(),
        ["event_bus"] = EmptyEventBus(),
        ["errors"] = new JsonArray(),
    };

    private static JsonObject PluginHotkeys() => new()
    {
        ["ok"] = true,
        ["hotkeys"] = new JsonArray(),
        ["occupied"] = new JsonObject(),
    };

    private static JsonObject PluginUiPanels() => new()
    {
        ["ok"] = true,
        ["panels"] = new JsonArray(),
    };

    private static JsonObject RenderUiPanel(JsonObject? payload)
    {
        var panelId = ReadString(payload, "panel_id");
        return new JsonObject
        {
            ["ok"] = false,
            ["panel_id"] = panelId,
            ["message"] = string.IsNullOrWhiteSpace(panelId)
                ? "ui panel not found"
                : $"ui panel not found: {panelId}",
            ["spec"] = EmptyUiSpec(),
            ["errors"] = new JsonArray { "panel not found" },
        };
    }

    private static JsonObject InvokeUiAction(JsonObject? payload) => new()
    {
        ["ok"] = false,
        ["panel_id"] = ReadString(payload, "panel_id"),
        ["action_id"] = ReadString(payload, "action_id"),
        ["message"] = "ui panel not found",
        ["errors"] = new JsonArray { "panel not found" },
    };

    private static JsonObject RenderSurfaces() => new()
    {
        ["ok"] = true,
        ["surfaces"] = new JsonObject(),
        ["overlays"] = new JsonObject(),
        ["hooks"] = new JsonObject(),
    };

    private static JsonObject RenderOverlays(JsonObject? payload) => new()
    {
        ["ok"] = true,
        ["surface"] = ReadString(payload, "surface"),
        ["overlays"] = new JsonArray(),
    };

    private static JsonObject RenderApplyHooks(JsonObject? payload) => new()
    {
        ["ok"] = true,
        ["surface"] = ReadString(payload, "surface"),
        ["payload"] = CoercePayload(payload),
        ["applied"] = new JsonArray(),
        ["errors"] = new JsonArray(),
    };

    private static JsonObject MiniParseStatus(JsonObject? payload) => new()
    {
        ["ok"] = true,
        ["formatter_id"] = string.IsNullOrWhiteSpace(ReadString(payload, "formatter_id"))
            ? "summary_table"
            : ReadString(payload, "formatter_id"),
        ["rows"] = new JsonArray(),
        ["preview"] = string.Empty,
        ["text"] = string.Empty,
        ["event_count"] = 0,
    };

    private static JsonObject EmptyStatus(string commandName, string surface, JsonObject? extra = null)
    {
        var result = new JsonObject
        {
            ["ok"] = true,
            ["command"] = commandName,
            ["surface"] = surface,
            ["empty"] = true,
        };
        if (extra is not null)
        {
            foreach (var (key, value) in extra)
                result[key] = CloneNode(value);
        }
        return result;
    }

    private static JsonObject CopyPayload(string commandName, JsonObject source) => new()
    {
        ["ok"] = true,
        ["command"] = commandName,
        ["text"] = source.ToJsonString(new JsonSerializerOptions { WriteIndented = true }),
    };

    private static JsonObject Unsupported(string commandName, JsonObject? payload, string message) => new()
    {
        ["ok"] = false,
        ["error"] = "unsupported",
        ["command"] = commandName,
        ["message"] = message,
        ["payload"] = CloneNode(payload) ?? new JsonObject(),
    };

    private static JsonObject EmptyEventBus() => new()
    {
        ["topics"] = new JsonArray(),
        ["topic_count"] = 0,
        ["subscriber_count"] = 0,
        ["subscriptions"] = 0,
    };

    private static JsonObject EmptyUiSpec() => new()
    {
        ["version"] = 1,
        ["title"] = string.Empty,
        ["nodes"] = new JsonArray(),
    };

    private JsonObject Identity()
    {
        var snapshot = _states?.Snapshot;
        return new JsonObject
        {
            ["player_uid"] = snapshot?.PlayerId ?? string.Empty,
            ["player_name"] = snapshot?.PlayerName ?? string.Empty,
            ["profession_id"] = snapshot?.ProfessionId ?? 0,
            ["profession_name"] = snapshot?.ProfessionName ?? string.Empty,
            ["source"] = snapshot is null ? "profile" : "packet",
        };
    }

    private static string ReadString(JsonObject? payload, string key)
    {
        if (payload is null || !payload.TryGetPropertyValue(key, out var node) || node is null)
            return string.Empty;
        try
        {
            return node.GetValue<string>() ?? string.Empty;
        }
        catch
        {
            return node.ToString();
        }
    }

    private static JsonNode CoercePayload(JsonObject? payload)
    {
        if (payload is null || !payload.TryGetPropertyValue("payload", out var node) || node is null)
            return new JsonObject();
        if (node is JsonValue value)
        {
            try
            {
                var text = value.GetValue<string>();
                if (!string.IsNullOrWhiteSpace(text))
                {
                    try { return JsonNode.Parse(text) ?? new JsonObject(); }
                    catch { return JsonValue.Create(text)!; }
                }
            }
            catch
            {
                // Fall through to clone.
            }
        }
        return CloneNode(node) ?? new JsonObject();
    }

    private static JsonNode? CloneNode(JsonNode? node)
        => node is null ? null : JsonNode.Parse(node.ToJsonString());
}