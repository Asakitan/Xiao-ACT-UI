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

  await window.pywebview.api.get_aggregate_status(1000, "hit", "history", 500, 7, "enc-1", "field", "skill_id");
  let last = calls[calls.length - 1];
  assert(last.name === "act.aggregate.status", "aggregate command name mismatch");
  assert(last.payload.group_by === "field", "aggregate group_by was not forwarded");
  assert(last.payload.group_field === "skill_id", "aggregate group_field was not forwarded");

  await window.pywebview.api.show_action_log_at(12345, "history", "enc-2");
  last = calls[calls.length - 1];
  assert(last.name === "act.action_log.jump_to_time", "show_action_log_at command mismatch");
  assert(last.payload.cursor_ms === 12345, "show_action_log_at cursor_ms was not forwarded");
  assert(last.payload.source === "history", "show_action_log_at source was not forwarded");
  assert(last.payload.encounter_id === "enc-2", "show_action_log_at encounter_id was not forwarded");

  await window.pywebview.api.exit_app();
  last = calls[calls.length - 1];
  assert(last.name === "ui.exit", "exit_app should map to ui.exit");

  await window.pywebview.api.context_action("exit");
  last = calls[calls.length - 1];
  assert(last.name === "ui.context_action", "context_action command mismatch");
  assert(last.payload.action === "exit", "context_action action was not forwarded");

  await window.pywebview.api.toggle_menu();
  last = calls[calls.length - 1];
  assert(last.name === "ui.toggle_menu", "toggle_menu command mismatch");

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

  let dpsSnapshotApplied = false;
  let dpsDetailApplied = false;
  window.DpsMeter = {
    showActSnapshot(payload) {
      dpsSnapshotApplied = !!payload;
    },
    updateDetail(payload) {
      dpsDetailApplied = payload && payload.uid === 1001 && payload.skills && payload.skills[0].name === "Slash";
    },
  };
  await window.pywebview.api.get_entity_detail(1001);
  last = calls[calls.length - 1];
  assert(last.name === "dps.entity_detail", "get_entity_detail command mismatch");
  assert(last.payload.uid === 1001, "get_entity_detail uid was not forwarded");
  assert(dpsDetailApplied, "get_entity_detail should apply the returned detail when DpsMeter is present");

  await window.pywebview.api.request_live_snapshot();
  last = calls[calls.length - 1];
  assert(last.name === "state.snapshot", "request_live_snapshot command mismatch");
  assert(dpsSnapshotApplied, "request_live_snapshot should apply the returned snapshot when DpsMeter is present");

  await window.pywebview.api.list_history(7);
  last = calls[calls.length - 1];
  assert(last.name === "act.history.status", "list_history command mismatch");
  assert(last.payload.limit === 7, "list_history limit was not forwarded");

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

  await window.pywebview.api.get_death_recap_status(55, 12.5, "1001");
  last = calls[calls.length - 1];
  assert(last.name === "act.death_recap.status", "get_death_recap_status command mismatch");
  assert(last.payload.limit === 55, "get_death_recap_status limit was not forwarded");
  assert(last.payload.window_s === 12.5, "get_death_recap_status window_s was not forwarded");
  assert(last.payload.entity_id === "1001", "get_death_recap_status entity_id was not forwarded");

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

  const timelineVcr = fs.readFileSync(path.join(root, "web/act_timeline_vcr.html"), "utf8");
  assert(!timelineVcr.includes("{ args: args || [] }"), "timeline VCR still sends bare fallback args");
  assert(timelineVcr.includes("timelinePayload(name, args || [])"), "timeline VCR is missing fallback payload mapping");
  assert(timelineVcr.includes("delta_ms: numericArg(args[0], 1000)"), "timeline VCR should forward step delta_ms");
  assert(timelineVcr.includes("cursor_ms: numericArg(args[0], 0)"), "timeline VCR should forward seek cursor_ms");

  const actionLog = fs.readFileSync(path.join(root, "web/act_action_log.html"), "utf8");
  assert(!actionLog.includes("{ args: args || [] }"), "action log still sends bare fallback args");
  assert(actionLog.includes("actionLogPayload(name, args || [])"), "action log is missing fallback payload mapping");
  assert(actionLog.includes("source: String(args[4] || 'live')"), "action log status should forward source");
  assert(actionLog.includes("encounter_id: String(args[5] || '')"), "action log status should forward encounter_id");
  assert(actionLog.includes("offset: numericArg(args[6], 0)"), "action log status should forward offset");

  const deathRecap = fs.readFileSync(path.join(root, "web/act_death_recap.html"), "utf8");
  assert(!deathRecap.includes("{ args: args || [] }"), "death recap still sends bare fallback args");
  assert(deathRecap.includes("deathRecapPayload(name, args || [])"), "death recap is missing fallback payload mapping");
  assert(deathRecap.includes("window_s: numericArg(args[1], 8.0)"), "death recap should forward window_s");
  assert(deathRecap.includes("entity_id: args[2] == null || args[2] === '' ? null : args[2]"), "death recap should forward entity_id");

  const dataSourceHealth = fs.readFileSync(path.join(root, "web/data_source_health.html"), "utf8");
  assert(!dataSourceHealth.includes("{ args: args || [] }"), "data source health still sends bare fallback args");
  assert(dataSourceHealth.includes("sourcePayload(name, args || [])"), "data source health is missing fallback payload mapping");
  assert(dataSourceHealth.includes("name === 'get_data_source_health' || name === 'diagnose_data_source' || name === 'copy_data_source_health'"), "data source health should send empty payloads for data commands");

  const reportExport = fs.readFileSync(path.join(root, "web/act_report_export.html"), "utf8");
  assert(!reportExport.includes("{ args: args || [] }"), "report export still sends bare fallback args");
  assert(reportExport.includes("reportPayload(name, args || [])"), "report export is missing fallback payload mapping");
  assert(reportExport.includes("fmt: String(args[0] || 'json')"), "report export should forward fmt as a named payload field");
  assert(reportExport.includes("index: numericArg(args[0], 0)"), "report export should forward history index as a named payload field");
  assert(reportExport.includes("path: String(args[0] || '')"), "report export should forward import path as a named payload field");

  const offlineImport = fs.readFileSync(path.join(root, "web/act_offline_import.html"), "utf8");
  assert(!offlineImport.includes("{ args: args || [] }"), "offline import still sends bare fallback args");
  assert(offlineImport.includes("offlinePayload(name, args || [])"), "offline import is missing fallback payload mapping");
  assert(offlineImport.includes("history_limit: numericArg(args[0], 20)"), "offline import should forward history_limit as a named payload field");
  assert(offlineImport.includes("path: String(args[0] || '')"), "offline import should forward import path as a named payload field");
  assert(offlineImport.includes("index: numericArg(args[0], 0)"), "offline import should forward history index as a named payload field");

  const menu = fs.readFileSync(path.join(root, "web/menu.html"), "utf8");
  assert(menu.includes("_pdPanelCards"), "menu detached plugin panel should keep panel cards across polls");
  assert(!menu.includes("body.innerHTML = '';"), "menu detached plugin panel still clears all cards on each poll");
  assert(menu.includes("entry.renderSig === renderSig"), "menu detached plugin panel should skip unchanged specs");

  console.log("web_pywebview_shim_selftest: ok");
})().catch((err) => {
  console.error(err && err.stack ? err.stack : err);
  process.exit(1);
});
