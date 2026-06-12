# -*- coding: utf-8 -*-
"""Static regression checks for the WebView SAO menu column layout."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MENU_HTML = ROOT / "web" / "menu.html"
SAO_WEBVIEW = ROOT / "sao_webview.py"
HUD_SETTINGS_BRIDGE = ROOT / "C#" / "src" / "SaoAuto.App" / "WebBridge" / "HudSettingsBridge.cs"
ITEM_SIZE_PX = 54
ITEM_GAP_PX = 15
FRAME_MARGIN_PX = 6


def _read_menu() -> str:
    return MENU_HTML.read_text(encoding="utf-8")


def _css_block(source: str, selector: str) -> str:
    match = re.search(rf"{re.escape(selector)}\s*\{{(?P<body>.*?)\}}", source, re.S)
    if not match:
        raise AssertionError(f"missing CSS block: {selector}")
    return match.group("body")


def _px_property(block: str, prop: str) -> int:
    match = re.search(rf"\b{re.escape(prop)}\s*:\s*(\d+)px\b", block)
    if not match:
        raise AssertionError(f"missing px property: {prop}")
    return int(match.group(1))


def main() -> int:
    html = _read_menu()
    item_count = len(re.findall(r'<li\s+class="[^"]*\bitem\b[^"]*"\s+data-name=', html))
    if item_count <= 0:
        raise AssertionError("no SAO menu items found")

    required_column_height = item_count * ITEM_SIZE_PX + max(0, item_count - 1) * ITEM_GAP_PX
    item_box_height = _px_property(_css_block(html, ".item_box"), "height")
    frame_height = _px_property(_css_block(html, ".menu-frame"), "height")

    scan_match = re.search(r"100%\s*\{\s*top:\s*(\d+)px;\s*opacity:\s*0;\s*\}", html)
    if not scan_match:
        raise AssertionError("missing menu-frame-scan final top")
    scan_top = int(scan_match.group(1))

    js_height_match = re.search(r"var\s+menuFrameHeight\s*=\s*(\d+)\s*;", html)
    if not js_height_match:
        raise AssertionError("openMenu() must bound by the actual menu frame height")
    js_frame_height = int(js_height_match.group(1))

    if item_box_height < required_column_height:
        raise AssertionError(
            f"item_box height {item_box_height}px clips {item_count} items "
            f"(needs >= {required_column_height}px)"
        )
    if frame_height < item_box_height + FRAME_MARGIN_PX * 2:
        raise AssertionError("menu-frame must wrap item_box plus both HUD margins")
    if scan_top < item_box_height:
        raise AssertionError("menu frame scan line does not sweep through the full item column")
    if js_frame_height != frame_height:
        raise AssertionError("openMenu() viewport clamp must match .menu-frame height")

    if "onclick=\"_akActivateProfile(' + JSON.stringify(id) + ')" in html:
        raise AssertionError("AutoKey profile onclick must HTML-escape JSON string args")
    if "onclick=\"_akDownloadRemote(' + JSON.stringify(remoteId) + ')" in html:
        raise AssertionError("AutoKey cloud download onclick must HTML-escape JSON string args")
    if "onclick=\"_brSelectProfile(\\'' + p.id + '\\')" in html:
        raise AssertionError("BossRaid profile card onclick must not single-quote raw profile ids")
    if "_brDownloadRemote(' + JSON.stringify(remoteId) + ')" in html:
        raise AssertionError("BossRaid cloud download onclick must HTML-escape JSON string args")
    if "function _jsAttrArg" not in html:
        raise AssertionError("menu.html must provide _jsAttrArg for inline handler arguments")
    if "_akActivateProfile(' + _jsAttrArg(id) + ')" not in html:
        raise AssertionError("AutoKey activate must use _jsAttrArg(id)")
    if "_akDownloadRemote(' + _jsAttrArg(remoteId) + ')" not in html:
        raise AssertionError("AutoKey cloud download must use _jsAttrArg(remoteId)")
    if "_brSelectProfile(' + _jsAttrArg(id) + ')" not in html:
        raise AssertionError("BossRaid select must use _jsAttrArg(id)")
    if "_brDownloadRemote(' + _jsAttrArg(remoteId) + ')" not in html:
        raise AssertionError("BossRaid cloud download must use _jsAttrArg(remoteId)")
    if html.count("function _escAttr") != 1:
        raise AssertionError("menu.html must define exactly one robust _escAttr helper")
    if "'<span class=\"lb-rank\">' + (e.rank <= 3" in html:
        raise AssertionError("leaderboard rank labels must be escaped before innerHTML")
    if "'<span class=\"lb-lv\">Lv.' + e.level" in html:
        raise AssertionError("leaderboard level labels must be escaped before innerHTML")
    if "'<span class=\"lb-stat\">' + _lbStatValue(e)" in html:
        raise AssertionError("leaderboard stat labels must be escaped before innerHTML")
    if "function _lbRankLabel" not in html:
        raise AssertionError("menu.html must provide a safe leaderboard rank label helper")
    leaderboard_raw_patterns = [
        "if (_lbCurrentSort === 'xp') return 'XP ' + e.xp;",
        "if (_lbCurrentSort === 'level') return 'Lv.' + e.level;",
        "if (_lbCurrentSort === 'songs_played') return e.songs_played + '曲';",
        "return '#' + String((e && e.rank) || '--');",
        "return 'Lv.' + String((e && e.level) || '--');",
        "Number(e.rank) === rank",
        "Number(e.rank) === Number(_lbFocusRank)",
    ]
    for pattern in leaderboard_raw_patterns:
        if pattern in html:
            raise AssertionError("leaderboard numeric labels must use finite/clamped helpers: " + pattern)
    leaderboard_safe_required = [
        "function _lbPositiveInt(value, hi)",
        "function _lbNonNegInt(value, hi)",
        "var xp = _lbNonNegInt(e && e.xp);",
        "var level = _lbPositiveInt(e && e.level, 999);",
        "var songs = _lbNonNegInt(e && e.songs_played, 999999);",
        "var rank = _lbPositiveInt(e && e.rank, 999999);",
        "return '#' + (rank == null ? '--' : String(rank));",
        "return 'Lv.' + (level == null ? '--' : String(level));",
        "return _lbPositiveInt(e.rank, 999999) === rank;",
        "if (_lbFocusRank && _lbPositiveInt(e.rank, 999999) === _lbFocusRank) row.className += ' lb-jump';",
        "sec = _clampNum(sec, 0, 0, Number.MAX_SAFE_INTEGER);",
    ]
    for snippet in leaderboard_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe leaderboard numeric snippet: " + snippet)
    if "return (s || '').replace(/&/g,'&amp;')" in html:
        raise AssertionError("_escHtml must coerce non-string values and escape quotes for attribute reuse")
    menu_raw_patterns = [
        "document.getElementById('info-xp').style.width = data.xp_pct + '%';",
        "window.pywebview.api.set_sound_volume(parseInt(this.value));",
        "document.getElementById('sound-volume').value = cfg.sound_volume;",
        "var value = Math.max(0, Math.min(100, Number(pct) || 0));",
        "var sizeMb = (s.size || 0) / 1024 / 1024;",
        "var pct = Math.round((s.progress || 0) * 100);",
        "var total = Number(_sessionPlayersPayload.count || rows.length || 0);",
        "slots.push(parseInt(el.getAttribute('data-slot')));",
        "var slot = parseInt(el.getAttribute('data-slot'));",
        "cfg.watched_slots.indexOf(slot) >= 0",
        "window.pywebview.api.set_watched_slots(slots);",
        "if (fadeEl) fadeEl.value = cfg.dps_fade_timeout_s;",
        "if (cfg.boss_bar_mode) {\n        _setBossBarModeUI(cfg.boss_bar_mode);",
        "btn.classList.toggle('active', btn.getAttribute('data-mode') === mode);",
        "var mode = this.getAttribute('data-mode');\n        _setBossBarModeUI(mode);",
        "window.pywebview.api.set_boss_bar_mode(mode);",
        "window.pywebview.api.set_component_source(component, mode);",
        "window.pywebview.api.set_data_source(mode);",
        "window.pywebview.api.set_burst_enabled(this.checked);",
        "window.pywebview.api.set_sound_enabled(this.checked);",
        "window.pywebview.api.set_sound_volume(volume);",
        "window.pywebview.api.set_dps_fade_timeout(v);",
        "window.pywebview.api.set_dps_enabled(on);\n            }\n            showToast('DPS METER: ' + (on ? 'ON' : 'OFF'));",
        "window.pywebview.api.set_buffmon_enabled(on);\n            }\n            showToast('BUFF MONITOR: ' + (on ? 'ON' : 'OFF'));",
    ]
    for pattern in menu_raw_patterns:
        if pattern in html:
            raise AssertionError("menu runtime values must be normalized before rendering or bridge calls: " + pattern)
    menu_safe_required = [
        "var xpPct = _clampNum(data.xp_pct, 0, 0, 100);",
        "document.getElementById('info-xp').style.width = xpPct + '%';",
        "function _setSoundVolumeUI(value, commit)",
        "if (commit !== false) _soundVolumeValue = volume;",
        "var _soundVolumeRequestSeq = 0;",
        "var volume = _setSoundVolumeUI(this.value, false);",
        "_callMenuSettingApi('set_sound_volume', [volume], 'SOUND', function(data) {",
        "if (requestSeq !== _soundVolumeRequestSeq) return false;",
        "var restoredVolume = _setSoundVolumeUI(cfg.sound_volume);",
        "var value = _clampInt(pct, 0, 0, 100);",
        "var sizeBytes = _clampNum(s.size, 0, 0, Number.MAX_SAFE_INTEGER);",
        "var sizeMb = sizeBytes / 1024 / 1024;",
        "var pct = _clampInt(_clampNum(s.progress, 0, 0, 1) * 100, 0, 0, 100);",
        "var total = _clampInt(_sessionPlayersPayload.count, rows.length, 0, 999999);",
        "function _slotId(value)",
        "function _watchedSlotsList()",
        "function _syncWatchedSlots(slots, commit)",
        "var _watchedSlotsRequestSeq = 0;",
        "var previousSlots = _watchedSlotsConfirmed.slice();",
        "_saveWatchedSlots(previousSlots);",
        "_callMenuSettingApi('set_watched_slots', [slots], 'BURST SLOTS', function(data) {",
        "_syncWatchedSlots(data && Array.isArray(data.slots) ? data.slots : slots);",
        "_syncWatchedSlots(_watchedSlotsList());",
        "_syncWatchedSlots(cfg.watched_slots);",
        "var restoredFadeTimeout = _setDpsFadeTimeoutUI(cfg.dps_fade_timeout_s);",
        "function _bossBarModeValue(value)",
        "return (mode === 'always' || mode === 'boss_raid' || mode === 'off') ? mode : 'boss_raid';",
        "_setBossBarModeUI(_bossBarModeValue(cfg.boss_bar_mode));",
        "var mode = _setBossBarModeUI(this.getAttribute('data-mode'));",
        "function _callMenuSettingApi(methodName, args, label, onOk, onFail)",
        "api[methodName].apply(api, args || [])",
        "try {\n        promise = Promise.resolve(api[methodName].apply(api, args || []));",
        "function _bossBarActiveMode()",
        "_callMenuSettingApi('set_boss_bar_mode', [mode], 'BOSS HP BAR', function(data) {",
        "_setBossBarModeUI(previousMode);",
        "_callMenuSettingApi('set_component_source', [component, mode], 'DATA SOURCE', function(data) {",
        "_syncDataSourceMap(previousMap);",
        "_callMenuSettingApi('set_data_source', [mode], 'DATA SOURCE', function(data) {",
        "_memDataSource = _memSourceValue(previousMode);",
        "function _setBridgeBackedCheckbox(el, methodName, requested, label)",
        "function _menuToggleResult(result, expected)",
        "showAlert(label, 'pywebview API 不可用 / pywebview API is not available', true);",
        "_setBridgeBackedCheckbox(this, 'set_dps_enabled', on, 'DPS METER');",
        "_setBridgeBackedCheckbox(this, 'set_buffmon_enabled', on, 'BUFF MONITOR');",
        "_setBridgeBackedCheckbox(this, 'set_burst_enabled', on, 'BURST ALERT');",
        "_setBridgeBackedCheckbox(this, 'set_sound_enabled', on, 'SOUND');",
        "function _setDpsFadeTimeoutUI(val, commit)",
        "var _dpsFadeTimeoutRequestSeq = 0;",
        "var v = _setDpsFadeTimeoutUI(val, false);",
        "_callMenuSettingApi('set_dps_fade_timeout', [v], 'DPS METER', function(data) {",
        "_setDpsFadeTimeoutUI(applied);",
        "_setDpsFadeTimeoutUI(previousValue);",
    ]
    for snippet in menu_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe menu runtime value snippet: " + snippet)

    file_picker_raw_patterns = [
        "function showFilePicker(dataJson) {\n    var data = (typeof dataJson === 'string') ? JSON.parse(dataJson) : dataJson;",
        "_pickerMode = data.mode || 'file';",
        "_pickerCurrentDir = data.current || '';",
        "if (data.parent) {",
        "_pickerBrowse(data.parent);",
        "(data.dirs || []).forEach(function(d) {",
        "(data.files || []).forEach(function(f) {",
        "var infoStr = f.size ? '<span class=\"item-info\">' + _fmtSize(f.size) + '</span>' : '';",
        "window.pywebview.api.select_file(f.path).then",
        "window.pywebview.api.select_file(_pickerEntryText(f.path)).then",
        "window.pywebview.api.select_folder(_pickerCurrentDir);",
        "window.pywebview.api.browse_dir(path).then(function(result) {",
        "if (!(data.parent) && !(data.dirs||[]).length && !(data.files||[]).length) {",
        "var d = (typeof result === 'string') ? JSON.parse(result) : result;",
        "d.mode = _pickerMode;",
    ]
    for pattern in file_picker_raw_patterns:
        if pattern in html:
            raise AssertionError("File Picker payload rendering must validate payload shape and preserve zero text: " + pattern)
    file_picker_safe_required = [
        "function _pickerPayload(dataJson)",
        "function _pickerModeValue(value)",
        "function _pickerEntry(entry)",
        "function _pickerEntryText(value)",
        "function _pickerConsumerLabel(consumer)",
        "function _pickerParseResult(consumer, result)",
        "var data = _pickerPayload(dataJson);",
        "_pickerMode = _pickerModeValue(data.mode);",
        "_pickerCurrentDir = _pickerEntryText(data.current);",
        "var parentPath = _pickerEntryText(data.parent);",
        "var dirs = Array.isArray(data.dirs) ? data.dirs : [];",
        "var files = Array.isArray(data.files) ? data.files : [];",
        "var d = _pickerEntry(rawDir);",
        "var f = _pickerEntry(rawFile);",
        "var sizeBytes = _clampNum(f.size, 0, 0, Number.MAX_SAFE_INTEGER);",
        "var infoStr = sizeBytes > 0 ? '<span class=\"item-info\">' + _fmtSize(sizeBytes) + '</span>' : '';",
        "var filePath = _pickerEntryText(f.path);",
        "var request = Promise.resolve().then(function() {\n                    return window.pywebview.api.select_file(filePath, consumer);",
        "var data = _pickerParseResult(consumer, result);",
        "showAlert(_pickerConsumerLabel(consumer), String(err || '无法导入文件 / Unable to import file'), true);",
        "if (!parentPath && !dirs.length && !files.length) {",
        "_callMenuSettingApi('select_folder', [folderPath], 'FILE PICKER');",
        "return window.pywebview.api.browse_dir(path);",
        "var d = _pickerPayload(result);",
        "if (d.error) {",
        "d.mode = _pickerModeValue(_pickerMode);",
        "showAlert('FILE PICKER', String(err || '无法打开目录 / Unable to open folder'), true);",
    ]
    for snippet in file_picker_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe File Picker payload snippet: " + snippet)

    session_players_raw_patterns = [
        "var name = row.name || '--';",
        "_escHtml(row.name || '')",
        "_escHtml(row.uid || '--')",
        "_escHtml(row.fight_power || '--')",
    ]
    for pattern in session_players_raw_patterns:
        if pattern in html:
            raise AssertionError("Session Players rows must guard malformed entries and preserve zero text: " + pattern)
    session_players_safe_required = [
        "row = (row && typeof row === 'object') ? row : {};",
        "function _spText(value, fallback)",
        "var name = _spText(row.name, '--');",
        "_escHtml(_spText(row.name, ''))",
        "_escHtml(_spText(row.uid, '--'))",
        "_escHtml(_spText(row.fight_power, '--'))",
    ]
    for snippet in session_players_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe Session Players row snippet: " + snippet)

    boss_raid_raw_patterns = [
        "'<div class=\"boss-raid-card-meta\">HP: ' + (p.boss_total_hp || '?')",
        "' · HP: ' + (item.boss_total_hp || '?')",
        "'<div class=\"boss-raid-field\"><label>触发值 Value</label><input type=\"number\" min=\"0\" value=\"' + (trigger.value || 0)",
        "'<input type=\"number\" min=\"0\" step=\"1\" value=\"' + (tl.time_s || 0)",
        "'<input type=\"number\" min=\"0\" step=\"1\" value=\"' + (tl.repeat_interval_s || 0)",
        "'<input type=\"number\" min=\"0\" step=\"1\" value=\"' + (tl.pre_warn_s || 0)",
        "'<input type=\"number\" min=\"0\" step=\"1\" value=\"' + (tl.duration_s || 0)",
        "'<input type=\"number\" min=\"0\" max=\"100\" value=\"' + (cond.value || 0)",
        "Math.floor(rt.elapsed_s || 0) + 's'",
        "text += ' | DPS: ' + (rt.dps || 0);",
        "text += ' | DMG: ' + (rt.total_damage || 0);",
        "if ((rt.boss_hp_est_pct || 0) > 0) {",
        "Math.floor(rt.enrage_remaining_s) + 's'",
        "hpEl.value = p.boss_total_hp || 0;",
        "enrageEl.value = p.enrage_time_s || 600;",
        "_brDraftProfile.phases[pi].trigger.value = parseFloat(val) || 0;",
        "if (type === 'float') val = parseFloat(val) || 0;",
        "tls[ti].condition.value = parseFloat(val) || 0;",
    ]
    for pattern in boss_raid_raw_patterns:
        if pattern in html:
            raise AssertionError("BossRaid numeric metadata/input values must be normalized before rendering: " + pattern)
    boss_raid_safe_required = [
        "function _brNumText",
        "function _brNumAttr",
        "function _clampNum",
        "function _clampInt",
        "_escHtml(_brNumText(p.boss_total_hp, '?'))",
        "_escHtml(_brNumText(item.boss_total_hp, '?'))",
        "value=\"' + _brNumAttr(trigger.value, 0, (trigger.type === 'hp_pct' || trigger.type === 'extinction_pct') ? 100 : null)",
        "value=\"' + _brNumAttr(tl.time_s, 0)",
        "value=\"' + _brNumAttr(tl.repeat_interval_s, 0)",
        "value=\"' + _brNumAttr(tl.pre_warn_s, 0)",
        "value=\"' + _brNumAttr(tl.duration_s, 0)",
        "value=\"' + _brNumAttr(cond.value, 0, 100)",
        "var elapsedSeconds = _clampInt(rt.elapsed_s, 0, 0, 86400);",
        "var dpsValue = _clampInt(rt.dps, 0, 0, Number.MAX_SAFE_INTEGER);",
        "var damageValue = _clampInt(rt.total_damage, 0, 0, Number.MAX_SAFE_INTEGER);",
        "var hpPct = _clampNum(rt.boss_hp_est_pct, 0, 0, 1);",
        "var enrageSeconds = _clampInt(rt.enrage_remaining_s, 0, 0, 86400);",
        "hpEl.value = _brNumText(p.boss_total_hp, 0);",
        "enrageEl.value = _brNumText(p.enrage_time_s, 600, 86400);",
        "trig.value = _clampNum(val, 0, 0, pctLike ? 100 : null);",
        "if (type === 'float') val = _clampNum(val, 0, 0, 86400);",
        "tls[ti].condition.value = _clampNum(val, 0, 0, 100);",
    ]
    for snippet in boss_raid_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe BossRaid numeric rendering snippet: " + snippet)

    plugin_menu_raw_patterns = [
        "((it.hotkey_count || 0) ? ' ⌨' + it.hotkey_count : '')",
        "var hk = (it.hotkey_count || 0) ? ' ⌨' + it.hotkey_count : '';",
        "Promise.resolve(ar.reload_plugins('')).then(function () {",
        "window.pywebview.api.toggle_raid_editor();",
        "window.pywebview.api.toggle_autokey_editor();",
        "window.pywebview.api.menu_action(action);",
        "if (a1 && a1.menu_action) a1.menu_action('toggle_plugin_manager');",
        "Promise.resolve(a3.pin_plugin(pid, !pinned)).then(function () { window.renderPluginPopup(); });",
        "Promise.resolve(en ? a2.disable_plugin(id) : a2.enable_plugin(id))",
        "Promise.resolve(a.invoke_ui_action(panelId, action, JSON.stringify(payload || {})))",
        "if (!a || !a.set_plugin_hotkey) return;",
        "Promise.resolve(a.set_plugin_hotkey(sel.getAttribute('data-action'), sel.value))",
        "Promise.resolve(window.pywebview.api.apply_update())",
        "Promise.resolve(window.pywebview.api.download_update())",
        "Promise.resolve(window.pywebview.api.skip_update())",
        "            window.pywebview.api.toggle_menu();",
        "window.pywebview.api.get_panel_themes().then(function(themes) {",
        "window.pywebview.api.exit_app();",
        "window.pywebview.api.menu_action('exit');",
        "window.pywebview.api.alert_ok();",
        "Promise.all([\n        window.pywebview.api.set_auto_key_server_url(serverUrl)",
        "Promise.all([\n        window.pywebview.api.set_boss_raid_server_url(serverUrl)",
    ]
    for pattern in plugin_menu_raw_patterns:
        if pattern in html:
            raise AssertionError("plugin menu hotkey counts must be normalized before rendering: " + pattern)
    plugin_menu_safe_required = [
        "function _pluginCountNumber(value)",
        "function _pluginCountText(value)",
        "var hkCount = _pluginCountNumber(it.hotkey_count);",
        "hkCount ? ' ⌨' + _pluginCountText(hkCount) : ''",
        "_callMenuSettingApi('reload_plugins', [''], 'PLUGINS', function() {",
        "showToast('PLUGINS RELOADED');",
        "_callMenuSettingApi('toggle_raid_editor', [], 'BOSS RAID', function() {",
        "_callMenuSettingApi('toggle_autokey_editor', [], 'AUTO KEYS', function() {",
        "_callMenuSettingApi('menu_action', [action], 'MENU');",
        "_callMenuSettingApi('menu_action', ['toggle_plugin_manager'], 'PLUGINS', function() {",
        "_callMenuSettingApi('pin_plugin', [pid, !pinned], 'PLUGINS', function() {",
        "showToast(pinned ? 'PLUGIN UNPINNED' : 'PLUGIN PINNED');",
        "_callMenuSettingApi(en ? 'disable_plugin' : 'enable_plugin', [id], 'PLUGINS', function() {",
        "showToast(en ? 'PLUGIN DISABLED' : 'PLUGIN ENABLED');",
        "_callMenuSettingApi('invoke_ui_action', [panelId, action, JSON.stringify(payload || {})], 'PLUGIN PANEL', function() {",
        "_callMenuSettingApi('set_plugin_hotkey', [sel.getAttribute('data-action'), sel.value], 'PLUGIN HOTKEYS', function() {",
        "showToast('HOTKEY UPDATED');",
        "function _saoUpdaterApiResult(result, fallback)",
        "function _saoCallUpdaterApi(methodName, fallback, onOk, onFail)",
        "_saoCallUpdaterApi('apply_update', '启动 updater 失败', function() {}, function(message) {",
        "_saoCallUpdaterApi('download_update', '请求更新失败', function() {}, function(message) {",
        "_saoCallUpdaterApi('skip_update', '跳过更新失败', function() {",
        "function _toggleMenuViaBridgeOrClose()",
        "return window.pywebview.api.toggle_menu();",
        "}).catch(function() {\n        closeMenu();",
        "return window.pywebview.api.get_panel_themes();",
        "function _callMenuBridgeQuiet(methodName, args, onFail)",
        "function _requestExitApplication()",
        "_callMenuBridgeQuiet('exit_app', [], fallbackExit)",
        "if (!_callMenuBridgeQuiet('menu_action', ['exit'], failExit)) {",
        "_callMenuBridgeQuiet('alert_ok');",
        "_callMenuSettingApi('set_auto_key_server_url', [serverUrl], 'AUTO KEYS', function(data) {",
        "_callMenuSettingApi('set_boss_raid_server_url', [serverUrl], 'BOSS RAID', function(data) {",
    ]
    for snippet in plugin_menu_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe plugin menu hotkey count snippet: " + snippet)

    plugin_popup_identity_raw_patterns = [
        "var nextPlugin = String(pluginId || '');",
        "return String(panel && (panel.id || panel.panel_id) || '');",
        "return p.plugin_id === _pdPlugin && p.available;",
        "return h.plugin_id === _pdPlugin;",
        "if (title) title.textContent = (panels[0] && panels[0].title) || _pdPlugin;",
    ]
    for pattern in plugin_popup_identity_raw_patterns:
        if pattern in html:
            raise AssertionError("plugin popup/detached ids must preserve zero text and normalize bridge ids: " + pattern)
    plugin_popup_identity_safe_required = [
        "function _pluginText(value, fallback)",
        "function _pluginFirstText(values, fallback)",
        "function _pluginId(value)",
        "var nextPlugin = _pluginId(pluginId);",
        "return _pluginFirstText([panel.id, panel.panel_id], '');",
        "return _pluginId(p.plugin_id) === _pdPlugin && p.available;",
        "return _pluginId(h.plugin_id) === _pdPlugin;",
        "if (title) title.textContent = _pluginFirstText([panels[0] && panels[0].title, _pdPlugin], 'Plugin');",
    ]
    for snippet in plugin_popup_identity_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe plugin popup/detached id snippet: " + snippet)

    plugin_popup_text_raw_patterns = [
        "var cur = String(h.current_key || '').toUpperCase();",
        "var dk = String(h.default_key || '').toUpperCase();",
        "var dTaken = dk && occupied[dk] && dk !== cur;",
        "var taken = occupied[k] && k !== cur;",
        "esc(h.label || h.hotkey_id)",
        "esc(it.label) + ' <small>'",
        "(it.pinned ? '★ ' : '') + esc(it.label) + hk",
    ]
    for pattern in plugin_popup_text_raw_patterns:
        if pattern in html:
            raise AssertionError("plugin popup labels/hotkey occupancy must preserve zero text and explicit occupied keys: " + pattern)
    plugin_popup_text_safe_required = [
        "var _pluginOwn = Object.prototype.hasOwnProperty;",
        "var cur = _pluginText(h.current_key, '').toUpperCase();",
        "var dk = _pluginText(h.default_key, '').toUpperCase();",
        "var dTaken = dk && _pluginOwn.call(occupied, dk) && dk !== cur;",
        "var taken = _pluginOwn.call(occupied, k) && k !== cur;",
        "esc(_pluginFirstText([h.label, h.hotkey_id, h.action], 'Hotkey'))",
        "var label = _pluginFirstText([it.label, it.name, id], 'Plugin');",
        "esc(label) + ' <small>'",
        "(it.pinned ? '★ ' : '') + esc(label) + hk",
    ]
    for snippet in plugin_popup_text_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe plugin popup text/hotkey snippet: " + snippet)

    auto_key_raw_patterns = [
        "if (kind === 'int') value = parseInt(value || 0, 10) || 0;",
        "Math.round(Number(condition.value || 0) * 100)",
        "var v = parseInt(val) || 0;",
        "api.set_linkage_global_cooldown(parseFloat(val) || 1.0);",
        "var actionCount = Number(summary.action_count || ((profile.actions || []).length || 0));",
        "var enabledCount = Number(summary.enabled_action_count || 0);",
        "var profileProfessionId = (draft && Number(draft.profession_id || 0) > 0) ? String(draft.profession_id) : '不限制 Any';",
        "_akSummaryItem('当前职业 ID Current Profession ID', Number(identity.profession_id || 0) > 0 ? String(identity.profession_id) : '--', !(Number(identity.profession_id || 0) > 0))",
        "document.getElementById('ak-identity-profession-id').textContent = Number(identity.profession_id || 0) > 0 ? String(identity.profession_id) : '--';",
        "document.getElementById('br-identity-profession-id').textContent = Number(identity.profession_id || 0) > 0 ? String(identity.profession_id) : '--';",
        "_escHtml(String(condition.value || ''))",
        "_escHtml(value || '--')",
        "_akIntValue('slot_index', condition.slot_index || 1)",
        "_akIntValue('profession_id', draft.profession_id || 0)",
        "_akIntValue('tick_ms', (draft.engine && draft.engine.tick_ms) || 50)",
        "_akIntValue('slot_index', action.slot_index || 1)",
        "_akIntValue('press_count', action.press_count || 1)",
        "_akIntValue('press_interval_ms', action.press_interval_ms || 0)",
        "_akIntValue('hold_ms', action.hold_ms || 0)",
        "_akIntValue('ready_delay_ms', action.ready_delay_ms || 0)",
        "_akIntValue('min_rearm_ms', action.min_rearm_ms || 0)",
        "_akIntValue('post_delay_ms', action.post_delay_ms || 0)",
    ]
    for pattern in auto_key_raw_patterns:
        if pattern in html:
            raise AssertionError("AutoKey numeric draft/render values must use bounded helpers: " + pattern)
    auto_key_safe_required = [
        "function _akIntValue",
        "_akIntValue('slot_index', condition.slot_index)",
        "_akIntValue('tick_ms', draft.engine && draft.engine.tick_ms)",
        "_akIntValue('slot_index', action.slot_index)",
        "_akIntValue('press_count', action.press_count)",
        "_akIntValue('post_delay_ms', action.post_delay_ms)",
        "if (kind === 'int') value = _akIntValue(fieldName, value);",
        "var pct = _clampNum(value, 0, 0, 100);",
        "var v = _clampInt(val, _dpsFadeTimeoutValue, 0, 120);",
        "if (cdEl) cdEl.value = _clampNum(s.global_cooldown_s, 1.0, 0, 60);",
        "var seconds = _clampNum(val, 1.0, 0, 60);",
        "api.set_linkage_global_cooldown(seconds);",
        "var actionCount = _clampInt(summary.action_count, _profileEntries(profile.actions).length, 0, 999999);",
        "var enabledCount = _clampInt(summary.enabled_action_count, 0, 0, actionCount);",
        "var profileProfessionIdValue = draft ? _clampInt(draft.profession_id, 0, 0, 999999999) : 0;",
        "var identityProfessionId = _clampInt(identity.profession_id, 0, 0, 999999999);",
        "_akSummaryItem('当前职业 ID Current Profession ID', identityProfessionId > 0 ? String(identityProfessionId) : '--', !(identityProfessionId > 0))",
        "var akProfessionId = _clampInt(identity.profession_id, 0, 0, 999999999);",
        "var brProfessionId = _clampInt(identity.profession_id, 0, 0, 999999999);",
        "function _akTextValue(value)",
        "_escHtml(_akTextValue(condition.value))",
        "function _akDisplayValue(value, fallback)",
        "_escHtml(_akDisplayValue(value, '--'))",
        "_akIntValue('slot_index', condition.slot_index)",
        "_akIntValue('profession_id', draft.profession_id)",
        "_akIntValue('tick_ms', draft.engine && draft.engine.tick_ms)",
        "_akIntValue('slot_index', action.slot_index)",
        "_akIntValue('press_count', action.press_count)",
        "_akIntValue('press_interval_ms', action.press_interval_ms)",
        "_akIntValue('hold_ms', action.hold_ms)",
        "_akIntValue('ready_delay_ms', action.ready_delay_ms)",
        "_akIntValue('min_rearm_ms', action.min_rearm_ms)",
        "_akIntValue('post_delay_ms', action.post_delay_ms)",
    ]
    for snippet in auto_key_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe AutoKey numeric form snippet: " + snippet)

    cloud_payload_raw_patterns = [
        "_akSummaryItem('当前 UID Current UID', identity.player_uid || '--', !identity.player_uid)",
        "_akSummaryItem('当前玩家 Current Player', identity.player_name || '--', !identity.player_name)",
        "document.getElementById('ak-identity-uid').textContent = identity.player_uid || '--';",
        "document.getElementById('ak-identity-name').textContent = identity.player_name || '--';",
        "document.getElementById('ak-identity-profession-name').textContent = identity.profession_name || '--';",
        "document.getElementById('br-identity-uid').textContent = identity.player_uid || '--';",
        "document.getElementById('br-identity-name').textContent = identity.player_name || '--';",
        "document.getElementById('br-identity-profession-name').textContent = identity.profession_name || '--';",
        "(identity.missing || []).join(', ') || 'identity'",
        "var results = Array.isArray(search.results) ? search.results : [];",
        "resultsEl.innerHTML = results.map(function(item) {",
        "var items = Array.isArray(search.results) ? search.results : [];",
        "resultsEl.innerHTML = items.map(function(item) {",
        "var remoteId = String(item.id || '');",
        "_escHtml('UID ' + (item.player_uid || '--'))",
        "_escHtml(item.player_name || '--')",
        "_escHtml(item.profession_name || '任意 Any')",
        "_escHtml(item.player_name || '?')",
        "_escHtml(item.player_uid || '--')",
    ]
    for pattern in cloud_payload_raw_patterns:
        if pattern in html:
            raise AssertionError("AutoKey/BossRaid cloud payloads must guard malformed entries and preserve zero text: " + pattern)
    cloud_payload_safe_required = [
        "function _cloudEntry(entry)",
        "function _cloudEntries(value)",
        "function _cloudText(value, fallback)",
        "function _cloudMissingText(value)",
        "var identityUid = _cloudText(identity.player_uid, '--');",
        "var identityName = _cloudText(identity.player_name, '--');",
        "_akSummaryItem('当前 UID Current UID', identityUid, identityUid === '--')",
        "_akSummaryItem('当前玩家 Current Player', identityName, identityName === '--')",
        "document.getElementById('ak-identity-uid').textContent = _cloudText(identity.player_uid, '--');",
        "document.getElementById('ak-identity-name').textContent = _cloudText(identity.player_name, '--');",
        "document.getElementById('ak-identity-profession-name').textContent = _cloudText(identity.profession_name, '--');",
        "var akMissingText = _cloudMissingText(identity.missing);",
        "bits.push('<span class=\"auto-key-pill auto-key-status-warn\">缺少 ' + _escHtml(akMissingText) + '</span>');",
        "var results = _cloudEntries(search.results);",
        "resultsEl.innerHTML = results.map(function(rawItem) {",
        "document.getElementById('br-identity-uid').textContent = _cloudText(identity.player_uid, '--');",
        "document.getElementById('br-identity-name').textContent = _cloudText(identity.player_name, '--');",
        "document.getElementById('br-identity-profession-name').textContent = _cloudText(identity.profession_name, '--');",
        "var brMissingText = _cloudMissingText(identity.missing);",
        "bits.push('<span class=\"boss-raid-pill auto-key-status-warn\">缺少 ' + _escHtml(brMissingText) + '</span>');",
        "var items = _cloudEntries(search.results);",
        "resultsEl.innerHTML = items.map(function(rawItem) {",
        "var item = _cloudEntry(rawItem);",
        "var remoteId = _cloudText(item.id, '');",
        "_escHtml('UID ' + _cloudText(item.player_uid, '--'))",
        "_escHtml(_cloudText(item.player_name, '--'))",
        "_escHtml(_cloudText(item.profession_name, '任意 Any'))",
        "_escHtml(_cloudText(item.player_name, '?'))",
        "_escHtml(_cloudText(item.player_uid, '--'))",
        "showAlert('AUTO KEYS', '当前角色信息不完整，无法上传。缺少: ' + _cloudMissingText(identity.missing), true);",
        "showAlert('BOSS RAID', '当前角色信息不完整，无法上传。缺少: ' + _cloudMissingText(identity.missing), true);",
    ]
    for snippet in cloud_payload_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe AutoKey/BossRaid cloud payload snippet: " + snippet)

    cloud_settings_raw_patterns = [
        "if (document.getElementById('ak-server-url')) document.getElementById('ak-server-url').value = state.server_url || '';",
        "if (document.getElementById('ak-search-q')) document.getElementById('ak-search-q').value = query.q || '';",
        "if (document.getElementById('br-server-url')) document.getElementById('br-server-url').value = state.server_url || '';",
        "if (document.getElementById('br-search-q')) document.getElementById('br-search-q').value = query.q || '';",
        "var serverUrl = (document.getElementById('ak-server-url').value || '').trim();",
        "q: (document.getElementById('ak-search-q').value || '').trim(),",
        "var serverUrl = (document.getElementById('br-server-url').value || '').trim();",
        "q: ((document.getElementById('br-search-q') || {}).value || '').trim(),",
        "var targetId = _autoKeyDraftProfile ? _autoKeyDraftProfile.id : ((_autoKeyState && _autoKeyState.active_profile_id) || '');",
        "showToast('UPLOAD COMPLETE #' + (data.remote_id || ''));",
        "var targetId = _brDraftProfile ? _brDraftProfile.id : ((_bossRaidState && _bossRaidState.active_profile_id) || '');",
        "showToast('BOSS RAID UPLOADED #' + (data.remote_id || ''));",
    ]
    for pattern in cloud_settings_raw_patterns:
        if pattern in html:
            raise AssertionError("AutoKey/BossRaid cloud settings and upload ids must preserve zero text and guard inputs: " + pattern)
    cloud_settings_safe_required = [
        "function _cloudInputText(id)",
        "function _cloudSetInputText(id, value)",
        "_cloudSetInputText('ak-server-url', state.server_url);",
        "_cloudSetInputText('ak-search-q', query.q);",
        "_cloudSetInputText('br-server-url', state.server_url);",
        "_cloudSetInputText('br-search-q', query.q);",
        "var serverUrl = _cloudInputText('ak-server-url');",
        "q: _cloudInputText('ak-search-q'),",
        "profession_name: _cloudInputText('ak-search-profession-name'),",
        "var serverUrl = _cloudInputText('br-server-url');",
        "q: _cloudInputText('br-search-q'),",
        "player_name: _cloudInputText('br-search-player-name'),",
        "var targetId = _autoKeyDraftProfile ? _cloudText(_autoKeyDraftProfile.id, '') : _cloudText(_autoKeyState && _autoKeyState.active_profile_id, '');",
        "showToast('UPLOAD COMPLETE #' + _cloudText(data && data.remote_id, ''));",
        "var targetId = _brDraftProfile ? _cloudText(_brDraftProfile.id, '') : _cloudText(_bossRaidState && _bossRaidState.active_profile_id, '');",
        "showToast('BOSS RAID UPLOADED #' + _cloudText(data && data.remote_id, ''));",
    ]
    for snippet in cloud_settings_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe AutoKey/BossRaid cloud settings/upload snippet: " + snippet)

    tab_state_raw_patterns = [
        "if (tabName) _autoKeyTab = tabName;",
        "_autoKeyTab = tabName || 'local';",
        "_brTab = tab || 'local';",
        "var active = document.querySelector('#boss-raid-tabs .boss-raid-tab[data-tab=\"' + _brTab + '\"]');",
        "var panel = document.querySelector('#sub-bossRaid .boss-raid-panel[data-tab=\"' + _brTab + '\"]');",
    ]
    for pattern in tab_state_raw_patterns:
        if pattern in html:
            raise AssertionError("AutoKey/BossRaid tab state must normalize tab names before UI state or selectors: " + pattern)
    tab_state_safe_required = [
        "function _menuTabName(value)",
        "var allowed = { local: true, editor: true, cloud: true };",
        "return allowed[key] ? key : 'local';",
        "_autoKeyTab = _menuTabName(tabName);",
        "_brTab = _menuTabName(tab);",
        "var active = document.querySelector('#boss-raid-tabs .boss-raid-tab[data-tab=\"' + _menuTabName(_brTab) + '\"]');",
        "var panel = document.querySelector('#sub-bossRaid .boss-raid-panel[data-tab=\"' + _menuTabName(_brTab) + '\"]');",
    ]
    for snippet in tab_state_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe AutoKey/BossRaid tab-state snippet: " + snippet)

    profile_payload_raw_patterns = [
        "return (_autoKeyState && Array.isArray(_autoKeyState.profiles_full)) ? _autoKeyState.profiles_full : [];",
        "((state.profiles || [])).forEach(function(item) { summaries[String(item.id || '')] = item; });",
        "listEl.innerHTML = profiles.map(function(profile) {",
        "var id = String(profile.id || '');",
        "((profile.actions || []).length || 0)",
        "_escHtml(profile.profile_name || '配置 Profile')",
        "_escHtml((profile.profession_name || '任意 Any').toUpperCase())",
        "_escHtml(profile.updated_at || '')",
        "_escHtml(profile.description || '暂无说明 / No description')",
        "var profiles = (_bossRaidState && Array.isArray(_bossRaidState.profiles_full)) ? _bossRaidState.profiles_full : [];",
        "var profiles = (_bossRaidState && Array.isArray(_bossRaidState.profiles)) ? _bossRaidState.profiles : [];",
        "var found = profiles.find(function(p) { return p.id === _brSelectedProfileId; });",
        "return profiles.find(function(p) { return String(p.id) === String(id); }) || null;",
        "list.innerHTML = profiles.map(function(p) {",
        "var isActive = (p.id === activeId);",
        "onclick=\"_brSelectProfile(' + _jsAttrArg(p.id) + ')\"",
        "_escHtml(p.profile_name || 'Boss Raid')",
    ]
    for pattern in profile_payload_raw_patterns:
        if pattern in html:
            raise AssertionError("AutoKey/BossRaid local profile payloads must guard malformed entries and preserve zero text: " + pattern)
    profile_payload_safe_required = [
        "function _profileEntry(entry)",
        "function _profileEntries(value)",
        "function _profileText(value, fallback)",
        "return _profileEntries(_autoKeyState && _autoKeyState.profiles_full).map(_profileEntry);",
        "_profileEntries(state.profiles).forEach(function(rawItem) {",
        "var item = _profileEntry(rawItem);",
        "summaries[_profileText(item.id, '')] = item;",
        "listEl.innerHTML = profiles.map(function(rawProfile) {",
        "var profile = _profileEntry(rawProfile);",
        "var id = _profileText(profile.id, '');",
        "var actionCount = _clampInt(summary.action_count, _profileEntries(profile.actions).length, 0, 999999);",
        "_escHtml(_profileText(profile.profile_name, '配置 Profile'))",
        "_escHtml(_profileText(profile.profession_name, '任意 Any').toUpperCase())",
        "_escHtml(_profileText(profile.updated_at, ''))",
        "_escHtml(_profileText(profile.description, '暂无说明 / No description'))",
        "function _brProfilesFull()",
        "function _brProfiles()",
        "var safeState = _profileEntry(state);",
        "var profiles = _brProfilesFull();",
        "var p = _profileEntry(rawProfile);",
        "return _profileText(p.id, '') === _brSelectedProfileId;",
        "return _profileText(p.id, '') === _profileText(id, '');",
        "var profiles = _brProfiles();",
        "list.innerHTML = profiles.map(function(rawProfile) {",
        "var id = _profileText(p.id, '');",
        "var isActive = (id === activeId);",
        "onclick=\"_brSelectProfile(' + _jsAttrArg(id) + ')\"",
        "_escHtml(_profileText(p.profile_name, 'Boss Raid'))",
    ]
    for snippet in profile_payload_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe AutoKey/BossRaid local profile payload snippet: " + snippet)

    profile_selection_raw_patterns = [
        "var id = String(profileId || '');",
        "var chosenId = String(profileId || _autoKeySelectedProfileId || '');",
        "String(_autoKeyDraftProfile.id || '') === _autoKeySelectedProfileId",
        "String(state.active_profile_id || '') === id",
        "String(_autoKeySelectedProfileId || '') === id",
        "_autoKeySelectedProfileId = String(profileId || '');",
        "if (String(_autoKeySelectedProfileId || '') === String(profileId || ''))",
        "_autoKeySelectedProfileId = String(payload.id || _autoKeySelectedProfileId || '');",
        "showAlert('AUTO KEYS', '已导出到 / Exported to:\\n' + (data.path || ''), true);",
        "return _profileText(p.id, '') === String(id);",
        "var targetId = String(profileId || _brSelectedProfileId || ((_bossRaidState && _bossRaidState.active_profile_id) || ''));",
        "String(_brDraftProfile.id || '') === String(profile.id || '')",
        "var activeId = (_bossRaidState && _bossRaidState.active_profile_id) || '';",
        "_brSelectedProfileId = String(id || '');",
        "_brSelectedProfileId = String((_bossRaidState && _bossRaidState.active_profile_id) || '');",
        "_brSelectedProfileId = String(payload.id || _brSelectedProfileId || '');",
        "showAlert('BOSS RAID', '已导出到 / Exported to:\\n' + (data.path || ''), true);",
    ]
    for pattern in profile_selection_raw_patterns:
        if pattern in html:
            raise AssertionError("AutoKey/BossRaid profile selection/export ids must preserve zero text: " + pattern)
    profile_selection_safe_required = [
        "var id = _profileText(profileId, '');",
        "var chosenId = _profileText(profileId, _autoKeySelectedProfileId);",
        "_profileText(_autoKeyDraftProfile.id, '') === _autoKeySelectedProfileId",
        "_profileText(state.active_profile_id, '') === id",
        "_profileText(_autoKeySelectedProfileId, '') === id",
        "_autoKeySelectedProfileId = _profileText(profileId, '');",
        "if (_profileText(_autoKeySelectedProfileId, '') === _profileText(profileId, ''))",
        "_autoKeySelectedProfileId = _profileText(payload.id, _autoKeySelectedProfileId);",
        "showAlert('AUTO KEYS', '已导出到 / Exported to:\\n' + _profileText(data && data.path, ''), true);",
        "return _profileText(p.id, '') === _profileText(id, '');",
        "var targetId = _profileText(profileId, _profileText(_brSelectedProfileId, _profileText(_bossRaidState && _bossRaidState.active_profile_id, '')));",
        "_profileText(_brDraftProfile.id, '') === _profileText(profile.id, '')",
        "var activeId = _profileText(_bossRaidState && _bossRaidState.active_profile_id, '');",
        "_brSelectedProfileId = _profileText(id, '');",
        "_brSelectedProfileId = _profileText(_bossRaidState && _bossRaidState.active_profile_id, '');",
        "_brSelectedProfileId = _profileText(payload.id, _brSelectedProfileId);",
        "showAlert('BOSS RAID', '已导出到 / Exported to:\\n' + _profileText(data && data.path, ''), true);",
    ]
    for snippet in profile_selection_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe AutoKey/BossRaid profile selection/export snippet: " + snippet)

    boss_raid_api_raw_patterns = [
        "api[method].apply(api, args || []).then(function(result) {\n        var data = (typeof result === 'string') ? JSON.parse(result) : result;",
        "results.forEach(function(item) {\n            var data = (typeof item === 'string') ? JSON.parse(item) : item;",
        "window.pywebview.api.get_boss_raid_state().then(function(result) {\n            var data = (typeof result === 'string') ? JSON.parse(result) : result;",
        "window.pywebview.api.get_auto_key_state().then(function(result) {\n            var data = _akParseApiResult(result);",
        "window.pywebview.api.get_boss_raid_state().then(function(result) {\n            var data = _brParseApiResult(result);",
    ]
    for pattern in boss_raid_api_raw_patterns:
        if pattern in html:
            raise AssertionError("BossRaid API responses must use safe parsing before state sync or user alerts: " + pattern)
    boss_raid_api_safe_required = [
        "function _brApiObject(value)",
        "return !!(value && typeof value === 'object' && !Array.isArray(value));",
        "function _brParseApiResult(result)",
        "var parsed = JSON.parse(result);",
        "return _brApiObject(parsed) ? parsed : { ok: false, message: String(result) };",
        "return _brApiObject(result) ? result : { ok: false, message: String(result) };",
        "var data = _brParseApiResult(result);",
        "function _loadInitialAutoKeyState()",
        "return api.get_auto_key_state();",
        "showToast(_menuApiMessage(data, 'AUTO KEY STATE LOAD FAILED'), 3200);",
        "function _loadInitialBossRaidState()",
        "return api.get_boss_raid_state();",
        "showToast(_menuApiMessage(data, 'BOSS RAID STATE LOAD FAILED'), 3200);",
        "_loadInitialAutoKeyState();",
        "_loadInitialBossRaidState();",
        "data.message || data.error || data.detail",
    ]
    for snippet in boss_raid_api_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe BossRaid API response snippet: " + snippet)

    boss_raid_editor_payload_raw_patterns = [
        "var phases = _brDraftProfile.phases || [];",
        "var timelines = phase.timelines || [];",
        "'<span class=\"br-phase-name\">' + _escHtml(phase.name || 'P' + (pi+1)) + '</span>'",
        "'<input type=\"text\" value=\"' + _escHtml(tl.label || '') + '\" placeholder=\"标签 Label\"",
    ]
    for pattern in boss_raid_editor_payload_raw_patterns:
        if pattern in html:
            raise AssertionError("BossRaid editor phase/timeline payloads must guard arrays and preserve zero text: " + pattern)
    boss_raid_editor_payload_safe_required = [
        "function _brEditorPhases()",
        "var phases = _profileEntries(_brDraftProfile.phases).map(function(rawPhase) {",
        "phase.trigger = _profileEntry(phase.trigger);",
        "phase.timelines = _profileEntries(phase.timelines).map(function(rawTl) {",
        "_brDraftProfile.phases = phases;",
        "var phases = _brEditorPhases();",
        "container.innerHTML = phases.map(function(phase, pi) {",
        "var trigger = phase.trigger;",
        "var timelines = phase.timelines;",
        "timelines.map(function(rawTl, ti) {",
        "var tl = _profileEntry(rawTl);",
        "tl.condition = _profileEntry(tl.condition);",
        "var cond = _profileEntry(tl.condition);",
        "_escHtml(_profileText(phase.name, 'P' + (pi+1)))",
        "_escHtml(_profileText(tl.label, ''))",
    ]
    for snippet in boss_raid_editor_payload_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe BossRaid editor phase/timeline snippet: " + snippet)

    auto_key_editor_text_raw_patterns = [
        "document.getElementById('ak-profile-name').value = draft.profile_name || '';",
        "document.getElementById('ak-profile-profession-name').value = draft.profession_name || '';",
        "document.getElementById('ak-profile-description').value = draft.description || '';",
        "if (!draft || !Array.isArray(draft.actions) || !draft.actions.length) {",
        "listEl.innerHTML = draft.actions.map(function(action, index) {",
        "var jsonText = String(action._conditions_text || JSON.stringify(action.conditions || [], null, 2));",
        "_escHtml(action.label || ('动作 Action ' + (index + 1)))",
        "_escHtml(action.label || '')",
        "_escHtml(String(action.key || ''))",
    ]
    for pattern in auto_key_editor_text_raw_patterns:
        if pattern in html:
            raise AssertionError("AutoKey editor text/action payloads must guard entries and preserve zero text: " + pattern)
    auto_key_editor_text_safe_required = [
        "document.getElementById('ak-profile-name').value = _profileText(draft.profile_name, '');",
        "document.getElementById('ak-profile-profession-name').value = _profileText(draft.profession_name, '');",
        "document.getElementById('ak-profile-description').value = _profileText(draft.description, '');",
        "var actions = draft ? _profileEntries(draft.actions).map(_profileEntry) : [];",
        "if (draft) draft.actions = actions;",
        "if (!draft || !actions.length) {",
        "listEl.innerHTML = actions.map(function(action, index) {",
        "var jsonText = _profileText(action._conditions_text, JSON.stringify(_profileEntries(action.conditions), null, 2));",
        "_escHtml(_profileText(action.label, '动作 Action ' + (index + 1)))",
        "_escHtml(_profileText(action.label, ''))",
        "_escHtml(_profileText(action.key, ''))",
    ]
    for snippet in auto_key_editor_text_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe AutoKey editor text/action snippet: " + snippet)

    auto_key_draft_action_raw_patterns = [
        "if (!action.id) action.id = _akNewClientId('action');",
        "if (!action.label) action.label = 'Action ' + (index + 1);",
        "if (!Array.isArray(action.conditions)) action.conditions = [];",
        "action._conditions_text = JSON.stringify(action.conditions || [], null, 2);",
        "if (fieldName === 'key') value = String(value || '').toUpperCase();",
        "_autoKeyDraftProfile.actions[index]._conditions_text = String(text || '');",
        "var parsed = String(text || '').trim();",
        "action._conditions_text = String(text || '');",
        "action.label = String(action.label || 'Action') + ' Copy';",
        "_autoKeySelectedProfileId = (data.state && data.state.active_profile_id) || _autoKeySelectedProfileId;",
    ]
    for pattern in auto_key_draft_action_raw_patterns:
        if pattern in html:
            raise AssertionError("AutoKey draft action text/id inputs must guard entries and preserve zero text: " + pattern)
    auto_key_draft_action_safe_required = [
        "draft.actions = _profileEntries(draft.actions).map(_profileEntry);",
        "action.id = _profileText(action.id, _akNewClientId('action'));",
        "action.label = _profileText(action.label, 'Action ' + (index + 1));",
        "action.conditions = _profileEntries(action.conditions).map(_profileEntry);",
        "action._conditions_text = JSON.stringify(action.conditions, null, 2);",
        "if (fieldName === 'key') value = _profileText(value, '').toUpperCase();",
        "_autoKeyDraftProfile.actions[index]._conditions_text = _profileText(text, '');",
        "var rawText = _profileText(text, '');",
        "var parsed = rawText.trim();",
        "action._conditions_text = rawText;",
        "action.label = _profileText(action.label, 'Action') + ' Copy';",
        "_autoKeySelectedProfileId = _profileText(data && data.state && data.state.active_profile_id, _autoKeySelectedProfileId);",
    ]
    for snippet in auto_key_draft_action_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe AutoKey draft action text/id snippet: " + snippet)

    auto_key_condition_type_raw_patterns = [
        "var type = String(conditionType || 'hp_pct_gte');",
        "var type = String(condition.type || 'hp_pct_gte');",
        "_akSlotStateOptions(String(condition.state || 'ready'))",
        "_akConditionTypeOptions(String(condition.type || 'hp_pct_gte'))",
        "conditions.push(_akDefaultCondition(conditionType || 'hp_pct_gte'));",
        "conditions[condIndex] = _akDefaultCondition(conditionType || 'hp_pct_gte');",
        "draft.actions = (draft.actions || []).map(function(action) {",
        "out.conditions = Array.isArray(action.conditions) ? _akClone(action.conditions) : [];",
    ]
    for pattern in auto_key_condition_type_raw_patterns:
        if pattern in html:
            raise AssertionError("AutoKey condition/action types must normalize enum values before render/save: " + pattern)
    auto_key_condition_type_safe_required = [
        "function _akConditionType(value)",
        "var allowed = { hp_pct_gte: true, hp_pct_lte: true, sta_pct_gte: true, burst_ready_is: true, slot_state_is: true, profession_is: true, player_name_is: true };",
        "function _akSlotState(value)",
        "var allowed = { ready: true, cooldown: true, active: true, insufficient_energy: true, unknown: true };",
        "function _akPressMode(value)",
        "var type = _akConditionType(conditionType);",
        "merged.type = _akConditionType(merged.type);",
        "merged.state = _akSlotState(merged.state);",
        "selected = _akConditionType(selected);",
        "selected = _akSlotState(selected);",
        "var type = _akConditionType(condition.type);",
        "_akSlotStateOptions(condition.state)",
        "_akConditionTypeOptions(condition.type)",
        "if (fieldName === 'press_mode') value = _akPressMode(value);",
        "if (fieldName === 'state') value = _akSlotState(value);",
        "draft.actions = _profileEntries(draft.actions).map(function(rawAction) {",
        "var out = _profileEntry(_akClone(rawAction));",
        "out.press_mode = _akPressMode(out.press_mode);",
        "out.conditions = _profileEntries(out.conditions).map(function(rawCondition) {",
        "return _akNormalizeCondition(rawCondition);",
    ]
    for snippet in auto_key_condition_type_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe AutoKey condition/action type snippet: " + snippet)

    auto_key_condition_value_raw_patterns = [
        "return items.map(function(condition) {\n        var merged = _akDefaultCondition(condition && condition.type);",
        "'<option value=\"true\"' + (condition.value ? ' selected' : '')",
        "'<option value=\"false\"' + (!condition.value ? ' selected' : '')",
        "out.conditions = _profileEntries(out.conditions).map(function(rawCondition) {\n            var cond = _profileEntry(_akClone(rawCondition));\n            cond.type = _akConditionType(cond.type);",
    ]
    for pattern in auto_key_condition_value_raw_patterns:
        if pattern in html:
            raise AssertionError("AutoKey condition values must normalize bool/numeric/text payloads before render/save: " + pattern)
    auto_key_condition_value_safe_required = [
        "function _akBoolValue(value, fallback)",
        "function _akNormalizeCondition(condition)",
        "merged.value = _akBoolValue(merged.value, true);",
        "merged.slot_index = _akIntValue('slot_index', merged.slot_index);",
        "merged.value = _akTextValue(merged.value);",
        "merged.value = _clampNum(merged.value, 0.5, 0, 1);",
        "return items.map(_akNormalizeCondition);",
        "var boolValue = _akBoolValue(condition.value, true);",
        "'<option value=\"true\"' + (boolValue ? ' selected' : '')",
        "'<option value=\"false\"' + (!boolValue ? ' selected' : '')",
        "return _akNormalizeCondition(rawCondition);",
    ]
    for snippet in auto_key_condition_value_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe AutoKey condition value snippet: " + snippet)

    auto_key_runtime_text_raw_patterns = [
        "bits.push('配置 Profile ' + (state.active_profile_name || '无 None'));",
        "bits.push('状态 Status ' + ((runtime.last_reason || (runtime.active ? 'RUNNING' : 'IDLE')) || 'IDLE'));",
        "if (runtime.last_action_label) {",
        "bits.push('上次 Last ' + runtime.last_action_label);",
    ]
    for pattern in auto_key_runtime_text_raw_patterns:
        if pattern in html:
            raise AssertionError("AutoKey runtime status text must preserve zero text: " + pattern)
    auto_key_runtime_text_safe_required = [
        "var activeProfileName = _profileText(state.active_profile_name, '无 None');",
        "var runtimeReason = _profileText(runtime.last_reason, runtime.active ? 'RUNNING' : 'IDLE');",
        "var lastActionLabel = _profileText(runtime.last_action_label, '');",
        "bits.push('配置 Profile ' + activeProfileName);",
        "bits.push('状态 Status ' + runtimeReason);",
        "if (lastActionLabel) {",
        "bits.push('上次 Last ' + lastActionLabel);",
    ]
    for snippet in auto_key_runtime_text_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe AutoKey runtime status text snippet: " + snippet)

    theme_settings_raw_patterns = [
        "var t = _panelThemes[p] || (_themeDefaultDark[p] ? 'dark' : 'light');",
        "var t = String(themes[p] || '').toLowerCase();",
        "function _applyMenuTheme(theme) {\n    document.documentElement.classList.toggle('theme-dark', theme === 'dark');",
        "function _toggleTheme(panel) {\n    var cur = _panelThemes[panel] || (_themeDefaultDark[panel] ? 'dark' : 'light');",
        "window.pywebview.api.set_panel_theme(panel, next);",
        "window.pywebview.api.set_panel_theme(panelName, next);",
        "_panelThemes[p] = theme;\n        if (window.pywebview && window.pywebview.api && window.pywebview.api.set_panel_theme) {\n            window.pywebview.api.set_panel_theme(p, theme);",
        "window.pywebview.api.set_panel_theme(p, themeName);",
    ]
    for pattern in theme_settings_raw_patterns:
        if pattern in html:
            raise AssertionError("panel theme settings must normalize panel/theme values before UI or API propagation: " + pattern)
    theme_settings_safe_required = [
        "function _themePanelName(panel)",
        "function _themeValue(value, fallback)",
        "return _themePanels.indexOf(key) >= 0 ? key : '';",
        "return (key === 'light' || key === 'dark') ? key : fallback;",
        "var _themeRequestSeq = 0;",
        "function _panelThemesSnapshot()",
        "function _applyPanelThemeAck(data, fallbackPanel, fallbackTheme)",
        "var t = _themeValue(_panelThemes[p], _themeDefault(p));",
        "var t = _themeValue(themes[p], _themeDefault(p));",
        "document.documentElement.classList.toggle('theme-dark', _themeValue(theme, 'dark') === 'dark');",
        "var panelName = _themePanelName(panel);",
        "if (!panelName) return;",
        "_callMenuSettingApi('set_panel_theme', [panelName, next], 'PANEL THEME', function(data) {",
        "_syncPanelThemes(previousThemes);",
        "var themeName = _themeValue(theme, 'dark');",
        "var pending = _themePanels.length;",
        "_panelThemes[p] = themeName;",
        "_callMenuSettingApi('set_panel_theme', [p, themeName], 'PANEL THEME', function(data) {",
        "pending -= 1;",
        "failed = true;",
    ]
    for snippet in theme_settings_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe panel theme settings snippet: " + snippet)

    info_update_raw_patterns = [
        "function updateInfo(data) {\n    if (data.username) document.getElementById('info-username').textContent = data.username;",
        "if (data.profession) document.getElementById('info-profession').textContent = data.profession;",
        "if (data.des) document.getElementById('info-des').textContent = data.des;",
        "if (data.file) document.getElementById('current-file').textContent = '♪ ' + data.file;",
    ]
    for pattern in info_update_raw_patterns:
        if pattern in html:
            raise AssertionError("info panel updates must guard payload shape and preserve zero text: " + pattern)
    info_update_safe_required = [
        "function updateInfo(data) {\n    data = _profileEntry(data);",
        "if (data.username !== undefined) document.getElementById('info-username').textContent = _profileText(data.username, '');",
        "if (data.profession !== undefined) document.getElementById('info-profession').textContent = _profileText(data.profession, '');",
        "if (data.des !== undefined) document.getElementById('info-des').textContent = _profileText(data.des, '');",
        "if (data.file !== undefined) document.getElementById('current-file').textContent = '♪ ' + _profileText(data.file, '');",
    ]
    for snippet in info_update_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe info panel update snippet: " + snippet)

    source_settings_raw_patterns = [
        "skills: 'vision',",
        "var component = this.getAttribute('data-component');\n        var mode = this.getAttribute('data-mode');\n        if (!component || !mode) return;",
        "var mode = this.getAttribute('data-mode') || 'tcp';",
        "if (cfg.data_source_map) {\n        Object.keys(_dataSourceMap).forEach(function(key) {\n            if (cfg.data_source_map[key]) {\n                _dataSourceMap[key] = cfg.data_source_map[key];",
    ]
    for pattern in source_settings_raw_patterns:
        if pattern in html:
            raise AssertionError("data source settings must normalize values before UI or API propagation: " + pattern)
    source_settings_safe_required = [
        "var _sourceComponents = ['hp', 'level', 'stamina', 'skills', 'identity'];",
        "skills: 'packet',",
        "function _sourceComponentName(value)",
        "function _sourceMode(value, fallback)",
        "function _componentSourceValue(component, value)",
        "function _syncDataSourceMap(map)",
        "function _memSourceValue(value)",
        "var component = _sourceComponentName(this.getAttribute('data-component'));",
        "var mode = _componentSourceValue(component, this.getAttribute('data-mode'));",
        "_callMenuSettingApi('set_component_source', [component, mode], 'DATA SOURCE', function(data) {",
        "var mode = _memSourceValue(this.getAttribute('data-mode'));",
        "_callMenuSettingApi('set_data_source', [mode], 'DATA SOURCE', function(data) {",
        "_memDataSource = _memSourceValue(cfg.mem_data_source);",
        "_syncDataSourceMap(cfg.data_source_map);",
        "var legacyMode = _sourceMode(cfg.data_source, 'packet');",
        "_syncDataSourceMap(legacyMap);",
    ]
    for snippet in source_settings_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe data source settings snippet: " + snippet)

    webview_py = SAO_WEBVIEW.read_text(encoding="utf-8")
    dps_settings_raw_patterns = [
        "val = max(0, int(seconds or 0))",
    ]
    for pattern in dps_settings_raw_patterns:
        if pattern in webview_py:
            raise AssertionError("DPS fade timeout bridge values must recover bad input and clamp to 0..120: " + pattern)
    dps_settings_safe_required = [
        "def _dps_fade_timeout_value(seconds: Any) -> int:",
        "if not math.isfinite(value):",
        "return max(0, min(120, int(value)))",
        "val = _dps_fade_timeout_value(seconds)",
    ]
    for snippet in dps_settings_safe_required:
        if snippet not in webview_py:
            raise AssertionError("missing safe DPS fade timeout bridge snippet: " + snippet)

    sound_burst_bridge_raw_patterns = [
        "self._g._set_setting('burst_enabled', bool(enabled))",
        "set_sound_enabled(bool(enabled))",
        "self._g._set_setting('sound_enabled', bool(enabled))",
        "set_sound_volume(int(volume_pct))",
        "self._g._set_setting('sound_volume', int(volume_pct))",
    ]
    for pattern in sound_burst_bridge_raw_patterns:
        if pattern in webview_py:
            raise AssertionError("Python WebView sound/burst bridge values must return ack and recover bad input: " + pattern)
    sound_burst_bridge_safe_required = [
        "def _setting_bool_value(value: Any) -> bool:",
        "def _sound_volume_value(volume_pct: Any) -> int:",
        "return max(0, min(100, int(value)))",
        "state = _setting_bool_value(enabled)",
        "return json.dumps({'ok': True, 'enabled': state}, ensure_ascii=False)",
        "volume = _sound_volume_value(volume_pct)",
        "return json.dumps({'ok': True, 'volume': volume}, ensure_ascii=False)",
    ]
    for snippet in sound_burst_bridge_safe_required:
        if snippet not in webview_py:
            raise AssertionError("missing Python WebView sound/burst bridge snippet: " + snippet)

    slots_theme_bridge_raw_patterns = [
        "slots = [int(x) for x in slots if isinstance(x, (int, float))]",
        "self._g._set_setting('watched_skill_slots', slots)",
        "return False\n            if theme not in {'light', 'dark'}:",
    ]
    for pattern in slots_theme_bridge_raw_patterns:
        if pattern in webview_py:
            raise AssertionError("Python WebView slots/theme bridge must return structured ack and normalize values: " + pattern)
    slots_theme_bridge_safe_required = [
        "def _watched_slot_ids(slots: Any) -> List[int]:",
        "normalized: List[int] = []",
        "if 1 <= slot <= 9 and slot not in seen:",
        "normalized = _watched_slot_ids(slots)",
        "return json.dumps({'ok': True, 'slots': normalized}, ensure_ascii=False)",
        "return json.dumps({'ok': False, 'panel': panel, 'message': 'Unknown panel'}, ensure_ascii=False)",
        "return json.dumps({'ok': True, 'panel': panel, 'theme': theme, 'panel_themes': themes}, ensure_ascii=False)",
    ]
    for snippet in slots_theme_bridge_safe_required:
        if snippet not in webview_py:
            raise AssertionError("missing Python WebView slots/theme bridge snippet: " + snippet)

    source_bridge_raw_patterns = [
        "def set_component_source(self, component, mode):\n        \"\"\"Legacy no-op: per-component source switching is no longer exposed.\"\"\"",
        "'mem_data_source': str(self._get_setting('mem_data_source', 'hybrid') or 'hybrid').lower(),\n            }\n            try:\n                cfg['panel_themes']",
    ]
    for pattern in source_bridge_raw_patterns:
        if pattern in webview_py:
            raise AssertionError("Python WebView source bridge must persist and sync data_source_map: " + pattern)
    source_bridge_safe_required = [
        "DATA_SOURCE_COMPONENTS,",
        "DEFAULT_DATA_SOURCE_MAP,",
        "normalize_source_map,",
        "normalize_source_mode,",
        "mode_name = normalize_source_mode(mode, DEFAULT_DATA_SOURCE_MAP.get(component_name, 'packet'))",
        "ref.set('data_source_map', dict(source_map))",
        "ref.set('data_source', 'mixed')",
        "self._g._reconfigure_data_engines(restart_packet=False)",
        "'data_source_map': dict(source_map),",
        "cfg['data_source_map'] = ref.get_data_source_map()",
        "cfg['data_source_map'] = normalize_source_map(",
    ]
    for snippet in source_bridge_safe_required:
        if snippet not in webview_py:
            raise AssertionError("missing Python WebView source bridge snippet: " + snippet)

    hud_bridge = HUD_SETTINGS_BRIDGE.read_text(encoding="utf-8")
    hud_bridge_raw_patterns = [
        "private static JsonObject HandleComponentSource(JsonObject? payload)",
        "[\"legacy_noop\"] = true,",
        "var seconds = Math.Max(0, rawSeconds);",
    ]
    for pattern in hud_bridge_raw_patterns:
        if pattern in hud_bridge:
            raise AssertionError("C# HUD settings bridge must persist component sources: " + pattern)
    hud_bridge_safe_required = [
        "private JsonObject HandleComponentSource(JsonObject? payload)",
        "private static readonly string[] SourceComponents",
        "private static readonly IReadOnlyDictionary<string, string> DefaultSourceMap",
        "var sourceMap = NormalizeDataSourceMap(_settings.Get<JsonObject?>(SettingsKeys.DataSourceMap));",
        "_settings.Set(SettingsKeys.DataSourceMap, SourceMapToJson(sourceMap));",
        "_settings.Set(SettingsKeys.DataSource, \"mixed\");",
        "[\"data_source_map\"] = SourceMapToJson(sourceMap),",
        "var seconds = Math.Clamp(rawSeconds, 0, 120);",
        "private static string NormalizeSourceComponent(string component)",
        "private static string NormalizeComponentSourceMode(string component, string mode)",
    ]
    for snippet in hud_bridge_safe_required:
        if snippet not in hud_bridge:
            raise AssertionError("missing C# HUD settings source bridge snippet: " + snippet)

    boss_raid_editor_text_raw_patterns = [
        "_escHtml(profile.id || '--')",
        "_escHtml(profile.source || 'local')",
        "_escHtml(profile.remote_id || '--')",
        "_escHtml(profile.updated_at || '--')",
        "if (nameEl) nameEl.value = p.profile_name || '';",
        "if (descEl) descEl.value = p.description || '';",
        "if (targetEl) targetEl.value = p.target_name_pattern || '';",
        "text += ' | ' + (rt.phase_name || 'P?');",
    ]
    for pattern in boss_raid_editor_text_raw_patterns:
        if pattern in html:
            raise AssertionError("BossRaid editor/runtime text values must preserve zero text: " + pattern)
    boss_raid_editor_text_safe_required = [
        "var safeProfile = _profileEntry(profile);",
        "_escHtml(_profileText(safeProfile.id, '--'))",
        "_escHtml(_profileText(safeProfile.source, 'local'))",
        "_escHtml(_profileText(safeProfile.remote_id, '--'))",
        "_escHtml(_profileText(safeProfile.updated_at, '--'))",
        "if (nameEl) nameEl.value = _profileText(p.profile_name, '');",
        "if (descEl) descEl.value = _profileText(p.description, '');",
        "if (targetEl) targetEl.value = _profileText(p.target_name_pattern, '');",
        "text += ' | ' + _profileText(rt.phase_name, 'P?');",
    ]
    for snippet in boss_raid_editor_text_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe BossRaid editor/runtime text snippet: " + snippet)

    updater_raw_patterns = [
        "ui.badge.className = 'saoUpdaterBadge' + (variant ? ' ' + variant : '');",
        "var content = String(text || fallback || '').trim();",
        "showToast('更新检查失败: ' + (s.error || ''), 3500);",
        "var promptKey = (s.state || '') + ':' + (s.latest_version || '');",
        "ui.version.textContent = 'v' + (s.latest_version || '?') + ' · ' + sizeStr;",
        "ui.version.textContent = 'v' + (s.latest_version || '?') + ' 已就绪';",
    ]
    for pattern in updater_raw_patterns:
        if pattern in html:
            raise AssertionError("Updater badge/text rendering must normalize class tokens and preserve zero text: " + pattern)
    updater_safe_required = [
        "function _saoUpdaterBadgeClass(variant)",
        "var allowed = { force: true, active: true, ready: true };",
        "return 'saoUpdaterBadge' + (allowed[key] ? ' ' + key : '');",
        "ui.badge.className = _saoUpdaterBadgeClass(variant);",
        "var content = _profileText(text, _profileText(fallback, '')).trim();",
        "showToast('更新检查失败: ' + _profileText(s.error, ''), 3500);",
        "var promptKey = _profileText(s.state, '') + ':' + _profileText(s.latest_version, '');",
        "ui.version.textContent = 'v' + _profileText(s.latest_version, '?') + ' · ' + sizeStr;",
        "ui.version.textContent = 'v' + _profileText(s.latest_version, '?') + ' 已就绪';",
    ]
    for snippet in updater_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe Updater badge/text snippet: " + snippet)

    menu_response_raw_patterns = [
        "window.pywebview.api.show_last_dps_report().then(function(result) {\n        var data = (typeof result === 'string') ? JSON.parse(result) : result;",
        "api.get_linkage_state().then(function(raw) {\n        var resp = typeof raw === 'string' ? JSON.parse(raw) : raw;",
        "var s = resp.state;\n        document.getElementById('linkage-enabled').checked = s.enabled;",
    ]
    for pattern in menu_response_raw_patterns:
        if pattern in html:
            raise AssertionError("DPS/Linkage menu responses must use safe parsing and state guards: " + pattern)
    menu_response_safe_required = [
        "function _menuApiObject(value)",
        "function _menuParseApiResult(result)",
        "return _menuApiObject(parsed) ? parsed : { ok: false, message: String(result) };",
        "return _menuApiObject(result) ? result : { ok: false, message: String(result) };",
        "var data = _menuParseApiResult(result);",
        "data.message || data.error || data.detail",
        "var resp = _menuParseApiResult(raw);",
        "var s = _menuApiObject(resp.state) ? resp.state : null;",
        "if (!s) return;",
        "var enabledEl = document.getElementById('linkage-enabled');",
        "if (enabledEl) enabledEl.checked = !!s.enabled;",
    ]
    for snippet in menu_response_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe DPS/Linkage response snippet: " + snippet)

    linkage_raw_patterns = [
        "_linkageMappings = s.mappings || [];",
        "if (_linkageMappings.length === 0) {",
        "for (var i = 0; i < _linkageMappings.length; i++) {",
        "var m = _linkageMappings[i];",
        "_escAttr(m.trigger_match || '')",
        "_escAttr(m.action_key || '')",
        "_escAttr(m.action_label || '')",
    ]
    for pattern in linkage_raw_patterns:
        if pattern in html:
            raise AssertionError("Boss/AutoKey linkage mapping values must validate payload shape and preserve zero text: " + pattern)
    linkage_safe_required = [
        "_linkageMappings = Array.isArray(s.mappings) ? s.mappings : [];",
        "var mappings = Array.isArray(_linkageMappings) ? _linkageMappings : [];",
        "_linkageMappings = mappings;",
        "if (mappings.length === 0) {",
        "for (var i = 0; i < mappings.length; i++) {",
        "var m = mappings[i] || {};",
        "_escAttr(_akTextValue(m.trigger_match))",
        "_escAttr(_akTextValue(m.action_key))",
        "_escAttr(_akTextValue(m.action_label))",
    ]
    for snippet in linkage_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe Boss/AutoKey linkage mapping snippet: " + snippet)

    leaderboard_payload_raw_patterns = [
        "function showLeaderboard(dataJson) {\n    var data = (typeof dataJson === 'string') ? JSON.parse(dataJson) : dataJson;",
        "_lbEntries = (data && data.entries) ? data.entries.slice() : [];",
        "_lbSelfDevice = data.self_device || '';",
        "_lbSelfDeviceName = data.self_player_id || data.self_device_name || '';",
        "var shown = _lbSelfDeviceName || '--';",
        "var primaryName = _escHtml(e.player_id || e.username || e.device_name || 'Player');",
        "_lbSelfDeviceName = _lbSelfDeviceName || e.player_id || e.username || e.device_name || '';",
        "document.getElementById('lb-self-id').textContent = 'PLAYER ID: ' + (_lbSelfDeviceName || '--');",
    ]
    for pattern in leaderboard_payload_raw_patterns:
        if pattern in html:
            raise AssertionError("Leaderboard payload/text rendering must guard malformed payloads and preserve zero text: " + pattern)
    leaderboard_payload_safe_required = [
        "function _lbPayload(dataJson)",
        "try { return JSON.parse(dataJson) || {}; } catch (e) { return {}; }",
        "function _lbText(value, fallback)",
        "function _lbFirstText(values, fallback)",
        "var shown = _lbText(_lbSelfDeviceName, '--');",
        "var primaryName = _escHtml(_lbFirstText([e.player_id, e.username, e.device_name], 'Player'));",
        "var data = _lbPayload(dataJson);",
        "_lbEntries = Array.isArray(data.entries) ? data.entries.map(function(e) { return (e && typeof e === 'object') ? e : {}; }) : [];",
        "_lbSelfDevice = _lbText(data.self_device, '');",
        "_lbSelfDeviceName = _lbFirstText([data.self_player_id, data.self_device_name], '');",
        "_lbSelfDeviceName = _lbFirstText([_lbSelfDeviceName, e.player_id, e.username, e.device_name], '');",
        "document.getElementById('lb-self-id').textContent = 'PLAYER ID: ' + _lbText(_lbSelfDeviceName, '--');",
    ]
    for snippet in leaderboard_payload_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe Leaderboard payload/text snippet: " + snippet)

    leaderboard_sort_raw_patterns = [
        "_lbCurrentSort = data.sort || _lbCurrentSort;",
        "var activeTab = document.querySelector('.lb-tab[data-sort=\"' + _lbCurrentSort + '\"]');",
        "_lbCurrentSort = tab.getAttribute('data-sort');",
        "window.pywebview.api.fetch_leaderboard(_lbCurrentSort);",
    ]
    for pattern in leaderboard_sort_raw_patterns:
        if pattern in html:
            raise AssertionError("Leaderboard sort keys must be normalized before state/query-selector use: " + pattern)
    leaderboard_sort_safe_required = [
        "function _lbSortKey(value, fallback)",
        "var allowed = { xp: true, level: true, songs_played: true, play_time: true };",
        "return allowed[key] ? key : fallback;",
        "function _lbSyncTabs()",
        "var activeTab = document.querySelector('.lb-tab[data-sort=\"' + _lbSortKey(_lbCurrentSort, 'xp') + '\"]');",
        "function _lbRestoreAfterFetchFailure(previousSort, message)",
        "function _lbRequestSort(nextSort, previousSort)",
        "_lbCurrentSort = _lbSortKey(data.sort, _lbCurrentSort);",
        "_lbSyncTabs();",
        "_lbCurrentSort = _lbSortKey(tab.getAttribute('data-sort'), _lbCurrentSort);",
        "_lbRequestSort(_lbCurrentSort, previousSort);",
        "_lbRestoreAfterFetchFailure(previousSort, _menuApiMessage(data, '无法刷新排行榜 / Unable to refresh leaderboard'));",
    ]
    for snippet in leaderboard_sort_safe_required:
        if snippet not in html:
            raise AssertionError("missing safe Leaderboard sort snippet: " + snippet)

    print(
        f"OK web menu layout: {item_count} items, "
        f"item_box={item_box_height}px, frame={frame_height}px"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL web menu layout selftest: {exc}", file=sys.stderr)
        raise SystemExit(1)
