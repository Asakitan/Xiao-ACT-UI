# -*- coding: utf-8 -*-
"""Static guards for small Web panel numeric rendering."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
RAID_EDITOR = ROOT / "web" / "raid_editor.html"
BUFF_COVERAGE = ROOT / "web" / "buff_coverage.html"
HP = ROOT / "web" / "hp.html"
STAMINA = ROOT / "web" / "stamina.html"
AUTOKEY_EDITOR = ROOT / "web" / "autokey_editor.html"
MECH_BANNER = ROOT / "web" / "mech_banner.html"
SKILLFX = ROOT / "web" / "skillfx.html"
MEM_SCOPE = ROOT / "web" / "mem_scope.html"


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
    autokey = AUTOKEY_EDITOR.read_text(encoding="utf-8")
    mech_banner = MECH_BANNER.read_text(encoding="utf-8")
    skillfx = SKILLFX.read_text(encoding="utf-8")
    mem_scope = MEM_SCOPE.read_text(encoding="utf-8")

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
            "'狂暴 ' + Number(enr.time_s || prof.enrage_time_s || 0) + 's",
            "Raid editor mechanics profile meta must normalize enrage seconds.",
        ),
        (
            "var sid = Number(rec.skill_id || 0);",
            "Raid editor mechanics inbox must normalize skill ids.",
        ),
        (
            "Number(rec.count || 0)",
            "Raid editor mechanics inbox must normalize observed counts.",
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
        (
            "var enrageSeconds = _clampInt(enr.time_s != null ? enr.time_s : prof.enrage_time_s, 0, 0, 86400);",
            "Raid editor mechanics profile meta should clamp enrage seconds.",
        ),
        (
            "var sid = _clampInt(rec.skill_id, 0, 0, 999999999);",
            "Raid editor mechanics inbox should clamp skill ids.",
        ),
        (
            "var dur = rec.last_cast_duration_ms == null ? 0 : _clampInt(rec.last_cast_duration_ms, 0, 0, 600000);",
            "Raid editor mechanics inbox should clamp cast durations.",
        ),
        (
            "var count = _clampInt(rec.count, 0, 0, 999999);",
            "Raid editor mechanics inbox should clamp observed counts.",
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

    autokey_forbidden = [
        (
            "var cdPct = Math.round((s.cooldown_pct || 0) * 100);",
            "AutoKey editor cooldown bars must not render raw percentages.",
        ),
        (
            "var remainStr = s.remaining_ms > 0 ? (s.remaining_ms / 1000).toFixed(1) + 's' : 'Ready';",
            "AutoKey editor remaining time must normalize finite milliseconds.",
        ),
        (
            "data-index=\"' + s.index",
            "AutoKey editor slot click parameters must not use raw slot indices.",
        ),
    ]
    for snippet, message in autokey_forbidden:
        _check_absent(autokey, snippet, message)

    autokey_required = [
        (
            "function _akPct(v)",
            "AutoKey editor should expose clamped cooldown percentage rendering.",
        ),
        (
            "function _akSlotIndex(v)",
            "AutoKey editor should normalize slot indices before inline parameters.",
        ),
        (
            "var cdPct = _akPct(s.cooldown_pct);",
            "AutoKey editor cooldown bars should use _akPct().",
        ),
        (
            "var remainingMs = _akNonNeg(s.remaining_ms, 0);",
            "AutoKey editor remaining time should use finite non-negative milliseconds.",
        ),
        (
            "var stateKey = /^(ready|active|cooldown|unknown|insufficient_energy)$/.test(state) ? state : 'unknown';",
            "AutoKey editor should preserve known non-ready slot states.",
        ),
        (
            ": stateKey === 'insufficient_energy' ? 'No Energy'",
            "AutoKey editor should show a clear no-energy state.",
        ),
        (
            "data-index=\"' + slotIndex + '\" onclick=\"onSlotClick(' + slotIndex + ')\"",
            "AutoKey editor inline slot parameters should use normalized indices.",
        ),
    ]
    for snippet, message in autokey_required:
        _check_present(autokey, snippet, message)

    mech_forbidden = [
        (
            "var frac = Math.max(0, Math.min(1, remaining / r.totalMs));",
            "Mechanic banner progress bars must not divide by raw totalMs.",
        ),
        (
            "var countdownMs = Number(e.countdown_ms != null ? e.countdown_ms",
            "Mechanic banner countdown must normalize finite milliseconds.",
        ),
        (
            "var remainingMs = Number(e.remaining_ms != null ? e.remaining_ms",
            "Mechanic banner remaining time must normalize finite milliseconds.",
        ),
    ]
    for snippet, message in mech_forbidden:
        _check_absent(mech_banner, snippet, message)

    mech_required = [
        (
            "function finiteNum(v, fallback)",
            "Mechanic banner should expose finite number normalization.",
        ),
        (
            "var remaining = nonNegNum(r.endsAt - performance.now(), 0);",
            "Mechanic banner tick should normalize remaining time.",
        ),
        (
            "var totalMs = Math.max(1, nonNegNum(r.totalMs, STATIC_HOLD_MS));",
            "Mechanic banner progress should avoid zero/non-finite totalMs.",
        ),
        (
            "var frac = pct01(remaining / totalMs);",
            "Mechanic banner progress should clamp bar percentages.",
        ),
        (
            "var countdownMs = nonNegNum(e.countdown_ms != null ? e.countdown_ms",
            "Mechanic banner countdown should use nonNegNum().",
        ),
    ]
    for snippet, message in mech_required:
        _check_present(mech_banner, snippet, message)

    skillfx_forbidden = [
        (
            "var idx = parseInt(slotIndex || 0, 10) || 0;",
            "SkillFX slot anchors must not parse raw slotIndex values.",
        ),
        (
            "x: parseInt(payload.callout.x || _viewport.callout.x || 0, 10) || 0,",
            "SkillFX callout x must not parse raw payload values.",
        ),
        (
            "_currentBurstSlot = parseInt(slotIndex || 0, 10) || 1;",
            "SkillFX current burst slot must use finite slot normalization.",
        ),
        (
            "var burstSlot = parseInt(payload.burst_slot || 0, 10) || 0;",
            "SkillFX update must normalize burst_slot before use.",
        ),
    ]
    for snippet, message in skillfx_forbidden:
        _check_absent(skillfx, snippet, message)

    skillfx_required = [
        (
            "function _finiteNum(value, fallback, lo, hi)",
            "SkillFX should expose finite number normalization.",
        ),
        (
            "function _normalizeCallout(raw)",
            "SkillFX should normalize callout rectangles before CSS writes.",
        ),
        (
            "var idx = _finiteInt(slotIndex, 0, 0, 9);",
            "SkillFX slot anchors should clamp slot indices.",
        ),
        (
            "var callout = _normalizeCallout(_viewport.callout || _defaultCallout());",
            "SkillFX positioning should sanitize callout dimensions.",
        ),
        (
            "var burstSlot = _finiteInt(payload.burst_slot, 0, 0, 9);",
            "SkillFX update should use normalized burst slots.",
        ),
    ]
    for snippet, message in skillfx_required:
        _check_present(skillfx, snippet, message)

    mem_forbidden = [
        (
            "rows.sort(function (a, b) { return Number(b.total) - Number(a.total); });",
            "Mem Scope damage sorting must not subtract raw Number() values.",
        ),
        (
            "var count = s.count || 0;",
            "Mem Scope search count must not render raw count values.",
        ),
        (
            "Math.round((s.progress || 0) * 100)",
            "Mem Scope search progress must not render raw progress values.",
        ),
    ]
    for snippet, message in mem_forbidden:
        _check_absent(mem_scope, snippet, message)

    mem_required = [
        (
            "function finiteNum(value, fallback, lo, hi)",
            "Mem Scope should expose finite number normalization.",
        ),
        (
            "var rows = keys.map(function (k) { return { uid: k, total: finiteNum(totals[k], 0, 0) }; });",
            "Mem Scope damage totals should normalize before sorting/rendering.",
        ),
        (
            "var count = finiteInt(s.count, 0, 0);",
            "Mem Scope search count should normalize before badges.",
        ),
        (
            "finiteInt(finiteNum(s.progress, 0, 0, 1) * 100, 0, 0, 100)",
            "Mem Scope progress should clamp to 0..100.",
        ),
    ]
    for snippet, message in mem_required:
        _check_present(mem_scope, snippet, message)

    print("web_small_panels_numeric_selftest: ok")


if __name__ == "__main__":
    main()
