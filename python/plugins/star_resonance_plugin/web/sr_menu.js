/* Star Resonance plugin — game-specific menu JS */
/* Extracted from menu.html during 5.0.0 restructure */

/* === Star Resonance WebView shim extensions === */
(function () {
    "use strict";

    function srCall(name, payload) {
        var shim = window.SAOPluginShim || {};
        if (typeof shim.call === 'function') {
            return shim.call(name, payload || {});
        }
        if (window.bridge && typeof window.bridge.cmd === 'function') {
            try {
                return window.bridge.cmd(name, payload || {});
            } catch (e) {
                return Promise.reject(e);
            }
        }
        return Promise.reject(new Error('Bridge command unavailable: ' + name));
    }

    function srNormalizeOk(result) {
        var shim = window.SAOPluginShim || {};
        if (typeof shim.normalizeOk === 'function') return shim.normalizeOk(result);
        if (result === true || result === null || result === undefined || result === '') return { ok: true };
        if (result === false) return { ok: false };
        if (!result || typeof result !== 'object') return { ok: true, value: result };
        if (result.error && result.ok == null) {
            result.ok = false;
            result.message = result.message || String(result.error);
        } else if (result.ok == null) {
            result.ok = true;
        }
        return result;
    }

    function srFiniteNumber(value, fallback) {
        var shim = window.SAOPluginShim || {};
        if (typeof shim.finiteNumber === 'function') return shim.finiteNumber(value, fallback);
        var number = Number(value);
        if (!isFinite(number)) number = Number(fallback || 0);
        return isFinite(number) ? number : 0;
    }

    function srClampNumber(value, fallback, lo, hi) {
        var shim = window.SAOPluginShim || {};
        if (typeof shim.clampNumber === 'function') return shim.clampNumber(value, fallback, lo, hi);
        var number = srFiniteNumber(value, fallback);
        if (lo != null) number = Math.max(lo, number);
        if (hi != null) number = Math.min(hi, number);
        return number;
    }

    function srClampInt(value, fallback, lo, hi) {
        var shim = window.SAOPluginShim || {};
        if (typeof shim.clampInt === 'function') return shim.clampInt(value, fallback, lo, hi);
        return Math.round(srClampNumber(value, fallback, lo, hi));
    }

    function srSafeLimit(value, fallback, hi) {
        var shim = window.SAOPluginShim || {};
        if (typeof shim.safeLimit === 'function') return shim.safeLimit(value, fallback, hi);
        return srClampInt(value, fallback, 1, hi || 1000);
    }

    function srSafeOffset(value) {
        var shim = window.SAOPluginShim || {};
        if (typeof shim.safeOffset === 'function') return shim.safeOffset(value);
        return srClampInt(value, 0, 0, Number.MAX_SAFE_INTEGER);
    }

    function srSafeTimelineSpeed(value) {
        var shim = window.SAOPluginShim || {};
        if (typeof shim.safeTimelineSpeed === 'function') return shim.safeTimelineSpeed(value);
        return srClampNumber(value, 1, 0.1, 8);
    }

    function srPluginPayload(value) {
        if (value == null) return '';
        return value;
    }

    function srOpenEmbeddedEditor(setterName) {
        try {
            if (typeof window[setterName] === 'function') {
                window[setterName]('editor');
                return true;
            }
        } catch (_) {}
        return false;
    }

    function srRegisterApi() {
        if (window.__starResonancePluginApiRegistered) return true;
        var methods = {
            set_auto_key_server_url: function (url) {
                return srCall('autokey.cloud.set_server_url', { url: String(url || '') }).then(srNormalizeOk);
            },
            get_auto_key_state: function () {
                return srCall('autokey.state.get', {}).then(srNormalizeOk);
            },
            set_auto_key_enabled: function (enabled) {
                return srCall('autokey.set_enabled', { enabled: !!enabled }).then(srNormalizeOk);
            },
            create_auto_key_profile: function () {
                return srCall('autokey.profile.create', {}).then(srNormalizeOk);
            },
            copy_auto_key_profile: function (id) {
                return srCall('autokey.profile.clone', { id: String(id || '') }).then(srNormalizeOk);
            },
            save_auto_key_profile: function (profile) {
                return srCall('autokey.profile.upsert', { profile: profile || {}, activate: false }).then(srNormalizeOk);
            },
            delete_auto_key_profile: function (id) {
                return srCall('autokey.profile.delete', { id: String(id || '') }).then(srNormalizeOk);
            },
            activate_auto_key_profile: function (id) {
                return srCall('autokey.profile.set_active', { id: String(id || '') }).then(srNormalizeOk);
            },
            export_auto_key_profile: function (id) {
                return srCall('autokey.profile.export', { id: String(id || '') }).then(srNormalizeOk);
            },
            start_auto_key_import_picker: function (path) {
                return srCall('autokey.import_picker.start', { path: String(path || '') }).then(srNormalizeOk);
            },
            refresh_auto_key_upload_auth: function (force) {
                return srCall('autokey.cloud.refresh_upload_auth', { force: !!force }).then(srNormalizeOk);
            },
            search_remote_profiles: function (query) {
                return srCall('autokey.cloud.search', { query: query || {} }).then(srNormalizeOk);
            },
            download_remote_profile: function (id) {
                return srCall('autokey.cloud.download', { id: String(id || '') }).then(srNormalizeOk);
            },
            upload_auto_key_profile: function (id) {
                return srCall('autokey.cloud.upload', { id: String(id || '') }).then(srNormalizeOk);
            },
            save_autokey_actions: function (actionsJson) {
                return srCall('autokey.actions.save', {
                    actions_json: String(actionsJson || '[]')
                }).then(srNormalizeOk);
            },
            set_boss_raid_server_url: function (url) {
                return srCall('bossraid.cloud.set_server_url', { url: String(url || '') }).then(srNormalizeOk);
            },
            get_boss_raid_state: function () {
                return srCall('bossraid.state.get', {}).then(srNormalizeOk);
            },
            raid_next_phase: function () {
                return srCall('bossraid.runtime.next_phase', {}).then(srNormalizeOk);
            },
            raid_reset: function () {
                return srCall('bossraid.runtime.reset', {}).then(srNormalizeOk);
            },
            set_entity_role: function (uuid, role) {
                return srCall('bossraid.runtime.set_entity_role', {
                    uuid: uuid,
                    role: String(role || '')
                }).then(srNormalizeOk);
            },
            boss_raid_next_phase: function () {
                return srCall('bossraid.runtime.next_phase', {}).then(srNormalizeOk);
            },
            boss_raid_reset: function () {
                return srCall('bossraid.runtime.reset', {}).then(srNormalizeOk);
            },
            boss_raid_start: function () {
                return srCall('bossraid.start', {}).then(srNormalizeOk);
            },
            boss_raid_stop: function () {
                return srCall('bossraid.stop', {}).then(srNormalizeOk);
            },
            start_boss_raid_import_picker: function (path) {
                try { window._pickerConsumer = 'boss_raid'; } catch (_) {}
                return srCall('bossraid.import_picker.start', { path: String(path || '') }).then(srNormalizeOk);
            },
            set_boss_raid_enabled: function (enabled) {
                return srCall('bossraid.set_enabled', { enabled: !!enabled }).then(srNormalizeOk);
            },
            set_buffmon_enabled: function (enabled) {
                return srCall('buffmon.set_enabled', { enabled: !!enabled }).then(srNormalizeOk);
            },
            get_buffmon_enabled: function () {
                return srCall('buffmon.get_enabled', {}).then(srNormalizeOk);
            },
            set_hit_regions: function (rects) {
                return srCall('ui.set_hit_regions', { regions: rects });
            },
            notify_hp_hit_regions_ready: function () {
                return srCall('ui.notify_hp_hit_regions_ready', {});
            },
            set_watched_slots: function (slots) {
                return srCall('settings.set_watched_slots', { slots: Array.isArray(slots) ? slots : [] }).then(srNormalizeOk);
            },
            set_burst_enabled: function (enabled) {
                return srCall('settings.set_burst_enabled', { enabled: !!enabled }).then(srNormalizeOk);
            },
            set_boss_bar_mode: function (mode) {
                return srCall('settings.set_boss_bar_mode', { mode: String(mode || '') }).then(srNormalizeOk);
            },
            set_dps_fade_timeout: function (seconds) {
                return srCall('settings.set_dps_fade_timeout', {
                    seconds: srClampInt(seconds, 0, 0, 120)
                }).then(srNormalizeOk);
            },
            set_data_source: function (mode) {
                return srCall('settings.set_data_source', { mode: String(mode || '') }).then(srNormalizeOk);
            },
            set_component_source: function (component, mode) {
                return srCall('settings.set_component_source', {
                    component: String(component || ''),
                    mode: String(mode || '')
                }).then(srNormalizeOk);
            },
            show_last_dps_report: function () {
                return srCall('dps.show_last_report', {}).then(srNormalizeOk);
            },
            show_last_report: function () {
                return srCall('dps.show_last_report', {}).then(srNormalizeOk);
            },
            reset_dps: function () {
                return srCall('dps.reset_combat', {}).then(srNormalizeOk);
            },
            set_dps_enabled: function (enabled) {
                return srCall('dps.toggle_enabled', { enabled: !!enabled }).then(srNormalizeOk);
            },
            get_dps_enabled: function () {
                return srCall('dps.toggle_enabled', {}).then(srNormalizeOk);
            },
            get_entity_detail: function (uid) {
                var uidText = String(uid == null ? '' : uid).trim();
                return srCall('dps.entity_detail', { uid: uidText }).then(function (detail) {
                    detail = srNormalizeOk(detail);
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
            request_live_snapshot: function () {
                return srCall('state.snapshot', {}).then(function (snapshot) {
                    try {
                        if (window.DpsMeter && typeof window.DpsMeter.showActSnapshot === 'function') {
                            window.DpsMeter.showActSnapshot(snapshot || {});
                        }
                    } catch (_) {}
                    return snapshot;
                });
            },
            list_history: function (limit) {
                return srCall('act.history.status', { limit: srSafeLimit(limit, 20, 200), query: '' }).then(function (data) {
                    if (data && data.items == null && data.encounters) data.items = data.encounters;
                    return srNormalizeOk(data);
                });
            },
            export_last_report: function (fmt) {
                return srCall('act.report.export', { fmt: String(fmt || 'json') }).then(srNormalizeOk);
            },
            fetch_leaderboard: function (sort) {
                return srCall('ui.fetch_leaderboard', { sort: String(sort || 'xp') }).then(srNormalizeOk);
            },
            toggle_trigger_timer_manager: function () {
                return srCall('ui.menu_action', { action: 'toggle_trigger_timer_manager' });
            },
            toggle_data_source_health: function () {
                return srCall('ui.menu_action', { action: 'toggle_data_source_health' });
            },
            toggle_report_export: function () {
                return srCall('ui.menu_action', { action: 'toggle_report_export' });
            },
            toggle_offline_import: function () {
                return srCall('ui.menu_action', { action: 'toggle_offline_import' });
            },
            toggle_timeline_vcr: function () {
                return srCall('ui.menu_action', { action: 'toggle_timeline_vcr' });
            },
            toggle_act_aggregate: function () {
                return srCall('ui.menu_action', { action: 'toggle_act_aggregate' });
            },
            toggle_mem_scope: function () {
                return srCall('ui.menu_action', { action: 'toggle_mem_scope' });
            },
            toggle_action_log: function () {
                return srCall('ui.menu_action', { action: 'toggle_action_log' });
            },
            toggle_death_recap: function () {
                return srCall('ui.menu_action', { action: 'toggle_death_recap' });
            },
            toggle_graph_timeseries: function () {
                return srCall('ui.menu_action', { action: 'toggle_graph_timeseries' });
            },
            toggle_combatant_drilldown: function () {
                return srCall('ui.menu_action', { action: 'toggle_combatant_drilldown' });
            },
            toggle_skill_drilldown: function () {
                return srCall('ui.menu_action', { action: 'toggle_skill_drilldown' });
            },
            get_death_recap_status: function (limit, window_s, entity_id) {
                return srCall('act.death_recap.status', {
                    limit: srSafeLimit(limit, 80, 500),
                    window_s: srClampNumber(window_s, 8.0, 0, 120),
                    entity_id: entity_id || null
                });
            },
            copy_death_recap: function (limit, window_s, entity_id) {
                return srCall('act.death_recap.copy', {
                    limit: srSafeLimit(limit, 80, 500),
                    window_s: srClampNumber(window_s, 8.0, 0, 120),
                    entity_id: entity_id || null
                });
            },
            get_data_source_health: function () {
                return srCall('act.sources.health', {});
            },
            diagnose_data_source: function () {
                return srCall('act.sources.diagnose', {});
            },
            copy_data_source_health: function () {
                return srCall('act.sources.copy', {});
            },
            get_report_export_status: function (limit, fmt) {
                return srCall('act.report.status', { limit: srSafeLimit(limit, 20, 200), fmt: String(fmt || 'json') });
            },
            copy_report_export: function (fmt) {
                return srCall('act.report.copy', { fmt: String(fmt || 'json') });
            },
            get_mini_parse_status: function (formatterId) {
                return srCall('act.mini_parse.status', { formatter_id: String(formatterId || 'summary_table') });
            },
            preview_mini_parse: function (formatterId) {
                return srCall('act.mini_parse.preview', { formatter_id: String(formatterId || 'summary_table') });
            },
            copy_mini_parse: function (formatterId) {
                return srCall('act.mini_parse.copy', { formatter_id: String(formatterId || 'summary_table') });
            },
            get_selective_parsing_status: function () {
                return srCall('act.selective_parsing.status', {});
            },
            update_selective_parsing: function (policy) {
                return srCall('act.selective_parsing.update', { policy: policy || {} });
            },
            clear_selective_parsing: function () {
                return srCall('act.selective_parsing.clear', {});
            },
            get_history_status: function (limit, query) {
                return srCall('act.history.status', { limit: srSafeLimit(limit, 20, 200), query: String(query || '') });
            },
            load_history_report: function (index, show) {
                return srCall('act.history.load', { index: srSafeOffset(index), show: show !== false });
            },
            delete_history_report: function (index) {
                return srCall('act.history.delete', { index: srSafeOffset(index) });
            },
            clear_history_reports: function () {
                return srCall('act.history.clear', {});
            },
            choose_offline_import_file: function () {
                return srCall('act.offline_import.choose_file', {});
            },
            import_offline_report: function (path, persist, show) {
                return srCall('act.offline_import.import', { path: String(path || ''), persist: persist !== false, show: show !== false });
            },
            get_offline_import_status: function (historyLimit) {
                return srCall('act.offline_import.status', { history_limit: srSafeLimit(historyLimit, 20, 200) });
            },
            get_timeline_status: function (limit, query) {
                return srCall('act.timeline.status', { limit: srSafeLimit(limit, 80, 500), query: String(query || '') });
            },
            get_aggregate_status: function (limit, query, source, windowMs, topN, encounterId, groupBy, groupField) {
                return srCall('act.aggregate.status', {
                    limit: srSafeLimit(limit, 1000, 5000),
                    query: String(query || ''),
                    source: String(source || 'live'),
                    window_ms: srClampInt(windowMs, 1000, 0, 86400000),
                    top_n: srSafeLimit(topN, 20, 500),
                    encounter_id: String(encounterId || ''),
                    group_by: String(groupBy || 'skill'),
                    group_field: String(groupField || '')
                });
            },
            get_mem_scope_status: function (query, dtype, jobId) {
                return srCall('act.mem_scope.status', { query: String(query || ''), dtype: String(dtype || 'i32'), job_id: String(jobId || '') });
            },
            mem_search: function (value, dtype, align) {
                return srCall('act.mem_scope.search', { value: value == null ? '' : String(value), dtype: String(dtype || 'i32'), align: srClampInt(align, 0, 0, 64) });
            },
            mem_search_status: function (jobId) {
                return srCall('act.mem_scope.search_status', { job_id: String(jobId || '') });
            },
            mem_narrow: function (jobId, value) {
                return srCall('act.mem_scope.narrow', { job_id: String(jobId || ''), value: value == null ? '' : String(value) });
            },
            mem_search_cancel: function (jobId) {
                return srCall('act.mem_scope.cancel', { job_id: String(jobId || '') });
            },
            mem_attr_map: function (entAddr) {
                return srCall('act.mem_scope.attr_map', { ent_addr: String(entAddr || '') });
            },
            play_timeline: function (speed) {
                return srCall('act.timeline.play', { speed: srSafeTimelineSpeed(speed) });
            },
            pause_timeline: function () {
                return srCall('act.timeline.pause', {});
            },
            step_timeline: function (deltaMs) {
                return srCall('act.timeline.step', { delta_ms: srClampInt(deltaMs, 1000, -86400000, 86400000) });
            },
            seek_timeline: function (cursorMs) {
                return srCall('act.timeline.seek', { cursor_ms: srClampInt(cursorMs, 0, 0, 86400000) });
            },
            set_timeline_speed: function (speed) {
                return srCall('act.timeline.speed', { speed: srSafeTimelineSpeed(speed) });
            },
            filter_timeline: function (query) {
                return srCall('act.timeline.filter', { query: String(query || '') });
            },
            get_action_log_status: function (limit, query, topic, cursorMs, source, encounterId, offset) {
                return srCall('act.action_log.status', {
                    limit: srSafeLimit(limit, 80, 500),
                    query: String(query || ''),
                    topic: String(topic || ''),
                    cursor_ms: srClampInt(cursorMs, 0, 0, 86400000),
                    source: String(source || 'live'),
                    encounter_id: String(encounterId || ''),
                    offset: srSafeOffset(offset)
                });
            },
            search_action_log: function (query, limit, source, encounterId, offset) {
                return srCall('act.action_log.search', { query: String(query || ''), limit: srSafeLimit(limit, 80, 500), source: String(source || 'live'), encounter_id: String(encounterId || ''), offset: srSafeOffset(offset) });
            },
            filter_action_log: function (topic, query, limit, source, encounterId, offset) {
                return srCall('act.action_log.filter', { topic: String(topic || ''), query: String(query || ''), limit: srSafeLimit(limit, 80, 500), source: String(source || 'live'), encounter_id: String(encounterId || ''), offset: srSafeOffset(offset) });
            },
            jump_action_log_time: function (cursorMs, limit, source, encounterId, offset, topic) {
                var payload = { cursor_ms: srClampInt(cursorMs, 0, 0, 86400000), limit: srSafeLimit(limit, 80, 500), source: String(source || 'live'), encounter_id: String(encounterId || ''), offset: srSafeOffset(offset) };
                if (arguments.length > 5) payload.topic = String(topic || '');
                return srCall('act.action_log.jump_to_time', payload);
            },
            show_action_log_at: function (cursorMs, source, encounterId, topic) {
                var payload = { cursor_ms: srClampInt(cursorMs, 0, 0, 86400000), limit: 80, source: String(source || 'live'), encounter_id: String(encounterId || ''), offset: 0 };
                if (arguments.length > 3) payload.topic = String(topic || '');
                return srCall('act.action_log.jump_to_time', payload);
            },
            copy_action_log: function (limit, query, topic, source, encounterId, offset) {
                return srCall('act.action_log.copy', { limit: srSafeLimit(limit, 80, 500), query: String(query || ''), topic: String(topic || ''), source: String(source || 'live'), encounter_id: String(encounterId || ''), offset: srSafeOffset(offset) });
            },
            get_graph_timeseries_status: function (metric, limit, query, topic, timeRangeMs) {
                return srCall('act.graph.status', { metric: String(metric || ''), limit: srSafeLimit(limit, 120, 1000), query: String(query || ''), topic: String(topic || ''), time_range_ms: srClampInt(timeRangeMs, 0, 0, 86400000) });
            },
            select_graph_metric: function (metric, limit) {
                return srCall('act.graph.select_metric', { metric: String(metric || 'damage'), limit: srSafeLimit(limit, 120, 1000) });
            },
            zoom_graph_timeseries: function (timeRangeMs, limit) {
                return srCall('act.graph.zoom', { time_range_ms: srClampInt(timeRangeMs, 0, 0, 86400000), limit: srSafeLimit(limit, 120, 1000) });
            },
            filter_graph_timeseries: function (query, topic, limit) {
                return srCall('act.graph.filter', { query: String(query || ''), topic: String(topic || ''), limit: srSafeLimit(limit, 120, 1000) });
            },
            export_graph_timeseries: function (metric, limit, query, topic) {
                return srCall('act.graph.export', { metric: String(metric || ''), limit: srSafeLimit(limit, 120, 1000), query: String(query || ''), topic: String(topic || '') });
            },
            get_combatant_drilldown_status: function (combatantId, query, focusTarget) {
                return srCall('act.combatant.status', { combatant_id: String(combatantId || ''), query: String(query || ''), focus_target: String(focusTarget || '') });
            },
            filter_combatant_drilldown: function (combatantId, query) {
                return srCall('act.combatant.filter', { combatant_id: String(combatantId || ''), query: String(query || '') });
            },
            focus_combatant_target: function (combatantId, targetId) {
                return srCall('act.combatant.focus_target', { combatant_id: String(combatantId || ''), target_id: String(targetId || '') });
            },
            back_combatant_drilldown: function () {
                return srCall('act.combatant.back', {});
            },
            get_skill_drilldown_status: function (combatantId, skillId, query, limit) {
                return srCall('act.skill.status', { combatant_id: String(combatantId || ''), skill_id: String(skillId || ''), query: String(query || ''), limit: srSafeLimit(limit, 80, 500) });
            },
            filter_skill_drilldown: function (combatantId, skillId, query, limit) {
                return srCall('act.skill.filter', { combatant_id: String(combatantId || ''), skill_id: String(skillId || ''), query: String(query || ''), limit: srSafeLimit(limit, 80, 500) });
            },
            copy_skill_drilldown: function (combatantId, skillId, query, limit) {
                return srCall('act.skill.copy', { combatant_id: String(combatantId || ''), skill_id: String(skillId || ''), query: String(query || ''), limit: srSafeLimit(limit, 80, 500) });
            },
            open_skill_drilldown: function (combatantId, skillId) {
                return srCall('act.skill.open', { combatant_id: String(combatantId || ''), skill_id: String(skillId || '') });
            },
            back_skill_drilldown: function () {
                return srCall('act.skill.back', {});
            },
            get_trigger_status: function () {
                return srCall('act.triggers.status', {});
            },
            enable_trigger: function (ruleId) {
                return srCall('act.triggers.enable', { rule_id: String(ruleId || '') });
            },
            disable_trigger: function (ruleId) {
                return srCall('act.triggers.disable', { rule_id: String(ruleId || '') });
            },
            reload_triggers: function () {
                return srCall('act.triggers.reload', {});
            },
            test_trigger: function (ruleId) {
                return srCall('act.triggers.test', { rule_id: String(ruleId || '') });
            },
            trigger_export_presets: function (ruleIds) {
                return srCall('act_trigger_export_presets', { rule_ids: Array.isArray(ruleIds) ? ruleIds : [] });
            },
            trigger_import_presets: function (payload, replace) {
                return srCall('act_trigger_import_presets', { payload: payload || {}, replace: !!replace });
            },
            render_overlays: function (surface) {
                return srCall('render.overlays', { surface: String(surface || '') });
            },
            render_apply_hooks: function (surface, payload) {
                return srCall('render.apply_hooks', { surface: String(surface || ''), payload: srPluginPayload(payload) });
            },
            activate_boss_raid_profile: function (id) {
                return srCall('bossraid.profile.set_active', { id: String(id || '') }).then(srNormalizeOk);
            },
            create_boss_raid_profile: function () {
                return srCall('bossraid.profile.create', {}).then(srNormalizeOk);
            },
            save_boss_raid_profile: function (profile) {
                return srCall('bossraid.profile.save', { profile: profile || {} }).then(srNormalizeOk);
            },
            delete_boss_raid_profile: function (id) {
                return srCall('bossraid.profile.delete', { id: String(id || '') }).then(srNormalizeOk);
            },
            export_boss_raid_profile: function (id) {
                return srCall('bossraid.export', { id: String(id || '') }).then(srNormalizeOk);
            },
            download_boss_raid_remote: function (id) {
                return srCall('bossraid.cloud.download', { id: String(id || '') }).then(srNormalizeOk);
            },
            search_boss_raid_remote: function (query) {
                return srCall('bossraid.cloud.search', { query: query || {} }).then(srNormalizeOk);
            },
            refresh_boss_raid_upload_auth: function (force) {
                return srCall('bossraid.cloud.refresh_upload_auth', { force: !!force }).then(srNormalizeOk);
            },
            upload_boss_raid_profile: function (id) {
                return srCall('bossraid.cloud.upload', { id: String(id || '') }).then(srNormalizeOk);
            },
            toggle_autokey_editor: function () {
                var localHandled = srOpenEmbeddedEditor('_akSetTab');
                return srCall('ui.menu_action', {
                    action: 'toggle_autokey_editor',
                    local_handled: localHandled
                }).then(srNormalizeOk).catch(function (e) {
                    if (localHandled) {
                        return { ok: true, command: 'menu_action', local_handled: true, bridge_error: String(e || '') };
                    }
                    throw e;
                });
            },
            toggle_raid_editor: function () {
                var localHandled = srOpenEmbeddedEditor('_brSetTab');
                return srCall('ui.menu_action', {
                    action: 'toggle_raid_editor',
                    local_handled: localHandled
                }).then(srNormalizeOk).catch(function (e) {
                    if (localHandled) {
                        return { ok: true, command: 'menu_action', local_handled: true, bridge_error: String(e || '') };
                    }
                    throw e;
                });
            }
        };
        var shim = window.SAOPluginShim || {};
        var register = typeof shim.register === 'function' ? shim.register : shim.extendApi;
        if (typeof register === 'function') {
            register.call(shim, methods);
            window.__starResonancePluginApiRegistered = true;
            return true;
        }
        if (window.pywebview && window.pywebview.api && window.bridge && typeof window.bridge.cmd === 'function') {
            Object.assign(window.pywebview.api, methods);
            window.__starResonancePluginApiRegistered = true;
            return true;
        }
        return false;
    }

    if (!srRegisterApi() && typeof window.addEventListener === 'function') {
        window.addEventListener('pywebviewready', srRegisterApi);
    }
})();

/* === Burst Slot Handlers === */
var _watchedSlotsRequestSeq = 0;
var _watchedSlotsConfirmed = [];
function _slotId(value) {
    var n = parseInt(String(value == null ? '' : value), 10);
    return isFinite(n) && n >= 1 && n <= 9 ? n : null;
}
function _watchedSlotsList() {
    var slots = [];
    document.querySelectorAll('#slot-grid .slot-chk.active').forEach(function(el) {
        var slot = _slotId(el.getAttribute('data-slot'));
        if (slot != null && slots.indexOf(slot) < 0) slots.push(slot);
    });
    return slots;
}
function _syncWatchedSlots(slots, commit) {
    var restoredSlots = {};
    var normalized = [];
    if (Array.isArray(slots)) {
        slots.forEach(function(slotValue) {
            var slot = _slotId(slotValue);
            if (slot != null && !restoredSlots[slot]) {
                restoredSlots[slot] = true;
                normalized.push(slot);
            }
        });
    }
    document.querySelectorAll('#slot-grid .slot-chk').forEach(function(el) {
        var slot = _slotId(el.getAttribute('data-slot'));
        if (slot != null && restoredSlots[slot]) {
            el.classList.add('active');
        } else {
            el.classList.remove('active');
        }
    });
    if (commit !== false) _watchedSlotsConfirmed = normalized.slice();
    return normalized;
}
function _brNumText(value, fallback, hi) {
    var fallbackText = String(fallback == null ? 0 : fallback);
    var n = _clampNum(value, null, 0, hi == null ? null : hi);
    return n == null ? fallbackText : String(n);
}
function _brNumAttr(value, fallback, hi) {
    return _escAttr(_brNumText(value, fallback, hi));
}
function _fmtSize(bytes) {
    if (bytes < 1024) return bytes + 'B';
    if (bytes < 1048576) return Math.round(bytes/1024) + 'K';
    return (bytes/1048576).toFixed(1) + 'M';
}

var _memDataSource = 'hybrid';
var _sourceComponents = ['hp', 'level', 'stamina', 'skills', 'identity'];
var _sourceDefaults = {
    hp: 'packet',
    level: 'packet',
    stamina: 'vision',
    skills: 'packet',
    identity: 'packet'
};
var _dataSourceMap = {
    hp: 'packet',
    level: 'packet',
    stamina: 'vision',
    skills: 'packet',
    identity: 'packet'
};
function _sourceComponentName(value) {
    var key = String(value || '').trim().toLowerCase();
    return _sourceComponents.indexOf(key) >= 0 ? key : '';
}
function _sourceMode(value, fallback) {
    var text = String(value || '').trim().toLowerCase();
    if (text === 'ocr' || text === 'vision' || text === 'screen' || text === 'screen_vision') return 'vision';
    if (text === 'packet' || text === 'network' || text === 'network_capture') return 'packet';
    return fallback || 'packet';
}
function _componentDefault(component) {
    var key = _sourceComponentName(component);
    return key ? _sourceDefaults[key] : 'packet';
}
function _componentSourceValue(component, value) {
    var key = _sourceComponentName(component);
    if (!key) return '';
    if (key === 'stamina') return 'vision';
    if (key === 'skills') return 'packet';
    return _sourceMode(value, _componentDefault(key));
}
function _syncDataSourceMap(map) {
    var source = (map && typeof map === 'object') ? map : {};
    _sourceComponents.forEach(function(key) {
        _dataSourceMap[key] = _componentSourceValue(key, source[key]);
    });
}
function _memSourceValue(value) {
    var mode = String(value || '').trim().toLowerCase();
    return ['tcp', 'memory', 'hybrid', 'auto'].indexOf(mode) >= 0 ? mode : 'hybrid';
}
function _sourceLabel(mode) {
    return _sourceMode(mode, 'packet') === 'vision' ? 'VISION' : 'NETWORK';
}
function _memSourceLabel(mode) {
    mode = _memSourceValue(mode);
    if (mode === 'memory') return 'MEM';
    if (mode === 'hybrid') return 'HYBRID';
    if (mode === 'auto') return 'AUTO';
    return 'TCP';
}
function _syncMemSourceButtons() {
    _memDataSource = _memSourceValue(_memDataSource);
    document.querySelectorAll('.mem-source-btn').forEach(function(btn) {
        btn.classList.toggle('active', _memSourceValue(btn.getAttribute('data-mode')) === _memDataSource);
    });
}
function _syncSourceButtons() {
    document.querySelectorAll('.source-mode-btn').forEach(function(btn) {
        var component = _sourceComponentName(btn.getAttribute('data-component'));
        var mode = _sourceMode(btn.getAttribute('data-mode'), 'packet');
        btn.classList.toggle('active', component && _componentSourceValue(component, _dataSourceMap[component]) === mode);
    });
}
function _updateSourceSummary() {
    _syncDataSourceMap(_dataSourceMap);
    var el = document.getElementById('data-source-summary');
    if (!el) return;
    el.textContent = [
        'DATA: ' + _memSourceLabel(_memDataSource),
        'HP: ' + _sourceLabel(_dataSourceMap.hp),
        'LV: ' + _sourceLabel(_dataSourceMap.level),
        'STA: ' + _sourceLabel(_dataSourceMap.stamina),
        'SKILL: ' + _sourceLabel(_dataSourceMap.skills)
    ].join(' · ');
}
function openSourceDialog() {
    playSound('snd-panel');
    _syncSourceButtons();
    document.getElementById('source-dialog').classList.add('show');
}
function closeSourceDialog() {
    document.getElementById('source-dialog').classList.remove('show');
}

function _srBridgeCheckbox(el, methodName, requested, label) {
    if (typeof _setBridgeBackedCheckbox === 'function') {
        _setBridgeBackedCheckbox(el, methodName, requested, label);
        return;
    }
    var api = window.pywebview && window.pywebview.api;
    var on = !!requested;
    if (!api || !api[methodName]) {
        el.checked = !on;
        showAlert(label, 'pywebview API 不可用 / pywebview API is not available', true);
        return;
    }
    Promise.resolve(api[methodName](on)).then(function(result) {
        var data = typeof _menuToggleResult === 'function' ? _menuToggleResult(result, on) : { ok: true, enabled: on };
        if (!data || data.ok === false) {
            el.checked = data && data.enabled !== undefined ? !!data.enabled : !on;
            showAlert(label, _menuApiMessage(data, '设置失败 / Unable to update setting'), true);
            return;
        }
        el.checked = !!data.enabled;
        showToast(label + ': ' + (el.checked ? 'ON' : 'OFF'));
    }).catch(function(err) {
        el.checked = !on;
        showAlert(label, String(err || '设置失败 / Unable to update setting'), true);
    });
}

var _pickerMode = 'file';
var _pickerConsumer = '';
var _pickerCurrentDir = '';
function _pickerPayload(dataJson) {
    if (typeof dataJson === 'string') {
        try { return JSON.parse(dataJson) || {}; } catch (e) { return {}; }
    }
    return (dataJson && typeof dataJson === 'object') ? dataJson : {};
}
function _pickerModeValue(value) {
    return value === 'folder' ? 'folder' : 'file';
}
function _pickerEntry(entry) {
    return (entry && typeof entry === 'object') ? entry : {};
}
function _pickerEntryText(value) {
    if (value == null) return '';
    var text = String(value);
    return text ? text : '';
}
function _pickerConsumerLabel(consumer) {
    return consumer === 'boss_raid' ? 'BOSS RAID' : 'AUTO KEYS';
}
function _pickerParseResult(consumer, result) {
    var fn = window['_' + (consumer === 'boss_raid' ? 'br' : 'ak') + 'ParseApiResult'];
    return typeof fn === 'function' ? fn(result) : result;
}
function showFilePicker(dataJson) {
    var data = _pickerPayload(dataJson);
    _pickerMode = _pickerModeValue(data.mode);
    _pickerCurrentDir = _pickerEntryText(data.current);
    var parentPath = _pickerEntryText(data.parent);
    var dirs = Array.isArray(data.dirs) ? data.dirs : [];
    var files = Array.isArray(data.files) ? data.files : [];
    document.getElementById('picker-title').textContent = _pickerMode === 'folder' ? '选择文件夹' : '选择文件';
    document.getElementById('picker-path').textContent = _pickerCurrentDir;
    document.getElementById('filepicker-box').className =
        'saoPickerBox' + (_pickerMode === 'folder' ? ' picker-mode-folder' : '');
    var list = document.getElementById('picker-list');
    list.innerHTML = '';
    if (parentPath) {
        var upEl = document.createElement('div');
        upEl.className = 'picker-item is-up';
        upEl.innerHTML = '<i class="menu-glyph mi-back"></i><span class="item-name">..</span>';
        upEl.addEventListener('click', function() { _pickerBrowse(parentPath); });
        list.appendChild(upEl);
    }
    dirs.forEach(function(rawDir) {
        var d = _pickerEntry(rawDir);
        var el = document.createElement('div');
        el.className = 'picker-item is-dir';
        el.innerHTML = '<i class="menu-glyph mi-folder"></i><span class="item-name">' + _escHtml(_pickerEntryText(d.name)) + '</span>';
        el.addEventListener('click', function() { _pickerBrowse(_pickerEntryText(d.path)); });
        list.appendChild(el);
    });
    files.forEach(function(rawFile) {
        var f = _pickerEntry(rawFile);
        var el = document.createElement('div');
        el.className = 'picker-item';
        var sizeBytes = _clampNum(f.size, 0, 0, Number.MAX_SAFE_INTEGER);
        var infoStr = sizeBytes > 0 ? '<span class="item-info">' + _fmtSize(sizeBytes) + '</span>' : '';
        el.innerHTML = '<i class="menu-glyph mi-mid"></i><span class="item-name">' + _escHtml(_pickerEntryText(f.name)) + '</span>' + infoStr;
        el.addEventListener('click', function() {
            var consumer = _pickerConsumer || 'auto_key';
            var filePath = _pickerEntryText(f.path);
            playSound('snd-click');
            if (window.pywebview && window.pywebview.api && window.pywebview.api.select_file) {
                var request = Promise.resolve().then(function() {
                    return window.pywebview.api.select_file(filePath, consumer);
                });
                closeFilePicker();
                request.then(function(result) {
                    var parsed = _pickerParseResult(consumer, result);
                    if (parsed && parsed.state) {
                        if (consumer === 'boss_raid') _syncBossRaidState(parsed.state);
                        else _syncAutoKeyState(parsed.state);
                    }
                    if (parsed && parsed.ok) showToast(consumer === 'boss_raid' ? 'BOSS RAID IMPORTED' : 'IMPORT COMPLETE');
                    else if (parsed && parsed.message) showAlert(_pickerConsumerLabel(consumer), parsed.message, true);
                }).catch(function(err) {
                    showAlert(_pickerConsumerLabel(consumer), String(err || '无法导入文件 / Unable to import file'), true);
                });
            } else {
                closeFilePicker();
                showAlert(_pickerConsumerLabel(consumer), 'pywebview API 不可用 / pywebview API is not available', true);
            }
        });
        list.appendChild(el);
    });
    if (!parentPath && !dirs.length && !files.length) {
        list.innerHTML = '<div class="picker-empty">暂无 MIDI 文件</div>';
    }
    playSound('snd-alert');
    document.getElementById('filepicker-dialog').classList.add('show');
}
function closeFilePicker() {
    document.getElementById('filepicker-dialog').classList.remove('show');
    _pickerConsumer = '';
}
function _pickerBrowse(path) {
    if (!window.pywebview || !window.pywebview.api || !window.pywebview.api.browse_dir) {
        showAlert('FILE PICKER', 'pywebview API 不可用 / pywebview API is not available', true);
        return;
    }
    Promise.resolve().then(function() {
        return window.pywebview.api.browse_dir(path);
    }).then(function(result) {
        var d = _pickerPayload(result);
        if (d.error) showAlert('FILE PICKER', _pickerEntryText(d.error), true);
        d.mode = _pickerModeValue(_pickerMode);
        showFilePicker(d);
    }).catch(function(err) {
        showAlert('FILE PICKER', String(err || '无法打开目录 / Unable to open folder'), true);
    });
}

var sessionPlayersEl = document.getElementById('session-players');
var _sessionPlayersPayload = { ok: true, count: 0, self_uid: '', players: [] };
var _sessionPlayersVisible = false;
var _sessionPlayersManualHidden = false;
var _sessionPlayersAnimTimer = null;
var _spRenderRows = [];
var _spRenderedCount = 0;
var _spRenderToken = 0;
var _spRenderPending = false;
var _SP_INITIAL_ROWS = 80;
var _SP_BATCH_ROWS = 120;
function _syncSessionPlayersAnchor(withChild) {
    if (!sessionPlayersEl) return;
    sessionPlayersEl.classList.toggle('with-child', !!withChild);
}
function _spText(value, fallback) {
    if (value == null) return fallback;
    var text = String(value);
    return text ? text : fallback;
}
function _makeSessionPlayerRow(row, idx) {
    row = (row && typeof row === 'object') ? row : {};
    var el = document.createElement('div');
    el.className = 'sp-row' + (row.is_self ? ' self' : '');
    el.style.setProperty('--sp-delay', (Math.max(0, idx || 0) % 12) * 18 + 'ms');
    var name = _spText(row.name, '--');
    if (row.is_self && name !== '--') name = '* ' + name;
    el.innerHTML =
        '<div class="sp-name" title="' + _escHtml(_spText(row.name, '')) + '">' + _escHtml(name) + '</div>' +
        '<div class="sp-uid">' + _escHtml(_spText(row.uid, '--')) + '</div>' +
        '<div class="sp-power">' + _escHtml(_spText(row.fight_power, '--')) + '</div>';
    return el;
}
function _syncSessionPlayersMore(list) {
    var old = list.querySelector('.sp-more');
    if (old) old.remove();
    if (_spRenderedCount >= _spRenderRows.length) return;
    var more = document.createElement('div');
    more.className = 'sp-more';
    more.textContent = '继续滚动加载 · 已载入 ' + _spRenderedCount + '/' + _spRenderRows.length;
    list.appendChild(more);
}
function _renderSessionPlayersBatch(token, batchSize) {
    var list = document.getElementById('sp-list');
    if (!list || token !== _spRenderToken) return;
    _spRenderPending = false;
    var old = list.querySelector('.sp-more');
    if (old) old.remove();
    var start = _spRenderedCount;
    var end = Math.min(_spRenderRows.length, start + Math.max(1, batchSize || _SP_BATCH_ROWS));
    var frag = document.createDocumentFragment();
    for (var i = start; i < end; i++) frag.appendChild(_makeSessionPlayerRow(_spRenderRows[i], i));
    list.appendChild(frag);
    _spRenderedCount = end;
    _syncSessionPlayersMore(list);
    if (_spRenderedCount < _spRenderRows.length && list.scrollHeight <= list.clientHeight + 8) {
        _scheduleSessionPlayersBatch(_SP_BATCH_ROWS);
    }
}
function _scheduleSessionPlayersBatch(batchSize) {
    if (_spRenderPending || _spRenderedCount >= _spRenderRows.length) return;
    _spRenderPending = true;
    var token = _spRenderToken;
    window.requestAnimationFrame(function() {
        _renderSessionPlayersBatch(token, batchSize || _SP_BATCH_ROWS);
    });
}
function _maybeRenderMoreSessionPlayers() {
    var list = document.getElementById('sp-list');
    if (!list || _spRenderedCount >= _spRenderRows.length) return;
    if (list.scrollTop + list.clientHeight >= list.scrollHeight - 96) {
        _scheduleSessionPlayersBatch(_SP_BATCH_ROWS);
    }
}
function _renderSessionPlayers(dataJson, reveal) {
    var data = dataJson || _sessionPlayersPayload;
    if (typeof data === 'string') {
        try { data = JSON.parse(data); } catch (e) { data = {}; }
    }
    _sessionPlayersPayload = data || { ok: true, count: 0, self_uid: '', players: [] };
    var rows = Array.isArray(_sessionPlayersPayload.players) ? _sessionPlayersPayload.players : [];
    var total = _clampInt(_sessionPlayersPayload.count, rows.length, 0, 999999);
    var summary = document.getElementById('sp-summary');
    var list = document.getElementById('sp-list');
    if (!summary || !list) return;
    _spRenderToken += 1;
    _spRenderPending = false;
    _spRenderRows = rows;
    _spRenderedCount = 0;
    summary.textContent = '本次登录出现过 ' + total + ' 人' +
        (_sessionPlayersPayload.self_uid ? ' · SELF ' + _sessionPlayersPayload.self_uid : '');
    list.textContent = '';
    list.scrollTop = 0;
    if (!rows.length) list.innerHTML = '<div class="sp-empty">等待抓包识别玩家<br>NO SESSION PLAYERS YET</div>';
    else _renderSessionPlayersBatch(_spRenderToken, _SP_INITIAL_ROWS);
    if (reveal && sessionPlayersEl && menuOpen) {
        var wasVisible = _sessionPlayersVisible && sessionPlayersEl.classList.contains('show');
        _sessionPlayersVisible = true;
        sessionPlayersEl.classList.add('show');
        if (!wasVisible) _playSessionPlayersOpenAnimation();
    }
}
function _playSessionPlayersOpenAnimation() {
    if (!sessionPlayersEl) return;
    var box = sessionPlayersEl.querySelector('.sessionPlayersBox');
    if (_sessionPlayersAnimTimer) clearTimeout(_sessionPlayersAnimTimer);
    _sessionPlayersAnimTimer = null;
    sessionPlayersEl.classList.remove('session-open-anim');
    sessionPlayersEl.style.animation = 'none';
    if (box) {
        box.style.animation = 'none';
        void sessionPlayersEl.offsetWidth;
        void box.offsetWidth;
        box.style.animation = '';
    } else {
        void sessionPlayersEl.offsetWidth;
    }
    sessionPlayersEl.style.animation = '';
    sessionPlayersEl.classList.add('session-open-anim');
    _sessionPlayersAnimTimer = setTimeout(function() {
        if (sessionPlayersEl) sessionPlayersEl.classList.remove('session-open-anim');
        _sessionPlayersAnimTimer = null;
    }, 1040);
}
function setSessionPlayersPayload(dataJson) {
    _renderSessionPlayers(dataJson, _sessionPlayersVisible || (menuOpen && !_sessionPlayersManualHidden));
}
function showSessionPlayers(dataJson) {
    _sessionPlayersManualHidden = false;
    _renderSessionPlayers(dataJson, true);
}
function toggleSessionPlayers(dataJson) {
    showSessionPlayers(dataJson || _sessionPlayersPayload);
}
function closeSessionPlayers(manual) {
    _sessionPlayersVisible = false;
    _sessionPlayersManualHidden = !!manual;
    if (sessionPlayersEl) {
        sessionPlayersEl.classList.remove('show');
        sessionPlayersEl.classList.remove('session-open-anim');
    }
}
(function() {
    var close = document.getElementById('picker-close');
    if (close) close.addEventListener('click', closeFilePicker);
    var useFolder = document.getElementById('picker-use-folder');
    if (useFolder) useFolder.addEventListener('click', function() {
        var folderPath = _pickerCurrentDir;
        closeFilePicker();
        _callMenuSettingApi('select_folder', [folderPath], 'FILE PICKER');
    });
    var sourceClose = document.getElementById('source-close');
    if (sourceClose) sourceClose.addEventListener('click', closeSourceDialog);
    var list = document.getElementById('sp-list');
    if (list) list.addEventListener('scroll', _maybeRenderMoreSessionPlayers, { passive: true });
})();

document.querySelectorAll('.source-mode-btn').forEach(function(btn) {
    btn.addEventListener('click', function(e) {
        e.stopPropagation();
        var component = _sourceComponentName(this.getAttribute('data-component'));
        var mode = _componentSourceValue(component, this.getAttribute('data-mode'));
        if (!component || !mode) return;
        var previousMap = {};
        _sourceComponents.forEach(function(key) { previousMap[key] = _dataSourceMap[key]; });
        playSound('snd-click');
        _dataSourceMap[component] = mode;
        _syncSourceButtons();
        _updateSourceSummary();
        _callMenuSettingApi('set_component_source', [component, mode], 'DATA SOURCE', function(data) {
            if (data && data.data_source_map && typeof data.data_source_map === 'object') {
                _syncDataSourceMap(data.data_source_map);
            } else if (data && data.component !== undefined && data.mode !== undefined) {
                var returnedComponent = _sourceComponentName(data.component);
                if (returnedComponent) _dataSourceMap[returnedComponent] = _componentSourceValue(returnedComponent, data.mode);
            }
            _syncSourceButtons();
            _updateSourceSummary();
        }, function() {
            _syncDataSourceMap(previousMap);
            _syncSourceButtons();
            _updateSourceSummary();
        });
    });
});
document.querySelectorAll('.mem-source-btn').forEach(function(btn) {
    btn.addEventListener('click', function(e) {
        e.stopPropagation();
        var mode = _memSourceValue(this.getAttribute('data-mode'));
        var previousMode = _memDataSource;
        playSound('snd-click');
        _memDataSource = mode;
        _syncMemSourceButtons();
        _updateSourceSummary();
        _callMenuSettingApi('set_data_source', [mode], 'DATA SOURCE', function(data) {
            _memDataSource = _memSourceValue(data && data.mode !== undefined ? data.mode : mode);
            _syncMemSourceButtons();
            _updateSourceSummary();
        }, function() {
            _memDataSource = _memSourceValue(previousMode);
            _syncMemSourceButtons();
            _updateSourceSummary();
        });
    });
});

/* ═══ Skill Effects — Slot checkboxes ═══ */
document.querySelectorAll('#slot-grid .slot-chk').forEach(function(el) {
    el.addEventListener('click', function(e) {
        e.stopPropagation();
        var previousSlots = _watchedSlotsConfirmed.slice();
        this.classList.toggle('active');
        playSound('snd-click');
        _saveWatchedSlots(previousSlots);
    });
});
function _saveWatchedSlots(previousSlots) {
    var slots = _watchedSlotsList();
    var requestSeq = ++_watchedSlotsRequestSeq;
    _callMenuSettingApi('set_watched_slots', [slots], 'BURST SLOTS', function(data) {
        if (requestSeq !== _watchedSlotsRequestSeq) return;
        _syncWatchedSlots(data && Array.isArray(data.slots) ? data.slots : slots);
    }, function() {
        if (requestSeq !== _watchedSlotsRequestSeq) return false;
        _watchedSlotsRequestSeq += 1;
        _syncWatchedSlots(Array.isArray(previousSlots) ? previousSlots : _watchedSlotsConfirmed);
    });
}
_syncWatchedSlots(_watchedSlotsList());
/* Burst enabled toggle */
document.getElementById('burst-enabled').addEventListener('change', function(e) {
    e.stopPropagation();
    playSound('snd-click');
    var on = !!this.checked;
    _srBridgeCheckbox(this, 'set_burst_enabled', on, 'BURST ALERT');
});

/* ═══ Sound controls ═══ */
document.getElementById('sound-enabled').addEventListener('change', function(e) {
    e.stopPropagation();
    playSound('snd-click');
    var on = !!this.checked;
    _srBridgeCheckbox(this, 'set_sound_enabled', on, 'SOUND');
});
document.getElementById('sound-volume').addEventListener('input', function(e) {
    e.stopPropagation();
    var previousVolume = _soundVolumeValue;
    var volume = _setSoundVolumeUI(this.value, false);
    var requestSeq = ++_soundVolumeRequestSeq;
    _callMenuSettingApi('set_sound_volume', [volume], 'SOUND', function(data) {
        if (requestSeq !== _soundVolumeRequestSeq) return;
        _setSoundVolumeUI(data && data.volume !== undefined ? data.volume : volume);
    }, function() {
        if (requestSeq !== _soundVolumeRequestSeq) return false;
        _setSoundVolumeUI(previousVolume);
    });
});

/* ═══ Data source panel ═══ */
document.getElementById('open-data-source-panel').addEventListener('click', function(e) {
    e.stopPropagation();
    openSourceDialog();
});
document.getElementById('source-close').addEventListener('click', function(e) {
    e.stopPropagation();
    closeSourceDialog();
});

/* === Auto-Key API & State === */

function _akParseApiResult(result) {
    if (!result) return {};
    if (typeof result === 'string') {
        try { return JSON.parse(result); } catch (e) { return { ok: false, message: result }; }
    }
    return result;
}

function _akClone(value) {
    return value ? JSON.parse(JSON.stringify(value)) : null;
}

function _menuTabName(value) {
    var key = String(value == null ? '' : value);
    var allowed = { local: true, editor: true, cloud: true };
    return allowed[key] ? key : 'local';
}

function _profileEntry(entry) {
    return (entry && typeof entry === 'object') ? entry : {};
}

function _profileEntries(value) {
    return Array.isArray(value) ? value : [];
}

function _profileText(value, fallback) {
    if (value == null) return fallback;
    var text = String(value);
    return text ? text : fallback;
}

function _srUserInfoEl(id) {
    return document.getElementById(id);
}
function _srSetUserInfoText(id, value, fallback) {
    var el = _srUserInfoEl(id);
    if (!el) return;
    el.textContent = _profileText(value, fallback || '');
}
function updateStarResonanceInfo(data) {
    data = _profileEntry(data);
    if (data.username !== undefined) _srSetUserInfoText('info-username', data.username, '');
    if (data.level !== undefined) _srSetUserInfoText('info-level', 'Lv.' + _profileText(data.level, ''), '');
    if (data.xp_pct !== undefined) {
        var xpFill = _srUserInfoEl('info-xp');
        if (xpFill) xpFill.style.width = _clampNum(data.xp_pct, 0, 0, 100) + '%';
    }
    if (data.profession !== undefined) _srSetUserInfoText('info-profession', data.profession, '');
    if (data.hp !== undefined) _srSetUserInfoText('info-hp', data.hp, '');
    if (data.sta !== undefined) _srSetUserInfoText('info-sta', data.sta, '');
    if (data.des !== undefined) _srSetUserInfoText('info-des', data.des, '');
    var currentFile = document.getElementById('current-file');
    if (currentFile && data.file !== undefined) {
        currentFile.textContent = '♪ ' + _profileText(data.file, '');
    }
}
function _srSetUserInfoVisible(visible) {
    var infoBox = _srUserInfoEl('info-box');
    if (!infoBox) return;
    infoBox.classList.toggle('show', !!visible);
    if (visible) playSound('snd-panel');
}
function _srHandleMenuActivate(detail) {
    var name = _profileText(detail && detail.name, '');
    _srSetUserInfoVisible(name === 'userInfo');
}
function _srMenuClock() {
    var el = _srUserInfoEl('info-clock');
    if (!el) return;
    var now = new Date();
    var h = String(now.getHours()).padStart(2,'0');
    var m = String(now.getMinutes()).padStart(2,'0');
    var s = String(now.getSeconds()).padStart(2,'0');
    el.textContent = h + ':' + m + ':' + s;
}
function _srStartUserInfoClock() {
    var clockEl = _srUserInfoEl('info-clock');
    if (!clockEl || clockEl.isConnected !== true || window.__srUserInfoClockStarted) return;
    window.__srUserInfoClockStarted = true;
    _srMenuClock();
    setInterval(_srMenuClock, 1000);
}

window.addEventListener('sao-menu-activate', function(event) {
    _srHandleMenuActivate((event && event.detail) || {});
});
window.addEventListener('sao-menu-close', function() {
    _srSetUserInfoVisible(false);
});
if (window.__saoMenuInfoPayload) {
    updateStarResonanceInfo(window.__saoMenuInfoPayload);
}
if (window.currentActive === 'userInfo') {
    _srSetUserInfoVisible(true);
}

function _akNewClientId(prefix) {
    return (prefix || 'ak') + '_' + Date.now().toString(36) + '_' + Math.floor(Math.random() * 1000000).toString(36);
}

function _akProfilesFull() {
    return _profileEntries(_autoKeyState && _autoKeyState.profiles_full).map(_profileEntry);
}

function _akFindProfile(profileId) {
    var id = _profileText(profileId, '');
    if (!id) return null;
    var profiles = _akProfilesFull();
    for (var i = 0; i < profiles.length; i++) {
        if (_profileText(profiles[i].id, '') === id) return profiles[i];
    }
    return null;
}

function _akEnsureSelectedProfile() {
    var profiles = _akProfilesFull();
    if (!profiles.length) {
        _autoKeySelectedProfileId = '';
        return;
    }
    if (_akFindProfile(_autoKeySelectedProfileId)) return;
    _autoKeySelectedProfileId = _profileText((_autoKeyState && _autoKeyState.active_profile_id), _profileText(profiles[0].id, ''));
}

function _akPrepareDraft(profile) {
    var draft = _akClone(profile);
    if (!draft) return null;
    if (!draft.engine) {
        draft.engine = { tick_ms: 50, require_foreground: true, pause_on_death: true };
    }
    draft.actions = _profileEntries(draft.actions).map(_profileEntry);
    draft.actions.forEach(function(action, index) {
        action.id = _profileText(action.id, _akNewClientId('action'));
        action.label = _profileText(action.label, 'Action ' + (index + 1));
        if (typeof action.enabled !== 'boolean') action.enabled = true;
        action.conditions = _profileEntries(action.conditions).map(_profileEntry);
        action._conditions_text = JSON.stringify(action.conditions, null, 2);
        action._conditions_error = '';
    });
    return draft;
}

function _akLoadDraftFromState(profileId, force) {
    _akEnsureSelectedProfile();
    var chosenId = _profileText(profileId, _autoKeySelectedProfileId);
    var profile = _akFindProfile(chosenId);
    if (!profile) {
        _autoKeyDraftProfile = null;
        return;
    }
    _autoKeySelectedProfileId = _profileText(profile.id, '');
    if (!force && _autoKeyDraftDirty && _autoKeyDraftProfile && _profileText(_autoKeyDraftProfile.id, '') === _autoKeySelectedProfileId) {
        return;
    }
    _autoKeyDraftProfile = _akPrepareDraft(profile);
    _autoKeyDraftDirty = false;
}

function _akCallApi(methodName, args, onOk) {
    if (!window.pywebview || !window.pywebview.api || !window.pywebview.api[methodName]) {
        showAlert('AUTO KEYS', 'pywebview API 不可用 / pywebview API is not available', true);
        return;
    }
    var fn = window.pywebview.api[methodName];
    var request;
    try {
        request = Promise.resolve(fn.apply(window.pywebview.api, args || []));
    } catch (err) {
        showAlert('AUTO KEYS', String(err || '未知错误 / Unknown error'), true);
        return;
    }
    request.then(function(result) {
        var data = _akParseApiResult(result);
        if (data && data.state) {
            _syncAutoKeyState(data.state);
        }
        if (data && data.ok) {
            if (onOk) onOk(data);
            return;
        }
        showAlert('AUTO KEYS', (data && data.message) ? data.message : ('请求失败 / Request failed: ' + methodName), true);
    }).catch(function(err) {
        showAlert('AUTO KEYS', String(err || '未知错误 / Unknown error'), true);
    });
}

function _akRefreshUploadAuth(force, onOk) {
    _akCallApi('refresh_auto_key_upload_auth', [!!force], function(data) {
        if (onOk) onOk(data);
        else showToast((data && data.ok) ? 'UPLOAD AUTH READY' : 'UPLOAD AUTH UPDATED');
    });
}

function _akRenderRuntime() {
    var runtimeEl = document.getElementById('auto-key-runtime');
    var enabledEl = document.getElementById('auto-key-enabled');
    if (!runtimeEl || !enabledEl) return;
    var state = _autoKeyState || {};
    var runtime = state.runtime || {};
    enabledEl.checked = !!state.enabled;
    var activeProfileName = _profileText(state.active_profile_name, '无 None');
    var runtimeReason = _profileText(runtime.last_reason, runtime.active ? 'RUNNING' : 'IDLE');
    var lastActionLabel = _profileText(runtime.last_action_label, '');
    var bits = [];
    bits.push('引擎 Engine ' + (state.enabled ? '开启 ON' : '关闭 OFF'));
    bits.push('配置 Profile ' + activeProfileName);
    bits.push('状态 Status ' + runtimeReason);
    if (lastActionLabel) {
        bits.push('上次 Last ' + lastActionLabel);
    }
    runtimeEl.innerHTML = bits.map(function(item) {
        return '<span class="auto-key-pill ' + (state.enabled ? 'active' : '') + '">' + _escHtml(item) + '</span>';
    }).join(' ');
}

function _akRenderLocal() {
    var listEl = document.getElementById('auto-key-profile-list');
    if (!listEl) return;
    var profiles = _akProfilesFull();
    var state = _autoKeyState || {};
    if (!profiles.length) {
        listEl.innerHTML = '<div class="auto-key-empty">还没有本地配置。<br>No local profiles yet. Create one here or import a JSON file.</div>';
        return;
    }
    var summaries = {};
    _profileEntries(state.profiles).forEach(function(rawItem) {
        var item = _profileEntry(rawItem);
        summaries[_profileText(item.id, '')] = item;
    });
    listEl.innerHTML = profiles.map(function(rawProfile) {
        var profile = _profileEntry(rawProfile);
        var id = _profileText(profile.id, '');
        var summary = summaries[id] || {};
        var actionCount = _clampInt(summary.action_count, _profileEntries(profile.actions).length, 0, 999999);
        var enabledCount = _clampInt(summary.enabled_action_count, 0, 0, actionCount);
        var classes = ['auto-key-profile'];
        if (_profileText(state.active_profile_id, '') === id) classes.push('active');
        if (_profileText(_autoKeySelectedProfileId, '') === id) classes.push('selected');
        return '' +
            '<div class="' + classes.join(' ') + '">' +
                '<div class="auto-key-card-head">' +
                    '<div class="auto-key-card-title">' + _escHtml(_profileText(profile.profile_name, '配置 Profile')) + '</div>' +
                    '<div class="auto-key-card-actions">' +
                        '<button class="auto-key-btn" type="button" onclick="_akSelectProfile(' + _jsAttrArg(id) + ', \'editor\'); return false;">编辑 Edit</button>' +
                        '<button class="auto-key-btn primary" type="button" onclick="_akActivateProfile(' + _jsAttrArg(id) + '); return false;">启用 Activate</button>' +
                        // 复制/导出/删除(同类档案管理动作)聚合进「更多 ▾」, 与 Tk _make_profile_more_button 1:1
                        '<select class="auto-key-btn card-more" onchange="_akCardAction(' + _jsAttrArg(id) + ', this.value); this.selectedIndex=0; return false;">' +
                            '<option value="">更多 ▾</option>' +
                            '<option value="copy">复制 Copy</option>' +
                            '<option value="export">导出 Export</option>' +
                            '<option value="delete">删除 Delete</option>' +
                        '</select>' +
                    '</div>' +
                '</div>' +
                '<div class="auto-key-card-meta">' +
                    '<span class="auto-key-pill ' + (_profileText(state.active_profile_id, '') === id ? 'active' : '') + '">' + _escHtml(_profileText(profile.profession_name, '任意 Any').toUpperCase()) + '</span>' +
                    '<span class="auto-key-pill">' + _escHtml(_profileText(profile.source, 'local').toUpperCase()) + '</span>' +
                    '<span class="auto-key-pill">' + _escHtml(actionCount + ' 动作 Actions / ' + enabledCount + ' 已启用 On') + '</span>' +
                    '<span class="auto-key-pill">' + _escHtml(_profileText(profile.updated_at, '')) + '</span>' +
                '</div>' +
                '<div class="auto-key-card-meta">' +
                    '<span>' + _escHtml(_profileText(profile.description, '暂无说明 / No description')) + '</span>' +
                '</div>' +
            '</div>';
    }).join('');
}

function _akSummaryItem(label, value, muted) {
    return '' +
        '<div class="auto-key-summary-item">' +
            '<span class="auto-key-summary-label">' + _escHtml(label || '') + '</span>' +
            '<span class="auto-key-summary-value' + (muted ? ' muted' : '') + '">' + _escHtml(_akDisplayValue(value, '--')) + '</span>' +
        '</div>';
}

function _akRenderEditorIdentitySummary(draft) {
    var summaryEl = document.getElementById('ak-editor-identity-summary');
    if (!summaryEl) return;
    var state = _autoKeyState || {};
    var identity = state.identity || {};
    var profileProfession = (draft && draft.profession_name) ? draft.profession_name : '任意 Any';
    var profileProfessionIdValue = draft ? _clampInt(draft.profession_id, 0, 0, 999999999) : 0;
    var identityProfessionId = _clampInt(identity.profession_id, 0, 0, 999999999);
    var profileProfessionId = profileProfessionIdValue > 0 ? String(profileProfessionIdValue) : '不限制 Any';
    var identityUid = _cloudText(identity.player_uid, '--');
    var identityName = _cloudText(identity.player_name, '--');
    summaryEl.innerHTML = [
        _akSummaryItem('当前 UID Current UID', identityUid, identityUid === '--'),
        _akSummaryItem('当前玩家 Current Player', identityName, identityName === '--'),
        _akSummaryItem('当前职业 ID Current Profession ID', identityProfessionId > 0 ? String(identityProfessionId) : '--', !(identityProfessionId > 0)),
        _akSummaryItem('脚本职业限制 Profile Filter', profileProfession + ' / ' + profileProfessionId, false)
    ].join('');
}

function _akConditionType(value) {
    var key = _profileText(value, 'hp_pct_gte');
    var allowed = { hp_pct_gte: true, hp_pct_lte: true, sta_pct_gte: true, burst_ready_is: true, slot_state_is: true, profession_is: true, player_name_is: true, dungeon_is: true, last_skill_is: true, boss_mechanic_is: true, boss_mechanic_family_is: true };
    return allowed[key] ? key : 'hp_pct_gte';
}

function _akSlotState(value) {
    var key = _profileText(value, 'ready');
    var allowed = { ready: true, cooldown: true, active: true, insufficient_energy: true, unknown: true };
    return allowed[key] ? key : 'ready';
}

function _akPressMode(value) {
    var key = _profileText(value, 'tap');
    return key === 'hold' ? 'hold' : 'tap';
}

function _akBoolValue(value, fallback) {
    if (typeof value === 'boolean') return value;
    if (value == null || value === '') return !!fallback;
    var text = String(value).trim().toLowerCase();
    if (text === 'false' || text === '0' || text === 'no' || text === 'off') return false;
    if (text === 'true' || text === '1' || text === 'yes' || text === 'on') return true;
    return !!fallback;
}

function _akDefaultCondition(conditionType) {
    var type = _akConditionType(conditionType);
    if (type === 'burst_ready_is') return { type: type, value: true };
    if (type === 'slot_state_is') return { type: type, slot_index: 1, state: 'ready' };
    if (type === 'profession_is' || type === 'player_name_is' || type === 'dungeon_is' || type === 'last_skill_is' || type === 'boss_mechanic_is' || type === 'boss_mechanic_family_is') return { type: type, value: '' };
    return { type: type, value: 0.5 };
}

function _akNormalizeCondition(condition) {
    var raw = _profileEntry(condition);
    var merged = _akDefaultCondition(raw.type);
    Object.keys(raw).forEach(function(key) { merged[key] = raw[key]; });
    merged.type = _akConditionType(merged.type);
    if (merged.type === 'burst_ready_is') {
        merged.value = _akBoolValue(merged.value, true);
    } else if (merged.type === 'slot_state_is') {
        merged.slot_index = _akIntValue('slot_index', merged.slot_index);
        merged.state = _akSlotState(merged.state);
    } else if (merged.type === 'profession_is' || merged.type === 'player_name_is' || merged.type === 'dungeon_is' || merged.type === 'last_skill_is' || merged.type === 'boss_mechanic_is' || merged.type === 'boss_mechanic_family_is') {
        merged.value = _akTextValue(merged.value);
    } else {
        merged.value = _clampNum(merged.value, 0.5, 0, 1);
    }
    return merged;
}

function _akGetDraftAction(index) {
    if (!_autoKeyDraftProfile || !Array.isArray(_autoKeyDraftProfile.actions)) return null;
    return _autoKeyDraftProfile.actions[index] || null;
}

function _akGetActionConditions(index) {
    var action = _akGetDraftAction(index);
    if (!action) return [];
    var items = Array.isArray(action.conditions) ? _akClone(action.conditions) : [];
    return items.map(_akNormalizeCondition);
}

function _akSetActionConditions(index, conditions, rerender) {
    var action = _akGetDraftAction(index);
    if (!action) return;
    action.conditions = _akClone(Array.isArray(conditions) ? conditions : []);
    action._conditions_text = JSON.stringify(action.conditions, null, 2);
    action._conditions_error = '';
    _autoKeyDraftDirty = true;
    if (rerender !== false) _akRenderActionList();
}

function _akConditionTypeOptions(selected) {
    selected = _akConditionType(selected);
    return [
        ['hp_pct_gte', 'HP 至少 >='],
        ['hp_pct_lte', 'HP 不高于 <='],
        ['sta_pct_gte', 'STA 至少 >='],
        ['burst_ready_is', 'Burst 状态'],
        ['slot_state_is', '槽位状态'],
        ['profession_is', '职业名称匹配'],
        ['player_name_is', '玩家名称匹配'],
        ['dungeon_is', '副本匹配 Dungeon'],
        ['last_skill_is', '上个技能匹配 Last Skill'],
        ['boss_mechanic_is', 'Boss机制匹配 Mechanic'],
        ['boss_mechanic_family_is', 'Boss机制族匹配 Family']
    ].map(function(item) {
        return '<option value="' + _escHtml(item[0]) + '"' + (selected === item[0] ? ' selected' : '') + '>' + _escHtml(item[1]) + '</option>';
    }).join('');
}

function _akSlotStateOptions(selected) {
    selected = _akSlotState(selected);
    return [
        ['ready', '就绪 Ready'],
        ['cooldown', '冷却中 Cooldown'],
        ['active', '施放中 Active'],
        ['insufficient_energy', '能量不足 No Energy'],
        ['unknown', '未知 Unknown']
    ].map(function(item) {
        return '<option value="' + _escHtml(item[0]) + '"' + (selected === item[0] ? ' selected' : '') + '>' + _escHtml(item[1]) + '</option>';
    }).join('');
}

function _akTextValue(value) {
    return value == null ? '' : String(value);
}

function _akDisplayValue(value, fallback) {
    if (value == null) return fallback;
    var text = String(value);
    return text ? text : fallback;
}

function _cloudEntry(entry) {
    return (entry && typeof entry === 'object') ? entry : {};
}

function _cloudEntries(value) {
    return Array.isArray(value) ? value : [];
}

function _cloudText(value, fallback) {
    if (value == null) return fallback;
    var text = String(value);
    return text ? text : fallback;
}

function _cloudInputText(id) {
    var el = document.getElementById(id);
    return el ? _cloudText(el.value, '').trim() : '';
}

function _cloudSetInputText(id, value) {
    var el = document.getElementById(id);
    if (el) el.value = _cloudText(value, '');
}

function _cloudMissingText(value) {
    var values = Array.isArray(value) ? value : ((value == null || value === '') ? [] : [value]);
    var text = values.map(function(item) {
        return _cloudText(item, '');
    }).filter(function(item) {
        return !!item;
    }).join(', ');
    return text || 'identity';
}

function _akRenderConditionFields(index, condIndex, condition) {
    var type = _akConditionType(condition.type);
    if (type === 'slot_state_is') {
        return '' +
            '<div class="auto-key-field"><label>槽位 Slot</label><input type="number" min="1" max="9" value="' + _escHtml(String(_akIntValue('slot_index', condition.slot_index))) + '" oninput="_akDraftConditionField(' + index + ', ' + condIndex + ', \'slot_index\', this.value, \'int\')" /></div>' +
            '<div class="auto-key-field"><label>状态 State</label><select onchange="_akDraftConditionField(' + index + ', ' + condIndex + ', \'state\', this.value)">' + _akSlotStateOptions(condition.state) + '</select></div>';
    }
    if (type === 'burst_ready_is') {
        var boolValue = _akBoolValue(condition.value, true);
        return '' +
            '<div class="auto-key-field"><label>目标值 Value</label><select onchange="_akDraftConditionField(' + index + ', ' + condIndex + ', \'value\', this.value, \'bool\')">' +
                '<option value="true"' + (boolValue ? ' selected' : '') + '>已准备 Ready</option>' +
                '<option value="false"' + (!boolValue ? ' selected' : '') + '>未准备 Not Ready</option>' +
            '</select></div>';
    }
    if (type === 'profession_is' || type === 'player_name_is' || type === 'dungeon_is' || type === 'last_skill_is' || type === 'boss_mechanic_is' || type === 'boss_mechanic_family_is') {
        return '' +
            '<div class="auto-key-field span-2"><label>匹配值 Match Value</label><input type="text" value="' + _escHtml(_akTextValue(condition.value)) + '" oninput="_akDraftConditionField(' + index + ', ' + condIndex + ', \'value\', this.value)" /></div>';
    }
    return '' +
        '<div class="auto-key-field"><label>阈值 Threshold (%)</label><input type="number" min="0" max="100" value="' + _escHtml(String(Math.round(_clampNum(condition.value || 0, 0, 0, 1) * 100))) + '" oninput="_akDraftConditionField(' + index + ', ' + condIndex + ', \'value\', this.value, \'pct\')" /></div>';
}

function _akRenderConditionCard(index, condition, condIndex) {
    return '' +
        '<div class="auto-key-condition-card">' +
            '<div class="auto-key-condition-head">' +
                '<div class="auto-key-condition-title">条件 Condition ' + (condIndex + 1) + '</div>' +
                '<div class="auto-key-card-actions">' +
                    '<button class="auto-key-btn warn" type="button" onclick="_akDeleteCondition(' + index + ', ' + condIndex + '); return false;">删除 Delete</button>' +
                '</div>' +
            '</div>' +
            '<div class="auto-key-condition-grid">' +
                '<div class="auto-key-field"><label>条件类型 Type</label><select onchange="_akSetConditionType(' + index + ', ' + condIndex + ', this.value)">' + _akConditionTypeOptions(condition.type) + '</select></div>' +
                _akRenderConditionFields(index, condIndex, condition) +
            '</div>' +
        '</div>';
}

function _akAddCondition(index, conditionType) {
    var conditions = _akGetActionConditions(index);
    conditions.push(_akDefaultCondition(conditionType));
    _akSetActionConditions(index, conditions, true);
}

function _akDeleteCondition(index, condIndex) {
    var conditions = _akGetActionConditions(index);
    conditions.splice(condIndex, 1);
    _akSetActionConditions(index, conditions, true);
}

function _akSetConditionType(index, condIndex, conditionType) {
    var conditions = _akGetActionConditions(index);
    if (!conditions[condIndex]) return;
    conditions[condIndex] = _akDefaultCondition(conditionType);
    _akSetActionConditions(index, conditions, true);
}

function _akDraftConditionField(index, condIndex, fieldName, value, kind) {
    var conditions = _akGetActionConditions(index);
    var condition = conditions[condIndex];
    if (!condition) return;
    if (kind === 'int') value = _akIntValue(fieldName, value);
    if (kind === 'bool') value = String(value) === 'true';
    if (fieldName === 'state') value = _akSlotState(value);
    if (kind === 'pct') {
        var pct = _clampNum(value, 0, 0, 100);
        value = pct / 100;
    }
    condition[fieldName] = value;
    _akSetActionConditions(index, conditions, false);
}

function _akPopulateEditorFields() {
    var draft = _autoKeyDraftProfile;
    var listEl = document.getElementById('auto-key-action-list');
    if (!draft) {
        if (listEl) {
            listEl.innerHTML = '<div class="auto-key-empty">先从本地列表选择一个配置，或新建一个配置再开始编辑。<br>Select a profile from Local or create a new one to start editing.</div>';
        }
        ['ak-profile-name', 'ak-profile-profession-name', 'ak-profile-profession-id', 'ak-engine-tick-ms', 'ak-profile-description'].forEach(function(id) {
            var el = document.getElementById(id);
            if (el) el.value = '';
        });
        if (document.getElementById('ak-engine-require-foreground')) document.getElementById('ak-engine-require-foreground').checked = true;
        if (document.getElementById('ak-engine-pause-on-death')) document.getElementById('ak-engine-pause-on-death').checked = true;
        _akRenderEditorIdentitySummary(null);
        return;
    }
    document.getElementById('ak-profile-name').value = _profileText(draft.profile_name, '');
    document.getElementById('ak-profile-profession-name').value = _profileText(draft.profession_name, '');
    document.getElementById('ak-profile-profession-id').value = _akIntValue('profession_id', draft.profession_id);
    document.getElementById('ak-engine-tick-ms').value = _akIntValue('tick_ms', draft.engine && draft.engine.tick_ms);
    document.getElementById('ak-profile-description').value = _profileText(draft.description, '');
    document.getElementById('ak-engine-require-foreground').checked = !(draft.engine && draft.engine.require_foreground === false);
    document.getElementById('ak-engine-pause-on-death').checked = !(draft.engine && draft.engine.pause_on_death === false);
    _akRenderEditorIdentitySummary(draft);
    _akRenderActionList();
}

function _akRenderActionList() {
    var listEl = document.getElementById('auto-key-action-list');
    if (!listEl) return;
    var draft = _autoKeyDraftProfile;
    var actions = draft ? _profileEntries(draft.actions).map(_profileEntry) : [];
    if (draft) draft.actions = actions;
    if (!draft || !actions.length) {
        listEl.innerHTML = '<div class="auto-key-empty">还没有动作。先新增一个动作并绑定到技能槽。<br>No actions yet. Add one and bind it to a skill slot.</div>';
        return;
    }
    listEl.innerHTML = actions.map(function(action, index) {
        var conditions = _akGetActionConditions(index);
        var jsonText = _profileText(action._conditions_text, JSON.stringify(_profileEntries(action.conditions), null, 2));
        var jsonStatus = action._conditions_error ?
            '<div class="auto-key-json-status auto-key-status-error">JSON 未应用 / JSON not applied: ' + _escHtml(action._conditions_error) + '</div>' :
            '<div class="auto-key-json-status">条件卡片会自动同步到高级 JSON。修改高级 JSON 后，离开输入框时会尝试应用。</div>';
        return '' +
            '<div class="auto-key-action-card">' +
                '<div class="auto-key-card-head">' +
                    '<div class="auto-key-card-title">动作 Action ' + (index + 1) + ': ' + _escHtml(_profileText(action.label, '动作 Action ' + (index + 1))) + '</div>' +
                    '<div class="auto-key-card-actions">' +
                        '<button class="auto-key-btn" type="button" onclick="_akMoveAction(' + index + ', -1); return false;">上移 Up</button>' +
                        '<button class="auto-key-btn" type="button" onclick="_akMoveAction(' + index + ', 1); return false;">下移 Down</button>' +
                        '<button class="auto-key-btn" type="button" onclick="_akCloneAction(' + index + '); return false;">复制 Copy</button>' +
                        '<button class="auto-key-btn warn" type="button" onclick="_akDeleteAction(' + index + '); return false;">删除 Delete</button>' +
                    '</div>' +
                '</div>' +
                '<div class="auto-key-toolbar">' +
                    '<label class="sao-toggle" style="max-width:180px;">' +
                        '<input type="checkbox" ' + (action.enabled ? 'checked' : '') + ' onchange="_akDraftActionField(' + index + ', \'enabled\', this.checked, \'bool\')" />' +
                        '<div class="toggle-track"><div class="toggle-thumb"></div></div>' +
                        '<span>动作启用 Action Enabled</span>' +
                    '</label>' +
                    '<button class="auto-key-btn" type="button" onclick="_akAddCondition(' + index + '); return false;">新增条件 Add Condition</button>' +
                '</div>' +
                '<div class="auto-key-grid compact">' +
                    '<div class="auto-key-field"><label>名称 Label</label><input type="text" value="' + _escHtml(_profileText(action.label, '')) + '" oninput="_akDraftActionField(' + index + ', \'label\', this.value)" /></div>' +
                    '<div class="auto-key-field"><label>槽位 Slot</label><input type="number" min="1" max="9" value="' + _escHtml(String(_akIntValue('slot_index', action.slot_index))) + '" oninput="_akDraftActionField(' + index + ', \'slot_index\', this.value, \'int\')" /></div>' +
                    '<div class="auto-key-field"><label>按键 Key</label><input type="text" value="' + _escHtml(_profileText(action.key, '')) + '" oninput="_akDraftActionField(' + index + ', \'key\', this.value)" /></div>' +
                    '<div class="auto-key-field"><label>按下模式 Press Mode</label><select onchange="_akDraftActionField(' + index + ', \'press_mode\', this.value)"><option value="tap"' + (action.press_mode === 'tap' ? ' selected' : '') + '>点击 Tap</option><option value="hold"' + (action.press_mode === 'hold' ? ' selected' : '') + '>按住 Hold</option></select></div>' +
                    '<div class="auto-key-field"><label>次数 Press Count</label><input type="number" min="1" max="20" value="' + _escHtml(String(_akIntValue('press_count', action.press_count))) + '" oninput="_akDraftActionField(' + index + ', \'press_count\', this.value, \'int\')" /></div>' +
                    '<div class="auto-key-field"><label>间隔毫秒 Press Interval MS</label><input type="number" min="0" value="' + _escHtml(String(_akIntValue('press_interval_ms', action.press_interval_ms))) + '" oninput="_akDraftActionField(' + index + ', \'press_interval_ms\', this.value, \'int\')" /></div>' +
                    '<div class="auto-key-field"><label>按住毫秒 Hold MS</label><input type="number" min="0" value="' + _escHtml(String(_akIntValue('hold_ms', action.hold_ms))) + '" oninput="_akDraftActionField(' + index + ', \'hold_ms\', this.value, \'int\')" /></div>' +
                    '<div class="auto-key-field"><label>就绪延迟 Ready Delay MS</label><input type="number" min="0" value="' + _escHtml(String(_akIntValue('ready_delay_ms', action.ready_delay_ms))) + '" oninput="_akDraftActionField(' + index + ', \'ready_delay_ms\', this.value, \'int\')" /></div>' +
                    '<div class="auto-key-field"><label>最小复位 Min Rearm MS</label><input type="number" min="0" value="' + _escHtml(String(_akIntValue('min_rearm_ms', action.min_rearm_ms))) + '" oninput="_akDraftActionField(' + index + ', \'min_rearm_ms\', this.value, \'int\')" /></div>' +
                    '<div class="auto-key-field"><label>后置延迟 Post Delay MS</label><input type="number" min="0" value="' + _escHtml(String(_akIntValue('post_delay_ms', action.post_delay_ms))) + '" oninput="_akDraftActionField(' + index + ', \'post_delay_ms\', this.value, \'int\')" /></div>' +
                '</div>' +
                '<div class="auto-key-condition-list">' +
                    (conditions.length ? conditions.map(function(condition, condIndex) {
                        return _akRenderConditionCard(index, condition, condIndex);
                    }).join('') : '<div class="auto-key-empty compact">这个动作还没有触发条件，表示只要技能槽就绪就会尝试按键。<br>No conditions yet. It will fire whenever the slot is ready.</div>') +
                '</div>' +
                '<details class="auto-key-details">' +
                    '<summary>高级 JSON Conditions JSON</summary>' +
                    '<div class="auto-key-details-body">' +
                        '<div class="auto-key-field"><label>Conditions JSON</label><textarea class="auto-key-mono" oninput="_akDraftActionConditions(' + index + ', this.value)" onchange="_akApplyActionConditionsJson(' + index + ', this.value)">' + _escHtml(jsonText) + '</textarea></div>' +
                        jsonStatus +
                    '</div>' +
                '</details>' +
                '<div class="auto-key-condition-note">支持的条件类型 Supported types: hp_pct_gte, hp_pct_lte, sta_pct_gte, burst_ready_is, slot_state_is, profession_is, player_name_is.</div>' +
            '</div>';
    }).join('');
}

function _akRenderCloud() {
    var state = _autoKeyState || {};
    var identity = state.identity || {};
    var auth = state.upload_auth || {};
    var search = state.last_remote_search || {};
    var query = search.query || {};
    var resultsEl = document.getElementById('auto-key-cloud-results');
    var uploadMetaEl = document.getElementById('ak-upload-auth-meta');
    var uploadBtn = document.getElementById('ak-upload-active-btn');
    _cloudSetInputText('ak-server-url', state.server_url);
    _cloudSetInputText('ak-search-q', query.q);
    _cloudSetInputText('ak-search-profile-name', query.profile_name);
    _cloudSetInputText('ak-search-player-uid', query.player_uid);
    _cloudSetInputText('ak-search-player-name', query.player_name);
    _cloudSetInputText('ak-search-profession-name', query.profession_name);
    if (document.getElementById('ak-identity-uid')) document.getElementById('ak-identity-uid').textContent = _cloudText(identity.player_uid, '--');
    if (document.getElementById('ak-identity-name')) document.getElementById('ak-identity-name').textContent = _cloudText(identity.player_name, '--');
    if (document.getElementById('ak-identity-profession-id')) {
        var akProfessionId = _clampInt(identity.profession_id, 0, 0, 999999999);
        document.getElementById('ak-identity-profession-id').textContent = akProfessionId > 0 ? String(akProfessionId) : '--';
    }
    if (document.getElementById('ak-identity-profession-name')) document.getElementById('ak-identity-profession-name').textContent = _cloudText(identity.profession_name, '--');
    if (uploadMetaEl) {
        var bits = [];
        if (identity.ready) {
            bits.push('<span class="auto-key-pill auto-key-status-good">身份完整 Identity Ready</span>');
        } else {
            var akMissingText = _cloudMissingText(identity.missing);
            bits.push('<span class="auto-key-pill auto-key-status-warn">缺少 ' + _escHtml(akMissingText) + '</span>');
        }
        if (auth.ready) {
            bits.push('<span class="auto-key-pill auto-key-status-good">凭证已就绪 Auth Ready</span>');
        } else if (auth.error) {
            bits.push('<span class="auto-key-pill auto-key-status-error">' + _escHtml(auth.error) + '</span>');
        } else {
            bits.push('<span class="auto-key-pill auto-key-status-warn">凭证未获取 Auth Pending</span>');
        }
        if (auth.token_masked) bits.push('<span class="auto-key-pill">Token ' + _escHtml(auth.token_masked) + '</span>');
        if (auth.expires_at) bits.push('<span class="auto-key-pill">Expires ' + _escHtml(auth.expires_at) + '</span>');
        if (auth.mode) bits.push('<span class="auto-key-pill">' + _escHtml(String(auth.mode || '').toUpperCase()) + '</span>');
        uploadMetaEl.innerHTML = bits.join('');
    }
    if (uploadBtn) {
        uploadBtn.disabled = !identity.ready;
    }
    if (!resultsEl) return;
    var results = _cloudEntries(search.results);
    if (search.error) {
        resultsEl.innerHTML = '<div class="auto-key-empty">云端搜索失败 Cloud search failed.<br>' + _escHtml(search.error) + '</div>';
        return;
    }
    if (!results.length) {
        resultsEl.innerHTML = '<div class="auto-key-empty">还没有云端配置。先填写上面的筛选条件再搜索。<br>No cloud profiles yet. Enter filters above and start a search.</div>';
        return;
    }
    resultsEl.innerHTML = results.map(function(rawItem) {
        var item = _cloudEntry(rawItem);
        var remoteId = _cloudText(item.id, '');
        return '' +
            '<div class="auto-key-cloud-card">' +
                '<div class="auto-key-card-head">' +
                    '<div class="auto-key-card-title">' + _escHtml(_cloudText(item.profile_name, 'Remote #' + _cloudText(remoteId, '?'))) + '</div>' +
                    '<div class="auto-key-card-actions">' +
                        '<button class="auto-key-btn primary" type="button" onclick="_akDownloadRemote(' + _jsAttrArg(remoteId) + '); return false;">下载 Download</button>' +
                    '</div>' +
                '</div>' +
                '<div class="auto-key-card-meta">' +
                    '<span class="auto-key-pill">' + _escHtml('UID ' + _cloudText(item.player_uid, '--')) + '</span>' +
                    '<span class="auto-key-pill">' + _escHtml(_cloudText(item.player_name, '--')) + '</span>' +
                    '<span class="auto-key-pill">' + _escHtml(_cloudText(item.profession_name, '任意 Any')) + '</span>' +
                    '<span class="auto-key-pill">' + _escHtml(_cloudText(item.updated_at, _cloudText(item.created_at, ''))) + '</span>' +
                '</div>' +
                '<div class="auto-key-card-meta"><span>' + _escHtml(_cloudText(item.description, '暂无说明 / No description')) + '</span></div>' +
            '</div>';
    }).join('');
}

function _akRenderTabState() {
    _autoKeyTab = _menuTabName(_autoKeyTab);
    document.querySelectorAll('#auto-key-tabs .auto-key-tab').forEach(function(tab) {
        tab.classList.toggle('active', tab.getAttribute('data-tab') === _autoKeyTab);
    });
    document.querySelectorAll('#sub-autoKeys .auto-key-panel').forEach(function(panel) {
        panel.classList.toggle('show', panel.getAttribute('data-tab') === _autoKeyTab);
    });
}

function _renderAutoKeyUI() {
    _akEnsureSelectedProfile();
    _akRenderRuntime();
    _akRenderLocal();
    _akRenderCloud();
    _akRenderTabState();
    if (!_autoKeyDraftProfile || !_autoKeyDraftDirty) {
        _akLoadDraftFromState(_autoKeySelectedProfileId, !_autoKeyDraftProfile);
        _akPopulateEditorFields();
    } else if (_autoKeyTab === 'editor') {
        _akRenderEditorIdentitySummary(_autoKeyDraftProfile);
        _akRenderActionList();
    }
}

function _syncAutoKeyState(state) {
    _autoKeyState = state || null;
    _akEnsureSelectedProfile();
    if (!_autoKeyDraftProfile || !_autoKeyDraftDirty || !_akFindProfile(_autoKeySelectedProfileId)) {
        _akLoadDraftFromState(_autoKeySelectedProfileId, true);
    }
    _renderAutoKeyUI();
}

function _akSelectProfile(profileId, tabName) {
    _autoKeySelectedProfileId = _profileText(profileId, '');
    _akLoadDraftFromState(_autoKeySelectedProfileId, true);
    if (tabName) _autoKeyTab = _menuTabName(tabName);
    _renderAutoKeyUI();
}

function _akSetTab(tabName) {
    _autoKeyTab = _menuTabName(tabName);
    if (_autoKeyTab === 'editor' && !_autoKeyDraftProfile) {
        _akLoadDraftFromState(_autoKeySelectedProfileId, true);
        _akPopulateEditorFields();
    }
    _renderAutoKeyUI();
    if (_autoKeyTab === 'cloud') {
        var state = _autoKeyState || {};
        var identity = state.identity || {};
        var auth = state.upload_auth || {};
        if (identity.ready && !auth.ready && !auth.error) {
            _akRefreshUploadAuth(false);
        }
    }
}

function _akIntValue(fieldName, value) {
    var ranges = {
        profession_id: [0, 0, 999999999],
        tick_ms: [50, 10, 1000],
        slot_index: [1, 1, 9],
        press_count: [1, 1, 20],
        press_interval_ms: [40, 0, 10000],
        hold_ms: [80, 0, 10000],
        ready_delay_ms: [0, 0, 60000],
        min_rearm_ms: [800, 0, 120000],
        post_delay_ms: [120, 0, 120000]
    };
    var r = ranges[fieldName] || [0, 0, 999999999];
    return _clampInt(value, r[0], r[1], r[2]);
}

function _akDraftField(fieldName, value, kind) {
    if (!_autoKeyDraftProfile) return;
    if (kind === 'int') value = _akIntValue(fieldName, value);
    _autoKeyDraftProfile[fieldName] = value;
    _autoKeyDraftDirty = true;
}

function _akDraftEngineField(fieldName, value, kind) {
    if (!_autoKeyDraftProfile) return;
    if (!_autoKeyDraftProfile.engine) _autoKeyDraftProfile.engine = {};
    if (kind === 'int') value = _akIntValue(fieldName, value);
    if (kind === 'bool') value = !!value;
    _autoKeyDraftProfile.engine[fieldName] = value;
    _autoKeyDraftDirty = true;
}

function _akDraftActionField(index, fieldName, value, kind) {
    if (!_autoKeyDraftProfile || !_autoKeyDraftProfile.actions || !_autoKeyDraftProfile.actions[index]) return;
    if (kind === 'int') value = _akIntValue(fieldName, value);
    if (kind === 'bool') value = !!value;
    if (fieldName === 'key') value = _profileText(value, '').toUpperCase();
    if (fieldName === 'press_mode') value = _akPressMode(value);
    _autoKeyDraftProfile.actions[index][fieldName] = value;
    _autoKeyDraftDirty = true;
}

function _akDraftActionConditions(index, text) {
    if (!_autoKeyDraftProfile || !_autoKeyDraftProfile.actions || !_autoKeyDraftProfile.actions[index]) return;
    _autoKeyDraftProfile.actions[index]._conditions_text = _profileText(text, '');
    _autoKeyDraftDirty = true;
}

function _akApplyActionConditionsJson(index, text) {
    var action = _akGetDraftAction(index);
    if (!action) return;
    var rawText = _profileText(text, '');
    try {
        var parsed = rawText.trim();
        parsed = parsed ? JSON.parse(parsed) : [];
        if (!Array.isArray(parsed)) throw new Error('must be an array');
        action.conditions = _akClone(parsed);
        action._conditions_text = JSON.stringify(action.conditions, null, 2);
        action._conditions_error = '';
        _autoKeyDraftDirty = true;
        _akRenderActionList();
    } catch (e) {
        action._conditions_text = rawText;
        action._conditions_error = e.message || String(e);
        _autoKeyDraftDirty = true;
        _akRenderActionList();
        showAlert('AUTO KEYS', 'Action ' + (index + 1) + ' conditions JSON is invalid: ' + (e.message || String(e)), true);
    }
}

function _akSerializeDraft() {
    if (!_autoKeyDraftProfile) return null;
    var draft = _akClone(_autoKeyDraftProfile);
    draft.updated_at = new Date().toISOString();
    draft.actions = _profileEntries(draft.actions).map(function(rawAction) {
        var out = _profileEntry(_akClone(rawAction));
        out.press_mode = _akPressMode(out.press_mode);
        out.conditions = _profileEntries(out.conditions).map(function(rawCondition) {
            return _akNormalizeCondition(rawCondition);
        });
        delete out._conditions_text;
        delete out._conditions_error;
        return out;
    });
    return draft;
}

function _akCreateProfile() {
    _autoKeyDraftDirty = false;
    _akCallApi('create_auto_key_profile', [], function(data) {
        _autoKeySelectedProfileId = _profileText(data && data.state && data.state.active_profile_id, _autoKeySelectedProfileId);
        _akLoadDraftFromState(_autoKeySelectedProfileId, true);
        _autoKeyTab = 'editor';
        _renderAutoKeyUI();
        showToast('PROFILE CREATED');
    });
}

function _akCopyProfile(profileId) {
    _autoKeyDraftDirty = false;
    _akCallApi('copy_auto_key_profile', [profileId], function() {
        showToast('PROFILE COPIED');
    });
}

function _akDeleteProfile(profileId) {
    if (!window.confirm('Delete this auto key profile?')) return;
    if (_profileText(_autoKeySelectedProfileId, '') === _profileText(profileId, '')) {
        _autoKeyDraftProfile = null;
        _autoKeyDraftDirty = false;
    }
    _akCallApi('delete_auto_key_profile', [profileId], function() {
        showToast('PROFILE DELETED');
    });
}

function _akActivateProfile(profileId) {
    _akCallApi('activate_auto_key_profile', [profileId], function() {
        showToast('PROFILE ACTIVATED');
    });
}

function _akExportProfile(profileId) {
    _akCallApi('export_auto_key_profile', [profileId], function(data) {
        showAlert('AUTO KEYS', '已导出到 / Exported to:\n' + _profileText(data && data.path, ''), true);
    });
}

// 「更多 ▾」下拉派发到聚合的档案管理动作(复制/导出/删除)。与 Tk _make_profile_more_button 1:1。
function _akCardAction(profileId, action) {
    if (!action) return;
    if (action === 'copy') { _akCopyProfile(profileId); }
    else if (action === 'export') { _akExportProfile(profileId); }
    else if (action === 'delete') { _akDeleteProfile(profileId); }
}

function _akRefreshState() {
    _akCallApi('get_auto_key_state', [], function() {
        showToast('自动按键已刷新 AUTO KEY REFRESH');
    });
}

function _loadInitialAutoKeyState() {
    var api = window.pywebview && window.pywebview.api;
    if (!api || !api.get_auto_key_state) return;
    Promise.resolve().then(function() {
        return api.get_auto_key_state();
    }).then(function(result) {
        var data = _akParseApiResult(result);
        if (data && data.ok && data.state) {
            _syncAutoKeyState(data.state);
        } else if (data && data.ok === false) {
            showToast(_menuApiMessage(data, 'AUTO KEY STATE LOAD FAILED'), 3200);
        }
    }).catch(function(err) {
        showToast(String(err || 'AUTO KEY STATE LOAD FAILED'), 3200);
    });
}

function _akAddAction() {
    if (!_autoKeyDraftProfile) return;
    var nextIndex = (_autoKeyDraftProfile.actions || []).length + 1;
    _autoKeyDraftProfile.actions.push({
        id: _akNewClientId('action'),
        label: 'Action ' + nextIndex,
        enabled: true,
        slot_index: Math.min(9, nextIndex),
        key: String(Math.min(9, nextIndex)),
        press_mode: 'tap',
        press_count: 1,
        press_interval_ms: 40,
        hold_ms: 80,
        ready_delay_ms: 0,
        min_rearm_ms: 800,
        post_delay_ms: 120,
        conditions: [],
        _conditions_text: '[]',
        _conditions_error: ''
    });
    _autoKeyDraftDirty = true;
    _akRenderActionList();
}

function _akCloneAction(index) {
    if (!_autoKeyDraftProfile || !_autoKeyDraftProfile.actions || !_autoKeyDraftProfile.actions[index]) return;
    var action = _akClone(_autoKeyDraftProfile.actions[index]);
    action.id = _akNewClientId('action');
    action.label = _profileText(action.label, 'Action') + ' Copy';
    _autoKeyDraftProfile.actions.splice(index + 1, 0, action);
    _autoKeyDraftDirty = true;
    _akRenderActionList();
}

function _akDeleteAction(index) {
    if (!_autoKeyDraftProfile || !_autoKeyDraftProfile.actions) return;
    _autoKeyDraftProfile.actions.splice(index, 1);
    _autoKeyDraftDirty = true;
    _akRenderActionList();
}

function _akMoveAction(index, delta) {
    if (!_autoKeyDraftProfile || !_autoKeyDraftProfile.actions) return;
    var target = index + delta;
    if (target < 0 || target >= _autoKeyDraftProfile.actions.length) return;
    var item = _autoKeyDraftProfile.actions.splice(index, 1)[0];
    _autoKeyDraftProfile.actions.splice(target, 0, item);
    _autoKeyDraftDirty = true;
    _akRenderActionList();
}

function _akSaveDraft() {
    if (!_autoKeyDraftProfile) {
        showAlert('AUTO KEYS', '未选择配置 / No profile selected', true);
        return;
    }
    try {
        var payload = _akSerializeDraft();
        _akCallApi('save_auto_key_profile', [payload], function() {
            _autoKeyDraftDirty = false;
            _autoKeySelectedProfileId = _profileText(payload.id, _autoKeySelectedProfileId);
            _akLoadDraftFromState(_autoKeySelectedProfileId, true);
            _renderAutoKeyUI();
            showToast('PROFILE SAVED');
        });
    } catch (e) {
        showAlert('AUTO KEYS', e.message || String(e), true);
    }
}

function _akActivateDraft() {
    if (!_autoKeyDraftProfile) return;
    if (_autoKeyDraftDirty) {
        showAlert('AUTO KEYS', '请先保存这个配置再启用。 / Save this profile before activating it.', true);
        return;
    }
    _akActivateProfile(_autoKeyDraftProfile.id);
}

function _akExportDraft() {
    if (!_autoKeyDraftProfile) return;
    if (_autoKeyDraftDirty) {
        showAlert('AUTO KEYS', '请先保存这个配置再导出。 / Save this profile before exporting it.', true);
        return;
    }
    _akExportProfile(_autoKeyDraftProfile.id);
}

function _akImportProfile() {
    var api = window.pywebview && window.pywebview.api;
    if (!api || !api.start_auto_key_import_picker) {
        showAlert('AUTO KEYS', 'pywebview API 不可用 / pywebview API is not available', true);
        return;
    }
    _pickerConsumer = 'auto_key';
    Promise.resolve().then(function() {
        return api.start_auto_key_import_picker();
    }).then(function(result) {
        var data = _akParseApiResult(result);
        if (data && data.ok && data.browser) {
            showFilePicker(data.browser);
            return;
        }
        showAlert('AUTO KEYS', (data && data.message) ? data.message : '无法打开导入选择器 / Unable to open import picker', true);
    }).catch(function(err) {
        showAlert('AUTO KEYS', String(err || '无法打开导入选择器 / Unable to open import picker'), true);
    });
}

function _akSaveCloudSettings(afterSave) {
    var serverUrl = _cloudInputText('ak-server-url');
    _callMenuSettingApi('set_auto_key_server_url', [serverUrl], 'AUTO KEYS', function(data) {
        if (data && data.state) _syncAutoKeyState(data.state);
        if (afterSave) {
            afterSave();
        } else {
            showToast('SERVER SETTINGS SAVED');
        }
    });
}

function _akSearchRemote() {
    _akSaveCloudSettings(function() {
        var query = {
            q: _cloudInputText('ak-search-q'),
            profile_name: _cloudInputText('ak-search-profile-name'),
            player_uid: _cloudInputText('ak-search-player-uid'),
            player_name: _cloudInputText('ak-search-player-name'),
            profession_name: _cloudInputText('ak-search-profession-name'),
            page: 1,
            page_size: 20
        };
        _akCallApi('search_remote_profiles', [query], function() {
            showToast('CLOUD SEARCH COMPLETE');
        });
    });
}

function _akDownloadRemote(remoteId) {
    _autoKeyDraftDirty = false;
    _akCallApi('download_remote_profile', [remoteId], function() {
        showToast('PROFILE DOWNLOADED');
    });
}

function restoreMenuSettings(cfg) {
    if (!cfg) return;

/* === Game Config Init === */
    if (cfg.watched_slots && Array.isArray(cfg.watched_slots)) {
        _syncWatchedSlots(cfg.watched_slots);
    }
    /* Burst enabled */
    if (cfg.burst_enabled !== undefined) {
        document.getElementById('burst-enabled').checked = !!cfg.burst_enabled;
    }
    /* Sound */
    if (cfg.sound_enabled !== undefined) {
        document.getElementById('sound-enabled').checked = !!cfg.sound_enabled;
    }
    if (cfg.sound_volume !== undefined) {
        var restoredVolume = _setSoundVolumeUI(cfg.sound_volume);
    }
    /* Data source */
    if (cfg.panel_themes) {
        _syncPanelThemes(cfg.panel_themes);
    }
    if (cfg.mem_data_source !== undefined) {
        _memDataSource = _memSourceValue(cfg.mem_data_source);
    }
    if (cfg.data_source_map && typeof cfg.data_source_map === 'object') {
        _syncDataSourceMap(cfg.data_source_map);
    } else if (cfg.data_source !== undefined) {
        var legacyMode = _sourceMode(cfg.data_source, 'packet');
        var legacyMap = {};
        _sourceComponents.forEach(function(key) {
            legacyMap[key] = legacyMode;
        });
        _syncDataSourceMap(legacyMap);
    }
    _syncSourceButtons();
    _syncMemSourceButtons();
    _updateSourceSummary();
    if (cfg.auto_key) {
        _syncAutoKeyState(cfg.auto_key);
    }
    /* Boss bar mode */
    if (cfg.boss_bar_mode !== undefined) {
        _setBossBarModeUI(_bossBarModeValue(cfg.boss_bar_mode));
    }
    /* DPS meter toggle */
    if (cfg.dps_enabled !== undefined) {
        var dpsEl = document.getElementById('dps-enabled');
        if (dpsEl) dpsEl.checked = !!cfg.dps_enabled;
    }
    /* Buff Monitor toggle */
    if (cfg.buffmon_enabled !== undefined) {
        var bmEl = document.getElementById('buffmon-enabled');
        if (bmEl) bmEl.checked = !!cfg.buffmon_enabled;
    }
    /* DPS fade timeout */
    if (cfg.dps_fade_timeout_s !== undefined) {
        _setDpsFadeTimeoutUI(cfg.dps_fade_timeout_s);
    }
    if (cfg.boss_raid) {
        _syncBossRaidState(cfg.boss_raid);
    }
}

/* === Boss-Raid API & State === */
var _brDraftDirty = false;
var _brTab = 'local';

function _brApiObject(value) {
    return !!(value && typeof value === 'object' && !Array.isArray(value));
}

function _brParseApiResult(result) {
    if (result === null || typeof result === 'undefined' || result === '') {
        return { ok: false, message: 'Empty API response' };
    }
    if (typeof result === 'string') {
        try {
            var parsed = JSON.parse(result);
            return _brApiObject(parsed) ? parsed : { ok: false, message: String(result) };
        } catch (e) {
            return { ok: false, message: result };
        }
    }
    return _brApiObject(result) ? result : { ok: false, message: String(result) };
}

function _syncBossRaidState(state) {
    var safeState = _profileEntry(state);
    _bossRaidState = safeState;
    var el = document.getElementById('boss-raid-enabled');
    if (el) el.checked = !!safeState.enabled;
    _brEnsureSelected();
    if (!_brDraftProfile || !_brDraftDirty || !_brFindProfile(_brSelectedProfileId)) {
        _brLoadDraftFromState(_brSelectedProfileId, true);
    }
    _brRenderLocal();
    _brRenderEditor();
    _brRenderCloud();
    _brRenderRuntime();
    _brSetTab(_brTab || 'local');
}

function _brCallApi(method, args, onOk) {
    var api = window.pywebview && window.pywebview.api;
    if (!api || !api[method]) {
        showAlert('BOSS RAID', 'pywebview API 不可用 / API unavailable: ' + method, true);
        return;
    }
    var request;
    try {
        request = Promise.resolve(api[method].apply(api, args || []));
    } catch (err) {
        showAlert('BOSS RAID', String(err || 'API error'), true);
        return;
    }
    request.then(function(result) {
        var data = _brParseApiResult(result);
        if (data && data.ok !== false) {
            if (data.state) _syncBossRaidState(data.state);
            if (onOk) onOk(data);
        } else {
            showAlert('BOSS RAID', (data && (data.message || data.error || data.detail)) || 'API error', true);
        }
    }).catch(function(err) {
        showAlert('BOSS RAID', String(err), true);
    });
}

function _brProfilesFull() {
    return _profileEntries(_bossRaidState && _bossRaidState.profiles_full).map(_profileEntry);
}

function _brProfiles() {
    return _profileEntries(_bossRaidState && _bossRaidState.profiles).map(_profileEntry);
}

function _brEnsureSelected() {
    var profiles = _brProfilesFull();
    if (!profiles.length) { _brSelectedProfileId = ''; return; }
    var found = profiles.find(function(rawProfile) {
        var p = _profileEntry(rawProfile);
        return _profileText(p.id, '') === _brSelectedProfileId;
    });
    if (found) return;
    _brSelectedProfileId = _profileText((_bossRaidState && _bossRaidState.active_profile_id), _profileText(_profileEntry(profiles[0]).id, ''));
}

function _brFindProfile(id) {
    var profiles = _brProfilesFull();
    return profiles.find(function(rawProfile) {
        var p = _profileEntry(rawProfile);
        return _profileText(p.id, '') === _profileText(id, '');
    }) || null;
}

function _brLoadDraftFromState(profileId, force) {
    var targetId = _profileText(profileId, _profileText(_brSelectedProfileId, _profileText(_bossRaidState && _bossRaidState.active_profile_id, '')));
    var profile = _brFindProfile(targetId);
    if (!profile) {
        if (force) _brDraftProfile = null;
        return;
    }
    if (!force && _brDraftProfile && _profileText(_brDraftProfile.id, '') === _profileText(profile.id, '')) {
        return;
    }
    _brDraftProfile = JSON.parse(JSON.stringify(profile));
    _brDraftDirty = false;
}

function _brSetTab(tab) {
    _brTab = _menuTabName(tab);
    document.querySelectorAll('#boss-raid-tabs .boss-raid-tab').forEach(function(t) { t.classList.remove('active'); });
    var active = document.querySelector('#boss-raid-tabs .boss-raid-tab[data-tab="' + _menuTabName(_brTab) + '"]');
    if (active) active.classList.add('active');
    document.querySelectorAll('#sub-bossRaid .boss-raid-panel').forEach(function(p) { p.classList.remove('show'); });
    var panel = document.querySelector('#sub-bossRaid .boss-raid-panel[data-tab="' + _menuTabName(_brTab) + '"]');
    if (panel) panel.classList.add('show');
    if (_brTab === 'editor' && !_brDraftProfile) {
        _brLoadDraftFromState(_brSelectedProfileId, true);
    }
    if (_brTab === 'editor') {
        _brRenderEditor();
    }
    if (_brTab === 'cloud') {
        var state = _bossRaidState || {};
        var identity = state.identity || {};
        var auth = state.upload_auth || {};
        if (identity.ready && !auth.ready && !auth.error) {
            _brRefreshUploadAuth(false);
        }
    }
}

function _brRenderLocal() {
    var list = document.getElementById('boss-raid-profile-list');
    if (!list) return;
    var profiles = _brProfiles();
    var activeId = _profileText(_bossRaidState && _bossRaidState.active_profile_id, '');
    if (!profiles.length) {
        list.innerHTML = '<div style="padding:12px;color:rgba(86,92,98,0.68);font-size:12px;">暂无配置 No profiles yet</div>';
        return;
    }
    list.innerHTML = profiles.map(function(rawProfile) {
        var p = _profileEntry(rawProfile);
        var id = _profileText(p.id, '');
        var isActive = (id === activeId);
        var isSelected = (id === _brSelectedProfileId);
        return '<div class="boss-raid-card' + (isActive ? ' active-profile' : '') + (isSelected ? ' selected' : '') +
            '" onclick="_brSelectProfile(' + _jsAttrArg(id) + ')">' +
            '<div class="boss-raid-card-info">' +
                '<div class="boss-raid-card-name">' + _escHtml(_profileText(p.profile_name, 'Boss Raid')) + '</div>' +
                '<div class="boss-raid-card-meta">HP: ' + _escHtml(_brNumText(p.boss_total_hp, '?')) + ' · Enrage: ' + _escHtml(_brNumText(p.enrage_time_s, 0)) + 's · P' + _escHtml(_brNumText(p.phase_count, 0)) + ' · TL' + _escHtml(_brNumText(p.timeline_count, 0)) + '</div>' +
            '</div>' +
            '<div class="boss-raid-card-actions">' +
                '<button class="boss-raid-btn" type="button" onclick="event.stopPropagation(); _brActivateProfile(' + _jsAttrArg(id) + ');">激活 Activate</button>' +
                '<button class="boss-raid-btn" type="button" onclick="event.stopPropagation(); _brEditProfile(' + _jsAttrArg(id) + ');">编辑 Edit</button>' +
            '</div>' +
        '</div>';
    }).join('');
}

function _brSelectProfile(id) {
    _brSelectedProfileId = _profileText(id, '');
    if (_brTab === 'editor' && !_brDraftDirty) {
        _brLoadDraftFromState(_brSelectedProfileId, true);
    }
    _brRenderLocal();
    if (_brTab === 'editor') _brRenderEditor();
}

function _brActivateProfile(id) {
    _brSelectedProfileId = _profileText(id, '');
    _brCallApi('activate_boss_raid_profile', [id], function() {
        showToast('BOSS RAID ACTIVATED');
    });
}

function _brEditProfile(id) {
    _brSelectedProfileId = _profileText(id, '');
    _brLoadDraftFromState(_brSelectedProfileId, true);
    _brSetTab('editor');
}

function _brRenderEditor() {
    _brRenderEditorFields();
}

function _brRenderEditorSummary(profile) {
    var el = document.getElementById('br-editor-summary');
    if (!el) return;
    if (!profile) {
        el.innerHTML = '' +
            '<div class="boss-raid-summary-item"><span class="boss-raid-summary-label">状态 State</span><span class="boss-raid-summary-value muted">未选择配置</span></div>' +
            '<div class="boss-raid-summary-item"><span class="boss-raid-summary-label">来源 Source</span><span class="boss-raid-summary-value muted">--</span></div>' +
            '<div class="boss-raid-summary-item"><span class="boss-raid-summary-label">远端 ID Remote</span><span class="boss-raid-summary-value muted">--</span></div>' +
            '<div class="boss-raid-summary-item"><span class="boss-raid-summary-label">更新时间 Updated</span><span class="boss-raid-summary-value muted">--</span></div>';
        return;
    }
    var safeProfile = _profileEntry(profile);
    el.innerHTML = '' +
        '<div class="boss-raid-summary-item"><span class="boss-raid-summary-label">配置 ID</span><span class="boss-raid-summary-value">' + _escHtml(_profileText(safeProfile.id, '--')) + '</span></div>' +
        '<div class="boss-raid-summary-item"><span class="boss-raid-summary-label">来源 Source</span><span class="boss-raid-summary-value">' + _escHtml(_profileText(safeProfile.source, 'local')) + '</span></div>' +
        '<div class="boss-raid-summary-item"><span class="boss-raid-summary-label">远端 ID Remote</span><span class="boss-raid-summary-value">' + _escHtml(_profileText(safeProfile.remote_id, '--')) + '</span></div>' +
        '<div class="boss-raid-summary-item"><span class="boss-raid-summary-label">更新时间 Updated</span><span class="boss-raid-summary-value">' + _escHtml(_profileText(safeProfile.updated_at, '--')) + '</span></div>';
}

function _brRenderEditorFields() {
    var p = _brDraftProfile;
    var nameEl = document.getElementById('br-profile-name');
    var hpEl = document.getElementById('br-boss-hp');
    var enrageEl = document.getElementById('br-enrage-time');
    var descEl = document.getElementById('br-description');
    var simpleEl = document.getElementById('br-simple-mode');
    var targetEl = document.getElementById('br-target-pattern');
    var phasesEl = document.getElementById('br-phases-container');
    if (!p) {
        if (nameEl) nameEl.value = '';
        if (hpEl) hpEl.value = 0;
        if (enrageEl) enrageEl.value = 600;
        if (descEl) descEl.value = '';
        if (simpleEl) simpleEl.checked = true;
        if (targetEl) targetEl.value = '';
        if (phasesEl) {
            phasesEl.innerHTML = '<div class="boss-raid-empty">先到本地页选择一个配置，或者新建一个配置，再开始编辑。<br>Select a profile from Local or create a new one to start editing.</div>';
        }
        _brRenderEditorSummary(null);
        return;
    }
    if (nameEl) nameEl.value = _profileText(p.profile_name, '');
    if (hpEl) hpEl.value = _brNumText(p.boss_total_hp, 0);
    if (enrageEl) enrageEl.value = _brNumText(p.enrage_time_s, 600, 86400);
    if (descEl) descEl.value = _profileText(p.description, '');
    if (simpleEl) simpleEl.checked = p.simple_mode !== false;
    if (targetEl) targetEl.value = _profileText(p.target_name_pattern, '');
    _brRenderEditorSummary(p);
    _brRenderPhases();
}

function _brEditorPhases() {
    if (!_brDraftProfile) return [];
    var phases = _profileEntries(_brDraftProfile.phases).map(function(rawPhase) {
        var phase = _profileEntry(rawPhase);
        phase.trigger = _profileEntry(phase.trigger);
        phase.timelines = _profileEntries(phase.timelines).map(function(rawTl) {
            var tl = _profileEntry(rawTl);
            tl.condition = _profileEntry(tl.condition);
            return tl;
        });
        return phase;
    });
    _brDraftProfile.phases = phases;
    return phases;
}

function _brRenderPhases() {
    var container = document.getElementById('br-phases-container');
    if (!container || !_brDraftProfile) return;
    var phases = _brEditorPhases();
    var noValueTriggers = { manual: 1, breaking: 1, overdrive: 1, shield_broken: 1 };
    // 这些触发类型的 value 是字符串(机制 key / 技能 id 文本), 与 boss_raid_engine
    // normalize_phase 的 _string(value) 分支对齐; 其余 value 是数值。
    var stringValueTriggers = { boss_mechanic: 1, boss_mechanic_family: 1, boss_skill: 1, boss_mechanic_skill: 1, ultimate_skill: 1 };
    if (!phases.length) {
        container.innerHTML = '<div class="boss-raid-empty compact">还没有阶段。点击上面的“新增阶段”开始配置。<br>No phases yet. Click "Add Phase" to start building the timeline.</div>';
        return;
    }
    container.innerHTML = phases.map(function(phase, pi) {
        var trigger = phase.trigger;
        var timelines = phase.timelines;
        return '<div class="br-phase-card">' +
            '<div class="br-phase-head">' +
                '<span class="br-phase-name">' + _escHtml(_profileText(phase.name, 'P' + (pi+1))) + '</span>' +
                '<div style="display:flex;gap:4px;">' +
                    '<button class="boss-raid-btn" type="button" onclick="_brAddTimeline(' + pi + '); return false;">+ 时间点</button>' +
                    (phases.length > 1 ? '<button class="boss-raid-btn danger" type="button" onclick="_brRemovePhase(' + pi + '); return false;">删除</button>' : '') +
                '</div>' +
            '</div>' +
            '<div class="boss-raid-grid compact">' +
                '<div class="boss-raid-field"><label>阶段名 Name</label><input type="text" value="' + _escHtml(_profileText(phase.name, '')) + '" oninput="_brDraftPhaseField(' + pi + ', \'name\', this.value)" /></div>' +
                '<div class="boss-raid-field"><label>触发 Trigger</label><select onchange="_brDraftTriggerType(' + pi + ', this.value)">' +
                    '<option value="manual"' + (trigger.type === 'manual' ? ' selected' : '') + '>手动 Manual</option>' +
                    '<option value="time"' + (trigger.type === 'time' ? ' selected' : '') + '>时间(秒) Time</option>' +
                    '<option value="dps_total"' + (trigger.type === 'dps_total' ? ' selected' : '') + '>总伤害 DPS Total</option>' +
                    '<option value="hp_pct"' + (trigger.type === 'hp_pct' ? ' selected' : '') + '>血量% HP%</option>' +
                    '<option value="breaking"' + (trigger.type === 'breaking' ? ' selected' : '') + '>进入破防 Breaking</option>' +
                    '<option value="buff_event"' + (trigger.type === 'buff_event' ? ' selected' : '') + '>Buff事件 Buff Event</option>' +
                    '<option value="shield_broken"' + (trigger.type === 'shield_broken' ? ' selected' : '') + '>盾破 Shield Broken</option>' +
                    '<option value="overdrive"' + (trigger.type === 'overdrive' ? ' selected' : '') + '>暴走 Overdrive</option>' +
                    '<option value="extinction_pct"' + (trigger.type === 'extinction_pct' ? ' selected' : '') + '>破灭条% Extinction%</option>' +
                    '<option value="breaking_stage"' + (trigger.type === 'breaking_stage' ? ' selected' : '') + '>破防阶段 Break Stage</option>' +
                    '<option value="boss_mechanic"' + (trigger.type === 'boss_mechanic' ? ' selected' : '') + '>Boss机制 Mechanic</option>' +
                    '<option value="boss_mechanic_family"' + (trigger.type === 'boss_mechanic_family' ? ' selected' : '') + '>Boss机制族 Mech Family</option>' +
                    '<option value="boss_skill"' + (trigger.type === 'boss_skill' ? ' selected' : '') + '>Boss技能 Boss Skill</option>' +
                    '<option value="boss_mechanic_skill"' + (trigger.type === 'boss_mechanic_skill' ? ' selected' : '') + '>机制技能 Mech Skill</option>' +
                    '<option value="ultimate_skill"' + (trigger.type === 'ultimate_skill' ? ' selected' : '') + '>奥义技能 Ultimate</option>' +
                '</select></div>' +
                (trigger.type in noValueTriggers ? '' :
                    (trigger.type in stringValueTriggers ?
                        '<div class="boss-raid-field"><label>触发值 Value</label><input type="text" value="' + _escHtml(_profileText(trigger.value, '')) + '" oninput="_brDraftTriggerValue(' + pi + ', this.value)" /></div>' :
                        '<div class="boss-raid-field"><label>触发值 Value</label><input type="number" min="0" value="' + _brNumAttr(trigger.value, 0, (trigger.type === 'hp_pct' || trigger.type === 'extinction_pct') ? 100 : null) + '" oninput="_brDraftTriggerValue(' + pi + ', this.value)" /></div>')) +
            '</div>' +
            '<div class="br-phase-tl-list">' +
                (timelines.length ? timelines.map(function(rawTl, ti) {
                    var tl = _profileEntry(rawTl);
                    var cond = _profileEntry(tl.condition);
                    return '<div class="br-tl-row">' +
                        '<input type="number" min="0" step="1" value="' + _brNumAttr(tl.time_s, 0) + '" title="秒 Seconds" placeholder="秒" style="width:48px;" oninput="_brDraftTlField(' + pi + ',' + ti + ',\'time_s\',this.value,\'float\')" />' +
                        '<input type="text" value="' + _escHtml(_profileText(tl.label, '')) + '" placeholder="标签 Label" oninput="_brDraftTlField(' + pi + ',' + ti + ',\'label\',this.value)" />' +
                        '<select onchange="_brDraftTlField(' + pi + ',' + ti + ',\'alert_type\',this.value)">' +
                            '<option value="both"' + (tl.alert_type === 'both' ? ' selected' : '') + '>声+视</option>' +
                            '<option value="sound"' + (tl.alert_type === 'sound' ? ' selected' : '') + '>声</option>' +
                            '<option value="visual"' + (tl.alert_type === 'visual' ? ' selected' : '') + '>视</option>' +
                        '</select>' +
                        '<input type="number" min="0" step="1" value="' + _brNumAttr(tl.repeat_interval_s, 0) + '" title="重复间隔(秒) Repeat" placeholder="重复" style="width:48px;" oninput="_brDraftTlField(' + pi + ',' + ti + ',\'repeat_interval_s\',this.value,\'float\')" />' +
                        '<input type="number" min="0" step="1" value="' + _brNumAttr(tl.pre_warn_s, 0) + '" title="预警(秒) Pre-warn" placeholder="预警" style="width:48px;" oninput="_brDraftTlField(' + pi + ',' + ti + ',\'pre_warn_s\',this.value,\'float\')" />' +
                        '<input type="number" min="0" step="1" value="' + _brNumAttr(tl.duration_s, 0) + '" title="持续(秒) Duration" placeholder="持续" style="width:48px;" oninput="_brDraftTlField(' + pi + ',' + ti + ',\'duration_s\',this.value,\'float\')" />' +
                        '<select title="条件 Condition" onchange="_brDraftTlCondType(' + pi + ',' + ti + ',this.value)">' +
                            '<option value="always"' + ((cond.type || 'always') === 'always' ? ' selected' : '') + '>总是 Always</option>' +
                            '<option value="hp_pct"' + (cond.type === 'hp_pct' ? ' selected' : '') + '>血量% HP%</option>' +
                            '<option value="shield_active"' + (cond.type === 'shield_active' ? ' selected' : '') + '>盾存在 Shield</option>' +
                            '<option value="breaking"' + (cond.type === 'breaking' ? ' selected' : '') + '>破防中 Break</option>' +
                        '</select>' +
                        (cond.type === 'hp_pct' ? '<select title="比较 Comparator" onchange="_brDraftTlCondComparator(' + pi + ',' + ti + ',this.value)" style="width:46px;">' +
                            ['>=','<=','>','<','=='].map(function(op){ return '<option value="' + _escHtml(op) + '"' + ((cond.comparator || '>=') === op ? ' selected' : '') + '>' + _escHtml(op) + '</option>'; }).join('') +
                            '</select>' : '') +
                        (cond.type === 'hp_pct' ? '<input type="number" min="0" max="100" value="' + _brNumAttr(cond.value, 0, 100) + '" title="条件值" placeholder="%" style="width:42px;" oninput="_brDraftTlCondValue(' + pi + ',' + ti + ',this.value)" />' : '') +
                        '<button class="boss-raid-btn danger" type="button" onclick="_brRemoveTimeline(' + pi + ',' + ti + '); return false;" style="padding:2px 6px;font-size:11px;">×</button>' +
                    '</div>';
                }).join('') : '<div style="padding:4px 0;color:rgba(86,92,98,0.5);font-size:11px;">暂无时间点 No timelines</div>') +
            '</div>' +
            '</div>';
    }).join('');
}

function _brDraftField(fieldName, value, kind) {
    if (!_brDraftProfile) return;
    if (kind === 'int') value = _clampInt(value, 0, 0, 999999999);
    if (kind === 'bool') value = !!value;
    _brDraftProfile[fieldName] = value;
    _brDraftDirty = true;
    if (fieldName === 'profile_name') _brRenderEditorSummary(_brDraftProfile);
}

function _brDraftPhaseField(pi, field, val) {
    var phases = _brEditorPhases();
    if (!phases[pi]) return;
    phases[pi][field] = val;
    _brDraftDirty = true;
}
function _brDraftTriggerType(pi, val) {
    var phases = _brEditorPhases();
    if (!phases[pi]) return;
    phases[pi].trigger.type = val;
    _brDraftDirty = true;
    _brRenderPhases();
}
function _brDraftTriggerValue(pi, val) {
    var phases = _brEditorPhases();
    if (!phases[pi]) return;
    var trig = phases[pi].trigger;
    // 机制/技能类触发的 value 是字符串(key / id 文本), 与 boss_raid_engine
    // normalize_phase 对齐; 不能 _clampNum 否则丢字符串。
    if (trig.type === 'boss_mechanic' || trig.type === 'boss_mechanic_family' || trig.type === 'boss_skill' || trig.type === 'boss_mechanic_skill' || trig.type === 'ultimate_skill') {
        trig.value = (val == null ? '' : String(val));
    } else {
        var pctLike = trig.type === 'hp_pct' || trig.type === 'extinction_pct';
        trig.value = _clampNum(val, 0, 0, pctLike ? 100 : null);
    }
    _brDraftDirty = true;
}
function _brDraftTlField(pi, ti, field, val, type) {
    var phases = _brEditorPhases();
    if (!phases[pi]) return;
    var tls = phases[pi].timelines;
    if (!tls[ti]) return;
    if (type === 'float') val = _clampNum(val, 0, 0, 86400);
    tls[ti][field] = val;
    _brDraftDirty = true;
}

function _brAddPhase() {
    if (!_brDraftProfile) return;
    var phases = _brEditorPhases();
    var idx = phases.length + 1;
    phases.push({ id: 'phase_new_' + Date.now(), name: 'P' + idx, trigger: { type: 'manual', value: 0 }, timelines: [] });
    _brDraftDirty = true;
    _brRenderPhases();
}
function _brRemovePhase(pi) {
    var phases = _brEditorPhases();
    if (!phases.length) return;
    phases.splice(pi, 1);
    _brDraftDirty = true;
    _brRenderPhases();
}
function _brAddTimeline(pi) {
    var phases = _brEditorPhases();
    if (!phases[pi]) return;
    phases[pi].timelines.push({ id: 'tl_new_' + Date.now(), time_s: 30, label: 'Alert', alert_type: 'both', repeat_interval_s: 0, pre_warn_s: 0, duration_s: 0, condition: { type: 'always' } });
    _brDraftDirty = true;
    _brRenderPhases();
}
function _brRemoveTimeline(pi, ti) {
    var phases = _brEditorPhases();
    if (!phases[pi]) return;
    phases[pi].timelines.splice(ti, 1);
    _brDraftDirty = true;
    _brRenderPhases();
}
function _brDraftTlCondType(pi, ti, val) {
    var phases = _brEditorPhases();
    if (!phases[pi]) return;
    var tls = phases[pi].timelines;
    if (!tls[ti]) return;
    tls[ti].condition.type = val;
    if (val === 'hp_pct' && !tls[ti].condition.value) tls[ti].condition.value = 50;
    _brDraftDirty = true;
    _brRenderPhases();
}
function _brDraftTlCondValue(pi, ti, val) {
    var phases = _brEditorPhases();
    if (!phases[pi]) return;
    var tls = phases[pi].timelines;
    if (!tls[ti]) return;
    tls[ti].condition.value = _clampNum(val, 0, 0, 100);
    _brDraftDirty = true;
}
function _brDraftTlCondComparator(pi, ti, val) {
    // 与 Tk 时间线条件 Cmp 下拉 + engine _eval_comparator 对齐(web 此前无比较符控件,
    // hp_pct 条件只能默认 '>='); 归一到引擎认得的 5 种, 其余回退 '>='。
    var phases = _brEditorPhases();
    if (!phases[pi]) return;
    var tls = phases[pi].timelines;
    if (!tls[ti]) return;
    var allowed = { '>=': 1, '<=': 1, '>': 1, '<': 1, '==': 1 };
    tls[ti].condition.comparator = allowed[val] ? val : '>=';
    _brDraftDirty = true;
}

function _brSerializeDraft() {
    if (!_brDraftProfile) return null;
    var draft = JSON.parse(JSON.stringify(_brDraftProfile));
    draft.updated_at = new Date().toISOString();
    return draft;
}

function _brRenderRuntime() {
    var el = document.getElementById('boss-raid-runtime');
    if (!el) return;
    var rt = (_bossRaidState && _bossRaidState.runtime) || {};
    var state = rt.state || 'idle';
    var text = 'Boss Raid: ' + state.toUpperCase();
    if (state === 'running') {
        var elapsedSeconds = _clampInt(rt.elapsed_s, 0, 0, 86400);
        var dpsValue = _clampInt(rt.dps, 0, 0, Number.MAX_SAFE_INTEGER);
        var damageValue = _clampInt(rt.total_damage, 0, 0, Number.MAX_SAFE_INTEGER);
        var hpPct = _clampNum(rt.boss_hp_est_pct, 0, 0, 1);
        var enrageSeconds = _clampInt(rt.enrage_remaining_s, 0, 0, 86400);
        text += ' | ' + _profileText(rt.phase_name, 'P?');
        text += ' | ' + elapsedSeconds + 's';
        text += ' | DPS: ' + dpsValue;
        text += ' | DMG: ' + damageValue;
        if (hpPct > 0) {
            text += ' | HP: ' + (Math.round(hpPct * 1000) / 10) + '%';
        }
        if (enrageSeconds > 0) text += ' | Enrage: ' + enrageSeconds + 's';
    } else if (state === 'completed') {
        text += ' | ✓ COMPLETED';
    }
    el.textContent = text;
    // Update start/stop button label
    var btnStart = document.getElementById('br-btn-start');
    if (btnStart) {
        if (state === 'running') {
            btnStart.textContent = '■ 停止 Stop';
            btnStart.className = 'boss-raid-btn danger';
        } else {
            btnStart.textContent = '▶ 开始 Start';
            btnStart.className = 'boss-raid-btn primary';
        }
    }
}

function _brStartStop() {
    var rt = (_bossRaidState && _bossRaidState.runtime) || {};
    var state = rt.state || 'idle';
    if (state === 'running') {
        _brCallApi('boss_raid_stop', [], function() { showToast('BOSS RAID STOPPED'); });
    } else {
        _brCallApi('boss_raid_start', [], function() { showToast('BOSS RAID STARTED'); });
    }
}
function _brNextPhase() {
    _brCallApi('boss_raid_next_phase', [], function() { showToast('NEXT PHASE'); });
}
function _brReset() {
    _brCallApi('boss_raid_reset', [], function() { showToast('BOSS RAID RESET'); });
}

function _toggleRaidEditorPanel() {
    _callMenuSettingApi('toggle_raid_editor', [], 'BOSS RAID', function() {
        showToast('BOSS RAID EDITOR');
    });
}
function _toggleAutoKeyEditorPanel() {
    _callMenuSettingApi('toggle_autokey_editor', [], 'AUTO KEYS', function() {
        showToast('AUTO KEY EDITOR');
    });
}

function _brRenderCloud() {
    var state = _bossRaidState || {};
    var identity = state.identity || {};
    var auth = state.upload_auth || {};
    var search = state.last_remote_search || {};
    var query = search.query || {};
    var resultsEl = document.getElementById('boss-raid-cloud-results');
    var uploadMetaEl = document.getElementById('br-upload-auth-meta');
    var uploadBtn = document.getElementById('br-upload-active-btn');
    _cloudSetInputText('br-server-url', state.server_url);
    _cloudSetInputText('br-search-q', query.q);
    _cloudSetInputText('br-search-profile-name', query.profile_name);
    _cloudSetInputText('br-search-player-uid', query.player_uid);
    _cloudSetInputText('br-search-player-name', query.player_name);
    if (document.getElementById('br-identity-uid')) document.getElementById('br-identity-uid').textContent = _cloudText(identity.player_uid, '--');
    if (document.getElementById('br-identity-name')) document.getElementById('br-identity-name').textContent = _cloudText(identity.player_name, '--');
    if (document.getElementById('br-identity-profession-id')) {
        var brProfessionId = _clampInt(identity.profession_id, 0, 0, 999999999);
        document.getElementById('br-identity-profession-id').textContent = brProfessionId > 0 ? String(brProfessionId) : '--';
    }
    if (document.getElementById('br-identity-profession-name')) document.getElementById('br-identity-profession-name').textContent = _cloudText(identity.profession_name, '--');
    if (uploadMetaEl) {
        var bits = [];
        if (identity.ready) {
            bits.push('<span class="boss-raid-pill auto-key-status-good">身份完整 Identity Ready</span>');
        } else {
            var brMissingText = _cloudMissingText(identity.missing);
            bits.push('<span class="boss-raid-pill auto-key-status-warn">缺少 ' + _escHtml(brMissingText) + '</span>');
        }
        if (auth.ready) {
            bits.push('<span class="boss-raid-pill auto-key-status-good">凭证已就绪 Auth Ready</span>');
        } else if (auth.error) {
            bits.push('<span class="boss-raid-pill auto-key-status-error">' + _escHtml(auth.error) + '</span>');
        } else {
            bits.push('<span class="boss-raid-pill auto-key-status-warn">凭证未获取 Auth Pending</span>');
        }
        if (auth.token_masked) bits.push('<span class="boss-raid-pill">Token ' + _escHtml(auth.token_masked) + '</span>');
        if (auth.expires_at) bits.push('<span class="boss-raid-pill">Expires ' + _escHtml(auth.expires_at) + '</span>');
        if (auth.mode) bits.push('<span class="boss-raid-pill">' + _escHtml(String(auth.mode || '').toUpperCase()) + '</span>');
        uploadMetaEl.innerHTML = bits.join('');
    }
    if (uploadBtn) {
        uploadBtn.disabled = !identity.ready || !state.local_profile_count;
    }
    if (!resultsEl) return;
    var items = _cloudEntries(search.results);
    if (search.error) {
        resultsEl.innerHTML = '<div class="boss-raid-empty">云端搜索失败 Cloud search failed.<br>' + _escHtml(search.error) + '</div>';
        return;
    }
    if (!items.length) {
        resultsEl.innerHTML = '<div class="boss-raid-empty">还没有云端结果。先填写上面的搜索条件，再发起搜索。<br>No cloud profiles yet. Fill in filters above and start a search.</div>';
        return;
    }
    resultsEl.innerHTML = items.map(function(rawItem) {
        var item = _cloudEntry(rawItem);
        var remoteId = _cloudText(item.id, '');
        return '<div class="boss-raid-card" onclick="_brDownloadRemote(' + _jsAttrArg(remoteId) + ')">' +
            '<div class="boss-raid-card-info">' +
                '<div class="boss-raid-card-name">' + _escHtml(_cloudText(item.profile_name, 'Boss')) + '</div>' +
                '<div class="boss-raid-card-meta">by ' + _escHtml(_cloudText(item.player_name, '?')) + ' · UID: ' + _escHtml(_cloudText(item.player_uid, '--')) + ' · HP: ' + _escHtml(_brNumText(item.boss_total_hp, '?')) + ' · ' + _escHtml(_cloudText(item.updated_at, _cloudText(item.created_at, ''))) + '</div>' +
            '</div>' +
            '<div class="boss-raid-card-actions">' +
                '<button class="boss-raid-btn primary" type="button" onclick="event.stopPropagation(); _brDownloadRemote(' + _jsAttrArg(remoteId) + '); return false;">下载 Download</button>' +
            '</div></div>';
    }).join('');
}

function _brCreateProfile() {
    _brDraftDirty = false;
    _brCallApi('create_boss_raid_profile', [], function(data) {
        showToast('BOSS RAID CREATED');
        _brSelectedProfileId = _profileText(_bossRaidState && _bossRaidState.active_profile_id, '');
        _brLoadDraftFromState(_brSelectedProfileId, true);
        _brSetTab('editor');
    });
}
function _brImportProfile() {
    _pickerConsumer = 'boss_raid';
    _brCallApi('start_boss_raid_import_picker', [], function(data) {
        if (data && data.browser) {
            showFilePicker(data.browser);
            return;
        }
        showAlert('BOSS RAID', '无法打开导入选择器 / Unable to open import picker', true);
    });
}
function _brRefreshState() {
    _brCallApi('get_boss_raid_state', [], function() {
        showToast('BOSS RAID REFRESHED');
    });
}
function _loadInitialBossRaidState() {
    var api = window.pywebview && window.pywebview.api;
    if (!api || !api.get_boss_raid_state) return;
    Promise.resolve().then(function() {
        return api.get_boss_raid_state();
    }).then(function(result) {
        var data = _brParseApiResult(result);
        if (data && data.ok !== false && data.state) {
            _syncBossRaidState(data.state);
        } else if (data && data.ok === false) {
            showToast(_menuApiMessage(data, 'BOSS RAID STATE LOAD FAILED'), 3200);
        }
    }).catch(function(err) {
        showToast(String(err || 'BOSS RAID STATE LOAD FAILED'), 3200);
    });
}
function _brSaveProfile() {
    if (!_brDraftProfile) {
        showAlert('BOSS RAID', '未选择配置 / No profile selected', true);
        return;
    }
    var payload = _brSerializeDraft();
    _brCallApi('save_boss_raid_profile', [payload], function() {
        _brDraftDirty = false;
        _brSelectedProfileId = _profileText(payload.id, _brSelectedProfileId);
        _brLoadDraftFromState(_brSelectedProfileId, true);
        showToast('BOSS RAID SAVED');
    });
}
function _brExportProfile() {
    if (!_brDraftProfile) return;
    if (_brDraftDirty) {
        showAlert('BOSS RAID', '请先保存这个配置再导出。 / Save this profile before exporting it.', true);
        return;
    }
    _brCallApi('export_boss_raid_profile', [_brDraftProfile.id], function(data) {
        showAlert('BOSS RAID', '已导出到 / Exported to:\n' + _profileText(data && data.path, ''), true);
    });
}
function _brDeleteProfile() {
    if (!_brDraftProfile) return;
    if (!window.confirm('Delete this boss raid profile?')) return;
    _brCallApi('delete_boss_raid_profile', [_brDraftProfile.id], function() {
        _brSelectedProfileId = _profileText(_bossRaidState && _bossRaidState.active_profile_id, '');
        _brLoadDraftFromState(_brSelectedProfileId, true);
        _brSetTab('local');
        showToast('BOSS RAID DELETED');
    });
}

function _brSaveCloudSettings(afterSave) {
    var serverUrl = _cloudInputText('br-server-url');
    _callMenuSettingApi('set_boss_raid_server_url', [serverUrl], 'BOSS RAID', function(data) {
        if (data && data.state) _syncBossRaidState(data.state);
        if (afterSave) {
            afterSave();
        } else {
            showToast('SERVER SETTINGS SAVED');
        }
    });
}

function _brRefreshUploadAuth(force, afterReady) {
    _brCallApi('refresh_boss_raid_upload_auth', [!!force], function() {
        if (afterReady) afterReady();
        if (force) showToast('AUTH REFRESHED');
    });
}

function _brIssueToken() {
    _brSaveCloudSettings(function() {
        _brRefreshUploadAuth(true);
    });
}
function _brUploadActive() {
    var targetId = _brDraftProfile ? _cloudText(_brDraftProfile.id, '') : _cloudText(_bossRaidState && _bossRaidState.active_profile_id, '');
    var identity = (_bossRaidState && _bossRaidState.identity) || {};
    if (!targetId) {
        showAlert('BOSS RAID', '未选择配置 / No profile selected', true);
        return;
    }
    if (!identity.ready) {
        showAlert('BOSS RAID', '当前角色信息不完整，无法上传。缺少: ' + _cloudMissingText(identity.missing), true);
        return;
    }
    if (_brDraftDirty) {
        showAlert('BOSS RAID', '请先保存这个配置再上传。 / Save this profile before uploading it.', true);
        return;
    }
    _brSaveCloudSettings(function() {
        _brRefreshUploadAuth(false, function() {
            _brCallApi('upload_boss_raid_profile', [targetId], function(data) {
                showToast('BOSS RAID UPLOADED #' + _cloudText(data && data.remote_id, ''));
            });
        });
    });
}
function _brSearchRemote() {
    _brSaveCloudSettings(function() {
        var query = {
            q: _cloudInputText('br-search-q'),
            profile_name: _cloudInputText('br-search-profile-name'),
            player_uid: _cloudInputText('br-search-player-uid'),
            player_name: _cloudInputText('br-search-player-name'),
            page: 1,
            page_size: 20
        };
        _brCallApi('search_boss_raid_remote', [query], function() {
            showToast('BOSS RAID SEARCH COMPLETE');
        });

/* === Panel Toggles & Handlers === */
    });
}

/* Boss Raid tab switching */
document.getElementById('boss-raid-enabled').addEventListener('change', function(e) {
    e.stopPropagation();
    _brCallApi('set_boss_raid_enabled', [this.checked], function() {
        showToast('BOSS RAID ' + (document.getElementById('boss-raid-enabled').checked ? 'ON' : 'OFF'));
    });
});
document.querySelectorAll('#boss-raid-tabs .boss-raid-tab').forEach(function(tab) {
    tab.addEventListener('click', function(e) {
        e.stopPropagation();
        playSound('snd-click');
        _brSetTab(this.getAttribute('data-tab'));
    });
});

/* Boss bar mode selector */
function _bossBarModeValue(value) {
    var mode = String(value || '').trim().toLowerCase();
    return (mode === 'always' || mode === 'boss_raid' || mode === 'off') ? mode : 'boss_raid';
}

function _setBossBarModeUI(mode) {
    var safeMode = _bossBarModeValue(mode);
    document.querySelectorAll('#boss-bar-mode-group .boss-bar-mode-btn').forEach(function(btn) {
        btn.classList.toggle('active', _bossBarModeValue(btn.getAttribute('data-mode')) === safeMode);
    });
    return safeMode;
}
function _bossBarActiveMode() {
    var active = document.querySelector('#boss-bar-mode-group .boss-bar-mode-btn.active');
    return _bossBarModeValue(active ? active.getAttribute('data-mode') : 'boss_raid');
}
document.querySelectorAll('#boss-bar-mode-group .boss-bar-mode-btn').forEach(function(btn) {
    btn.addEventListener('click', function(e) {
        e.stopPropagation();
        playSound('snd-click');
        var previousMode = _bossBarActiveMode();
        var mode = _setBossBarModeUI(this.getAttribute('data-mode'));
        var labels = { always: '有敌人就显示', boss_raid: '仅 Boss Raid', off: '关闭' };
        _callMenuSettingApi('set_boss_bar_mode', [mode], 'BOSS HP BAR', function(data) {
            var applied = _setBossBarModeUI(data && data.mode !== undefined ? data.mode : mode);
            showToast('BOSS HP BAR: ' + (labels[applied] || applied));
        }, function() {
            _setBossBarModeUI(previousMode);
        });
    });
});

/* DPS meter toggle */
(function() {
    var el = document.getElementById('dps-enabled');
    if (el) {
        el.addEventListener('change', function(e) {
            e.stopPropagation();
            playSound('snd-click');
            var on = this.checked;
            _srBridgeCheckbox(this, 'set_dps_enabled', on, 'DPS METER');
        });
    }
})();

/* Buff Monitor toggle */
(function() {
    var el = document.getElementById('buffmon-enabled');
    if (el) {
        el.addEventListener('change', function(e) {
            e.stopPropagation();
            playSound('snd-click');
            var on = this.checked;
            _srBridgeCheckbox(this, 'set_buffmon_enabled', on, 'BUFF MONITOR');
        });
    }
})();

var _dpsFadeTimeoutValue = 5;
var _dpsFadeTimeoutRequestSeq = 0;
function _setDpsFadeTimeoutUI(val, commit) {
    var v = _clampInt(val, _dpsFadeTimeoutValue, 0, 120);
    var el = document.getElementById('dps-fade-timeout');
    if (el) el.value = v;
    if (commit !== false) _dpsFadeTimeoutValue = v;
    return v;
}
function _setDpsFadeTimeout(val) {
    var previousValue = _dpsFadeTimeoutValue;
    var v = _setDpsFadeTimeoutUI(val, false);
    var requestSeq = ++_dpsFadeTimeoutRequestSeq;
    _callMenuSettingApi('set_dps_fade_timeout', [v], 'DPS METER', function(data) {
        if (requestSeq !== _dpsFadeTimeoutRequestSeq) return;
        var applied = data && data.timeout !== undefined ? data.timeout : (data && data.seconds !== undefined ? data.seconds : v);
        _setDpsFadeTimeoutUI(applied);
    }, function() {
        if (requestSeq !== _dpsFadeTimeoutRequestSeq) return false;
        _setDpsFadeTimeoutUI(previousValue);
    });
}

function _setDpsLastReportAvailable(available) {
    var btn = document.getElementById('dps-last-report-btn');
    if (!btn) return;
    btn.classList.toggle('disabled', !available);
    btn.setAttribute('data-available', available ? '1' : '0');
}

function _showLastDpsReport() {
    var btn = document.getElementById('dps-last-report-btn');
    if (btn && btn.classList.contains('disabled')) {
        showAlert('DPS METER', '暂无上一场战斗报告 / No last combat report yet.', true);
        return;
    }
    if (!window.pywebview || !window.pywebview.api || !window.pywebview.api.show_last_dps_report) {
        showAlert('DPS METER', 'pywebview API 不可用 / pywebview API is not available', true);
        return;
    }
    window.pywebview.api.show_last_dps_report().then(function(result) {
        var data = _menuParseApiResult(result);
        if (!data || data.ok === false) {
            showAlert('DPS METER', (data && (data.message || data.error || data.detail)) ? (data.message || data.error || data.detail) : '无法打开上一场战斗报告 / Unable to open last combat report', true);
            return;
        }
        showToast('DPS REPORT: LAST REPORT');
    }).catch(function(err) {
        showAlert('DPS METER', String(err || '无法打开上一场战斗报告 / Unable to open last combat report'), true);
    });
}

/* === Linkage Management === */

/* ═══ Boss ↔ AutoKey Linkage ═══ */
var _linkageMappings = [];

function _initLinkageUI() {
    var api = window.pywebview && window.pywebview.api;
    if (!api || !api.get_linkage_state) return;
    api.get_linkage_state().then(function(raw) {
        var resp = _menuParseApiResult(raw);
        if (!resp.ok) return;
        var s = _menuApiObject(resp.state) ? resp.state : null;
        if (!s) return;
        var enabledEl = document.getElementById('linkage-enabled');
        var debugEl = document.getElementById('linkage-debug');
        var cdEl = document.getElementById('linkage-global-cd');
        if (enabledEl) enabledEl.checked = !!s.enabled;
        if (debugEl) debugEl.checked = !!s.debug_log;
        if (cdEl) cdEl.value = _clampNum(s.global_cooldown_s, 1.0, 0, 60);
        _linkageMappings = Array.isArray(s.mappings) ? s.mappings : [];
        _renderLinkageMappings();
    });
}

function _renderLinkageMappings() {
    var container = document.getElementById('linkage-mappings');
    if (!container) return;
    var mappings = Array.isArray(_linkageMappings) ? _linkageMappings : [];
    _linkageMappings = mappings;
    if (mappings.length === 0) {
        container.innerHTML = '<div style="font-size:10px;color:rgb(160,158,160);">No mappings. Click + to add.</div>';
        return;
    }
    var triggerLabels = {
        phase_enter: 'Phase Enter',
        timeline_alert: 'Timeline Alert',
        breaking: 'Breaking',
        enrage: 'Enrage',
    };
    var html = '';
    for (var i = 0; i < mappings.length; i++) {
        var m = mappings[i] || {};
        html += '<div style="display:flex;gap:4px;align-items:center;flex-wrap:wrap;padding:4px 6px;background:rgba(245,243,240,0.80);border:1px solid rgba(201,198,198,0.45);border-radius:6px;">';
        html += '<select data-idx="' + i + '" data-field="trigger_type" onchange="_updateMapping(this)" '
             + 'style="font-size:10px;padding:1px 3px;background:rgba(255,255,255,0.90);border:1px solid rgba(201,198,198,0.55);border-radius:4px;color:rgb(80,78,80);">';
        ['phase_enter','timeline_alert','breaking','enrage'].forEach(function(t) {
            html += '<option value="' + t + '"' + (m.trigger_type === t ? ' selected' : '') + '>' + (triggerLabels[t] || t) + '</option>';
        });
        html += '</select>';
        html += '<input type="text" data-idx="' + i + '" data-field="trigger_match" value="' + _escAttr(_akTextValue(m.trigger_match)) + '" '
             + 'placeholder="match..." onchange="_updateMapping(this)" '
             + 'style="width:60px;font-size:10px;padding:1px 3px;background:rgba(255,255,255,0.90);border:1px solid rgba(201,198,198,0.55);border-radius:4px;color:rgb(80,78,80);" />';
        html += '<span style="font-size:9px;color:rgb(160,158,160);">→</span>';
        html += '<input type="text" data-idx="' + i + '" data-field="action_key" value="' + _escAttr(_akTextValue(m.action_key)) + '" '
             + 'placeholder="Key" onchange="_updateMapping(this)" '
             + 'style="width:36px;font-size:10px;padding:1px 3px;background:rgba(255,255,255,0.90);border:1px solid rgba(201,198,198,0.55);border-radius:4px;color:rgb(80,78,80);text-transform:uppercase;" />';
        html += '<input type="text" data-idx="' + i + '" data-field="action_label" value="' + _escAttr(_akTextValue(m.action_label)) + '" '
             + 'placeholder="Label" onchange="_updateMapping(this)" '
             + 'style="width:50px;font-size:10px;padding:1px 3px;background:rgba(255,255,255,0.90);border:1px solid rgba(201,198,198,0.55);border-radius:4px;color:rgb(80,78,80);" />';
        html += '<span style="font-size:8px;color:rgb(185,183,183);cursor:pointer;" onclick="_removeLinkageMapping(' + i + ')">✕</span>';
        html += '</div>';
    }
    container.innerHTML = html;
}

function _updateMapping(el) {
    var idx = parseInt(el.getAttribute('data-idx'));
    var field = el.getAttribute('data-field');
    if (idx >= 0 && idx < _linkageMappings.length && field) {
        _linkageMappings[idx][field] = el.value;
        _saveLinkageMappings();
    }
}

function _addLinkageMapping() {
    _linkageMappings.push({
        id: 'lnk_' + Math.random().toString(36).substring(2, 14),
        enabled: true,
        trigger_type: 'phase_enter',
        trigger_match: '',
        action_key: '',
        action_label: '',
        press_mode: 'tap',
        hold_ms: 80,
        press_count: 1,
        cooldown_s: 3.0,
    });
    _renderLinkageMappings();
    _saveLinkageMappings();
}

function _removeLinkageMapping(idx) {
    _linkageMappings.splice(idx, 1);
    _renderLinkageMappings();
    _saveLinkageMappings();
}

function _saveLinkageMappings() {
    var api = window.pywebview && window.pywebview.api;
    if (api && api.save_linkage_mappings) {
        api.save_linkage_mappings(JSON.stringify(_linkageMappings));
    }
}

function _setLinkageGlobalCD(val) {
    var api = window.pywebview && window.pywebview.api;
    if (api && api.set_linkage_global_cooldown) {
        var seconds = _clampNum(val, 1.0, 0, 60);
        var el = document.getElementById('linkage-global-cd');
        if (el) el.value = seconds;
        api.set_linkage_global_cooldown(seconds);
    }
}

(function() {
    var elEnabled = document.getElementById('linkage-enabled');
    if (elEnabled) {
        elEnabled.addEventListener('change', function(e) {
            e.stopPropagation();
            playSound('snd-click');
            var api = window.pywebview && window.pywebview.api;
            if (api && api.set_linkage_enabled) api.set_linkage_enabled(this.checked);
            showToast('LINKAGE: ' + (this.checked ? 'ON' : 'OFF'));
        });
    }
    var elDebug = document.getElementById('linkage-debug');
    if (elDebug) {
        elDebug.addEventListener('change', function(e) {
            e.stopPropagation();
            var api = window.pywebview && window.pywebview.api;
            if (api && api.set_linkage_debug) api.set_linkage_debug(this.checked);
        });
    }
    // Init on pywebview ready
    if (window.pywebview && window.pywebview.api) {
        _initLinkageUI();
    } else {
        window.addEventListener('pywebviewready', _initLinkageUI);
    }
})();

/* ═══ Python bridge: expose Star Resonance JS functions ═══ */
var __srBaseUpdateInfo = window.SAO && window.SAO.updateInfo;
function _srUpdateInfo(data) {
    if (typeof __srBaseUpdateInfo === 'function') {
        __srBaseUpdateInfo(data);
    }
    updateStarResonanceInfo(data);
}
window.SAO = Object.assign(window.SAO || {}, {
    updateInfo: _srUpdateInfo,
    showFilePicker: showFilePicker,
    closeFilePicker: closeFilePicker,
    setSessionPlayersPayload: setSessionPlayersPayload,
    showSessionPlayers: showSessionPlayers,
    toggleSessionPlayers: toggleSessionPlayers,
    closeSessionPlayers: closeSessionPlayers,
    syncAutoKeyState: _syncAutoKeyState,
    syncBossRaidState: _syncBossRaidState,
    restoreStarResonanceMenuSettings: restoreMenuSettings
});
window.SRMenu = Object.assign(window.SRMenu || {}, {
    updateInfo: updateStarResonanceInfo,
    setUserInfoVisible: _srSetUserInfoVisible,
    setAutoKeyState: _syncAutoKeyState,
    setBossRaidState: _syncBossRaidState,
    restoreMenuSettings: restoreMenuSettings,
    showFilePicker: showFilePicker,
    showSessionPlayers: showSessionPlayers,
    closeSessionPlayers: closeSessionPlayers
});
if (window.SAO && !window.SAO.__srRestoreWrapped) {
    var __srBaseRestoreMenuSettings = window.SAO.restoreMenuSettings;
    window.SAO.restoreMenuSettings = function(cfg) {
        if (typeof __srBaseRestoreMenuSettings === 'function') {
            __srBaseRestoreMenuSettings(cfg);
        }
        restoreMenuSettings(cfg);
    };
    window.SAO.__srRestoreWrapped = true;
}

setTimeout(function() {
    if (typeof _initPanelThemes === 'function') _initPanelThemes();
    _loadInitialAutoKeyState();
    _loadInitialBossRaidState();
    _srStartUserInfoClock();
}, 250);
