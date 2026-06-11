namespace SaoAuto.Core.Configuration;

public static class SettingsKeys
{
    public const string Hotkeys = "hotkeys";
    public const string UiMode = "ui_mode";
    public const string DataSourceMap = "data_source_map";
    public const string DataSource = "data_source";
    public const string EntityLastMenu = "entity_last_menu";
    public const string GameCache = "game_cache";
    public const string ProfessionSkillCache = "profession_skill_cache";
    public const string WatchedSkillSlots = "watched_skill_slots";
    public const string BurstEnabled = "burst_enabled";
    public const string PanelThemes = "panel_themes";
    public const string MemDataSource = "mem_data_source";
    public const string CaptureDevice = "capture_device";
    public const string BuffMonEnabled = "buffmon_enabled";
    public const string UpdateCheckEnabled = "update_check_enabled";
    public const string UpdateHost = "update_host";
    public const string UpdateAutoPoll = "update_auto_poll";
    public const string Roi = "roi";
    public const string HideSeekToggleHotkey = "hide_seek_toggle_hotkey";
    // S196b — HUD horizontal offset (Python `hud_offset_x`, default 0.04 of
    // monitor width). The rest of the HUD geometry (width 0.75*sw, height
    // 500, y at bottom) is derived from monitor dimensions every launch
    // for 1:1 parity with sao_webview.py.
    public const string HudOffsetX = "hud_offset_x";

    /// <summary>
    /// R8 / DPS-04: DPS overlay master toggle. Mirrors Python's
    /// <c>dps_enabled</c> setting at sao_gui_engine_lifecycle_mixin.py:287
    /// + sao_gui_dps_theme_mixin.py:274-285. Default true; when false the
    /// per-tick DPS pump (DPS-03) skips show-edge / hides the panel even
    /// when <see cref="DpsTracker.PollOverlayState"/> reports has-live.
    /// </summary>
    public const string DpsEnabled = "dps_enabled";

    public static readonly IReadOnlyList<string> LegacyPrunedOnSave = new[]
    {
        "last_file",
        "speed",
        "transpose",
        "chord_mode",
    };
}
