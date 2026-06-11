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

    function pluginPayload(value) {
        if (value == null) return '';
        return value;
    }

    function normalizeOk(result) {
        if (result && result.error && result.ok == null) {
            result.ok = false;
            result.message = result.message || String(result.error);
        } else if (result && result.ok == null) {
            result.ok = true;
        }
        return result;
    }

    function openEmbeddedEditor(setterName) {
        try {
            if (typeof window[setterName] === 'function') {
                window[setterName]('editor');
                return true;
            }
        } catch (_) {}
        return false;
    }

    var api = {
        play_sound: function (name) {
            return call('sound.play', { name: String(name || '') });
        },
        set_sound_enabled: function (enabled) {
            return call('sound.set_enabled', { enabled: !!enabled }).then(normalizeOk);
        },
        set_sound_volume: function (volumePct) {
            var volume = parseInt(volumePct, 10);
            if (!isFinite(volume)) volume = 70;
            return call('sound.set_volume', { volume: volume }).then(normalizeOk);
        },
        set_hit_regions: function (rects) {
            return call('ui.set_hit_regions', { regions: rects });
        },
        boss_hp_hit_regions: function (rects) {
            return call('ui.boss_hp_hit_regions', { regions: Array.isArray(rects) ? rects : [] });
        },
        notify_hp_hit_regions_ready: function () {
            return call('ui.notify_hp_hit_regions_ready', {});
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
            return call('ui.set_panel_theme', { panel: String(panel || ''), theme: String(theme || '') });
        },
        set_watched_slots: function (slots) {
            return call('settings.set_watched_slots', { slots: Array.isArray(slots) ? slots : [] }).then(normalizeOk);
        },
        set_burst_enabled: function (enabled) {
            return call('settings.set_burst_enabled', { enabled: !!enabled }).then(normalizeOk);
        },
        set_boss_bar_mode: function (mode) {
            return call('settings.set_boss_bar_mode', { mode: String(mode || 'boss_raid') }).then(normalizeOk);
        },
        set_dps_fade_timeout: function (seconds) {
            var value = parseInt(seconds, 10);
            if (!isFinite(value)) value = 0;
            return call('settings.set_dps_fade_timeout', { seconds: value }).then(normalizeOk);
        },
        set_data_source: function (mode) {
            return call('settings.set_data_source', { mode: String(mode || '') }).then(normalizeOk);
        },
        set_component_source: function (component, mode) {
            return call('settings.set_component_source', {
                component: String(component || ''),
                mode: String(mode || '')
            }).then(normalizeOk);
        },
        set_auto_key_server_url: function (url) {
            return call('autokey.cloud.set_server_url', { url: String(url || '') }).then(normalizeOk);
        },
        set_boss_raid_server_url: function (url) {
            return call('bossraid.cloud.set_server_url', { url: String(url || '') }).then(normalizeOk);
        },
        get_auto_key_state: function () {
            return call('autokey.state.get', {}).then(normalizeOk);
        },
        get_boss_raid_state: function () {
            return call('bossraid.state.get', {}).then(normalizeOk);
        },
        raid_next_phase: function () {
            return call('bossraid.runtime.next_phase', {}).then(normalizeOk);
        },
        raid_reset: function () {
            return call('bossraid.runtime.reset', {}).then(normalizeOk);
        },
        set_entity_role: function (uuid, role) {
            return call('bossraid.runtime.set_entity_role', {
                uuid: uuid,
                role: String(role || '')
            }).then(normalizeOk);
        },
        boss_raid_next_phase: function () {
            return call('bossraid.runtime.next_phase', {}).then(normalizeOk);
        },
        boss_raid_reset: function () {
            return call('bossraid.runtime.reset', {}).then(normalizeOk);
        },
        boss_raid_start: function () {
            return call('bossraid.start', {}).then(normalizeOk);
        },
        boss_raid_stop: function () {
            return call('bossraid.stop', {}).then(normalizeOk);
        },
        browse_dir: function (path) {
            return call('file.browse_dir', { path: String(path || '') });
        },
        select_folder: function (path) {
            return call('file.select_folder', { path: String(path || '') }).then(normalizeOk);
        },
        start_auto_key_import_picker: function (path) {
            return call('autokey.import_picker.start', { path: String(path || '') }).then(normalizeOk);
        },
        start_boss_raid_import_picker: function (path) {
            try { window._pickerConsumer = 'boss_raid'; } catch (_) {}
            return call('bossraid.import_picker.start', { path: String(path || '') }).then(normalizeOk);
        },
        select_file: function (path) {
            var consumer = '';
            try { consumer = String(window._pickerConsumer || ''); } catch (_) {}
            return call('file.select_file', {
                path: String(path || ''),
                consumer: consumer
            }).then(normalizeOk);
        },
        set_boss_raid_enabled: function (enabled) {
            return call('bossraid.set_enabled', { enabled: !!enabled }).then(normalizeOk);
        },
        activate_boss_raid_profile: function (id) {
            return call('bossraid.profile.set_active', { id: String(id || '') }).then(normalizeOk);
        },
        create_boss_raid_profile: function () {
            return call('bossraid.profile.create', {}).then(normalizeOk);
        },
        save_boss_raid_profile: function (profile) {
            return call('bossraid.profile.save', { profile: profile || {} }).then(normalizeOk);
        },
        delete_boss_raid_profile: function (id) {
            return call('bossraid.profile.delete', { id: String(id || '') }).then(normalizeOk);
        },
        export_boss_raid_profile: function (id) {
            return call('bossraid.export', { id: String(id || '') }).then(normalizeOk);
        },
        download_boss_raid_remote: function (id) {
            return call('bossraid.cloud.download', { id: String(id || '') }).then(normalizeOk);
        },
        search_boss_raid_remote: function (query) {
            return call('bossraid.cloud.search', { query: query || {} }).then(normalizeOk);
        },
        refresh_boss_raid_upload_auth: function (force) {
            return call('bossraid.cloud.refresh_upload_auth', { force: !!force }).then(normalizeOk);
        },
        save_autokey_actions: function (actionsJson) {
            return call('autokey.actions.save', {
                actions_json: String(actionsJson || '[]')
            }).then(normalizeOk);
        },
        toggle_autokey_editor: function () {
            var localHandled = openEmbeddedEditor('_akSetTab');
            return call('ui.menu_action', {
                action: 'toggle_autokey_editor',
                local_handled: localHandled
            }).then(normalizeOk).catch(function (e) {
                if (localHandled) {
                    return { ok: true, command: 'menu_action', local_handled: true, bridge_error: String(e || '') };
                }
                throw e;
            });
        },
        toggle_raid_editor: function () {
            var localHandled = openEmbeddedEditor('_brSetTab');
            return call('ui.menu_action', {
                action: 'toggle_raid_editor',
                local_handled: localHandled
            }).then(normalizeOk).catch(function (e) {
                if (localHandled) {
                    return { ok: true, command: 'menu_action', local_handled: true, bridge_error: String(e || '') };
                }
                throw e;
            });
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
            return call('ui.window_drag', { dx: dx || 0, dy: dy || 0 });
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
        show_last_dps_report: function () {
            return call('dps.show_last_report', {}).then(normalizeOk);
        },
        show_last_report: function () {
            return call('dps.show_last_report', {}).then(normalizeOk);
        },
        reset_dps: function () {
            return call('dps.reset_combat', {}).then(normalizeOk);
        },
        set_dps_enabled: function (enabled) {
            return call('dps.toggle_enabled', { enabled: !!enabled }).then(normalizeOk);
        },
        get_dps_enabled: function () {
            return call('dps.toggle_enabled', {}).then(normalizeOk);
        },
        get_entity_detail: function (uid) {
            var numericUid = Number(uid || 0);
            if (!isFinite(numericUid)) numericUid = 0;
            return call('dps.entity_detail', { uid: numericUid }).then(function (detail) {
                detail = normalizeOk(detail);
                try {
                    if (detail && detail.ok !== false
                            && window.DpsMeter
                            && typeof window.DpsMeter.updateDetail === 'function') {
                        window.DpsMeter.updateDetail(detail);
                    }
                } catch (_) {}
                return detail;
            });
        },
        set_buffmon_enabled: function (enabled) {
            return call('buffmon.set_enabled', { enabled: !!enabled }).then(normalizeOk);
        },
        get_buffmon_enabled: function () {
            return call('buffmon.get_enabled', {}).then(normalizeOk);
        },
        request_live_snapshot: function () {
            return call('state.snapshot', {}).then(function (snapshot) {
                try {
                    if (window.DpsMeter && typeof window.DpsMeter.showActSnapshot === 'function') {
                        window.DpsMeter.showActSnapshot(snapshot || {});
                    }
                } catch (_) {}
                return snapshot;
            });
        },
        list_history: function (limit) {
            return call('act.history.status', { limit: limit || 20, query: '' }).then(function (data) {
                if (data && data.items == null && data.encounters) data.items = data.encounters;
                return normalizeOk(data);
            });
        },
        export_last_report: function (fmt) {
            return call('act.report.export', { fmt: String(fmt || 'json') }).then(normalizeOk);
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
        fetch_leaderboard: function (sort) {
            return call('ui.fetch_leaderboard', { sort: String(sort || 'xp') }).then(normalizeOk);
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
        toggle_offline_import: function () {
            return call('ui.menu_action', { action: 'toggle_offline_import' });
        },
        toggle_timeline_vcr: function () {
            return call('ui.menu_action', { action: 'toggle_timeline_vcr' });
        },
        toggle_act_aggregate: function () {
            return call('ui.menu_action', { action: 'toggle_act_aggregate' });
        },
        toggle_mem_scope: function () {
            return call('ui.menu_action', { action: 'toggle_mem_scope' });
        },
        toggle_action_log: function () {
            return call('ui.menu_action', { action: 'toggle_action_log' });
        },
        toggle_death_recap: function () {
            return call('ui.menu_action', { action: 'toggle_death_recap' });
        },
        get_death_recap_status: function (limit, window_s, entity_id) {
            return call('act.death_recap.status', { limit: limit || 80, window_s: window_s || 8.0, entity_id: entity_id || null });
        },
        copy_death_recap: function (limit, window_s, entity_id) {
            return call('act.death_recap.copy', { limit: limit || 80, window_s: window_s || 8.0, entity_id: entity_id || null });
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
        get_mini_parse_status: function (formatterId) {
            return call('act.mini_parse.status', { formatter_id: String(formatterId || 'summary_table') });
        },
        preview_mini_parse: function (formatterId) {
            return call('act.mini_parse.preview', { formatter_id: String(formatterId || 'summary_table') });
        },
        copy_mini_parse: function (formatterId) {
            return call('act.mini_parse.copy', { formatter_id: String(formatterId || 'summary_table') });
        },
        get_selective_parsing_status: function () {
            return call('act.selective_parsing.status', {});
        },
        update_selective_parsing: function (policy) {
            return call('act.selective_parsing.update', { policy: policy || {} });
        },
        clear_selective_parsing: function () {
            return call('act.selective_parsing.clear', {});
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
        choose_offline_import_file: function () {
            return call('act.offline_import.choose_file', {});
        },
        import_offline_report: function (path, persist, show) {
            return call('act.offline_import.import', { path: String(path || ''), persist: persist !== false, show: show !== false });
        },
        get_offline_import_status: function (historyLimit) {
            return call('act.offline_import.status', { history_limit: historyLimit || 20 });
        },
        get_timeline_status: function (limit, query) {
            return call('act.timeline.status', { limit: limit || 80, query: String(query || '') });
        },
        get_aggregate_status: function (limit, query, source, windowMs, topN, encounterId, groupBy, groupField) {
            return call('act.aggregate.status', {
                limit: limit || 1000,
                query: String(query || ''),
                source: String(source || 'live'),
                window_ms: windowMs || 1000,
                top_n: topN || 20,
                encounter_id: String(encounterId || ''),
                group_by: String(groupBy || 'skill'),
                group_field: String(groupField || '')
            });
        },
        get_mem_scope_status: function (query, dtype, jobId) {
            return call('act.mem_scope.status', { query: String(query || ''), dtype: String(dtype || 'i32'), job_id: String(jobId || '') });
        },
        mem_search: function (value, dtype, align) {
            return call('act.mem_scope.search', { value: value == null ? '' : String(value), dtype: String(dtype || 'i32'), align: align || 0 });
        },
        mem_search_status: function (jobId) {
            return call('act.mem_scope.search_status', { job_id: String(jobId || '') });
        },
        mem_narrow: function (jobId, value) {
            return call('act.mem_scope.narrow', { job_id: String(jobId || ''), value: value == null ? '' : String(value) });
        },
        mem_search_cancel: function (jobId) {
            return call('act.mem_scope.cancel', { job_id: String(jobId || '') });
        },
        mem_attr_map: function (entAddr) {
            return call('act.mem_scope.attr_map', { ent_addr: String(entAddr || '') });
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
        get_action_log_status: function (limit, query, topic, cursorMs, source, encounterId, offset) {
            return call('act.action_log.status', { limit: limit || 80, query: String(query || ''), topic: String(topic || ''), cursor_ms: cursorMs || 0, source: String(source || 'live'), encounter_id: String(encounterId || ''), offset: offset || 0 });
        },
        search_action_log: function (query, limit, source, encounterId, offset) {
            return call('act.action_log.search', { query: String(query || ''), limit: limit || 80, source: String(source || 'live'), encounter_id: String(encounterId || ''), offset: offset || 0 });
        },
        filter_action_log: function (topic, query, limit, source, encounterId, offset) {
            return call('act.action_log.filter', { topic: String(topic || ''), query: String(query || ''), limit: limit || 80, source: String(source || 'live'), encounter_id: String(encounterId || ''), offset: offset || 0 });
        },
        jump_action_log_time: function (cursorMs, limit, source, encounterId, offset) {
            return call('act.action_log.jump_to_time', { cursor_ms: cursorMs || 0, limit: limit || 80, source: String(source || 'live'), encounter_id: String(encounterId || ''), offset: offset || 0 });
        },
        show_action_log_at: function (cursorMs, source, encounterId) {
            return call('act.action_log.jump_to_time', { cursor_ms: cursorMs || 0, limit: 80, source: String(source || 'live'), encounter_id: String(encounterId || ''), offset: 0 });
        },
        copy_action_log: function (limit, query, topic, source, encounterId, offset) {
            return call('act.action_log.copy', { limit: limit || 80, query: String(query || ''), topic: String(topic || ''), source: String(source || 'live'), encounter_id: String(encounterId || ''), offset: offset || 0 });
        },
        get_graph_timeseries_status: function (metric, limit, query, topic, timeRangeMs) {
            return call('act.graph.status', { metric: String(metric || ''), limit: limit || 120, query: String(query || ''), topic: String(topic || ''), time_range_ms: timeRangeMs || 0 });
        },
        select_graph_metric: function (metric, limit) {
            return call('act.graph.select_metric', { metric: String(metric || 'damage'), limit: limit || 120 });
        },
        zoom_graph_timeseries: function (timeRangeMs, limit) {
            return call('act.graph.zoom', { time_range_ms: timeRangeMs || 0, limit: limit || 120 });
        },
        filter_graph_timeseries: function (query, topic, limit) {
            return call('act.graph.filter', { query: String(query || ''), topic: String(topic || ''), limit: limit || 120 });
        },
        export_graph_timeseries: function (metric, limit, query, topic) {
            return call('act.graph.export', { metric: String(metric || ''), limit: limit || 120, query: String(query || ''), topic: String(topic || '') });
        },
        toggle_graph_timeseries: function () {
            return call('ui.menu_action', { action: 'toggle_graph_timeseries' });
        },
        get_combatant_drilldown_status: function (combatantId, query, focusTarget) {
            return call('act.combatant.status', { combatant_id: String(combatantId || ''), query: String(query || ''), focus_target: String(focusTarget || '') });
        },
        filter_combatant_drilldown: function (combatantId, query) {
            return call('act.combatant.filter', { combatant_id: String(combatantId || ''), query: String(query || '') });
        },
        focus_combatant_target: function (combatantId, targetId) {
            return call('act.combatant.focus_target', { combatant_id: String(combatantId || ''), target_id: String(targetId || '') });
        },
        back_combatant_drilldown: function () {
            return call('act.combatant.back', {});
        },
        toggle_combatant_drilldown: function () {
            return call('ui.menu_action', { action: 'toggle_combatant_drilldown' });
        },
        get_skill_drilldown_status: function (combatantId, skillId, query, limit) {
            return call('act.skill.status', { combatant_id: String(combatantId || ''), skill_id: String(skillId || ''), query: String(query || ''), limit: limit || 80 });
        },
        filter_skill_drilldown: function (combatantId, skillId, query, limit) {
            return call('act.skill.filter', { combatant_id: String(combatantId || ''), skill_id: String(skillId || ''), query: String(query || ''), limit: limit || 80 });
        },
        copy_skill_drilldown: function (combatantId, skillId, query, limit) {
            return call('act.skill.copy', { combatant_id: String(combatantId || ''), skill_id: String(skillId || ''), query: String(query || ''), limit: limit || 80 });
        },
        open_skill_drilldown: function (combatantId, skillId) {
            return call('act.skill.open', { combatant_id: String(combatantId || ''), skill_id: String(skillId || '') });
        },
        back_skill_drilldown: function () {
            return call('act.skill.back', {});
        },
        toggle_skill_drilldown: function () {
            return call('ui.menu_action', { action: 'toggle_skill_drilldown' });
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
        pin_plugin: function (pluginId, pinned) {
            return call('act.plugins.pin', { plugin_id: String(pluginId || ''), pinned: pinned !== false });
        },
        import_plugin_dialog: function () {
            return call('act.plugins.import_dialog', {});
        },
        import_plugin: function (archivePath) {
            return call('act.plugins.import', { archive_path: String(archivePath || '') });
        },
        uninstall_plugin: function (pluginId) {
            return call('act.plugins.uninstall', { plugin_id: String(pluginId || '') });
        },
        get_plugin_hotkeys: function () {
            return call('act.plugins.hotkeys', {});
        },
        set_plugin_hotkey: function (action, key) {
            return call('act.plugins.set_hotkey', { action: String(action || ''), key: String(key || '') });
        },
        get_plugin_ui_panels: function () {
            return call('act.plugins.ui_panels', {});
        },
        render_ui_panel: function (panelId, payload) {
            return call('act.plugins.render_ui_panel', { panel_id: String(panelId || ''), payload: pluginPayload(payload) });
        },
        invoke_ui_action: function (panelId, actionId, payload) {
            return call('act.plugins.invoke_ui_action', { panel_id: String(panelId || ''), action_id: String(actionId || ''), payload: pluginPayload(payload) });
        },
        act_render_surfaces: function () {
            return call('act.render.surfaces', {});
        },
        act_render_overlays: function (surface) {
            return call('act.render.overlays', { surface: String(surface || '') });
        },
        act_render_apply_hooks: function (surface, payload) {
            return call('act.render.apply_hooks', { surface: String(surface || ''), payload: pluginPayload(payload) });
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
    window.__pywebviewShim = { version: 's194' };
})();
