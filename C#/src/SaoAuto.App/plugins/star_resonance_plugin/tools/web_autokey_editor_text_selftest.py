# -*- coding: utf-8 -*-
"""Static checks for user-visible editor text and basic tags."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
AUTOKEY_HTML = ROOT / "web" / "autokey_editor.html"
RAID_HTML = ROOT / "web" / "raid_editor.html"

MOJIBAKE_TOKENS = (
    "鈥",
    "鈻",
    "鈼",
    "鈫",
    "鈹",
    "鈺",
    "鈮",
    "鉁",
    "脳",
    "\ufffd",
)


def main() -> int:
    autokey = AUTOKEY_HTML.read_text(encoding="utf-8")
    raid = RAID_HTML.read_text(encoding="utf-8")
    for label, source in (("AutoKey editor", autokey), ("Raid editor", raid)):
        found = [token for token in MOJIBAKE_TOKENS if token in source]
        if found:
            raise AssertionError(f"{label} contains mojibake tokens: " + ", ".join(found))
    autokey_required = [
        '<div class="burst-indicator" id="burst-indicator">BURST READY</div>',
        '<span id="status-profession">-</span>',
        '<span id="status-burst">Burst: -</span>',
    ]
    for snippet in autokey_required:
        if snippet not in autokey:
            raise AssertionError("missing clean AutoKey editor snippet: " + snippet)
    raid_required = [
        '<div class="phase-btn" onclick="advancePhase()">NEXT</div>',
        "case 'hp_pct': return 'HP >= ' + v + '%';",
        "case 'dps_total': return 'DMG >= ' + _fmtNum(v);",
    ]
    for snippet in raid_required:
        if snippet not in raid:
            raise AssertionError("missing clean Raid editor snippet: " + snippet)
    raid_bad_arg_patterns = [
        "onclick=\"saveReaction(\\'' + trig",
        "JSON.stringify(String(rec.name || '')).replace",
        "onchange=\"mechToggle(\\'' + mid",
        "onclick=\"mechTest(\\'' + mid",
        "onclick=\"mechEdit(\\'' + mid",
        "onclick=\"mechDelete(\\'' + mid",
        "border-left:3px solid ' + _esc(m.color || '#68e4ff')",
        "'<div class=\"phase-trigger\">' + _triggerText(p.trigger) + '</div>'",
        "Number(master.tts_volume || 80)",
        "var v = parseFloat(el.value);",
        "Math.round(_mNum('mf-move', 600))",
    ]
    for pattern in raid_bad_arg_patterns:
        if pattern in raid:
            raise AssertionError("Raid editor inline handler must not single-quote raw dynamic args: " + pattern)
    if 'onclick="toggleRole(' + " + e.uuid + " + ')"' in raid:
        raise AssertionError("Raid editor role toggle must not pass entity uuid as a raw JS number")
    if "if (_lastEntities[i].uuid === uuid)" in raid:
        raise AssertionError("Raid editor role toggle must compare entity uuid as a string key")
    raid_safe_arg_required = [
        "function _jsArg",
        "onclick=\"saveReaction(' + _jsArg(trig) + ',' + skillId + ',' + _jsArg(id) + ')",
        "onclick=\"mechCreateFromSkill('",
        "+ sid + ',' + _jsArg(rec.name || '') + ',' + (dur > 0 ? dur : 'null') + ')",
        "var midArg = _jsArg(m.id);",
        "var uuidArg = _jsArg(e.uuid);",
        "data-uuid=\"' + _esc(uuidText)",
        "onclick=\"toggleRole(' + uuidArg + ')",
        "if (String(_lastEntities[i].uuid == null ? '' : _lastEntities[i].uuid) === uuid)",
        "onchange=\"mechToggle(' + midArg + ', this.checked)",
        # 卡片头三个试发聚合成「试▾」下拉; 删除收进表单底部 — mid 仍走 _jsArg
        "mechTest(' + midArg + ',[k]);",
        "onclick=\"mechEdit(' + midArg + ')",
        "onclick=\"mechDelete(' + _jsArg(m.id) + ')",
        "function _finiteNum",
        "function _clampNum",
        "function _clampInt",
        "mechMaster({tts_volume:_clampInt(this.value,80,0,100)})",
        "var delay = _clampInt(document.getElementById('rd_' + tag).value || '0', 0, 0, 60000);",
        "inline.move_ms = _clampInt(_mNum('mf-move', 600), 600, 80, 6000);",
        "function _cssColor",
        "border-left:3px solid ' + _cssColor(m.color)",
        "'<div class=\"phase-trigger\">' + _esc(_triggerText(p.trigger)) + '</div>'",
    ]
    for snippet in raid_safe_arg_required:
        if snippet not in raid:
            raise AssertionError("missing safe Raid editor inline handler snippet: " + snippet)
    print("web_editor_text_selftest: ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL web_autokey_editor_text_selftest: {exc}", file=sys.stderr)
        raise SystemExit(1)
