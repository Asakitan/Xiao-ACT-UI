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
    // S177 — cloud script-share endpoints (served by AutoKeyCloudBridge).
    public const string SearchAutoKeyScripts = "autokey.cloud.search";
    public const string GetAutoKeyScript = "autokey.cloud.get";
    public const string IssueAutoKeyUploadToken = "autokey.cloud.issue_token";
    public const string UploadAutoKeyScript = "autokey.cloud.upload";

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

    // Commander
    public const string OpenCommander = "commander.open";
    public const string CloseCommander = "commander.close";
    public const string SyncCommanderState = "commander.sync";

    // DPS
    public const string ShowLastDpsReport = "dps.show_last_report";
    public const string ResetCombat = "dps.reset_combat";

    // Buff monitor
    public const string SetBuffMonEnabled = "buffmon.set_enabled";
    public const string GetBuffMonEnabled = "buffmon.get_enabled";

    // State pull (S190) — a HUD page that opens mid-run can ask for the
    // current snapshot instead of waiting for the next state.changed.
    public const string StateSnapshot = "state.snapshot";

    // Sound + UI surface (S193) — invoked by the pywebview compat shim
    // so legacy HUD HTML keeps working without rewrites.
    public const string PlaySound = "sound.play";
    public const string SetHitRegions = "ui.set_hit_regions";
    public const string NotifyHpHitRegionsReady = "ui.notify_hp_hit_regions_ready";
    public const string ExitApplication = "ui.exit";
    public const string SetPanelVisible = "ui.set_panel_visible";

    // Updater
    public const string CheckUpdate = "updater.check";
    public const string DownloadUpdate = "updater.download";
    public const string ApplyUpdate = "updater.apply";
}

/// <summary>Canonical bridge event names emitted from C# to JS.</summary>
public static class BridgeEvents
{
    public const string GameStateChanged = "state.changed";
    public const string HealthChanged = "state.hp";
    public const string StaminaChanged = "state.stamina";
    public const string DpsSnapshot = "state.dps";
    public const string BossHpSnapshot = "state.bosshp";
    public const string BurstReady = "state.burst_ready";
    public const string BuffSnapshot = "state.buffs";
    public const string HideSeekStatus = "state.hideseek";
    public const string AlertRaised = "alert.raised";
    public const string CommanderUpdated = "commander.updated";
    public const string UpdaterStatus = "updater.status";
}
