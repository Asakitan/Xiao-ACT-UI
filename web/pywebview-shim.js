// S193 — Compatibility shim for pages written against pywebview's
// `window.pywebview.api.X` style. Translates each call into a
// `window.bridge.cmd('<name>', payload)` Promise so the existing
// HUD HTML (hp.html, stamina.html, etc.) keeps working without
// surgery.
//
// Surface mirrors the Python panel modules:
//   pywebview.api.play_sound(name)
//   pywebview.api.set_hit_regions(rects)
//   pywebview.api.notify_hp_hit_regions_ready()
//   pywebview.api.exit_application()
//   pywebview.api.set_panel_visible(panel, visible)
//   pywebview.api.menu_action(action)
//
// Each method returns a Promise. Callers that ignore the return
// value (the common case) see no change in behaviour; callers that
// await get whatever C# replied.
(function () {
    "use strict";

    if (window.pywebview && window.pywebview.api) {
        return; // real pywebview already provided
    }
    if (!window.bridge || !window.bridge.cmd) {
        if (window.console) console.warn('[pywebview-shim] window.bridge missing; shim disabled');
        return;
    }

    function call(name, payload) {
        try {
            return window.bridge.cmd(name, payload || {});
        } catch (e) {
            return Promise.reject(e);
        }
    }

    var api = {
        play_sound: function (name) {
            return call('sound.play', { name: String(name || '') });
        },
        set_hit_regions: function (rects) {
            return call('ui.set_hit_regions', { regions: rects });
        },
        notify_hp_hit_regions_ready: function () {
            return call('ui.notify_hp_hit_regions_ready', {});
        },
        exit_application: function () {
            return call('ui.exit', {});
        },
        set_panel_visible: function (panel, visible) {
            return call('ui.set_panel_visible', { panel: String(panel || ''), visible: !!visible });
        },
        menu_action: function (action) {
            return call('ui.menu_action', { action: String(action || '') });
        },
        toggle_plugin_manager: function () {
            return call('ui.menu_action', { action: 'toggle_plugin_manager' });
        },
        toggle_trigger_timer_manager: function () {
            return call('ui.menu_action', { action: 'toggle_trigger_timer_manager' });
        },
        toggle_data_source_health: function () {
            return call('ui.menu_action', { action: 'toggle_data_source_health' });
        },
        toggle_report_export: function () {
            return call('ui.menu_action', { action: 'toggle_report_export' });
        },
        toggle_timeline_vcr: function () {
            return call('ui.menu_action', { action: 'toggle_timeline_vcr' });
        },
        toggle_action_log: function () {
            return call('ui.menu_action', { action: 'toggle_action_log' });
        },
        get_data_source_health: function () {
            return call('act.sources.health', {});
        },
        diagnose_data_source: function () {
            return call('act.sources.diagnose', {});
        },
        copy_data_source_health: function () {
            return call('act.sources.copy', {});
        },
        get_report_export_status: function (limit, fmt) {
            return call('act.report.status', { limit: limit || 20, fmt: String(fmt || 'json') });
        },
        export_last_report: function (fmt) {
            return call('act.report.export', { fmt: String(fmt || 'json') });
        },
        copy_report_export: function (fmt) {
            return call('act.report.copy', { fmt: String(fmt || 'json') });
        },
        get_history_status: function (limit, query) {
            return call('act.history.status', { limit: limit || 20, query: String(query || '') });
        },
        load_history_report: function (index, show) {
            return call('act.history.load', { index: index || 0, show: show !== false });
        },
        delete_history_report: function (index) {
            return call('act.history.delete', { index: index || 0 });
        },
        clear_history_reports: function () {
            return call('act.history.clear', {});
        },
        get_timeline_status: function (limit, query) {
            return call('act.timeline.status', { limit: limit || 80, query: String(query || '') });
        },
        play_timeline: function (speed) {
            return call('act.timeline.play', { speed: speed || 1 });
        },
        pause_timeline: function () {
            return call('act.timeline.pause', {});
        },
        step_timeline: function (deltaMs) {
            return call('act.timeline.step', { delta_ms: deltaMs || 1000 });
        },
        seek_timeline: function (cursorMs) {
            return call('act.timeline.seek', { cursor_ms: cursorMs || 0 });
        },
        set_timeline_speed: function (speed) {
            return call('act.timeline.speed', { speed: speed || 1 });
        },
        filter_timeline: function (query) {
            return call('act.timeline.filter', { query: String(query || '') });
        },
        get_action_log_status: function (limit, query, topic, cursorMs) {
            return call('act.action_log.status', { limit: limit || 80, query: String(query || ''), topic: String(topic || ''), cursor_ms: cursorMs || 0 });
        },
        search_action_log: function (query, limit) {
            return call('act.action_log.search', { query: String(query || ''), limit: limit || 80 });
        },
        filter_action_log: function (topic, query, limit) {
            return call('act.action_log.filter', { topic: String(topic || ''), query: String(query || ''), limit: limit || 80 });
        },
        jump_action_log_time: function (cursorMs, limit) {
            return call('act.action_log.jump_to_time', { cursor_ms: cursorMs || 0, limit: limit || 80 });
        },
        copy_action_log: function (limit, query, topic) {
            return call('act.action_log.copy', { limit: limit || 80, query: String(query || ''), topic: String(topic || '') });
        },
        get_plugin_status: function () {
            return call('act.plugins.status', {});
        },
        list_plugins: function () {
            return call('act.plugins.list', {});
        },
        enable_plugin: function (pluginId) {
            return call('act.plugins.enable', { plugin_id: String(pluginId || '') });
        },
        disable_plugin: function (pluginId) {
            return call('act.plugins.disable', { plugin_id: String(pluginId || '') });
        },
        reload_plugins: function (pluginId) {
            var payload = pluginId ? { plugin_id: String(pluginId || '') } : {};
            return call('act.plugins.reload', payload);
        },
        get_trigger_status: function () {
            return call('act.triggers.status', {});
        },
        enable_trigger: function (ruleId) {
            return call('act.triggers.enable', { rule_id: String(ruleId || '') });
        },
        disable_trigger: function (ruleId) {
            return call('act.triggers.disable', { rule_id: String(ruleId || '') });
        },
        reload_triggers: function () {
            return call('act.triggers.reload', {});
        },
        test_trigger: function (ruleId) {
            return call('act.triggers.test', { rule_id: String(ruleId || '') });
        },
        // Generic escape hatch: any unhandled name routes through
        // `ui.legacy_call` so the C# side can log + decide.
        _call: call,
    };

    window.pywebview = window.pywebview || {};
    window.pywebview.api = api;
    window.__pywebviewShim = { version: 's193' };
})();
