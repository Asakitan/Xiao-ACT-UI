# -*- coding: utf-8 -*-
# Build and update lightweight runtime name tables.
#
# These tables are the small assets consumed by ``NameResolver`` at runtime.  They
# store stable ID -> name mappings only and intentionally ignore live dump
# provenance files such as full StringPool exports or pointer-table scans.
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
_ASSETS = os.path.join(_SAO, "assets")
_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
_DEFAULT_CACHE = os.path.join(_NAME_TABLES, "tcp_preparse_name_cache.json")
_LOCAL_CACHE = os.environ.get(
    "SAO_TCP_PREPARSE_NAME_CACHE_LOCAL",
    os.path.join(_NAME_TABLES, "tcp_preparse_name_cache.local.json"),
)
_DATATOOLS_CN = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Data", "CN")
_RESONANCE_METER = os.path.join(_REPO, "resonance-logs-cn", "src-tauri", "meter-data")
_RESONANCE_CONFIG = os.path.join(_REPO, "resonance-logs-cn", "src", "lib", "config")
_SR_WINFORM_TABLE = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WinForm", "Core", "TabelJson")
_SR_WPF_MONSTER = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WPF", "Data", "Monster")
_SR_OLD_MONSTER = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Old", "Data", "monster")

_RUNTIME_KINDS = (
    "player_skill", "monster_skill", "environment_skill",
    "field_marker", "boss_skill", "ultimate_skill", "roguelike_affix",
    "profession_skill", "scripted_skill", "virtual_skill", "boss_mechanic_skill",
    "client_effect_skill", "interaction_skill", "companion_skill", "projectile_skill",
    "passive_skill", "test_skill", "system_skill",
    "dungeon", "monster", "boss",
    "boss_mechanic",
    "buff", "player_buff", "factor_buff", "profession_skill_buff", "event",
)
_KIND_FILENAMES = {kind: os.path.join(_NAME_TABLES, f"{kind}.json") for kind in _RUNTIME_KINDS}
_CLASSIFIED_KINDS = {
    "player_skill", "monster_skill", "environment_skill",
    "field_marker", "boss_skill", "ultimate_skill", "roguelike_affix",
    "profession_skill", "scripted_skill", "virtual_skill", "boss_mechanic_skill",
    "client_effect_skill", "interaction_skill", "companion_skill", "projectile_skill",
    "passive_skill", "test_skill", "system_skill",
    "boss", "boss_mechanic", "buff", "player_buff", "factor_buff", "profession_skill_buff", "event",
}
_FALLBACK_PREFIXES = (
    "技能#", "玩家技能#", "怪物技能#", "环境技能#",
    "场地标记#", "Boss技能#", "幻想技能#", "肉鸽词条#", "职业技能#",
    "剧情表演#", "虚拟体技能#", "Boss机制技能#", "怪物#", "Boss#", "地牢#",
    "客户端表现技能#", "交互玩法技能#", "伙伴技能#", "投射物技能#", "被动/修饰技能#", "测试技能#", "系统技能#",
    "Buff#", "玩家Buff#", "因子Buff#", "职业技能Buff#", "事件#", "机制#", "NPC#", "道具#",
)
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


# Reference data from neighbouring projects (StarResonanceDps DataTools /
# resonance-logs-cn) sometimes carries scene/login placeholders that are not real
# display names. Our own TCP/MEM parse is authoritative, so these tokens must
# never be materialised into the runtime tables.
_TEXT_DENYLIST = {"login", "logout", "unknown", "none", "null"}

# Buff sub-kinds aggregate into the generic buff.json so that resolve("buff", id)
# (the fallback path when the classifier cannot refine the kind) also serves our
# in-memory names. There is no aggregate skill.json (deliberately removed), so
# skill sub-kinds need no parent here.
_AGGREGATE_PARENT_KIND = {
    "player_buff": "buff",
    "factor_buff": "buff",
    "profession_skill_buff": "buff",
}


