# -*- coding: utf-8 -*-
"""Build and update lightweight runtime name tables.

These tables are the small assets consumed by ``NameResolver`` at runtime.  They
store stable ID -> name mappings only and intentionally ignore live dump
provenance files such as full StringPool exports or pointer-table scans.
"""
from __future__ import annotations

import argparse
import json
import os
import tempfile
import time
from typing import Any, Mapping

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_REPO = os.path.dirname(_SAO)
_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
_DEFAULT_CACHE = os.path.join(_NAME_TABLES, "tcp_preparse_name_cache.json")
_RESONANCE_METER = os.path.join(_REPO, "resonance-logs-cn", "src-tauri", "meter-data")
_RESONANCE_CONFIG = os.path.join(_REPO, "resonance-logs-cn", "src", "lib", "config")
_SR_WINFORM_TABLE = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WinForm", "Core", "TabelJson")
_SR_WPF_MONSTER = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WPF", "Data", "Monster")

_RUNTIME_KINDS = ("skill", "dungeon", "monster", "boss", "boss_mechanic", "buff")
_KIND_FILENAMES = {kind: os.path.join(_NAME_TABLES, f"{kind}.json") for kind in _RUNTIME_KINDS}
_FALLBACK_PREFIXES = ("技能#", "怪物#", "Boss#", "地牢#", "Buff#", "机制#", "NPC#", "道具#")
_CONFIDENCE_RANK = {
    "static": 50,
    "curated": 45,
    "mem": 40,
    "tcp": 35,
    "high": 30,
    "medium": 20,
    "low": 10,
}


