# -*- coding: utf-8 -*-
"""Static guards for commander webview rendering safety."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
HTML = ROOT / "web" / "commander.html"


def _check_absent(html: str, snippet: str, message: str) -> None:
    if snippet in html:
        raise AssertionError(message)


def _check_present(html: str, snippet: str, message: str) -> None:
    if snippet not in html:
        raise AssertionError(message)


def main() -> None:
    html = HTML.read_text(encoding="utf-8")

    forbidden = [
        (
            'data-uid="\' + m.uid + \'"',
            "Commander member data-uid must be escaped before entering HTML attributes.",
        ),
        (
            "'Lv.' + (m.level || '--')",
            "Commander member levels must be normalized and escaped before rendering.",
        ),
        (
            "'CP ' + fmtFightPoint(m.fight_point)",
            "Commander fight power labels must be escaped before rendering.",
        ),
        (
            "Dungeon ID: ' + _data.dungeon_id",
            "Commander dungeon id must be escaped before rendering.",
        ),
        (
            "if (!_data || !_data.dungeon_id) {",
            "Commander dungeon id must be normalized before deciding active/empty state.",
        ),
        (
            "Dungeon ID: ' + esc(_data.dungeon_id)",
            "Commander dungeon id must render the normalized finite id text.",
        ),
        (
            "if (m.max_hp > 0) {",
            "Commander HP mini bar must use normalized finite HP values.",
        ),
        (
            "var hpPct = Math.max(0, Math.min(1, m.hp / m.max_hp));",
            "Commander HP mini bar must not divide raw HP values.",
        ),
        (
            "var slotCls = s.state || 'ready';",
            "Commander skill slot state must be whitelisted before becoming a CSS class.",
        ),
        (
            "'<span class=\"cd-idx\">' + s.index + '</span>'",
            "Commander skill slot index must be escaped before rendering.",
        ),
        (
            "'<span class=\"cd-time\">' + timeStr + '</span>'",
            "Commander skill slot time must be escaped before rendering.",
        ),
        (
            "if (!_data || !_data.members || _data.members.length === 0) {",
            "Commander members payload must be normalized before empty-state checks.",
        ),
        (
            "for (var i = 0; i < _data.members.length; i++) {",
            "Commander member rows must iterate normalized object items.",
        ),
        (
            "if (m.is_self && m.skill_slots && m.skill_slots.length > 0) {",
            "Commander skill slot payload must be normalized before rendering.",
        ),
        (
            "for (var j = 0; j < m.skill_slots.length; j++) {",
            "Commander skill slots must iterate normalized object items.",
        ),
        (
            "if (_data && _data.members && _data.members.length > 0) {",
            "Commander boss overview members payload must be normalized before branching.",
        ),
    ]
    for snippet, message in forbidden:
        _check_absent(html, snippet, message)

    required = [
        (
            "function attr(s)",
            "Commander renderer should expose an attribute escaping helper.",
        ),
        (
            "function safeSlotState(state)",
            "Commander renderer should whitelist skill slot CSS states.",
        ),
        (
            "function pct01(value)",
            "Commander renderer should clamp cooldown percentages.",
        ),
        (
            "function nonNegNum(value, fallback)",
            "Commander renderer should normalize non-negative finite numbers.",
        ),
        (
            "function hpPct01(hp, maxHp)",
            "Commander HP renderer should clamp HP ratios through a helper.",
        ),
        (
            "function dungeonIdText(value)",
            "Commander dungeon renderer should expose normalized dungeon id text.",
        ),
        (
            'data-uid="\' + attr(uidText) + \'"',
            "Commander member data-uid should use escaped stable text.",
        ),
        (
            "'<span>Lv.' + esc(fmtLevel(m.level)) + '</span>'",
            "Commander member levels should use escaped formatted text.",
        ),
        (
            "'<span>CP ' + esc(fmtFightPoint(m.fight_point)) + '</span>'",
            "Commander fight power labels should use escaped formatted text.",
        ),
        (
            "var hpPct = hpPct01(m.hp, m.max_hp);",
            "Commander HP bar should compute ratio through hpPct01().",
        ),
        (
            "if (hpPct !== null) {",
            "Commander HP bar should render only when max HP is valid.",
        ),
        (
            "var dungeonText = dungeonIdText(data.dungeon_id);",
            "Commander boss tab should normalize dungeon id before branching.",
        ),
        (
            "Dungeon ID: ' + esc(dungeonText)",
            "Commander dungeon id should render escaped normalized text.",
        ),
        (
            "var slotCls = safeSlotState(s.state);",
            "Commander skill slot class should use the safe state helper.",
        ),
        (
            "'<span class=\"cd-idx\">' + esc(s.index || (j + 1)) + '</span>'",
            "Commander skill slot index should be escaped with a stable fallback.",
        ),
        (
            "'<span class=\"cd-time\">' + esc(timeStr) + '</span>'",
            "Commander skill slot time should be escaped before rendering.",
        ),
        (
            "function isObjectValue(value)",
            "Commander renderer should expose object-shape detection.",
        ),
        (
            "function objectValue(value)",
            "Commander renderer should normalize object payloads.",
        ),
        (
            "function objectItems(value)",
            "Commander renderer should filter object-list payloads.",
        ),
        (
            "var data = objectValue(_data);",
            "Commander renderer should normalize root data before rendering.",
        ),
        (
            "var members = objectItems(data.members);",
            "Commander renderer should normalize member lists.",
        ),
        (
            "if (!members.length) {",
            "Commander empty-state checks should use normalized members.",
        ),
        (
            "var m = objectValue(members[i]);",
            "Commander member rows should guard each member object.",
        ),
        (
            "var slots = objectItems(m.skill_slots);",
            "Commander skill slots should normalize slot lists.",
        ),
        (
            "if (m.is_self && slots.length > 0) {",
            "Commander skill slot rendering should branch on normalized slots.",
        ),
        (
            "var s = objectValue(slots[j]);",
            "Commander skill slot rows should guard each slot object.",
        ),
    ]
    for snippet, message in required:
        _check_present(html, snippet, message)

    print("web_commander_layout_selftest: ok")


if __name__ == "__main__":
    main()
