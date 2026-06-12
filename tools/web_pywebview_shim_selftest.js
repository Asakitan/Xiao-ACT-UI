// Lightweight contract checks for WebView2's pywebview compatibility shim.
// Run from repo root with: node tools/web_pywebview_shim_selftest.js

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const root = path.resolve(__dirname, "..");
const calls = [];

global.window = {
  bridge: {
    cmd(name, payload) {
      calls.push({ name, payload });
      if (name === "dps.entity_detail") {
        return Promise.resolve({
          uid: payload.uid,
          name: "Asuna",
          skills: [{ skill_id: 42, name: "Slash", total: 100 }],
        });
      }
      return Promise.resolve({ ok: true });
    },
  },
  console,
};

const shim = fs.readFileSync(path.join(root, "web", "pywebview-shim.js"), "utf8");
vm.runInThisContext(shim, { filename: "pywebview-shim.js" });

function assert(cond, message) {
  if (!cond) throw new Error(message);
}

(async function main() {
  assert(window.pywebview && window.pywebview.api, "pywebview api was not installed");
  const apiNames = Array.from(shim.matchAll(/^\s*([A-Za-z0-9_]+):\s*function\s*\(/gm), (match) => match[1]);
  const duplicateNames = Array.from(new Set(apiNames.filter((name, index) => apiNames.indexOf(name) !== index)));
  assert(duplicateNames.length === 0, "duplicate pywebview api methods: " + duplicateNames.join(", "));

  await window.pywebview.api.get_aggregate_status(1000, "hit", "history", 500, 7, "enc-1", "field", "skill_id");
  let last = calls[calls.length - 1];
  assert(last.name === "act.aggregate.status", "aggregate command name mismatch");
  assert(last.payload.limit === 1000, "aggregate limit was not forwarded");
  assert(last.payload.window_ms === 500, "aggregate window_ms was not forwarded");
  assert(last.payload.top_n === 7, "aggregate top_n was not forwarded");
  assert(last.payload.group_by === "field", "aggregate group_by was not forwarded");
  assert(last.payload.group_field === "skill_id", "aggregate group_field was not forwarded");

  await window.pywebview.api.show_action_log_at(12345, "history", "enc-2", "damage");
  last = calls[calls.length - 1];
  assert(last.name === "act.action_log.jump_to_time", "show_action_log_at command mismatch");
  assert(last.payload.cursor_ms === 12345, "show_action_log_at cursor_ms was not forwarded");
  assert(last.payload.source === "history", "show_action_log_at source was not forwarded");
  assert(last.payload.encounter_id === "enc-2", "show_action_log_at encounter_id was not forwarded");
  assert(last.payload.topic === "damage", "show_action_log_at topic was not forwarded");

  await window.pywebview.api.jump_action_log_time(23456, 24, "live", "", 8, "skill");
  last = calls[calls.length - 1];
  assert(last.name === "act.action_log.jump_to_time", "jump_action_log_time command mismatch");
  assert(last.payload.topic === "skill", "jump_action_log_time topic was not forwarded");

  await window.pywebview.api.get_action_log_status("9999", "", "", "999999999", "live", "", "-5");
  last = calls[calls.length - 1];
  assert(last.name === "act.action_log.status", "get_action_log_status command mismatch");
  assert(last.payload.limit === 500, "action log limit should be clamped");
  assert(last.payload.cursor_ms === 86400000, "action log cursor should be clamped");
  assert(last.payload.offset === 0, "action log offset should be clamped");

  await window.pywebview.api.play_timeline(-99);
  last = calls[calls.length - 1];
  assert(last.name === "act.timeline.play", "play_timeline command mismatch");
  assert(last.payload.speed === 0.1, "play_timeline speed was not clamped");

  await window.pywebview.api.set_timeline_speed("999");
  last = calls[calls.length - 1];
  assert(last.name === "act.timeline.speed", "set_timeline_speed command mismatch");
  assert(last.payload.speed === 8, "set_timeline_speed speed was not clamped");

  await window.pywebview.api.exit_app();
  last = calls[calls.length - 1];
  assert(last.name === "ui.exit", "exit_app should map to ui.exit");

  await window.pywebview.api.boss_hp_hit_regions([{ left: 1, top: 2, width: 3, height: 4 }]);
  last = calls[calls.length - 1];
  assert(last.name === "ui.boss_hp_hit_regions", "boss_hp_hit_regions command mismatch");
  assert(last.payload.regions[0].width === 3, "boss_hp_hit_regions regions were not forwarded");

  await window.pywebview.api.context_action("exit");
  last = calls[calls.length - 1];
  assert(last.name === "ui.context_action", "context_action command mismatch");
  assert(last.payload.action === "exit", "context_action action was not forwarded");

  await window.pywebview.api.toggle_menu();
  last = calls[calls.length - 1];
  assert(last.name === "ui.toggle_menu", "toggle_menu command mismatch");

  await window.pywebview.api.window_drag("999999", "-999999");
  last = calls[calls.length - 1];
  assert(last.name === "ui.window_drag", "window_drag command mismatch");
  assert(last.payload.dx === 10000 && last.payload.dy === -10000, "window_drag deltas should be clamped");

  await window.pywebview.api.show_last_dps_report();
  last = calls[calls.length - 1];
  assert(last.name === "dps.show_last_report", "show_last_dps_report command mismatch");

  await window.pywebview.api.show_last_report();
  last = calls[calls.length - 1];
  assert(last.name === "dps.show_last_report", "show_last_report command mismatch");

  await window.pywebview.api.reset_dps();
  last = calls[calls.length - 1];
  assert(last.name === "dps.reset_combat", "reset_dps command mismatch");

  await window.pywebview.api.alert_ok();
  last = calls[calls.length - 1];
  assert(last.name === "ui.menu_action", "alert_ok command mismatch");
  assert(last.payload.action === "alert_ok", "alert_ok action was not forwarded");

  await window.pywebview.api.set_dps_enabled(false);
  last = calls[calls.length - 1];
  assert(last.name === "dps.toggle_enabled", "set_dps_enabled command mismatch");
  assert(last.payload.enabled === false, "set_dps_enabled flag was not forwarded");

  await window.pywebview.api.get_dps_enabled();
  last = calls[calls.length - 1];
  assert(last.name === "dps.toggle_enabled", "get_dps_enabled command mismatch");

  await window.pywebview.api.set_buffmon_enabled(true);
  last = calls[calls.length - 1];
  assert(last.name === "buffmon.set_enabled", "set_buffmon_enabled command mismatch");
  assert(last.payload.enabled === true, "set_buffmon_enabled flag was not forwarded");

  await window.pywebview.api.get_buffmon_enabled();
  last = calls[calls.length - 1];
  assert(last.name === "buffmon.get_enabled", "get_buffmon_enabled command mismatch");

  await window.pywebview.api.download_update();
  last = calls[calls.length - 1];
  assert(last.name === "updater.download", "download_update command mismatch");

  await window.pywebview.api.apply_update();
  last = calls[calls.length - 1];
  assert(last.name === "updater.apply", "apply_update command mismatch");

  await window.pywebview.api.skip_update();
  last = calls[calls.length - 1];
  assert(last.name === "updater.skip", "skip_update command mismatch");

  await window.pywebview.api.fetch_leaderboard("level");
  last = calls[calls.length - 1];
  assert(last.name === "ui.fetch_leaderboard", "fetch_leaderboard command mismatch");
  assert(last.payload.sort === "level", "fetch_leaderboard sort was not forwarded");

  let dpsSnapshotApplied = false;
  let dpsDetailApplied = false;
  window.DpsMeter = {
    showActSnapshot(payload) {
      dpsSnapshotApplied = !!payload;
    },
    updateDetail(payload) {
      dpsDetailApplied = payload && payload.uid === "9007199254740993" && payload.skills && payload.skills[0].name === "Slash";
    },
  };
  await window.pywebview.api.get_entity_detail("9007199254740993");
  last = calls[calls.length - 1];
  assert(last.name === "dps.entity_detail", "get_entity_detail command mismatch");
  assert(last.payload.uid === "9007199254740993", "get_entity_detail uid should be forwarded as a string");
  assert(dpsDetailApplied, "get_entity_detail should apply the returned detail when DpsMeter is present");

  await window.pywebview.api.set_sound_enabled(false);
  last = calls[calls.length - 1];
  assert(last.name === "sound.set_enabled", "set_sound_enabled command mismatch");
  assert(last.payload.enabled === false, "set_sound_enabled flag was not forwarded");

  await window.pywebview.api.set_sound_volume("999");
  last = calls[calls.length - 1];
  assert(last.name === "sound.set_volume", "set_sound_volume command mismatch");
  assert(last.payload.volume === 100, "set_sound_volume value should be clamped");

  await window.pywebview.api.request_live_snapshot();
  last = calls[calls.length - 1];
  assert(last.name === "state.snapshot", "request_live_snapshot command mismatch");
  assert(dpsSnapshotApplied, "request_live_snapshot should apply the returned snapshot when DpsMeter is present");

  await window.pywebview.api.list_history("9999");
  last = calls[calls.length - 1];
  assert(last.name === "act.history.status", "list_history command mismatch");
  assert(last.payload.limit === 200, "list_history limit should be clamped");

  await window.pywebview.api.export_last_report("csv");
  last = calls[calls.length - 1];
  assert(last.name === "act.report.export", "export_last_report command mismatch");
  assert(last.payload.fmt === "csv", "export_last_report fmt was not forwarded");

  await window.pywebview.api.set_ctx_menu_active(true, { left: 1, top: 2, width: 3, height: 4 });
  last = calls[calls.length - 1];
  assert(last.name === "ui.set_ctx_menu_active", "set_ctx_menu_active command mismatch");
  assert(last.payload.active === true, "set_ctx_menu_active active flag was not forwarded");
  assert(last.payload.bounds.left === 1, "set_ctx_menu_active bounds were not forwarded");

  await window.pywebview.api.get_panel_themes();
  last = calls[calls.length - 1];
  assert(last.name === "ui.get_panel_themes", "get_panel_themes command mismatch");

  await window.pywebview.api.set_panel_theme("act", "light");
  last = calls[calls.length - 1];
  assert(last.name === "ui.set_panel_theme", "set_panel_theme command mismatch");
  assert(last.payload.panel === "act", "set_panel_theme panel was not forwarded");
  assert(last.payload.theme === "light", "set_panel_theme theme was not forwarded");

  await window.pywebview.api.set_watched_slots([3, 1, 5]);
  last = calls[calls.length - 1];
  assert(last.name === "settings.set_watched_slots", "set_watched_slots command mismatch");
  assert(Array.isArray(last.payload.slots), "set_watched_slots slots should be an array");
  assert(last.payload.slots.join(",") === "3,1,5", "set_watched_slots values were not forwarded");

  await window.pywebview.api.set_burst_enabled(false);
  last = calls[calls.length - 1];
  assert(last.name === "settings.set_burst_enabled", "set_burst_enabled command mismatch");
  assert(last.payload.enabled === false, "set_burst_enabled flag was not forwarded");

  await window.pywebview.api.set_boss_bar_mode("always");
  last = calls[calls.length - 1];
  assert(last.name === "settings.set_boss_bar_mode", "set_boss_bar_mode command mismatch");
  assert(last.payload.mode === "always", "set_boss_bar_mode mode was not forwarded");

  await window.pywebview.api.set_dps_fade_timeout(12);
  last = calls[calls.length - 1];
  assert(last.name === "settings.set_dps_fade_timeout", "set_dps_fade_timeout command mismatch");
  assert(last.payload.seconds === 12, "set_dps_fade_timeout seconds were not forwarded");
  await window.pywebview.api.set_dps_fade_timeout(-5);
  last = calls[calls.length - 1];
  assert(last.payload.seconds === 0, "set_dps_fade_timeout should clamp negative seconds");
  await window.pywebview.api.set_dps_fade_timeout(999);
  last = calls[calls.length - 1];
  assert(last.payload.seconds === 120, "set_dps_fade_timeout should clamp oversized seconds");

  await window.pywebview.api.set_data_source("memory");
  last = calls[calls.length - 1];
  assert(last.name === "settings.set_data_source", "set_data_source command mismatch");
  assert(last.payload.mode === "memory", "set_data_source mode was not forwarded");

  await window.pywebview.api.set_component_source("boss", "packet");
  last = calls[calls.length - 1];
  assert(last.name === "settings.set_component_source", "set_component_source command mismatch");
  assert(last.payload.component === "boss", "set_component_source component was not forwarded");
  assert(last.payload.mode === "packet", "set_component_source mode was not forwarded");

  await window.pywebview.api.set_auto_key_server_url("http://ak.local/");
  last = calls[calls.length - 1];
  assert(last.name === "autokey.cloud.set_server_url", "set_auto_key_server_url command mismatch");
  assert(last.payload.url === "http://ak.local/", "set_auto_key_server_url url was not forwarded");

  await window.pywebview.api.set_boss_raid_server_url("http://raid.local/");
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.cloud.set_server_url", "set_boss_raid_server_url command mismatch");
  assert(last.payload.url === "http://raid.local/", "set_boss_raid_server_url url was not forwarded");

  await window.pywebview.api.get_auto_key_state();
  last = calls[calls.length - 1];
  assert(last.name === "autokey.state.get", "get_auto_key_state command mismatch");

  await window.pywebview.api.get_boss_raid_state();
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.state.get", "get_boss_raid_state command mismatch");

  await window.pywebview.api.raid_next_phase();
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.runtime.next_phase", "raid_next_phase command mismatch");

  await window.pywebview.api.raid_reset();
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.runtime.reset", "raid_reset command mismatch");

  await window.pywebview.api.set_entity_role(1001, "boss");
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.runtime.set_entity_role", "set_entity_role command mismatch");
  assert(last.payload.uuid === 1001, "set_entity_role uuid was not forwarded");
  assert(last.payload.role === "boss", "set_entity_role role was not forwarded");

  await window.pywebview.api.boss_raid_next_phase();
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.runtime.next_phase", "boss_raid_next_phase command mismatch");

  await window.pywebview.api.boss_raid_reset();
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.runtime.reset", "boss_raid_reset command mismatch");

  await window.pywebview.api.boss_raid_start();
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.start", "boss_raid_start command mismatch");

  await window.pywebview.api.boss_raid_stop();
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.stop", "boss_raid_stop command mismatch");

  await window.pywebview.api.browse_dir("C:\\temp");
  last = calls[calls.length - 1];
  assert(last.name === "file.browse_dir", "browse_dir command mismatch");
  assert(last.payload.path === "C:\\temp", "browse_dir path was not forwarded");

  await window.pywebview.api.select_folder("C:\\profiles");
  last = calls[calls.length - 1];
  assert(last.name === "file.select_folder", "select_folder command mismatch");
  assert(last.payload.path === "C:\\profiles", "select_folder path was not forwarded");

  await window.pywebview.api.start_auto_key_import_picker("D:\\profiles");
  last = calls[calls.length - 1];
  assert(last.name === "autokey.import_picker.start", "start_auto_key_import_picker command mismatch");
  assert(last.payload.path === "D:\\profiles", "start_auto_key_import_picker path was not forwarded");

  await window.pywebview.api.start_boss_raid_import_picker("D:\\raid");
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.import_picker.start", "start_boss_raid_import_picker command mismatch");
  assert(last.payload.path === "D:\\raid", "start_boss_raid_import_picker path was not forwarded");
  assert(window._pickerConsumer === "boss_raid", "start_boss_raid_import_picker did not set picker consumer");

  window._pickerConsumer = "auto_key";
  await window.pywebview.api.select_file("D:\\profiles\\ak.json");
  last = calls[calls.length - 1];
  assert(last.name === "file.select_file", "select_file command mismatch");
  assert(last.payload.path === "D:\\profiles\\ak.json", "select_file path was not forwarded");
  assert(last.payload.consumer === "auto_key", "select_file consumer was not forwarded");

  await window.pywebview.api.set_boss_raid_enabled(true);
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.set_enabled", "set_boss_raid_enabled command mismatch");
  assert(last.payload.enabled === true, "set_boss_raid_enabled enabled was not forwarded");

  await window.pywebview.api.activate_boss_raid_profile("raid-2");
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.profile.set_active", "activate_boss_raid_profile command mismatch");
  assert(last.payload.id === "raid-2", "activate_boss_raid_profile id was not forwarded");

  await window.pywebview.api.create_boss_raid_profile();
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.profile.create", "create_boss_raid_profile command mismatch");

  await window.pywebview.api.save_boss_raid_profile({ id: "raid-2", profile_name: "Raid Two" });
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.profile.save", "save_boss_raid_profile command mismatch");
  assert(last.payload.profile.profile_name === "Raid Two", "save_boss_raid_profile payload was not forwarded");

  await window.pywebview.api.delete_boss_raid_profile("raid-2");
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.profile.delete", "delete_boss_raid_profile command mismatch");
  assert(last.payload.id === "raid-2", "delete_boss_raid_profile id was not forwarded");

  await window.pywebview.api.export_boss_raid_profile("raid-2");
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.export", "export_boss_raid_profile command mismatch");
  assert(last.payload.id === "raid-2", "export_boss_raid_profile id was not forwarded");

  await window.pywebview.api.download_boss_raid_remote("remote-2");
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.cloud.download", "download_boss_raid_remote command mismatch");
  assert(last.payload.id === "remote-2", "download_boss_raid_remote id was not forwarded");

  await window.pywebview.api.search_boss_raid_remote({ q: "dragon" });
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.cloud.search", "search_boss_raid_remote command mismatch");
  assert(last.payload.query.q === "dragon", "search_boss_raid_remote query was not forwarded");

  await window.pywebview.api.refresh_boss_raid_upload_auth(true);
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.cloud.refresh_upload_auth", "refresh_boss_raid_upload_auth command mismatch");
  assert(last.payload.force === true, "refresh_boss_raid_upload_auth force was not forwarded");

  await window.pywebview.api.upload_boss_raid_profile("raid-2");
  last = calls[calls.length - 1];
  assert(last.name === "bossraid.cloud.upload", "upload_boss_raid_profile command mismatch");
  assert(last.payload.id === "raid-2", "upload_boss_raid_profile id was not forwarded");

  await window.pywebview.api.save_autokey_actions("[{\"trigger_slot\":1,\"action_slot\":3}]");
  last = calls[calls.length - 1];
  assert(last.name === "autokey.actions.save", "save_autokey_actions command mismatch");
  assert(last.payload.actions_json.indexOf("trigger_slot") >= 0, "save_autokey_actions JSON was not forwarded");

  let autoKeyEditorTab = "";
  window._akSetTab = function (tab) { autoKeyEditorTab = tab; };
  await window.pywebview.api.toggle_autokey_editor();
  last = calls[calls.length - 1];
  assert(last.name === "ui.menu_action", "toggle_autokey_editor command mismatch");
  assert(last.payload.action === "toggle_autokey_editor", "toggle_autokey_editor action was not forwarded");
  assert(last.payload.local_handled === true, "toggle_autokey_editor should mark embedded tab fallback");
  assert(autoKeyEditorTab === "editor", "toggle_autokey_editor should open the embedded editor tab");

  let raidEditorTab = "";
  window._brSetTab = function (tab) { raidEditorTab = tab; };
  await window.pywebview.api.toggle_raid_editor();
  last = calls[calls.length - 1];
  assert(last.name === "ui.menu_action", "toggle_raid_editor command mismatch");
  assert(last.payload.action === "toggle_raid_editor", "toggle_raid_editor action was not forwarded");
  assert(last.payload.local_handled === true, "toggle_raid_editor should mark embedded tab fallback");
  assert(raidEditorTab === "editor", "toggle_raid_editor should open the embedded editor tab");

  await window.pywebview.api.toggle_mem_scope();
  last = calls[calls.length - 1];
  assert(last.name === "ui.menu_action", "toggle_mem_scope command mismatch");
  assert(last.payload.action === "toggle_mem_scope", "toggle_mem_scope action was not forwarded");

  await window.pywebview.api.get_mem_scope_status("hp", "u32", "job-1");
  last = calls[calls.length - 1];
  assert(last.name === "act.mem_scope.status", "get_mem_scope_status command mismatch");
  assert(last.payload.query === "hp", "get_mem_scope_status query was not forwarded");
  assert(last.payload.dtype === "u32", "get_mem_scope_status dtype was not forwarded");
  assert(last.payload.job_id === "job-1", "get_mem_scope_status job_id was not forwarded");

  await window.pywebview.api.get_death_recap_status("9999", "999", "1001");
  last = calls[calls.length - 1];
  assert(last.name === "act.death_recap.status", "get_death_recap_status command mismatch");
  assert(last.payload.limit === 500, "get_death_recap_status limit should be clamped");
  assert(last.payload.window_s === 120, "get_death_recap_status window_s should be clamped");
  assert(last.payload.entity_id === "1001", "get_death_recap_status entity_id was not forwarded");

  await window.pywebview.api.get_graph_timeseries_status("damage", "9999", "", "", "999999999");
  last = calls[calls.length - 1];
  assert(last.name === "act.graph.status", "get_graph_timeseries_status command mismatch");
  assert(last.payload.limit === 1000, "graph limit should be clamped");
  assert(last.payload.time_range_ms === 86400000, "graph time range should be clamped");

  await window.pywebview.api.get_skill_drilldown_status("c1", "s1", "", "9999");
  last = calls[calls.length - 1];
  assert(last.name === "act.skill.status", "get_skill_drilldown_status command mismatch");
  assert(last.payload.limit === 500, "skill drilldown limit should be clamped");

  await window.pywebview.api.mem_search("123", "i64", 4);
  last = calls[calls.length - 1];
  assert(last.name === "act.mem_scope.search", "mem_search command mismatch");
  assert(last.payload.value === "123", "mem_search value was not forwarded");
  assert(last.payload.align === 4, "mem_search align was not forwarded");

  await window.pywebview.api.mem_attr_map("0x1234");
  last = calls[calls.length - 1];
  assert(last.name === "act.mem_scope.attr_map", "mem_attr_map command mismatch");
  assert(last.payload.ent_addr === "0x1234", "mem_attr_map ent_addr was not forwarded");

  await window.pywebview.api.enable_plugin("plugin-1");
  last = calls[calls.length - 1];
  assert(last.name === "act.plugins.enable", "enable_plugin command mismatch");
  assert(last.payload.plugin_id === "plugin-1", "enable_plugin plugin_id was not forwarded");

  await window.pywebview.api.pin_plugin("plugin-1", false);
  last = calls[calls.length - 1];
  assert(last.name === "act.plugins.pin", "pin_plugin command mismatch");
  assert(last.payload.plugin_id === "plugin-1", "pin_plugin plugin_id was not forwarded");
  assert(last.payload.pinned === false, "pin_plugin pinned flag was not forwarded");

  await window.pywebview.api.invoke_ui_action("panel-1", "open", "{\"ok\":true}");
  last = calls[calls.length - 1];
  assert(last.name === "act.plugins.invoke_ui_action", "invoke_ui_action command mismatch");
  assert(last.payload.panel_id === "panel-1", "invoke_ui_action panel_id was not forwarded");
  assert(last.payload.action_id === "open", "invoke_ui_action action_id was not forwarded");

  await window.pywebview.api.render_ui_panel("panel-2", { filter: "boss" });
  last = calls[calls.length - 1];
  assert(last.name === "act.plugins.render_ui_panel", "render_ui_panel command mismatch");
  assert(last.payload.payload.filter === "boss", "render_ui_panel object payload was not preserved");

  await window.pywebview.api.invoke_ui_action("panel-2", "apply", { threshold: 7 });
  last = calls[calls.length - 1];
  assert(last.name === "act.plugins.invoke_ui_action", "invoke_ui_action object command mismatch");
  assert(last.payload.payload.threshold === 7, "invoke_ui_action object payload was not preserved");

  await window.pywebview.api.act_render_overlays("dps");
  last = calls[calls.length - 1];
  assert(last.name === "act.render.overlays", "act_render_overlays command mismatch");
  assert(last.payload.surface === "dps", "act_render_overlays surface was not forwarded");

  await window.pywebview.api.act_render_apply_hooks("dps", { total: 99 });
  last = calls[calls.length - 1];
  assert(last.name === "act.render.apply_hooks", "act_render_apply_hooks command mismatch");
  assert(last.payload.surface === "dps", "act_render_apply_hooks surface was not forwarded");
  assert(last.payload.payload.total === 99, "act_render_apply_hooks object payload was not preserved");

  await window.pywebview.api.open_skill_drilldown("1001", "slash-7");
  last = calls[calls.length - 1];
  assert(last.name === "act.skill.open", "open_skill_drilldown command mismatch");
  assert(last.payload.combatant_id === "1001", "open_skill_drilldown combatant_id was not forwarded");
  assert(last.payload.skill_id === "slash-7", "open_skill_drilldown skill_id was not forwarded");

  for (const rel of ["web/act_graph_timeseries.html", "web/act_combatant_drilldown.html", "web/act_skill_drilldown.html"]) {
    const text = fs.readFileSync(path.join(root, rel), "utf8");
    assert(!text.includes("bridge.cmd(fallbackAction, args)"), `${rel} still sends bare fallback args`);
    assert(text.includes("fallbackPayload(fallbackAction, args)"), `${rel} is missing fallbackPayload bridge mapping`);
  }

  const pluginManager = fs.readFileSync(path.join(root, "web/plugin_manager.html"), "utf8");
  assert(!pluginManager.includes("{ args: args || [] }"), "plugin manager still sends bare fallback args");
  assert(pluginManager.includes("pluginCommand(name, args || [])"), "plugin manager is missing fallback payload mapping");
  assert(pluginManager.includes("pluginPayloadArg(args, 2)"), "plugin manager should preserve object action payloads");
  assert(pluginManager.includes("PluginManager._panelCards"), "plugin manager should keep panel card entries across polls");
  assert(!pluginManager.includes("grid.innerHTML = '';"), "plugin manager still clears all panel cards on each poll");
  assert(pluginManager.includes("entry.renderSig === renderSig"), "plugin manager should skip unchanged panel specs");

  const pluginLayer = fs.readFileSync(path.join(root, "web/plugin_layer.js"), "utf8");
  assert(!pluginLayer.includes('window.bridge.cmd("act." + name, { args: args || [] })'), "plugin layer still sends bare bridge args");
  assert(pluginLayer.includes('name: "act.plugins.invoke_ui_action"'), "plugin layer should map fallback UI actions to act.plugins.invoke_ui_action");
  assert(pluginLayer.includes('name: "act.render.apply_hooks"'), "plugin layer should map render hooks to the bridge command");
  assert(pluginLayer.includes(".splg-tablewrap{max-width:100%;overflow-x:auto;"), "plugin layer table wrapper should scroll wide plugin tables");
  for (const snippet of [
    "(node.children || []).forEach(function (c) {",
    "(node.ops || []).forEach(function (op) {",
    "var cols = node.columns || [];",
    "var rows = node.rows || [];",
    "return !!(spec && (spec.title || ((spec.nodes || []).length)));",
    "spec = spec || {};",
    "if (!(spec.nodes || []).length && !spec.title)",
    "(spec.nodes || []).forEach(function (n) {",
    "var overlays = (data && data.overlays) || [];",
  ]) {
    assert(!pluginLayer.includes(snippet), "plugin layer still trusts malformed plugin spec payloads: " + snippet);
  }
  for (const snippet of [
    "function isObjectValue(value)",
    "function objectValue(value)",
    "function listItems(value)",
    "function objectItems(value)",
    "node = objectValue(node);",
    "objectItems(node.children).forEach(function (c) {",
    "objectItems(node.ops).forEach(function (op) {",
    "var cols = objectItems(node.columns);",
    "var rows = objectItems(node.rows);",
    "spec = objectValue(spec);",
    "var nodes = objectItems(spec.nodes);",
    "nodes.forEach(function (n) {",
    "var overlays = objectItems(objectValue(data).overlays);",
    "renderSpec(objectValue(ov.spec), card, overlayAction(ov.panel_id || (objectValue(ov.spec).panel_id)));",
  ]) {
    assert(pluginLayer.includes(snippet), "plugin layer is missing guarded plugin spec handling: " + snippet);
  }

  const triggerManager = fs.readFileSync(path.join(root, "web/trigger_timer_manager.html"), "utf8");
  assert(!triggerManager.includes("{ args: args || [] }"), "trigger manager still sends bare fallback args");
  assert(triggerManager.includes("triggerPayload(name, args || [])"), "trigger manager is missing fallback payload mapping");
  assert(triggerManager.includes("toggle_trigger_timer_manager: 'ui.menu_action'"), "trigger manager close should map through ui.menu_action");
  assert(triggerManager.includes("apiCall('toggle_trigger_timer_manager', [])"), "trigger manager close should use bridge-aware apiCall");
  assert(triggerManager.includes("function displayRules(data)"), "trigger manager should merge trigger and timer rows for rendering");
  assert(triggerManager.includes("var rules = displayRules(data);"), "trigger manager render should use merged trigger/timer rows");

  const combatantDrilldown = fs.readFileSync(path.join(root, "web/act_combatant_drilldown.html"), "utf8");
  assert(combatantDrilldown.includes("'act.skill.open'"), "combatant drilldown should use explicit act.skill.open fallback");
  assert(combatantDrilldown.includes("skill_id: String(args[1] || '')"), "combatant drilldown should forward skill_id when opening skill drilldown");

  const dpsPanel = fs.readFileSync(path.join(root, "web/dps.html"), "utf8");
  assert(dpsPanel.includes("sk.skill_name || sk.name || sk.skill_id"), "DPS detail should render C# skill name fields");
  assert(!shim.includes("var numericUid = Number(uid || 0);"), "pywebview shim must not coerce DPS entity uid to Number");
  assert(shim.includes("var uidText = String(uid == null ? '' : uid).trim();"), "pywebview shim should preserve DPS entity uid as a string");
  assert(!shim.includes("var volume = parseInt(volumePct, 10);"), "pywebview shim must not pass raw volume integers");
  assert(shim.includes("function clampNumber(value, fallback, lo, hi)"), "pywebview shim should provide a numeric clamp helper");
  assert(shim.includes("function clampInt(value, fallback, lo, hi)"), "pywebview shim should provide an integer clamp helper");
  assert(shim.includes("function safeLimit(value, fallback, hi)"), "pywebview shim should provide a limit clamp helper");
  assert(shim.includes("var volume = clampInt(volumePct, 70, 0, 100);"), "pywebview shim should clamp sound volume");
  assert(!shim.includes("{ dx: dx || 0, dy: dy || 0 }"), "pywebview shim must not pass raw drag deltas");
  assert(!shim.includes("limit: limit || 20, query: ''"), "pywebview shim must not pass raw history limits");
  assert(!shim.includes("window_s: window_s || 8.0"), "pywebview shim must not pass raw death recap windows");
  assert(!shim.includes("delta_ms: deltaMs || 1000"), "pywebview shim must not pass raw timeline step deltas");
  assert(!shim.includes("cursor_ms: cursorMs || 0"), "pywebview shim must not pass raw action-log cursors");
  assert(!shim.includes("time_range_ms: timeRangeMs || 0"), "pywebview shim must not pass raw graph time ranges");
  assert(!shim.includes("limit: limit || 80"), "pywebview shim must not pass raw 80-row limits");
  assert(!shim.includes("limit: limit || 120"), "pywebview shim must not pass raw graph limits");

  const timelineVcr = fs.readFileSync(path.join(root, "web/act_timeline_vcr.html"), "utf8");
  assert(!timelineVcr.includes("{ args: args || [] }"), "timeline VCR still sends bare fallback args");
  assert(timelineVcr.includes("timelinePayload(name, args || [])"), "timeline VCR is missing fallback payload mapping");
  assert(timelineVcr.includes("delta_ms: safeDeltaMs(args[0])"), "timeline VCR should forward normalized step delta_ms");
  assert(timelineVcr.includes("cursor_ms: safeCursorMs(args[0])"), "timeline VCR should forward normalized seek cursor_ms");
  assert(timelineVcr.includes("speed: safeSpeed(args[0])"), "timeline VCR should normalize fallback speed");
  assert(!timelineVcr.includes("Number(speed.value || 1)"), "timeline VCR should not pass raw speed input");

  const actionLog = fs.readFileSync(path.join(root, "web/act_action_log.html"), "utf8");
  assert(!actionLog.includes("{ args: args || [] }"), "action log still sends bare fallback args");
  assert(actionLog.includes("actionLogPayload(name, args || [])"), "action log is missing fallback payload mapping");
  assert(actionLog.includes("source: String(args[4] || 'live')"), "action log status should forward source");
  assert(actionLog.includes("encounter_id: String(args[5] || '')"), "action log status should forward encounter_id");
  assert(actionLog.includes("offset: safePageOffset(args[6])"), "action log status should forward normalized offset");

  const deathRecap = fs.readFileSync(path.join(root, "web/act_death_recap.html"), "utf8");
  assert(!deathRecap.includes("{ args: args || [] }"), "death recap still sends bare fallback args");
  assert(deathRecap.includes("deathRecapPayload(name, args || [])"), "death recap is missing fallback payload mapping");
  assert(deathRecap.includes("window_s: safeWindowSeconds(args[1])"), "death recap should forward normalized window_s");
  assert(deathRecap.includes("entity_id: args[2] == null || args[2] === '' ? null : args[2]"), "death recap should forward entity_id");

  const dataSourceHealth = fs.readFileSync(path.join(root, "web/data_source_health.html"), "utf8");
  assert(!dataSourceHealth.includes("{ args: args || [] }"), "data source health still sends bare fallback args");
  assert(dataSourceHealth.includes("sourcePayload(name, args || [])"), "data source health is missing fallback payload mapping");
  assert(dataSourceHealth.includes("name === 'get_data_source_health' || name === 'diagnose_data_source' || name === 'copy_data_source_health'"), "data source health should send empty payloads for data commands");

  const reportExport = fs.readFileSync(path.join(root, "web/act_report_export.html"), "utf8");
  assert(!reportExport.includes("{ args: args || [] }"), "report export still sends bare fallback args");
  assert(reportExport.includes("reportPayload(name, args || [])"), "report export is missing fallback payload mapping");
  assert(reportExport.includes("fmt: String(args[0] || 'json')"), "report export should forward fmt as a named payload field");
  assert(reportExport.includes("index: safeHistoryIndex(args[0], 0)"), "report export should forward normalized history index as a named payload field");
  assert(reportExport.includes("path: String(args[0] || '')"), "report export should forward import path as a named payload field");

  const offlineImport = fs.readFileSync(path.join(root, "web/act_offline_import.html"), "utf8");
  assert(!offlineImport.includes("{ args: args || [] }"), "offline import still sends bare fallback args");
  assert(offlineImport.includes("offlinePayload(name, args || [])"), "offline import is missing fallback payload mapping");
  assert(offlineImport.includes("history_limit: safeLimit(args[0], 20, 200)"), "offline import should forward normalized history_limit as a named payload field");
  assert(offlineImport.includes("path: String(args[0] || '')"), "offline import should forward import path as a named payload field");
  assert(offlineImport.includes("index: safeHistoryIndex(args[0], 0)"), "offline import should forward normalized history index as a named payload field");

  const menu = fs.readFileSync(path.join(root, "web/menu.html"), "utf8");
  assert(menu.includes("_pdPanelCards"), "menu detached plugin panel should keep panel cards across polls");
  assert(!menu.includes("body.innerHTML = '';"), "menu detached plugin panel still clears all cards on each poll");
  assert(menu.includes("entry.renderSig === renderSig"), "menu detached plugin panel should skip unchanged specs");
  for (const snippet of [
    "var panels = ((data && data.panels) || []).filter(function (p) {",
    "var spec = (r && r.spec) || { nodes: [] };",
    "var hks = ((data && data.hotkeys) || []).filter(function (h) { return h.plugin_id === _pdPlugin; });",
    "var occupied = (data && data.occupied) || {};",
    "(plugins || []).forEach(function (it) {",
    "var plugins = (data && data.plugins) || [];",
    "var plugins = ((data && data.plugins) || []).filter(function (p) {",
  ]) {
    assert(!menu.includes(snippet), "menu plugin surfaces still trust malformed payloads: " + snippet);
  }
  for (const snippet of [
    "function _pluginIsObjectValue(value)",
    "function _pluginObjectValue(value)",
    "function _pluginListItems(value)",
    "function _pluginObjectItems(value)",
    "var panels = _pluginObjectItems(_pluginObjectValue(data).panels).filter(function (p) {",
    "var spec = _pluginObjectValue(_pluginObjectValue(r).spec);",
    "var hks = _pluginObjectItems(_pluginObjectValue(data).hotkeys).filter(function (h) { return h.plugin_id === _pdPlugin; });",
    "var occupied = _pluginObjectValue(_pluginObjectValue(data).occupied);",
    "plugins = _pluginObjectItems(plugins);",
    "var plugins = _pluginObjectItems(_pluginObjectValue(data).plugins);",
    "var plugins = _pluginObjectItems(_pluginObjectValue(data).plugins).filter(function (p) {",
  ]) {
    assert(menu.includes(snippet), "menu plugin surfaces are missing guarded payload handling: " + snippet);
  }

  console.log("web_pywebview_shim_selftest: ok");
})().catch((err) => {
  console.error(err && err.stack ? err.stack : err);
  process.exit(1);
});
