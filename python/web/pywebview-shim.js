// Generic pywebview compatibility shim.
//
// This file only provides a transport adapter for pages that expect
// `window.pywebview.api`. Feature-specific methods are registered by the
// owning plugin through `window.SAOPluginShim.register(...)`.
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
            return window.bridge.cmd(String(name || ''), payload || {});
        } catch (e) {
            return Promise.reject(e);
        }
    }

    function pluginPayload(value) {
        if (value == null) return '';
        return value;
    }

    function finiteNumber(value, fallback) {
        var number = Number(value);
        if (!isFinite(number)) number = Number(fallback || 0);
        return isFinite(number) ? number : 0;
    }

    function clampNumber(value, fallback, lo, hi) {
        var number = finiteNumber(value, fallback);
        if (lo != null) number = Math.max(lo, number);
        if (hi != null) number = Math.min(hi, number);
        return number;
    }

    function clampInt(value, fallback, lo, hi) {
        return Math.round(clampNumber(value, fallback, lo, hi));
    }

    function safeLimit(value, fallback, hi) {
        return clampInt(value, fallback, 1, hi || 1000);
    }

    function safeOffset(value) {
        return clampInt(value, 0, 0, Number.MAX_SAFE_INTEGER);
    }

    function safeTimelineSpeed(value) {
        return clampNumber(value, 1, 0.1, 8);
    }

    function normalizeOk(result) {
        if (result === true || result === null || result === undefined || result === '') {
            return { ok: true };
        }
        if (result === false) {
            return { ok: false };
        }
        if (!result || typeof result !== 'object') {
            return { ok: true, value: result };
        }
        if (result && result.error && result.ok == null) {
            result.ok = false;
            result.message = result.message || String(result.error);
        } else if (result && result.ok == null) {
            result.ok = true;
        }
        return result;
    }

    var api = {
        play_sound: function (name) {
            return call('sound.play', { name: String(name || '') });
        },
        set_sound_enabled: function (enabled) {
            return call('sound.set_enabled', { enabled: !!enabled }).then(normalizeOk);
        },
        set_sound_volume: function (volumePct) {
            var volume = clampInt(volumePct, 70, 0, 100);
            return call('sound.set_volume', { volume: volume }).then(normalizeOk);
        },
        exit_application: function () {
            return call('ui.exit', {});
        },
        exit_app: function () {
            return call('ui.exit', {});
        },
        set_panel_visible: function (panel, visible) {
            return call('ui.set_panel_visible', { panel: String(panel || ''), visible: !!visible });
        },
        get_panel_themes: function () {
            return call('ui.get_panel_themes', {});
        },
        set_panel_theme: function (panel, theme) {
            return call('ui.set_panel_theme', { panel: String(panel || ''), theme: String(theme || '') }).then(normalizeOk);
        },
        browse_dir: function (path) {
            return call('file.browse_dir', { path: String(path || '') });
        },
        select_folder: function (path) {
            return call('file.select_folder', { path: String(path || '') }).then(normalizeOk);
        },
        select_file: function (path, consumerName) {
            return call('file.select_file', {
                path: String(path || ''),
                consumer: String(consumerName || '')
            }).then(normalizeOk);
        },
        toggle_menu: function () {
            return call('ui.toggle_menu', {});
        },
        alert_ok: function () {
            return call('ui.menu_action', { action: 'alert_ok' }).then(normalizeOk);
        },
        context_action: function (action) {
            return call('ui.context_action', { action: String(action || '') });
        },
        menu_action: function (action) {
            return call('ui.menu_action', { action: String(action || '') });
        },
        window_drag: function (dx, dy) {
            return call('ui.window_drag', {
                dx: clampInt(dx, 0, -10000, 10000),
                dy: clampInt(dy, 0, -10000, 10000)
            });
        },
        set_ctx_menu_active: function (active, bounds) {
            return call('ui.set_ctx_menu_active', { active: !!active, bounds: bounds || null });
        },
        close_panel: function () {
            return call('ui.close_panel', {});
        },
        panel_action: function (action) {
            return call('ui.panel_action', { action: String(action || '') });
        },
        download_update: function () {
            return call('updater.download', {}).then(normalizeOk);
        },
        apply_update: function () {
            return call('updater.apply', {}).then(normalizeOk);
        },
        skip_update: function () {
            return call('updater.skip', {}).then(normalizeOk);
        },
        toggle_plugin_manager: function () {
            return call('ui.menu_action', { action: 'toggle_plugin_manager' });
        },
        get_plugin_status: function () {
            return call('plugins.status', {});
        },
        list_plugins: function () {
            return call('plugins.list', {});
        },
        enable_plugin: function (pluginId) {
            return call('plugins.enable', { plugin_id: String(pluginId || '') });
        },
        disable_plugin: function (pluginId) {
            return call('plugins.disable', { plugin_id: String(pluginId || '') });
        },
        reload_plugins: function (pluginId) {
            var payload = pluginId ? { plugin_id: String(pluginId || '') } : {};
            return call('plugins.reload', payload);
        },
        pin_plugin: function (pluginId, pinned) {
            return call('plugins.pin', { plugin_id: String(pluginId || ''), pinned: pinned !== false });
        },
        import_plugin_dialog: function () {
            return call('plugins.import_dialog', {});
        },
        import_plugin: function (archivePath) {
            return call('plugins.import', { archive_path: String(archivePath || '') });
        },
        uninstall_plugin: function (pluginId) {
            return call('plugins.uninstall', { plugin_id: String(pluginId || '') });
        },
        get_plugin_hotkeys: function () {
            return call('plugins.hotkeys', {});
        },
        set_plugin_hotkey: function (action, key) {
            return call('plugins.set_hotkey', { action: String(action || ''), key: String(key || '') });
        },
        render_ui_panel: function (panelId, payload) {
            return call('plugins.render_ui_panel', { panel_id: String(panelId || ''), payload: pluginPayload(payload) });
        },
        invoke_ui_action: function (panelId, actionId, payload) {
            return call('plugins.invoke_ui_action', { panel_id: String(panelId || ''), action_id: String(actionId || ''), payload: pluginPayload(payload) });
        },
        render_surfaces: function () {
            return call('render.surfaces', {});
        },
        render_overlays: function (surface) {
            return call('render.overlays', { surface: String(surface || '') });
        },
        render_apply_hooks: function (surface, payload) {
            return call('render.apply_hooks', { surface: String(surface || ''), payload: pluginPayload(payload) });
        },
        license_status: function () {
            return Promise.resolve(JSON.stringify({
                ok: true,
                is_paid: false,
                tier: 'free',
                hwid: 'standalone',
                source: 'standalone'
            }));
        },
        _call: call
    };

    function extendApi(methods) {
        var target = window.pywebview && window.pywebview.api ? window.pywebview.api : api;
        if (!methods || typeof methods !== 'object') return target;
        Object.keys(methods).forEach(function (name) {
            if (typeof methods[name] === 'function') target[name] = methods[name];
        });
        return target;
    }

    window.pywebview = window.pywebview || {};
    window.pywebview.api = api;
    window.SAOPluginShim = Object.assign(window.SAOPluginShim || {}, {
        register: extendApi,
        extendApi: extendApi,
        call: call,
        normalizeOk: normalizeOk,
        finiteNumber: finiteNumber,
        clampNumber: clampNumber,
        clampInt: clampInt,
        safeLimit: safeLimit,
        safeOffset: safeOffset,
        safeTimelineSpeed: safeTimelineSpeed
    });
    window.__pywebviewShim = { version: 'generic-1' };
    try {
        if (typeof window.dispatchEvent === 'function' && typeof window.Event === 'function') {
            window.dispatchEvent(new window.Event('pywebviewready'));
        }
    } catch (_) {}
})();
