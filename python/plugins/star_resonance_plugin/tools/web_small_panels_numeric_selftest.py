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
PANEL = ROOT / "web" / "panel.html"


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
    panel = PANEL.read_text(encoding="utf-8")

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
        (
            "Number(m.boss_base_id || 0) !== Number(baseId || 0)",
            "Raid editor reaction mapping must normalize boss base ids before matching.",
        ),
        (
            "Number(m.skill_id || 0) !== Number(skillId || 0)",
            "Raid editor reaction mapping must normalize skill ids before matching.",
        ),
        (
            "return !Number(s.hold_ms || 0);",
            "Raid editor mechanic dash preset must not treat malformed hold_ms as zero.",
        ),
        (
            "Number(s.delay_ms || 0) === Number(seq[1].delay_ms || 0)",
            "Raid editor mechanic dash preset must normalize delay_ms before comparison.",
        ),
        (
            "_lastEntities = entities || [];",
            "Raid editor entity payload must guard non-list data.",
        ),
        (
            "var e = _lastEntities[i];",
            "Raid editor entity rows must guard non-object entries.",
        ),
        (
            "if (!phases || !phases.length)",
            "Raid editor phase payload must guard non-list data.",
        ),
        (
            "var p = phases[i];",
            "Raid editor phase rows must guard non-object entries.",
        ),
        (
            "_lastStatus = status || {};",
            "Raid editor status payload must guard non-object data.",
        ),
        (
            "if (data.status) updateStatus(data.status);",
            "Raid editor full-state payload must guard non-object data.",
        ),
        (
            "var tags = (rec && rec.tags) || [];",
            "Raid editor badges must guard non-list tag payloads.",
        ),
        (
            "var st = _reactState || {};",
            "Raid editor reaction state must guard non-object payloads.",
        ),
        (
            "var scenes = st.scenes || [];",
            "Raid editor reaction scenes must guard non-list payloads.",
        ),
        (
            "var bosses = st.bosses || [];",
            "Raid editor reaction bosses must guard non-list payloads.",
        ),
        (
            "var detail = st.boss_detail || {};",
            "Raid editor reaction boss detail must guard non-object payloads.",
        ),
        (
            "var skills = detail.skills || [];",
            "Raid editor reaction skills must guard non-list payloads.",
        ),
        (
            "var mechs = detail.mechanics || [];",
            "Raid editor reaction mechanics must guard non-list payloads.",
        ),
        (
            "var tl = detail.timeline || [];",
            "Raid editor reaction timeline must guard non-list payloads.",
        ),
        (
            "var st = _mechState || {};",
            "Raid editor mechanics state must guard non-object payloads.",
        ),
        (
            "var master = st.master || {};",
            "Raid editor mechanics master payload must guard non-object data.",
        ),
        (
            "var inbox = st.inbox || [];",
            "Raid editor mechanics inbox payload must guard non-list data.",
        ),
        (
            "var mechs = st.mechanics || [];",
            "Raid editor mechanics list payload must guard non-list data.",
        ),
        (
            "var prof = st.profile || {};",
            "Raid editor mechanics profile payload must guard non-object data.",
        ),
        (
            "var enr = prof.enrage || {};",
            "Raid editor mechanics enrage payload must guard non-object data.",
        ),
        (
            "var names = m.skill_names || {};",
            "Raid editor mechanic cards/forms must guard skill-name payloads.",
        ),
        (
            "var ids = (det.skill_ids || []).concat(det.buff_ids || []);",
            "Raid editor mechanic cards must guard detection ID lists.",
        ),
        (
            "var seq = inline.sequence || [];",
            "Raid editor mechanic forms must guard sequence payloads.",
        ),
        (
            "var observed = ((_mechState && _mechState.observed) || []).filter(function (o) {",
            "Raid editor mechanic forms must guard observed payloads.",
        ),
        (
            "html += (_mechCatalog || []).map(function (h) {",
            "Raid editor mechanic forms must guard catalog payloads.",
        ),
        (
            "var phases = (_mechState && _mechState.phases) || [];",
            "Raid editor mechanic forms must guard phase payloads.",
        ),
        (
            "var d = _mechDraft || {};",
            "Raid editor mechanic draft collection must guard draft payload shape.",
        ),
        (
            "var det = d.detect = d.detect || {};",
            "Raid editor mechanic draft collection must guard detect payload shape.",
        ),
        (
            "var ids = det.skill_ids = det.skill_ids || [];",
            "Raid editor mechanic draft skill IDs must guard list payload shape.",
        ),
        (
            "var ids = det.buff_ids = det.buff_ids || [];",
            "Raid editor mechanic draft buff IDs must guard list payload shape.",
        ),
        (
            "det.skill_ids = (det.skill_ids || []).filter(function (x) { return Number(x) !== Number(sid); });",
            "Raid editor mechanic draft skill removal must guard list payload shape.",
        ),
        (
            "det.buff_ids = (det.buff_ids || []).filter(function (x) { return Number(x) !== Number(bid); });",
            "Raid editor mechanic draft buff removal must guard list payload shape.",
        ),
        (
            "_mechCatalog = (r && r.ok) ? (r.results || []) : [];",
            "Raid editor mechanic catalog search must guard result payload shape.",
        ),
        (
            "else if (_v === 'seq' && !(inl.sequence || []).length)",
            "Raid editor mechanic preset changes must guard sequence payload shape.",
        ),
        (
            "(inl.sequence = inl.sequence || []).push({ key: '', delay_ms: 0, hold_ms: 0 });",
            "Raid editor mechanic sequence add must guard sequence payload shape.",
        ),
        (
            "(_mechDraft.dodge.inline.sequence || []).splice(i, 1);",
            "Raid editor mechanic sequence removal must guard nested draft payload shape.",
        ),
        (
            "if (m2.dodge && m2.dodge.inline && (m2.dodge.inline.sequence || []).length)",
            "Raid editor mechanic save must guard nested sequence payload shape.",
        ),
        (
            "return '<span onclick=\"mechDraftRemoveSkill(' + sid + ')\"",
            "Raid editor mechanic skill chips must normalize inline remove IDs.",
        ),
        (
            "return '<span onclick=\"mechDraftRemoveBuff(' + bid + ')\"",
            "Raid editor mechanic buff chips must normalize inline remove IDs.",
        ),
        (
            "return '<option value=\"' + o.id + '\">'",
            "Raid editor mechanic observed options must normalize option IDs.",
        ),
        (
            "onclick=\"mechDraftAddSkill(' + h.id + ')\">绑定",
            "Raid editor mechanic catalog binding must normalize inline add IDs.",
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
        (
            "function _idNum(v, fallback)",
            "Raid editor should expose ID normalization for reaction mapping.",
        ),
        (
            "function _sameId(a, b, fallback)",
            "Raid editor should compare reaction IDs through normalized values.",
        ),
        (
            "function _stepMs(v, fallback)",
            "Raid editor should normalize mechanic step timings.",
        ),
        (
            "if (!_sameId(m.boss_base_id, baseId, 0)) continue;",
            "Raid editor reaction mapping should use normalized boss base IDs.",
        ),
        (
            "if (trig === 'boss_cast' && !_sameId(m.skill_id, skillId, 0)) continue;",
            "Raid editor reaction mapping should use normalized skill IDs.",
        ),
        (
            "seq.every(function (s) { return _isZeroStep(s.hold_ms); })",
            "Raid editor mechanic dash preset should normalize hold_ms.",
        ),
        (
            "seq.slice(1).every(function (s) { return _sameStepMs(s.delay_ms, seq[1].delay_ms); });",
            "Raid editor mechanic dash preset should normalize delay_ms.",
        ),
        (
            "function _isObjectValue(v)",
            "Raid editor should expose object-shape detection.",
        ),
        (
            "function _objectValue(v)",
            "Raid editor should normalize object payloads.",
        ),
        (
            "function _objectItems(v)",
            "Raid editor should filter object-list payloads.",
        ),
        (
            "_lastEntities = _objectItems(entities);",
            "Raid editor should normalize entity list payloads.",
        ),
        (
            "var e = _objectValue(_lastEntities[i]);",
            "Raid editor should normalize each entity row.",
        ),
        (
            "phases = _objectItems(phases);",
            "Raid editor should normalize phase list payloads.",
        ),
        (
            "var p = _objectValue(phases[i]);",
            "Raid editor should normalize each phase row.",
        ),
        (
            "status = _objectValue(status);",
            "Raid editor should normalize status payloads.",
        ),
        (
            "data = _objectValue(data);",
            "Raid editor should normalize full-state payloads.",
        ),
        (
            "if (_isObjectValue(data.status)) updateStatus(data.status);",
            "Raid editor updateFull should guard status payload shape.",
        ),
        (
            "function _listItems(v)",
            "Raid editor should expose plain-list payload normalization.",
        ),
        (
            "rec = _objectValue(rec);",
            "Raid editor badges should normalize record payloads.",
        ),
        (
            "var tags = _listItems(rec.tags);",
            "Raid editor badges should normalize tag list payloads.",
        ),
        (
            "t = String(t);",
            "Raid editor badges should normalize tag values.",
        ),
        (
            "var st = _objectValue(_reactState);",
            "Raid editor reactions should normalize root state payloads.",
        ),
        (
            "var scenes = _objectItems(st.scenes);",
            "Raid editor reactions should normalize scene list payloads.",
        ),
        (
            "var bosses = _objectItems(st.bosses);",
            "Raid editor reactions should normalize boss list payloads.",
        ),
        (
            "var detail = _objectValue(st.boss_detail);",
            "Raid editor reactions should normalize boss detail payloads.",
        ),
        (
            "var skills = _objectItems(detail.skills);",
            "Raid editor reactions should normalize skill list payloads.",
        ),
        (
            "var mechs = _objectItems(detail.mechanics);",
            "Raid editor reactions should normalize mechanic list payloads.",
        ),
        (
            "var tl = _objectItems(detail.timeline);",
            "Raid editor reactions should normalize timeline list payloads.",
        ),
        (
            "var st = _objectValue(_mechState);",
            "Raid editor mechanics should normalize root state payloads.",
        ),
        (
            "var master = _objectValue(st.master);",
            "Raid editor mechanics should normalize master payloads.",
        ),
        (
            "var inbox = _objectItems(st.inbox);",
            "Raid editor mechanics should normalize inbox payloads.",
        ),
        (
            "var mechs = _objectItems(st.mechanics);",
            "Raid editor mechanics should normalize mechanic list payloads.",
        ),
        (
            "var prof = _objectValue(st.profile);",
            "Raid editor mechanics should normalize profile payloads.",
        ),
        (
            "var enr = _objectValue(prof.enrage);",
            "Raid editor mechanics should normalize enrage payloads.",
        ),
        (
            "m = _objectValue(m);",
            "Raid editor mechanic cards/forms should normalize mechanic rows.",
        ),
        (
            "var names = _objectValue(m.skill_names);",
            "Raid editor mechanic cards/forms should normalize skill-name maps.",
        ),
        (
            "var ids = _idList(det.skill_ids).concat(_idList(det.buff_ids));",
            "Raid editor mechanic cards should normalize detection ID lists.",
        ),
        (
            "var seq = _objectItems(inline.sequence);",
            "Raid editor mechanic forms should normalize sequence rows.",
        ),
        (
            "var observed = _objectItems(_objectValue(_mechState).observed).map(function (o) {",
            "Raid editor mechanic forms should normalize observed rows.",
        ),
        (
            "html += _objectItems(_mechCatalog).map(function (h) {",
            "Raid editor mechanic forms should normalize catalog rows.",
        ),
        (
            "var phases = _objectItems(_objectValue(_mechState).phases);",
            "Raid editor mechanic forms should normalize phase rows.",
        ),
        (
            "var d = _isObjectValue(_mechDraft) ? _mechDraft : {};",
            "Raid editor mechanic draft collection should normalize draft payloads.",
        ),
        (
            "var det = d.detect = _objectValue(d.detect);",
            "Raid editor mechanic draft collection should normalize detect payloads.",
        ),
        (
            "var ids = det.skill_ids = _idList(det.skill_ids);",
            "Raid editor mechanic draft skill IDs should normalize ID list payloads.",
        ),
        (
            "var ids = det.buff_ids = _idList(det.buff_ids);",
            "Raid editor mechanic draft buff IDs should normalize ID list payloads.",
        ),
        (
            "det.skill_ids = _idList(det.skill_ids).filter(function (x) { return Number(x) !== Number(sid); });",
            "Raid editor mechanic draft skill removal should normalize ID list payloads.",
        ),
        (
            "det.buff_ids = _idList(det.buff_ids).filter(function (x) { return Number(x) !== Number(bid); });",
            "Raid editor mechanic draft buff removal should normalize ID list payloads.",
        ),
        (
            "_mechCatalog = (r && r.ok) ? _objectItems(r.results) : [];",
            "Raid editor mechanic catalog search should normalize result rows.",
        ),
        (
            "else if (_v === 'seq' && !_listItems(inl.sequence).length)",
            "Raid editor mechanic preset changes should normalize sequence lists.",
        ),
        (
            "inl.sequence = _listItems(inl.sequence);",
            "Raid editor mechanic sequence add/remove should normalize sequence lists.",
        ),
        (
            "inl.sequence.splice(i, 1);",
            "Raid editor mechanic sequence removal should mutate a normalized list.",
        ),
        (
            "var seq = _objectItems(_objectValue(_objectValue(m2.dodge).inline).sequence);",
            "Raid editor mechanic save should normalize nested sequence payloads.",
        ),
        (
            "function _idList(v)",
            "Raid editor mechanics should expose ID-list normalization.",
        ),
        (
            "var id = _idNum(v, null);",
            "Raid editor mechanics should normalize each ID-list item.",
        ),
        (
            "var ids = _idList(det.skill_ids);",
            "Raid editor mechanic forms should normalize skill chip IDs.",
        ),
        (
            "var buffIds = _idList(det.buff_ids);",
            "Raid editor mechanic forms should normalize buff chip IDs.",
        ),
        (
            "var observed = _objectItems(_objectValue(_mechState).observed).map(function (o) {",
            "Raid editor mechanic forms should normalize observed option rows before rendering.",
        ),
        (
            "var oid = _idNum(o.id, null);",
            "Raid editor mechanic forms should normalize observed option IDs.",
        ),
        (
            "if (hid == null) return '';",
            "Raid editor mechanic catalog rows should skip malformed binding IDs.",
        ),
        (
            "var fn = isBuff ? 'mechDraftAddBuff' : 'mechDraftAddSkill';",
            "Raid editor mechanic catalog bindings should route buff/skill binders.",
        ),
        (
            "onclick=\"' + fn + '(' + hid + ')\">绑定",
            "Raid editor mechanic catalog bindings should pass normalized IDs.",
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
        (
            "data = data || {};",
            "Buff coverage update payload must guard non-object data.",
        ),
        (
            "var buffs = data.buffs || [];",
            "Buff coverage buffs payload must guard non-list data.",
        ),
        (
            "var b = buffs[i];",
            "Buff coverage rows must guard non-object buff entries.",
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
        (
            "function isObjectValue(v)",
            "Buff coverage should expose object-shape detection.",
        ),
        (
            "function objectValue(v)",
            "Buff coverage should normalize object payloads.",
        ),
        (
            "function objectItems(v)",
            "Buff coverage should filter object-list payloads.",
        ),
        (
            "data = objectValue(data);",
            "Buff coverage should normalize update payload objects.",
        ),
        (
            "var buffs = objectItems(data.buffs);",
            "Buff coverage should normalize buff list payloads.",
        ),
        (
            "var b = objectValue(buffs[i]);",
            "Buff coverage should normalize each buff row.",
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
        (
            "_slots = slots || [];",
            "AutoKey editor slots payload must guard non-list data.",
        ),
        (
            "var s = _slots[i];",
            "AutoKey editor slot rows must guard non-object entries.",
        ),
        (
            "_actions = actions || [];",
            "AutoKey editor actions payload must guard non-list data.",
        ),
        (
            "var a = _actions[i];",
            "AutoKey editor action rows must guard non-object entries.",
        ),
        (
            "var enabled = data.enabled || false;",
            "AutoKey editor update state must guard non-object data.",
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
        (
            "function _akIsObjectValue(v)",
            "AutoKey editor should expose object-shape detection.",
        ),
        (
            "function _akObjectValue(v)",
            "AutoKey editor should normalize object payloads.",
        ),
        (
            "function _akObjectItems(v)",
            "AutoKey editor should filter object-list payloads.",
        ),
        (
            "_slots = _akObjectItems(slots);",
            "AutoKey editor should normalize slot list payloads.",
        ),
        (
            "var s = _akObjectValue(_slots[i]);",
            "AutoKey editor should normalize each slot row.",
        ),
        (
            "var a = _akObjectValue(_actions[i]);",
            "AutoKey editor should normalize each action row.",
        ),
        (
            "data = _akObjectValue(data);",
            "AutoKey editor should normalize update state payloads.",
        ),
        (
            "_actions = _akObjectItems(actions);",
            "AutoKey editor should normalize action list payloads.",
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
            "parseInt(slotIndex || 1, 10) || 1",
            "HP burst anchor slot should not parse raw slotIndex values.",
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

    hp_burst_required = [
        (
            "function _clampBurstSlot(slotIndex)",
            "HP burst anchor should expose finite slot normalization.",
        ),
        (
            "var idx = _clampBurstSlot(slotIndex) - 1;",
            "HP burst anchor should use finite slot normalization.",
        ),
    ]
    for snippet, message in hp_burst_required:
        _check_present(hp, snippet, message)

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
            "d = d || {};",
            "Mem Scope root payload must guard non-object data.",
        ),
        (
            "var st = d.status || {};",
            "Mem Scope status payload must guard non-object data.",
        ),
        (
            "var cats = (d.catalog && d.catalog.categories) || [];",
            "Mem Scope catalog categories must guard non-list data.",
        ),
        (
            "var s = d.self || {};",
            "Mem Scope self payload must guard non-object data.",
        ),
        (
            "var rows = (d.entities && d.entities.entities) || [];",
            "Mem Scope entity rows must guard non-list data.",
        ),
        (
            "var totals = (d.damage && d.damage.totals) || {};",
            "Mem Scope damage totals must guard non-object data.",
        ),
        (
            "var s = d.search || {};",
            "Mem Scope search payload must guard non-object data.",
        ),
        (
            "var results = s.results || [];",
            "Mem Scope search results must guard non-list data.",
        ),
        (
            "var av = r.as || {}, bits = [esc(r.addr)];",
            "Mem Scope result hint rows must guard nested as payloads.",
        ),
        (
            "var loc = r['in'] || {};",
            "Mem Scope result hint rows must guard nested location payloads.",
        ),
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
            "function isObjectValue(value)",
            "Mem Scope should expose object-shape detection.",
        ),
        (
            "function objectValue(value)",
            "Mem Scope should normalize object payloads.",
        ),
        (
            "function listItems(value)",
            "Mem Scope should normalize list payloads.",
        ),
        (
            "function objectItems(value)",
            "Mem Scope should filter object-list payloads.",
        ),
        (
            "function finiteNum(value, fallback, lo, hi)",
            "Mem Scope should expose finite number normalization.",
        ),
        (
            "d = objectValue(d);",
            "Mem Scope should normalize root render payloads.",
        ),
        (
            "var st = objectValue(d.status);",
            "Mem Scope should normalize status payloads.",
        ),
        (
            "var cats = objectItems(objectValue(d.catalog).categories);",
            "Mem Scope should normalize catalog category rows.",
        ),
        (
            "var s = objectValue(d.self);",
            "Mem Scope should normalize self payloads.",
        ),
        (
            "var rows = objectItems(objectValue(d.entities).entities);",
            "Mem Scope should normalize entity rows.",
        ),
        (
            "var totals = objectValue(objectValue(d.damage).totals);",
            "Mem Scope should normalize damage totals maps.",
        ),
        (
            "var s = objectValue(d.search);",
            "Mem Scope should normalize search payloads.",
        ),
        (
            "var results = objectItems(s.results);",
            "Mem Scope should normalize search result rows.",
        ),
        (
            "var av = objectValue(r.as), bits = [esc(r.addr)];",
            "Mem Scope should normalize result hint value maps.",
        ),
        (
            "var loc = objectValue(r['in']);",
            "Mem Scope should normalize result hint location maps.",
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

    panel_forbidden = [
        (
            "data.speed.toFixed(2)",
            "Floating panel speed must not call toFixed() on raw payload values.",
        ),
        (
            "Math.round(data.bpm)",
            "Floating panel BPM must not round raw payload values.",
        ),
    ]
    for snippet, message in panel_forbidden:
        _check_absent(panel, snippet, message)

    panel_required = [
        (
            "function _panelFiniteNumber(value, fallback)",
            "Floating panel should expose finite numeric normalization.",
        ),
        (
            "function _panelFmtSpeed(value)",
            "Floating panel should format speed through a guarded helper.",
        ),
        (
            "function _panelFmtBpm(value)",
            "Floating panel should format BPM through a guarded helper.",
        ),
        (
            "el.textContent = _panelFmtSpeed(data.speed);",
            "Floating panel control speed should use guarded formatting.",
        ),
        (
            "b.textContent=_panelFmtBpm(data.bpm);",
            "Floating panel status BPM should use guarded formatting.",
        ),
        (
            "sp.textContent=_panelFmtSpeed(data.speed);",
            "Floating panel status speed should use guarded formatting.",
        ),
    ]
    for snippet, message in panel_required:
        _check_present(panel, snippet, message)

    print("web_small_panels_numeric_selftest: ok")


if __name__ == "__main__":
    main()
