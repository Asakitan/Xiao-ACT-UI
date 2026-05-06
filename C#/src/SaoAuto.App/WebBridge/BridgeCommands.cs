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

    // BossRaid
    public const string StartBossRaid = "bossraid.start";
    public const string StopBossRaid = "bossraid.stop";
    public const string ImportBossRaid = "bossraid.import";
    public const string ExportBossRaid = "bossraid.export";
    public const string OpenRaidEditor = "bossraid.editor.open";

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
    public const string AlertRaised = "alert.raised";
    public const string CommanderUpdated = "commander.updated";
    public const string UpdaterStatus = "updater.status";
}
