# -*- coding: utf-8 -*-
"""Static regression checks for the WebView SAO menu column layout."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MENU_HTML = ROOT / "web" / "menu.html"
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
    ]
    for pattern in menu_raw_patterns:
        if pattern in html:
            raise AssertionError("menu runtime values must be normalized before rendering or bridge calls: " + pattern)
    menu_safe_required = [
        "var xpPct = _clampNum(data.xp_pct, 0, 0, 100);",
        "document.getElementById('info-xp').style.width = xpPct + '%';",
        "var volume = _clampInt(this.value, 80, 0, 100);",
        "this.value = volume;",
        "window.pywebview.api.set_sound_volume(volume);",
        "var restoredVolume = _clampInt(cfg.sound_volume, 80, 0, 100);",
        "document.getElementById('sound-volume').value = restoredVolume;",
        "var value = _clampInt(pct, 0, 0, 100);",
        "var sizeBytes = _clampNum(s.size, 0, 0, Number.MAX_SAFE_INTEGER);",
        "var sizeMb = sizeBytes / 1024 / 1024;",
        "var pct = _clampInt(_clampNum(s.progress, 0, 0, 1) * 100, 0, 0, 100);",
        "var total = _clampInt(_sessionPlayersPayload.count, rows.length, 0, 999999);",
        "function _slotId(value)",
        "var slot = _slotId(el.getAttribute('data-slot'));",
        "if (slot != null) slots.push(slot);",
        "var restoredSlots = {};",
        "cfg.watched_slots.forEach(function(slotValue) {",
        "if (slot != null) restoredSlots[slot] = true;",
        "if (slot != null && restoredSlots[slot]) {",
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
        "if (!parentPath && !dirs.length && !files.length) {",
        "var d = _pickerPayload(result);",
        "d.mode = _pickerModeValue(_pickerMode);",
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
    ]
    for pattern in plugin_menu_raw_patterns:
        if pattern in html:
            raise AssertionError("plugin menu hotkey counts must be normalized before rendering: " + pattern)
    plugin_menu_safe_required = [
        "function _pluginCountNumber(value)",
        "function _pluginCountText(value)",
        "var hkCount = _pluginCountNumber(it.hotkey_count);",
        "hkCount ? ' ⌨' + _pluginCountText(hkCount) : ''",
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
        "var v = _clampInt(val, 0, 0, 120);",
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
        "var rows = Array.isArray(results) ? results : [];",
        "rows.forEach(function(item) {",
        "var data = _brParseApiResult(item);",
        "window.pywebview.api.get_boss_raid_state().then(function(result) {\n            var data = _brParseApiResult(result);",
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
    ]
    for pattern in leaderboard_sort_raw_patterns:
        if pattern in html:
            raise AssertionError("Leaderboard sort keys must be normalized before state/query-selector use: " + pattern)
    leaderboard_sort_safe_required = [
        "function _lbSortKey(value, fallback)",
        "var allowed = { xp: true, level: true, songs_played: true, play_time: true };",
        "return allowed[key] ? key : fallback;",
        "_lbCurrentSort = _lbSortKey(data.sort, _lbCurrentSort);",
        "var activeTab = document.querySelector('.lb-tab[data-sort=\"' + _lbSortKey(_lbCurrentSort, 'xp') + '\"]');",
        "_lbCurrentSort = _lbSortKey(tab.getAttribute('data-sort'), _lbCurrentSort);",
        "window.pywebview.api.fetch_leaderboard(_lbCurrentSort);",
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
