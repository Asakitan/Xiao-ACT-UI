namespace SaoAuto.App.WebBridge;

/// <summary>
/// Canonical bridge command names. The JS layer sends these via
/// <c>postMessage({type:"command", name:"...", payload:{...}})</c>.
/// Mirrors `docs/ui-bridge-contract.md` (Session 1 deliverable).
/// </summary>
public static class BridgeCommands
{
    // Recognition
    public const string StartRecognition = "recognition.start";
    public const string StopRecognition = "recognition.stop";
    public const string RecognitionStatus = "recognition.status";

    // AutoKey
    public const string StartAutoKey = "autokey.start";
    public const string StopAutoKey = "autokey.stop";
    public const string ImportAutoKey = "autokey.import";
    public const string ExportAutoKey = "autokey.export";
    public const string OpenAutoKeyEditor = "autokey.editor.open";
    // S156 — profile CRUD (served by AutoKeyProfileBridge).
    public const string SetAutoKeyEnabled = "autokey.set_enabled";
    public const string ListAutoKeyProfiles = "autokey.profile.list";
    public const string SetAutoKeyActiveProfile = "autokey.profile.set_active";
    public const string DeleteAutoKeyProfile = "autokey.profile.delete";
    public const string CloneAutoKeyProfile = "autokey.profile.clone";
    public const string UpsertAutoKeyProfile = "autokey.profile.upsert";
    public const string ExportAutoKeyProfile = "autokey.profile.export";
    public const string ImportAutoKeyProfile = "autokey.profile.import";
    public const string GetAutoKeyState = "autokey.state.get";
    // S200 — file-picker entry used by menu.html's AutoKey import flow.
    public const string StartAutoKeyImportPicker = "autokey.import_picker.start";
    public const string SaveAutoKeyActions = "autokey.actions.save";
    // S177 — cloud script-share endpoints (served by AutoKeyCloudBridge).
    public const string SearchAutoKeyScripts = "autokey.cloud.search";
    public const string GetAutoKeyScript = "autokey.cloud.get";
    public const string IssueAutoKeyUploadToken = "autokey.cloud.issue_token";
    public const string UploadAutoKeyScript = "autokey.cloud.upload";
    public const string SetAutoKeyServerUrl = "autokey.cloud.set_server_url";

    // BossRaid
    public const string StartBossRaid = "bossraid.start";
    public const string StopBossRaid = "bossraid.stop";
    public const string ImportBossRaid = "bossraid.import";
    public const string ExportBossRaid = "bossraid.export";
    public const string OpenRaidEditor = "bossraid.editor.open";
    // S179 — cloud script-share endpoints (served by BossRaidCloudBridge).
    public const string SearchBossRaids = "bossraid.cloud.search";
    public const string GetBossRaid = "bossraid.cloud.get";
    public const string IssueBossRaidUploadToken = "bossraid.cloud.issue_token";
    public const string UploadBossRaid = "bossraid.cloud.upload";
    public const string SetBossRaidServerUrl = "bossraid.cloud.set_server_url";
    public const string GetBossRaidState = "bossraid.state.get";
    public const string RaidNextPhase = "bossraid.runtime.next_phase";
    public const string RaidReset = "bossraid.runtime.reset";

    // Commander
    public const string OpenCommander = "commander.open";
    public const string CloseCommander = "commander.close";
    public const string SyncCommanderState = "commander.sync";

    // DPS
    public const string ShowLastDpsReport = "dps.show_last_report";
    public const string ResetCombat = "dps.reset_combat";
    public const string DpsEntityDetail = "dps.entity_detail";

    /// <summary>R8 / DPS-04: toggle the DPS overlay master switch. Payload
    /// <c>{enabled:boolean}</c> writes <see cref="SettingsKeys.DpsEnabled"/>;
    /// passing no payload returns the current value. Mirrors Python's
    /// <c>_toggle_dps_enabled</c> at sao_gui_dps_theme_mixin.py:274-285.
    /// Reply shape: <c>{enabled:boolean}</c>.</summary>
    public const string DpsToggleEnabled = "dps.toggle_enabled";

    // Buff monitor
    public const string SetBuffMonEnabled = "buffmon.set_enabled";
    public const string GetBuffMonEnabled = "buffmon.get_enabled";

    // State pull (S190) — a HUD page that opens mid-run can ask for the
    // current snapshot instead of waiting for the next state.changed.
    public const string StateSnapshot = "state.snapshot";

