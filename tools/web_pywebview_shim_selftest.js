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

  for (const rel of ["web/act_graph_timeseries.html", "web/act_combatant_drilldown.html", "web/act_skill_drilldown.html"]) {
    const text = fs.readFileSync(path.join(root, rel), "utf8");
    assert(!text.includes("bridge.cmd(fallbackAction, args)"), `${rel} still sends bare fallback args`);
    assert(text.includes("fallbackPayload(fallbackAction, args)"), `${rel} is missing fallbackPayload bridge mapping`);
  }

  const pluginManager = fs.readFileSync(path.join(root, "web/plugin_manager.html"), "utf8");
  assert(!pluginManager.includes("{ args: args || [] }"), "plugin manager still sends bare fallback args");
  assert(pluginManager.includes("pluginCommand(name, args || [])"), "plugin manager is missing fallback payload mapping");
  assert(pluginManager.includes("PluginManager._panelCards"), "plugin manager should keep panel card entries across polls");
  assert(!pluginManager.includes("grid.innerHTML = '';"), "plugin manager still clears all panel cards on each poll");
  assert(pluginManager.includes("entry.renderSig === renderSig"), "plugin manager should skip unchanged panel specs");

  const triggerManager = fs.readFileSync(path.join(root, "web/trigger_timer_manager.html"), "utf8");
  assert(!triggerManager.includes("{ args: args || [] }"), "trigger manager still sends bare fallback args");
  assert(triggerManager.includes("triggerPayload(name, args || [])"), "trigger manager is missing fallback payload mapping");

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
