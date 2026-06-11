# -*- coding: utf-8 -*-
"""Static guards for small Web panel numeric rendering."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
RAID_EDITOR = ROOT / "web" / "raid_editor.html"
BUFF_COVERAGE = ROOT / "web" / "buff_coverage.html"
HP = ROOT / "web" / "hp.html"
STAMINA = ROOT / "web" / "stamina.html"


def _check_absent(source: str, snippet: str, message: str) -> None:
    if snippet in source:
        raise AssertionError(message)


def _check_present(source: str, snippet: str, message: str) -> None:
    if snippet not in source:
        raise AssertionError(message)


def main() -> None:
    raid = RAID_EDITOR.read_text(encoding="utf-8")
    buff = BUFF_COVERAGE.read_text(encoding="utf-8")
    hp = HP.read_text(encoding="utf-8")
    stamina = STAMINA.read_text(encoding="utf-8")

    raid_forbidden = [
        (
            "var hpPct = e.max_hp > 0 ? Math.round(e.hp / e.max_hp * 100) : 0;",
            "Raid editor entity HP percent must not divide raw HP values.",
        ),
        (
            "Math.round(rec.hp_line_pct * 100)",
            "Raid editor HP-line badges must normalize raw percentages.",
        ),
        (
            "Math.round(rec.time_fixed_s)",
            "Raid editor fixed-time badges must normalize raw seconds.",
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
        (
            "var hpLinePct = _clampInt(_clampNum(rec.hp_line_pct, 0, 0, 1) * 100, 0, 0, 100);",
            "Raid editor HP-line badges should clamp percentages to 0..100.",
        ),
        (
            "var timeFixed = _clampInt(rec.time_fixed_s, 0, 0, 86400);",
            "Raid editor fixed-time badges should clamp seconds before rendering.",
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

    hp_forbidden = [
        (
            "var pct = total > 0 ? Math.round(current / total * 100) : 100;",
            "HP HUD must not derive bar widths from raw HP/STA values.",
        ),
        (
            "Math.floor(current) + '/' + Math.floor(total)",
            "HP HUD should normalize values before rendering text.",
        ),
    ]
    for snippet, message in hp_forbidden:
        _check_absent(hp, snippet, message)

    hp_required = [
        (
            "function _hudFinite(v, fallback)",
            "HP HUD should expose finite number normalization.",
        ),
        (
            "function _hudBarPct(current, total, fallbackPct)",
            "HP HUD should clamp bar percentages through _hudBarPct().",
        ),
        (
            "var pct = _hudBarPct(hpCurrent, hpTotal, 100);",
            "HP HUD updateHP should use clamped bar percentages.",
        ),
        (
            "var pct = _hudBarPct(staCurrent, staTotal, 100);",
            "HP HUD updateSTA should use clamped bar percentages.",
        ),
    ]
    for snippet, message in hp_required:
        _check_present(hp, snippet, message)

    stamina_forbidden = [
        (
            "var staPct = staMax > 0 ? Math.round(sta / staMax * 100) : 100;",
            "Standalone STA panel must not derive bar widths from raw values.",
        ),
        (
            "Math.floor(sta) + '/' + Math.floor(staMax)",
            "Standalone STA panel should normalize values before rendering text.",
        ),
    ]
    for snippet, message in stamina_forbidden:
        _check_absent(stamina, snippet, message)

    stamina_required = [
        (
            "function _staFinite(v, fallback)",
            "Standalone STA panel should expose finite number normalization.",
        ),
        (
            "function _staBarPct(current, total, fallbackPct)",
            "Standalone STA panel should clamp bar percentages through _staBarPct().",
        ),
        (
            "var staPct = _staBarPct(staCurrent, staTotal, 100);",
            "Standalone STA panel updateBars should use clamped bar percentages.",
        ),
    ]
    for snippet, message in stamina_required:
        _check_present(stamina, snippet, message)

    print("web_small_panels_numeric_selftest: ok")


if __name__ == "__main__":
    main()