def _iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _load_json(path: str) -> Any:
    if not path or not os.path.isfile(path):
        return {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def _atomic_write_json(path: str, payload: Any) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    fd, tmp_path = tempfile.mkstemp(prefix=os.path.basename(path) + ".", suffix=".tmp", dir=os.path.dirname(path))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(tmp_path, path)
    finally:
        try:
            if os.path.exists(tmp_path):
                os.unlink(tmp_path)
        except Exception:
            pass


def _clean_text(value: Any) -> str:
    text = str(value or "").strip()
    if not text:
        return ""
    if any(text.startswith(prefix) for prefix in _FALLBACK_PREFIXES):
        return ""
    return text


def _entry_text(value: Any) -> str:
    if isinstance(value, str):
        return _clean_text(value)
    if isinstance(value, Mapping):
        return _clean_text(value.get("text") or value.get("name") or value.get("Name") or value.get("NameDesign"))
    return ""


def _entry_confidence(value: Any) -> str:
    if isinstance(value, Mapping):
        return str(value.get("confidence") or "medium").strip().lower()
    return "static"


def _rank(confidence: str) -> int:
    return _CONFIDENCE_RANK.get(str(confidence or "").strip().lower(), 0)


def _coerce_name_map(obj: Any) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    if not isinstance(obj, Mapping):
        return out
    for key, value in obj.items():
        try:
            iid = int(key)
        except (TypeError, ValueError):
            continue
        if iid <= 0:
            continue
        text = _entry_text(value)
        if not text:
            continue
        out[str(iid)] = {"text": text, "confidence": _entry_confidence(value)}
    return out


def _plain_table(entries: Mapping[str, Mapping[str, str]]) -> dict[str, str]:
    return {key: str(value.get("text") or "") for key, value in sorted(entries.items(), key=lambda item: int(item[0])) if value.get("text")}


def _seed_boss_mechanics() -> dict[str, dict[str, str]]:
    try:
        from tools.tablekit.combat_preparse import BOSS_MECHANIC_EVENTS
    except Exception:
        return {}
    out: dict[str, dict[str, str]] = {}
    for event_type, meta in BOSS_MECHANIC_EVENTS.items():
        try:
            key = str(int(event_type))
        except Exception:
            continue
        text = _clean_text((meta or {}).get("label"))
        if text:
            out[key] = {"text": text, "confidence": "static"}
    return out


def _seed_named_json(path: str, *, confidence: str = "static") -> dict[str, dict[str, str]]:
    return {key: {"text": value.get("text") or "", "confidence": confidence} for key, value in _coerce_name_map(_load_json(path)).items()}


def _community_entries(kind: str) -> dict[str, dict[str, str]]:
    if kind == "dungeon":
        merged: dict[str, dict[str, str]] = {}
        for path in (
            os.path.join(_RESONANCE_METER, "SceneName.json"),
            os.path.join(_RESONANCE_CONFIG, "SceneName.json"),
        ):
            merged, _ = merge_entries(merged, _seed_named_json(path), fill_missing_only=True)
        return merged
    if kind == "monster":
        merged: dict[str, dict[str, str]] = {}
        for path in (
            os.path.join(_SR_WINFORM_TABLE, "monster_names.json"),
            os.path.join(_SR_WPF_MONSTER, "monster.zh-CN.json"),
        ):
            merged, _ = merge_entries(merged, _seed_named_json(path), fill_missing_only=True)
        return merged
    if kind == "boss":
        names = _community_entries("monster")
        types = _load_json(os.path.join(_RESONANCE_METER, "MonsterIdNameType.json"))
        if not isinstance(types, Mapping):
            types = _load_json(os.path.join(_RESONANCE_CONFIG, "MonsterIdNameType.json"))
        if not isinstance(types, Mapping):
            return {}
        out: dict[str, dict[str, str]] = {}
        for key, type_value in types.items():
            try:
                if int(type_value) != 2:
                    continue
            except Exception:
                continue
            entry = names.get(str(key))
            if entry and _clean_text(entry.get("text")):
                out[str(key)] = {"text": _clean_text(entry.get("text")), "confidence": "static"}
        return out
    if kind == "boss_mechanic":
        return _seed_boss_mechanics()
    return {}


def _cache_entries(cache: Any, kind: str) -> dict[str, dict[str, str]]:
    if not isinstance(cache, Mapping):
        return {}
    by_kind = (((cache.get("names") or {}).get("by_kind") or {}))
    bucket = by_kind.get(kind) if isinstance(by_kind, Mapping) else {}
    return _coerce_name_map(bucket)


def merge_entries(existing: Mapping[str, Mapping[str, str]], incoming: Mapping[str, Mapping[str, str]], *, fill_missing_only: bool = True) -> tuple[dict[str, dict[str, str]], int]:
    merged = {str(k): {"text": str(v.get("text") or ""), "confidence": str(v.get("confidence") or "static")} for k, v in existing.items() if v.get("text")}
    changed = 0
    for key, value in incoming.items():
        text = _clean_text(value.get("text"))
        if not text:
            continue
        conf = str(value.get("confidence") or "medium").strip().lower()
        old = merged.get(str(key))
        if old is None:
            merged[str(key)] = {"text": text, "confidence": conf}
            changed += 1
        elif not fill_missing_only and _rank(conf) > _rank(old.get("confidence", "")) and old.get("text") != text:
            merged[str(key)] = {"text": text, "confidence": conf}
            changed += 1
    return merged, changed


def update_runtime_tables(cache_path: str = _DEFAULT_CACHE, *, output_dir: str = _NAME_TABLES, write: bool = True, fill_missing_only: bool = True) -> dict[str, Any]:
    cache = _load_json(cache_path)
    summary: dict[str, Any] = {"cache": os.path.normpath(cache_path), "updated_at": _iso(), "kinds": {}}
    for kind in _RUNTIME_KINDS:
        path = os.path.join(output_dir, f"{kind}.json")
        existing = _coerce_name_map(_load_json(path))
        incoming, _ = merge_entries(_community_entries(kind), _cache_entries(cache, kind), fill_missing_only=False)
        merged, changed = merge_entries(existing, incoming, fill_missing_only=fill_missing_only)
        if write and (changed or not os.path.isfile(path)):
            _atomic_write_json(path, _plain_table(merged))
        summary["kinds"][kind] = {
            "path": os.path.normpath(path),
            "existing": len(existing),
            "incoming": len(incoming),
            "written": len(merged),
            "changed": changed,
        }
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build/update lightweight runtime name tables from compact TCP/MEM cache")
    parser.add_argument("--cache", default=_DEFAULT_CACHE)
    parser.add_argument("--output-dir", default=_NAME_TABLES)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--allow-overwrite", action="store_true", help="Allow higher-confidence incoming names to replace existing names")
    args = parser.parse_args(argv)
    result = update_runtime_tables(
        args.cache,
        output_dir=args.output_dir,
        write=not args.dry_run,
        fill_missing_only=not args.allow_overwrite,
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
