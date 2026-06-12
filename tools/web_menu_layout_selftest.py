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
    if "_brSelectProfile(' + _jsAttrArg(p.id) + ')" not in html:
        raise AssertionError("BossRaid select must use _jsAttrArg(p.id)")
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
        "document.getElementById('linkage-global-cd').value = _clampNum(s.global_cooldown_s, 1.0, 0, 60);",
        "var seconds = _clampNum(val, 1.0, 0, 60);",
        "api.set_linkage_global_cooldown(seconds);",
        "var actionCount = _clampInt(summary.action_count, ((profile.actions || []).length || 0), 0, 999999);",
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
