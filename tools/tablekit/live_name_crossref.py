"""Cross-reference live runtime Chinese name candidates with known ID tables.

This tool does not promote live memory strings into authoritative tables.  It
merges ``assets/name_tables/live_probe_candidates.json`` with existing TCP and
community/config sources, then emits a confidence-scored draft that keeps every
claim tied to its source.
"""

from __future__ import annotations

import argparse
import ast
import json
import os
from collections import defaultdict
from typing import Any, DefaultDict, Dict, Iterable, List, Optional, Tuple


_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_REPO = os.path.dirname(_SAO)
_ASSETS = os.path.join(_SAO, "assets")
_NAME_TABLES = os.path.join(_ASSETS, "name_tables")
_SR_CN = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Data", "CN")
_SR_OLD_SKILL = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Old", "Data", "skill")
_RESONANCE = os.path.join(_REPO, "resonance-logs-cn")


MatchIndex = DefaultDict[str, List[Dict[str, Any]]]


def _load_json(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _safe_int(value: Any) -> Optional[int]:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _norm_text(value: Any) -> str:
    return str(value or "").strip()


def _source_path(path: str) -> str:
    try:
        return os.path.relpath(path, _REPO).replace("\\", "/")
    except ValueError:
        return path.replace("\\", "/")


def _add_match(index: MatchIndex, text: str, match: Dict[str, Any]) -> None:
    text = _norm_text(text)
    if not text:
        return
    row = dict(match)
    row.setdefault("match", "exact")
    row.setdefault("confidence", "medium")
    row.setdefault("tcp_status", "known_id_space")
    index[text].append(row)


def _iter_alias_texts(text: str) -> Iterable[Tuple[str, str]]:
    text = _norm_text(text)
    if not text:
        return
    for sep in ("-", "·", "_"):
        if sep in text:
            for part in text.split(sep):
                part = _norm_text(part)
                if 2 <= len(part) <= 16 and part != text:
                    yield part, f"compound_split_{sep}"
    suffixes = ("词条刷新概率提高", "刷新概率提高")
    for suffix in suffixes:
        if text.endswith(suffix) and len(text) > len(suffix):
            part = _norm_text(text[:-len(suffix)])
            if 2 <= len(part) <= 16 and part != text:
                yield part, f"suffix_strip_{suffix}"


def _iter_table_rows(obj: Any) -> Iterable[Tuple[str, Dict[str, Any]]]:
    if isinstance(obj, dict):
        for key, value in obj.items():
            if isinstance(value, dict):
                yield str(key), value
            else:
                yield str(key), {"Id": key, "Name": value}
    elif isinstance(obj, list):
        for value in obj:
            if isinstance(value, dict):
                key = value.get("Id") or value.get("id") or ""
                yield str(key), value


def _index_named_json_table(
    index: MatchIndex,
    path: str,
    *,
    id_space: str,
    tcp_field: str,
    source_kind: str,
    confidence: str,
    tcp_status: str = "known_id_space",
    fields: Tuple[str, ...] = ("Name", "NameDesign"),
) -> None:
    if not os.path.isfile(path):
        return
    data = _load_json(path)
    source = _source_path(path)
    for key, row in _iter_table_rows(data):
        raw_id = row.get("Id") or row.get("id") or key
        item_id = _safe_int(raw_id)
        if item_id is None:
            continue
        for field in fields:
            text = _norm_text(row.get(field))
            if not text:
                continue
            base_match = {
                "source": source,
                "source_kind": source_kind,
                "id_space": id_space,
                "id": item_id,
                "tcp_field": tcp_field,
                "field": field,
                "confidence": confidence,
                "tcp_status": tcp_status,
            }
            _add_match(index, text, base_match)
            for alias, alias_reason in _iter_alias_texts(text):
                alias_match = dict(base_match)
                alias_match.update({
                    "match": "alias_from_compound_name",
                    "alias_of": text,
                    "alias_reason": alias_reason,
                    "confidence": "medium" if confidence == "high" else "low",
                })
                _add_match(index, alias, alias_match)


def _index_skill_name_map(index: MatchIndex) -> None:
    sources = [
        os.path.join(_SR_OLD_SKILL, "skill_name_mapping.json"),
        os.path.join(_ASSETS, "skill_names.json"),
    ]
    for path in sources:
        if not os.path.isfile(path):
            continue
        data = _load_json(path)
        source = _source_path(path)
        for key, value in data.items():
            item_id = _safe_int(key)
            text = _norm_text(value)
            if item_id is None or not text:
                continue
            confidence = "high" if "StarResonanceDps" in source else "low"
            _add_match(index, text, {
                "source": source,
                "source_kind": "skill_name_map",
                "id_space": "skill_id_or_legacy_skill_name_index",
                "id": item_id,
                "tcp_field": "skill_id",
                "field": "value",
                "confidence": confidence,
                "tcp_status": "known_id_space" if confidence == "high" else "auxiliary_name_source",
            })


def _literal_dicts_from_python(path: str) -> Dict[str, Dict[int, Any]]:
    with open(path, "r", encoding="utf-8-sig") as f:
        source = f.read()
    module = ast.parse(source, filename=path)
    out: Dict[str, Dict[int, Any]] = {}
    wanted = {
        "PROFESSION_NAMES",
        "PROFESSION_NORMAL_ATTACK",
        "PROFESSION_SKILL",
        "PROFESSION_ULTIMATE",
        "PROFESSION_SKILL_VARIANTS",
        "SUB_PROFESSION_NAMES",
    }
    for node in module.body:
        if not isinstance(node, ast.Assign) or not isinstance(node.value, ast.Dict):
            continue
        names = [target.id for target in node.targets if isinstance(target, ast.Name)]
        for name in names:
            if name not in wanted:
                continue
            try:
                value = ast.literal_eval(node.value)
            except Exception:
                continue
            if isinstance(value, dict):
                out[name] = value
    return out


def _index_packet_parser_skills(index: MatchIndex) -> None:
    path = os.path.join(_SAO, "packet_parser", "skills.py")
    if not os.path.isfile(path):
        return
    tables = _literal_dicts_from_python(path)
    source = _source_path(path)
    for profession_id, name in (tables.get("PROFESSION_NAMES") or {}).items():
        _add_match(index, str(name), {
            "source": source,
            "source_kind": "tcp_parser_profession_map",
            "id_space": "profession_id",
            "id": int(profession_id),
            "tcp_field": "CharField.PROFESSION_ID/profession_id",
            "field": "PROFESSION_NAMES",
            "confidence": "high",
            "tcp_status": "parsed_by_tcp",
        })
    for table_name, tcp_note in (
        ("PROFESSION_NORMAL_ATTACK", "skill_id SlotPositionId=1"),
        ("PROFESSION_SKILL", "skill_id SlotPositionId=2"),
        ("PROFESSION_ULTIMATE", "skill_id SkillType=1 SlotPositionId=6"),
    ):
        for profession_id, skill_id in (tables.get(table_name) or {}).items():
            _add_match(index, str(skill_id), {
                "source": source,
                "source_kind": "tcp_parser_skill_anchor",
                "id_space": "skill_id",
                "id": int(skill_id),
                "profession_id": int(profession_id),
                "tcp_field": tcp_note,
                "field": table_name,
                "match": "id_anchor_only",
                "confidence": "high",
                "tcp_status": "parsed_by_tcp",
            })
    for skill_id, branch_name in (tables.get("SUB_PROFESSION_NAMES") or {}).items():
        _add_match(index, str(branch_name), {
            "source": source,
            "source_kind": "tcp_parser_sub_profession_map",
            "id_space": "skill_id",
            "id": int(skill_id),
            "tcp_field": "skill_id/sub_profession",
            "field": "SUB_PROFESSION_NAMES",
            "confidence": "high",
            "tcp_status": "parsed_by_tcp",
        })


def _index_curated_flow_aliases(index: MatchIndex) -> None:
    # These are not decoded from runtime table records yet.  They connect the
    # UI flow names observed in memory to TCP branch anchors already maintained
    # in packet_parser/skills.py, with explicit alias confidence.
    source = "manual:packet_parser_skills_flow_aliases"
    aliases = [
        ("防护回复流", 12, 2405, "防盾"),
        ("光盾回复流", 12, 2406, "光盾"),
    ]
    for text, profession_id, skill_id, branch in aliases:
        _add_match(index, text, {
            "source": source,
            "source_kind": "tcp_flow_alias",
            "id_space": "sub_profession_skill_id",
            "id": skill_id,
            "profession_id": profession_id,
            "branch_name": branch,
            "tcp_field": "skill_id/sub_profession",
            "field": "curated_alias",
            "match": "alias",
            "confidence": "medium",
            "tcp_status": "parsed_by_tcp_alias",
        })


def _build_index() -> MatchIndex:
    index: MatchIndex = defaultdict(list)
    _index_named_json_table(
        index,
        os.path.join(_SR_CN, "SkillTable.json"),
        id_space="skill_id",
        tcp_field="skill_id/skill_level_id",
        source_kind="StarResonanceDps.SkillTable",
        confidence="high",
        fields=("Name", "NameDesign"),
    )
    _index_named_json_table(
        index,
        os.path.join(_SR_CN, "BuffTable.json"),
        id_space="buff_id",
        tcp_field="buff_id/BaseId",
        source_kind="StarResonanceDps.BuffTable",
        confidence="high",
        fields=("Name", "NameDesign"),
    )
    _index_named_json_table(
        index,
        os.path.join(_RESONANCE, "src", "lib", "config", "BuffName.json"),
        id_space="buff_id",
        tcp_field="buff_id/BaseId",
        source_kind="resonance-logs-cn.BuffName",
        confidence="high",
        fields=("Name", "NameDesign"),
    )
    _index_named_json_table(
        index,
        os.path.join(_RESONANCE, "src-tauri", "meter-data", "TempAttrTable.json"),
        id_space="temp_attr_id",
        tcp_field="temp_attr_id/config_only",
        source_kind="resonance-logs-cn.TempAttrTable",
        confidence="medium",
        tcp_status="config_only_not_confirmed_tcp",
        fields=("Name", "AttrDesc", "Desc"),
    )
    _index_named_json_table(
        index,
        os.path.join(_RESONANCE, "src-tauri", "meter-data", "SkillEffectTable.json"),
        id_space="skill_effect_id",
        tcp_field="skill_effect_id/SkillId",
        source_kind="resonance-logs-cn.SkillEffectTable",
        confidence="medium",
        fields=("Name",),
    )
    _index_skill_name_map(index)
    _index_packet_parser_skills(index)
    _index_curated_flow_aliases(index)
    return index


def _dedupe_matches(matches: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    seen = set()
    out: List[Dict[str, Any]] = []
    for match in matches:
        key = (
            match.get("source"),
            match.get("source_kind"),
            match.get("id_space"),
            match.get("id"),
            match.get("field"),
            match.get("match"),
        )
        if key in seen:
            continue
        seen.add(key)
        out.append(match)
    rank = {"high": 0, "medium": 1, "low": 2}
    out.sort(key=lambda row: (rank.get(str(row.get("confidence")), 9), str(row.get("id_space")), int(row.get("id") or 0)))
    return out


def _classify(matches: List[Dict[str, Any]]) -> str:
    if not matches:
        return "runtime_only"
    if any(match.get("confidence") == "high" and match.get("tcp_status") in {"parsed_by_tcp", "known_id_space"} for match in matches):
        return "authoritative_external_or_tcp"
    if any(match.get("tcp_status") == "parsed_by_tcp_alias" for match in matches):
        return "tcp_alias"
    return "candidate_external"


def _runtime_key(row: Dict[str, Any]) -> Tuple[str, str, str]:
    return (
        str(row.get("obj") or ""),
        str(row.get("chars") or ""),
        _norm_text(row.get("text")),
    )


def crossref(candidate_input: str, output: str) -> Dict[str, Any]:
    candidates = _load_json(candidate_input)
    index = _build_index()
    groups: Dict[str, List[Dict[str, Any]]] = {}
    unique_candidates: Dict[Tuple[str, str, str], Dict[str, Any]] = {}
    total = 0
    matched = 0
    for group_key, rows in (candidates.get("groups") or {}).items():
        out_rows: List[Dict[str, Any]] = []
        for row in rows or []:
            total += 1
            text = _norm_text(row.get("text"))
            matches = _dedupe_matches(index.get(text, []))
            if matches:
                matched += 1
            item = {
                "text": text,
                "runtime": {
                    "obj": row.get("obj"),
                    "chars": row.get("chars"),
                    "klass": row.get("klass") or "0x30252490",
                    "klass_name": "System.String",
                    "anchor_status": "runtime_heap_address_only",
                },
                "classification": _classify(matches),
                "matches": matches,
            }
            out_rows.append(item)
            unique_candidates.setdefault(_runtime_key(row), item)
        groups[group_key] = out_rows
    unique_rows = sorted(
        unique_candidates.values(),
        key=lambda item: int(str(item.get("runtime", {}).get("obj") or "0"), 16),
    )
    unique_matched = sum(1 for item in unique_rows if item.get("matches"))
    result = {
        "source": "live_name_crossref",
        "input": _source_path(candidate_input),
        "process": candidates.get("process"),
        "pid": candidates.get("pid"),
        "note": (
            "Cross-reference draft. Runtime obj/chars are volatile heap addresses; "
            "ID mappings are only as authoritative as their listed source. "
            "Persistent table/container anchors are not decoded yet."
        ),
        "known_runtime_klass": {
            "System.String": {
                "runtime_klass": "0x30252490",
                "anchor_status": "runtime_pointer_confirmed_name_System.String_not_persistent_rva",
            }
        },
        "summary": {
            "total_runtime_candidates": total,
            "matched_candidates": matched,
            "unmatched_candidates": total - matched,
            "unique_runtime_candidates": len(unique_rows),
            "matched_unique_candidates": unique_matched,
            "unmatched_unique_candidates": len(unique_rows) - unique_matched,
        },
        "unique_candidates": unique_rows,
        "groups": groups,
    }
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
    return result


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Cross-reference live name candidates with known TCP/config tables")
    parser.add_argument("--candidate-input", default=os.path.join(_NAME_TABLES, "live_probe_candidates.json"))
    parser.add_argument("--output", default=os.path.join(_NAME_TABLES, "live_probe_id_candidates.json"))
    args = parser.parse_args(argv)
    result = crossref(args.candidate_input, args.output)
    print(json.dumps({
        "output": args.output,
        "summary": result.get("summary"),
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())