def _clean_text(value: Any) -> str:
    text = str(value or "").strip()
    if not text:
        return ""
    if text.lower() in _TEXT_DENYLIST:
        return ""
    if any(text.startswith(prefix) for prefix in _FALLBACK_PREFIXES):
        return ""
    return text


def _entry_text(value: Any, *, fields: tuple[str, ...] = ("text", "name", "Name", "NameDesign")) -> str:
    if isinstance(value, str):
        return _clean_text(value)
    if isinstance(value, Mapping):
        for field in fields:
            text = _clean_text(value.get(field))
            if text:
                return text
    return ""


def _entry_confidence(value: Any) -> str:
    if isinstance(value, Mapping):
        return str(value.get("confidence") or "medium").strip().lower()
    return "static"


def _rank(confidence: str) -> int:
    return _CONFIDENCE_RANK.get(str(confidence or "").strip().lower(), 0)


def _iter_source_entries(obj: Any):
    if isinstance(obj, Mapping):
        yield from obj.items()
    elif isinstance(obj, list):
        for item in obj:
            if isinstance(item, Mapping):
                yield item.get("Id") or item.get("id") or item.get("ID"), item


def _coerce_name_map(obj: Any, *, fields: tuple[str, ...] = ("text", "name", "Name", "NameDesign")) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    for key, value in _iter_source_entries(obj):
        try:
            iid = int(key)
        except (TypeError, ValueError):
            continue
        if iid <= 0:
            continue
        text = _entry_text(value, fields=fields)
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


def _seed_named_json(path: str, *, confidence: str = "static", fields: tuple[str, ...] = ("text", "name", "Name", "NameDesign")) -> dict[str, dict[str, str]]:
    return {key: {"text": value.get("text") or "", "confidence": confidence} for key, value in _coerce_name_map(_load_json(path), fields=fields).items()}


def _classified_entries(kind: str) -> dict[str, dict[str, str]]:
    try:
        from tools.tablekit import name_table_classifier as classifier
    except Exception:
        return {}
    try:
        # Invalidate the classifier's self-contained caches so a freshly written
        # table is reflected on the next kind in this run.
        for name in ("_our_index", "_cache_by_kind", "profession_skill_ids",
                     "ultimate_skill_ids", "load_classified_tables"):
            fn = getattr(classifier, name, None)
            clear = getattr(fn, "cache_clear", None)
            if callable(clear):
                clear()
        table = classifier.load_classified_tables().get(kind, {})
        return {str(k): {"text": str(v.get("text") or ""), "confidence": str(v.get("confidence") or "static")} for k, v in table.items() if isinstance(v, Mapping) and v.get("text")}
    except Exception:
        return {}


def _community_entries(kind: str) -> dict[str, dict[str, str]]:
    # Per-kind base names from OUR project only (no neighbour repo).
    #
    # Classified kinds come from the self-contained classifier; boss mechanics from
    # combat_preparse; every other kind falls back to our own committed
    # ``<kind>.json`` so a missing StarResonanceDps/resonance checkout never empties
    # a table.
    if kind in _CLASSIFIED_KINDS:
        classified = _classified_entries(kind)
        if classified:
            return classified
    if kind == "boss_mechanic":
        seeded = _seed_boss_mechanics()
        if seeded:
            return seeded
    return _seed_named_json(os.path.join(_NAME_TABLES, f"{kind}.json"))


def _cache_entries(cache: Any, kind: str) -> dict[str, dict[str, str]]:
    if not isinstance(cache, Mapping):
        return {}
    by_kind = (((cache.get("names") or {}).get("by_kind") or {}))
    bucket = by_kind.get(kind) if isinstance(by_kind, Mapping) else {}
    return _coerce_name_map(bucket)


def _classify_cache_kind(generic_kind: str, key: str) -> str:
    try:
        from tools.tablekit.name_table_classifier import classify_id
        return classify_id(generic_kind, int(key)) or generic_kind
    except Exception:
        return generic_kind