    // Sound + UI surface (S193) — invoked by the pywebview compat shim
    // so legacy HUD HTML keeps working without rewrites.
    public const string PlaySound = "sound.play";
    public const string SetSoundEnabled = "sound.set_enabled";
    public const string SetSoundVolume = "sound.set_volume";
    public const string SetHitRegions = "ui.set_hit_regions";
    public const string NotifyHpHitRegionsReady = "ui.notify_hp_hit_regions_ready";
    public const string ExitApplication = "ui.exit";
    public const string SetPanelVisible = "ui.set_panel_visible";
    public const string GetPanelThemes = "ui.get_panel_themes";
    public const string SetPanelTheme = "ui.set_panel_theme";
    public const string ToggleMenu = "ui.toggle_menu";
    public const string ContextAction = "ui.context_action";
    public const string MenuAction = "ui.menu_action";
    public const string WindowDrag = "ui.window_drag";
    public const string SetCtxMenuActive = "ui.set_ctx_menu_active";
    public const string ClosePanel = "ui.close_panel";
    public const string PanelAction = "ui.panel_action";

    // Generic file browser for legacy pywebview menu pickers.
    public const string BrowseDir = "file.browse_dir";
    public const string SelectFile = "file.select_file";
    public const string SelectFolder = "file.select_folder";
    public const string BossHpHitRegions = "ui.boss_hp_hit_regions";

    // HUD settings surfaced by menu.html through the pywebview shim.
    public const string SetWatchedSlots = "settings.set_watched_slots";
    public const string SetBurstEnabled = "settings.set_burst_enabled";
    public const string SetBossBarMode = "settings.set_boss_bar_mode";
    public const string SetDpsFadeTimeout = "settings.set_dps_fade_timeout";
    public const string SetDataSource = "settings.set_data_source";
    public const string SetComponentSource = "settings.set_component_source";

    // Updater
    public const string CheckUpdate = "updater.check";
    public const string DownloadUpdate = "updater.download";
    public const string ApplyUpdate = "updater.apply";
    public const string SkipUpdate = "updater.skip";
    public const string FetchLeaderboard = "ui.fetch_leaderboard";

    // ACT platform commands used by pywebview-shim.js when hosted by
    // native WebView2 instead of Python pywebview.
    public const string ActSourcesHealth = "act.sources.health";
    public const string ActSourcesDiagnose = "act.sources.diagnose";
    public const string ActSourcesCopy = "act.sources.copy";

    public const string ActReportStatus = "act.report.status";
    public const string ActReportExport = "act.report.export";
    public const string ActReportCopy = "act.report.copy";

    public const string ActMiniParseStatus = "act.mini_parse.status";
    public const string ActMiniParsePreview = "act.mini_parse.preview";
    public const string ActMiniParseCopy = "act.mini_parse.copy";

    public const string ActSelectiveParsingStatus = "act.selective_parsing.status";
    public const string ActSelectiveParsingUpdate = "act.selective_parsing.update";
    public const string ActSelectiveParsingClear = "act.selective_parsing.clear";

    public const string ActHistoryStatus = "act.history.status";
    public const string ActHistoryLoad = "act.history.load";
    public const string ActHistoryDelete = "act.history.delete";
    public const string ActHistoryClear = "act.history.clear";

    public const string ActOfflineImportChooseFile = "act.offline_import.choose_file";
    public const string ActOfflineImportImport = "act.offline_import.import";
    public const string ActOfflineImportStatus = "act.offline_import.status";

    public const string ActTimelineStatus = "act.timeline.status";
    public const string ActTimelinePlay = "act.timeline.play";
    public const string ActTimelinePause = "act.timeline.pause";
    public const string ActTimelineStep = "act.timeline.step";
    public const string ActTimelineSeek = "act.timeline.seek";
    public const string ActTimelineSpeed = "act.timeline.speed";
    public const string ActTimelineFilter = "act.timeline.filter";

    public const string ActAggregateStatus = "act.aggregate.status";

    public const string ActMemScopeStatus = "act.mem_scope.status";
    public const string ActMemScopeSearch = "act.mem_scope.search";
    public const string ActMemScopeSearchStatus = "act.mem_scope.search_status";
    public const string ActMemScopeNarrow = "act.mem_scope.narrow";
    public const string ActMemScopeCancel = "act.mem_scope.cancel";
    public const string ActMemScopeAttrMap = "act.mem_scope.attr_map";

    public const string ActActionLogStatus = "act.action_log.status";
    public const string ActActionLogSearch = "act.action_log.search";
    public const string ActActionLogFilter = "act.action_log.filter";
    public const string ActActionLogJumpToTime = "act.action_log.jump_to_time";
    public const string ActActionLogCopy = "act.action_log.copy";

