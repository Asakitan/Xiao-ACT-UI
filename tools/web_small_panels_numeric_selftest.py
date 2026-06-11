# -*- coding: utf-8 -*-
"""Static guards for small Web panel numeric rendering."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
RAID_EDITOR = ROOT / "web" / "raid_editor.html"
BUFF_COVERAGE = ROOT / "web" / "buff_coverage.html"


def _check_absent(source: str, snippet: str, message: str) -> None:
    if snippet in source:
        raise AssertionError(message)


def _check_present(source: str, snippet: str, message: str) -> None:
    if snippet not in source:
        raise AssertionError(message)


def main() -> None:
    raid = RAID_EDITOR.read_text(encoding="utf-8")
    buff = BUFF_COVERAGE.read_text(encoding="utf-8")

    raid_forbidden = [
        (
            "var hpPct = e.max_hp > 0 ? Math.round(e.hp / e.max_hp * 100) : 0;",
            "Raid editor entity HP percent must not divide raw HP values.",
        ),
        (
            "return '' + v;",
            "Raid editor _fmtNum must normalize non-finite values before rendering.",
        ),
    ]
    for snippet, message in raid_forbidden:
        _check_absent(raid, snippet, message)

    raid_required = [
        (
            "var hp = _clampNum(e.hp, 0, 0, Number.MAX_SAFE_INTEGER);",
            "Raid editor entity HP should normalize current HP.",
        ),
        (
            "var maxHp = _clampNum(e.max_hp, 0, 0, Number.MAX_SAFE_INTEGER);",
            "Raid editor entity HP should normalize max HP.",
        ),
        (
            "var hpPct = maxHp > 0 ? _clampInt(hp / maxHp * 100, 0, 0, 100) : 0;",
            "Raid editor entity HP percent should be clamped to 0..100.",
        ),
        (
            "v = _clampNum(v, 0, 0, Number.MAX_SAFE_INTEGER);",
            "Raid editor _fmtNum should normalize finite non-negative values.",
        ),
    ]
    for snippet, message in raid_required:
        _check_present(raid, snippet, message)

    buff_forbidden = [
        (
            "var pct = Math.max(0, Math.min(1, b.uptime_pct == null ? 0 : b.uptime_pct));",
            "Buff coverage percent must not clamp raw values with Math.min/Math.max.",
        ),
        (
            "var s = Math.floor((ms || 0) / 1000);",
            "Buff coverage elapsed time must normalize finite milliseconds.",
        ),
        (
            "var rem = (b.rem_s == null) ? -1 : b.rem_s;",
            "Buff coverage remaining seconds must normalize finite values.",
        ),
    ]
    for snippet, message in buff_forbidden:
        _check_absent(buff, snippet, message)

    buff_required = [
        (
            "function finiteNum(v, fallback)",
            "Buff coverage should expose finite number normalization.",
        ),
        (
            "function nonNegNum(v, fallback)",
            "Buff coverage should expose non-negative number normalization.",
        ),
        (
            "function pct01(v)",
            "Buff coverage should clamp coverage percentages through pct01().",
        ),
        (
            "var pct = pct01(b.uptime_pct);",
            "Buff coverage uptime should use pct01().",
        ),
        (
            "var rem = (b.rem_s == null) ? -1 : finiteNum(b.rem_s, -1);",
            "Buff coverage remaining time should use finiteNum().",
        ),
        (
            "var s = Math.floor(nonNegNum(ms, 0) / 1000);",
            "Buff coverage elapsed time should use nonNegNum().",
        ),
    ]
    for snippet, message in buff_required:
        _check_present(buff, snippet, message)

    print("web_small_panels_numeric_selftest: ok")


if __name__ == "__main__":
    main()
