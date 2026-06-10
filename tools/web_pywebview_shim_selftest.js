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

  const triggerManager = fs.readFileSync(path.join(root, "web/trigger_timer_manager.html"), "utf8");
  assert(!triggerManager.includes("{ args: args || [] }"), "trigger manager still sends bare fallback args");
  assert(triggerManager.includes("triggerPayload(name, args || [])"), "trigger manager is missing fallback payload mapping");

  console.log("web_pywebview_shim_selftest: ok");
})().catch((err) => {
  console.error(err && err.stack ? err.stack : err);
  process.exit(1);
});
