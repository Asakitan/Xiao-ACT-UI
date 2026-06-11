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
    print("web_editor_text_selftest: ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL web_autokey_editor_text_selftest: {exc}", file=sys.stderr)
        raise SystemExit(1)