    public const string ActDeathRecapStatus = "act.death_recap.status";
    public const string ActDeathRecapCopy = "act.death_recap.copy";

    public const string ActGraphStatus = "act.graph.status";
    public const string ActGraphSelectMetric = "act.graph.select_metric";
    public const string ActGraphZoom = "act.graph.zoom";
    public const string ActGraphFilter = "act.graph.filter";
    public const string ActGraphExport = "act.graph.export";

    public const string ActCombatantStatus = "act.combatant.status";
    public const string ActCombatantFilter = "act.combatant.filter";
    public const string ActCombatantFocusTarget = "act.combatant.focus_target";
    public const string ActCombatantBack = "act.combatant.back";

    public const string ActSkillStatus = "act.skill.status";
    public const string ActSkillFilter = "act.skill.filter";
    public const string ActSkillCopy = "act.skill.copy";
    public const string ActSkillBack = "act.skill.back";
    public const string ActSkillOpen = "act.skill.open";

    public const string ActPluginsStatus = "act.plugins.status";
    public const string ActPluginsList = "act.plugins.list";
    public const string ActPluginsEnable = "act.plugins.enable";
    public const string ActPluginsDisable = "act.plugins.disable";
    public const string ActPluginsReload = "act.plugins.reload";
    public const string ActPluginsPin = "act.plugins.pin";
    public const string ActPluginsImportDialog = "act.plugins.import_dialog";
    public const string ActPluginsImport = "act.plugins.import";
    public const string ActPluginsUninstall = "act.plugins.uninstall";
    public const string ActPluginsHotkeys = "act.plugins.hotkeys";
    public const string ActPluginsSetHotkey = "act.plugins.set_hotkey";
    public const string ActPluginsUiPanels = "act.plugins.ui_panels";
    public const string ActPluginsRenderUiPanel = "act.plugins.render_ui_panel";
    public const string ActPluginsInvokeUiAction = "act.plugins.invoke_ui_action";

    public const string ActRenderSurfaces = "act.render.surfaces";
    public const string ActRenderApplyHooks = "act.render.apply_hooks";
    public const string ActRenderOverlays = "act.render.overlays";

    public const string ActTriggersStatus = "act.triggers.status";
    public const string ActTriggersEnable = "act.triggers.enable";
    public const string ActTriggersDisable = "act.triggers.disable";
    public const string ActTriggersReload = "act.triggers.reload";
    public const string ActTriggersTest = "act.triggers.test";
}

/// <summary>Canonical bridge event names emitted from C# to JS.</summary>
public static class BridgeEvents
{
    public const string GameStateChanged = "state.changed";
    public const string HealthChanged = "state.hp";
    public const string StaminaChanged = "state.stamina";
    public const string DpsSnapshot = "state.dps";

    /// <summary>R8 / DPS-07: emitted when the per-tick DPS pump detects an
    /// edge — show, fade-out, or visibility-toggle. Payload carries
    /// <c>action</c> ("show"|"fade_out"|"hide"), the live DPS snapshot
    /// (<c>total_damage</c>, <c>dps</c>, <c>total_heal</c>,
    /// <c>total_damage_boss</c>, <c>duration_s</c>), and an <c>enabled</c>
    /// flag mirroring <c>SettingsKeys.DpsEnabled</c>. Mirrors Python's
    /// <c>_show_dps_live_snapshot</c> / <c>_hide_dps_overlay</c> bridge
    /// events at sao_gui_dps_theme_mixin.py:227-285.</summary>
    public const string DpsOverlay = "state.dps_overlay";

    /// <summary>R8 / scene-change subscriber surface: emitted whenever
    /// <see cref="SaoAuto.Core.Packets.SceneChangeEvent"/> reaches the
    /// bridge so JS panels can react (BossHP wipe, DPS panel fade, etc.)
    /// without polling state. Mirrors Python's <c>_emit_scene_change</c>
    /// callback at packet_parser.py:1552-1573.</summary>
    public const string SceneChanged = "state.scene_changed";
    public const string BossHpSnapshot = "state.bosshp";
    public const string BurstReady = "state.burst_ready";
    public const string BuffSnapshot = "state.buffs";
    public const string HideSeekStatus = "state.hideseek";
    public const string AlertRaised = "alert.raised";
    public const string CommanderUpdated = "commander.updated";
    public const string UpdaterStatus = "updater.status";
}