def _semantic_cache_entries(cache: Any, kind: str) -> dict[str, dict[str, str]]:
    if not isinstance(cache, Mapping):
        return {}
    by_kind = (((cache.get("names") or {}).get("by_kind") or {}))
    if not isinstance(by_kind, Mapping):
        return {}
    out: dict[str, dict[str, str]] = {}
    for source_kind in ("skill", "buff"):
        for key, value in _coerce_name_map(by_kind.get(source_kind) or {}).items():
            classified = _classify_cache_kind(source_kind, key)
            if classified == kind or (kind == "buff" and classified in {"buff", "player_buff", "factor_buff", "profession_skill_buff"}):
                out[key] = value
    return out


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


def _overlay_entries(base: Mapping[str, Mapping[str, str]], overlay: Mapping[str, Mapping[str, str]]) -> dict[str, dict[str, str]]:
    # Force ``overlay`` on top of ``base``: overlay wins every id collision.
    #
    # ``base`` is the neighbouring reference data (StarResonanceDps DataTools /
    # resonance-logs-cn static tables) used only to fill ids we have not parsed.
    # ``overlay`` is our own live MEM/TCP parse (the runtime name cache, whose text
    # is the in-memory display name and whose id is pointer-table/anchor matched),
    # which is authoritative and therefore overrides the reference name on conflict.
    out = {
        str(k): {"text": str(v.get("text") or ""), "confidence": str(v.get("confidence") or "static")}
        for k, v in base.items() if v.get("text")
    }
    for key, value in overlay.items():
        text = _clean_text(value.get("text"))
        if not text:
            continue
        out[str(key)] = {"text": text, "confidence": str(value.get("confidence") or "medium").strip().lower()}
    return out


def _merge_cache_payloads(primary: Any, secondary: Any) -> dict[str, Any]:
    merged = {"schema_version": 1, "names": {"by_kind": {}}}
    for cache in (primary, secondary):
        if not isinstance(cache, Mapping):
            continue
        by_kind = (((cache.get("names") or {}).get("by_kind") or {}))
        if not isinstance(by_kind, Mapping):
            continue
        for kind, bucket in by_kind.items():
            if str(kind) == "player" or not isinstance(bucket, Mapping):
                continue
            target = merged["names"]["by_kind"].setdefault(str(kind), {})
            target.update(bucket)
    return merged


def update_runtime_tables(cache_path: str = _DEFAULT_CACHE, *, output_dir: str = _NAME_TABLES, write: bool = True, fill_missing_only: bool = True, include_local_cache: bool = False) -> dict[str, Any]:
    cache = _load_json(cache_path)
    if include_local_cache and os.path.abspath(cache_path) == os.path.abspath(_DEFAULT_CACHE):
        cache = _merge_cache_payloads(cache, _load_json(_LOCAL_CACHE))
    summary: dict[str, Any] = {"cache": os.path.normpath(cache_path), "updated_at": _iso(), "kinds": {}}
    for kind in _RUNTIME_KINDS:
        path = os.path.join(output_dir, f"{kind}.json")
        existing = _coerce_name_map(_load_json(path))
        cache_incoming = _cache_entries(cache, kind)
        if kind in _CLASSIFIED_KINDS:
            cache_incoming, _ = merge_entries(cache_incoming, _semantic_cache_entries(cache, kind), fill_missing_only=False)
        # Our live MEM/TCP parse (cache_incoming) is authoritative: its text is the
        # in-memory display name and its id is pointer-table/anchor matched. The
        # neighbouring reference tables (_community_entries) only fill ids we have
        # not parsed ourselves, and never override a name we read from memory.
        incoming = _overlay_entries(_community_entries(kind), cache_incoming)
        if kind in _CLASSIFIED_KINDS:
            merged = dict(incoming)
            changed = 1 if _plain_table(existing) != _plain_table(merged) else 0
        else:
            merged, changed = merge_entries(existing, incoming, fill_missing_only=fill_missing_only)
            overlaid = _overlay_entries(merged, cache_incoming)
            if _plain_table(overlaid) != _plain_table(merged):
                changed = 1
            merged = overlaid
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


