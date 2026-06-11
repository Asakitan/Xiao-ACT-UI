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
            "Dungeon ID: ' + esc(_data.dungeon_id)",
            "Commander dungeon id should be escaped before rendering.",
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
    ]
    for snippet, message in required:
        _check_present(html, snippet, message)

    print("web_commander_layout_selftest: ok")


if __name__ == "__main__":
    main()
