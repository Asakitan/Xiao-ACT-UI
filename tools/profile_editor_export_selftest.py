# -*- coding: utf-8 -*-
"""Static guards for Tk profile editor export/import failure feedback.

The AutoKey and BossRaid profile editors must not silently swallow export
failures: a failing `export_profile_to_default_path` call has to surface as
an `Export failed: ...` status instead of leaving the previous status text
on screen.

    python tools/profile_editor_export_selftest.py
"""
from __future__ import annotations

import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "gui_modules" / "sao_gui_profile_editors.py"

_passed = 0
_failed = 0


def check(name: str, cond: bool, detail: str = "") -> None:
    global _passed, _failed
    if cond:
        _passed += 1
        print(f"  [ok] {name}")
    else:
        _failed += 1
        print(f"  [FAIL] {name} :: {detail}")


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")

    guarded_export = "self._set_status(f'Export failed: {exc}', ok=False)"
    check(
        "both editors surface export failures",
        source.count(guarded_export) == 2,
        f"expected 2 guarded export paths, found {source.count(guarded_export)}",
    )
    check(
        "autokey export call stays guarded",
        "try:\n            path = export_auto_key_profile(normalized)" in source,
    )
    check(
        "bossraid export call stays guarded",
        "try:\n            path = export_boss_raid_profile(normalized)" in source,
    )
    check(
        "unguarded export call pattern is gone",
        "        path = export_auto_key_profile(normalized)\n        self._set_status" not in source
        and "        path = export_boss_raid_profile(normalized)\n        self._set_status" not in source,
    )
    check(
        "import failures stay surfaced",
        source.count("Import failed: {exc}") >= 2,
        f"found {source.count('Import failed: {exc}')}",
    )

    print(f"\n{_passed} passed, {_failed} failed")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
