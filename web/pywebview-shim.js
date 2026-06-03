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
        // Generic escape hatch: any unhandled name routes through
        // `ui.legacy_call` so the C# side can log + decide.
        _call: call,
    };

    window.pywebview = window.pywebview || {};
    window.pywebview.api = api;
    window.__pywebviewShim = { version: 's193' };
})();