def overlay_cache_into_existing_tables(cache_path: str = _DEFAULT_CACHE, *, output_dir: str = _NAME_TABLES,
                                       write: bool = True, include_local_cache: bool = False) -> dict[str, Any]:
    # Fold our live MEM/TCP parse (the name cache) onto the existing on-disk tables.
    #
    # Unlike :func:`update_runtime_tables`, this does NOT re-run the semantic
    # classifier and does NOT pull neighbouring reference tables.  It only writes
    # the names we parsed from memory (cache ``text``) over the ids they belong to,
    # leaving every other on-disk entry untouched.  Use this to align the runtime
    # tables with the authoritative cache without risking classifier drift.
    cache = _load_json(cache_path)
    if include_local_cache and os.path.abspath(cache_path) == os.path.abspath(_DEFAULT_CACHE):
        cache = _merge_cache_payloads(cache, _load_json(_LOCAL_CACHE))
    by_kind = (((cache.get("names") or {}).get("by_kind") or {})) if isinstance(cache, Mapping) else {}
    # Each cache bucket overrides its own <kind>.json, and buff sub-kinds also
    # override the aggregate buff.json so the generic ``resolve("buff", id)`` path
    # (used when the classifier can't refine, e.g. neighbouring BuffTable missing)
    # still returns our in-memory name instead of a stale aggregate entry.
    targets: dict[str, dict[str, str]] = {}
    for kind, bucket in by_kind.items():
        if str(kind) == "player" or not isinstance(bucket, Mapping):
            continue
        for key, entry in bucket.items():
            try:
                iid = int(key)
            except (TypeError, ValueError):
                continue
            if iid <= 0:
                continue
            text = _entry_text(entry)
            if not text:
                continue
            targets.setdefault(str(kind), {})[str(iid)] = text
            parent = _AGGREGATE_PARENT_KIND.get(str(kind))
            if parent:
                targets.setdefault(parent, {})[str(iid)] = text
    summary: dict[str, Any] = {"cache": os.path.normpath(cache_path), "updated_at": _iso(), "kinds": {}}
    for kind, overlay in targets.items():
        path = os.path.join(output_dir, f"{kind}.json")
        existing = _load_json(path)
        if not isinstance(existing, dict):
            existing = {}
        existing = {str(k): str(v) for k, v in existing.items() if isinstance(v, str)}
        changes: dict[str, dict[str, str]] = {}
        for sid, text in overlay.items():
            if existing.get(sid) != text:
                changes[sid] = {"old": existing.get(sid) or "", "new": text}
                existing[sid] = text
        if changes and write:
            _atomic_write_json(path, existing)
        summary["kinds"][kind] = {
            "path": os.path.normpath(path),
            "changed": len(changes),
            "detail": changes,
        }
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build/update lightweight runtime name tables from compact TCP/MEM cache")
    parser.add_argument("--cache", default=_DEFAULT_CACHE)
    parser.add_argument("--output-dir", default=_NAME_TABLES)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--allow-overwrite", action="store_true", help="Allow higher-confidence incoming names to replace existing names")
    parser.add_argument("--include-local-cache", action="store_true", help="Also merge ignored local runtime cache into generated local outputs")
    parser.add_argument("--overlay-only", action="store_true",
                        help="Safe mode: only overlay our parsed cache names onto existing tables; "
                             "skip the semantic classifier and neighbouring reference rebuild")
    args = parser.parse_args(argv)
    if args.overlay_only:
        result = overlay_cache_into_existing_tables(
            args.cache,
            output_dir=args.output_dir,
            write=not args.dry_run,
            include_local_cache=args.include_local_cache,
        )
    else:
        result = update_runtime_tables(
            args.cache,
            output_dir=args.output_dir,
            write=not args.dry_run,
            fill_missing_only=not args.allow_overwrite,
            include_local_cache=args.include_local_cache,
        )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
