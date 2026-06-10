using System.Text.Json.Nodes;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Registers ACT platform commands used by the shared HTML panels when they
/// run inside the native WebView2 host. The Python host already serves these
/// through pywebview; C# can attach a backend delegate later, while today's
/// no-delegate path returns structured <c>{error:"act_unavailable"}</c>
/// instead of letting pages hit <c>unknown_command</c>.
/// </summary>
public sealed class ActBridge : IDisposable
{
    public delegate JsonObject? ActCommandHandler(string commandName, JsonObject? payload);

    private static readonly string[] s_commandNames =
    [
        BridgeCommands.ActSourcesHealth,
        BridgeCommands.ActSourcesDiagnose,
        BridgeCommands.ActSourcesCopy,
        BridgeCommands.ActReportStatus,
        BridgeCommands.ActReportExport,
        BridgeCommands.ActReportCopy,
        BridgeCommands.ActMiniParseStatus,
        BridgeCommands.ActMiniParsePreview,
        BridgeCommands.ActMiniParseCopy,
        BridgeCommands.ActSelectiveParsingStatus,
        BridgeCommands.ActSelectiveParsingUpdate,
        BridgeCommands.ActSelectiveParsingClear,
        BridgeCommands.ActHistoryStatus,
        BridgeCommands.ActHistoryLoad,
        BridgeCommands.ActHistoryDelete,
        BridgeCommands.ActHistoryClear,
        BridgeCommands.ActOfflineImportChooseFile,
        BridgeCommands.ActOfflineImportImport,
        BridgeCommands.ActOfflineImportStatus,
        BridgeCommands.ActTimelineStatus,
        BridgeCommands.ActTimelinePlay,
        BridgeCommands.ActTimelinePause,
        BridgeCommands.ActTimelineStep,
        BridgeCommands.ActTimelineSeek,
        BridgeCommands.ActTimelineSpeed,
        BridgeCommands.ActTimelineFilter,
        BridgeCommands.ActAggregateStatus,
        BridgeCommands.ActMemScopeStatus,
        BridgeCommands.ActMemScopeSearch,
        BridgeCommands.ActMemScopeSearchStatus,
        BridgeCommands.ActMemScopeNarrow,
        BridgeCommands.ActMemScopeCancel,
        BridgeCommands.ActMemScopeAttrMap,
        BridgeCommands.ActActionLogStatus,
        BridgeCommands.ActActionLogSearch,
        BridgeCommands.ActActionLogFilter,
        BridgeCommands.ActActionLogJumpToTime,
        BridgeCommands.ActActionLogCopy,
        BridgeCommands.ActDeathRecapStatus,
        BridgeCommands.ActDeathRecapCopy,
        BridgeCommands.ActGraphStatus,
        BridgeCommands.ActGraphSelectMetric,
        BridgeCommands.ActGraphZoom,
        BridgeCommands.ActGraphFilter,
        BridgeCommands.ActGraphExport,
        BridgeCommands.ActCombatantStatus,
        BridgeCommands.ActCombatantFilter,
        BridgeCommands.ActCombatantFocusTarget,
        BridgeCommands.ActCombatantBack,
        BridgeCommands.ActSkillStatus,
        BridgeCommands.ActSkillFilter,
        BridgeCommands.ActSkillCopy,
        BridgeCommands.ActSkillBack,
        BridgeCommands.ActPluginsStatus,
        BridgeCommands.ActPluginsList,
        BridgeCommands.ActPluginsEnable,
        BridgeCommands.ActPluginsDisable,
        BridgeCommands.ActPluginsReload,
        BridgeCommands.ActPluginsPin,
        BridgeCommands.ActPluginsImportDialog,
        BridgeCommands.ActPluginsImport,
        BridgeCommands.ActPluginsUninstall,
        BridgeCommands.ActPluginsHotkeys,
        BridgeCommands.ActPluginsSetHotkey,
        BridgeCommands.ActPluginsUiPanels,
        BridgeCommands.ActPluginsRenderUiPanel,
        BridgeCommands.ActPluginsInvokeUiAction,
        BridgeCommands.ActRenderSurfaces,
        BridgeCommands.ActRenderApplyHooks,
        BridgeCommands.ActRenderOverlays,
        BridgeCommands.ActTriggersStatus,
        BridgeCommands.ActTriggersEnable,
        BridgeCommands.ActTriggersDisable,
        BridgeCommands.ActTriggersReload,
        BridgeCommands.ActTriggersTest,
    ];

    private readonly BridgeRouter _router;
    private readonly ActCommandHandler? _handler;
    private bool _disposed;

    public ActBridge(BridgeRouter router, ActCommandHandler? handler = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _handler = handler;
        foreach (var commandName in s_commandNames)
        {
            _router.Register(commandName, payload => Handle(commandName, payload));
        }
    }

    public static IReadOnlyCollection<string> CommandNames => s_commandNames;

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var commandName in s_commandNames)
        {
            _router.Unregister(commandName);
        }
    }

    private JsonObject Handle(string commandName, JsonObject? payload)
    {
        if (_handler is null)
        {
            return Unavailable(commandName);
        }

        return _handler(commandName, payload) ?? new JsonObject { ["ok"] = true };
    }

    private static JsonObject Unavailable(string commandName)
    {
        return new JsonObject
        {
            ["ok"] = false,
            ["error"] = "act_unavailable",
            ["command"] = commandName,
            ["surface"] = SurfaceFromCommand(commandName),
            ["message"] = "Native ACT backend is not attached for this WebView2 host.",
        };
    }

    private static string SurfaceFromCommand(string commandName)
    {
        const string Prefix = "act.";
        var rest = commandName.StartsWith(Prefix, StringComparison.Ordinal)
            ? commandName[Prefix.Length..]
            : commandName;
        var dot = rest.IndexOf('.');
        return dot > 0 ? rest[..dot] : rest;
    }
}
