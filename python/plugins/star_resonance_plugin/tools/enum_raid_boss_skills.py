# -*- coding: utf-8 -*-
"""enum_raid_boss_skills - enumerate a raid dungeon's boss skill list straight from
the game's own config tables (read-only memory; works for skills never observed
in combat).

Chain:
  RaidDungeonTable  rows filtered by DungeonId -> difficulty rows (Difficult,
                    localized Name) -> chosen row's BossId[] (one id per phase)
  MonsterTable      boss row by Id -> Name(mlid), SkillIds[], BornSkillId
  SkillTable        each skill id -> Name(mlid)
  StringPool        mlid -> localized CN string

Column offsets come from table_columns (getter-thunk extraction with versioned
cache + curated fallback + live self-check); class fields go through
auto_offsets inside MemConfigTableReader.

Usage:
  python -m tools.enum_raid_boss_skills --dungeon 13023 --list-difficulties
  python -m tools.enum_raid_boss_skills --dungeon 13023 --difficulty 4 --dry-run
  python -m tools.enum_raid_boss_skills --dungeon 13023 --difficulty 4 --all-phases --dry-run
  python -m tools.enum_raid_boss_skills --dungeon 13023 --difficulty 4 --apply

--dry-run prints + writes the exports JSON only. --apply additionally records
every skill into the BossSkillStore observation library and overlays the
skill/monster names onto the runtime name tables.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from typing import Dict, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.il2cpp import table_columns
from mem_probe.il2cpp.mem_config_table_reader import MemConfigTableReader, TABLE_CLASS
from mem_probe.il2cpp.mem_string_pool import StringPoolBridge, _is_cjk

RAID_CLS = TABLE_CLASS["raid_dungeon"]
MONSTER_CLS = TABLE_CLASS["monster"]
SKILL_CLS = TABLE_CLASS["skill"]

_NAME_TABLES = os.path.join(_ROOT, "assets", "name_tables")
_OVERLAY_CACHE = os.path.join(_NAME_TABLES, "live_raid_skill_name_cache.json")


def _utf8_stdout() -> None:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass


def _dungeon_offline_name(dungeon_id: int) -> str:
    try:
        with open(os.path.join(_NAME_TABLES, "dungeon.json"), "r", encoding="utf-8") as f:
            return str(json.load(f).get(str(dungeon_id)) or "")
    except Exception:
        return ""


class RaidSkillEnumerator:
    """Read-only enumeration over RaidDungeon/Monster/Skill config tables."""

    def __init__(self, src, log=print):
        self.src = src
        self.rd = MemConfigTableReader(src)
        self.pool = StringPoolBridge(src)
        self.log = log
        self.notes: List[str] = []
        self.fallbacks_used: List[str] = []
        self.columns: Dict[str, Dict[str, Tuple[int, str]]] = {}
        self._monster_index: Optional[Dict[int, Tuple[int, int]]] = None
        self._skill_index: Optional[Dict[int, Tuple[int, int]]] = None

    # ── setup ─────────────────────────────────────────────────────────────────
    def prepare(self) -> bool:
        t0 = time.time()
        if not self.pool.build():
            self.log("[enum] string pool NOT located — names will be empty")
            self.notes.append("string_pool_unavailable")
        else:
            self.log(f"[enum] string pool ready ({time.time()-t0:.1f}s)")
        classes = [RAID_CLS, MONSTER_CLS, SKILL_CLS, TABLE_CLASS["buff"]]
        self.columns = table_columns.load_columns(classes, log=self.log)
        # live self-check on the raid table; downgrade to literals on failure
        cols = self.columns[RAID_CLS]
        if not table_columns.self_check(self.rd, RAID_CLS, cols, pool=self.pool,
                                        expect={"Difficult": (0, 10)}, log=self.log):
            lit = dict(table_columns.FALLBACK.get(RAID_CLS, {}))
            if lit and lit != cols:
                self.log(f"[enum] {RAID_CLS}: extracted columns failed live "
                         f"self-check; downgrading to curated literals")
                self.fallbacks_used.append("raid_columns_literal")
                self.columns[RAID_CLS] = lit
                if not table_columns.self_check(self.rd, RAID_CLS, lit, pool=self.pool,
                                                expect={"Difficult": (0, 10)},
                                                log=self.log):
                    self.log(f"[enum] {RAID_CLS}: literal columns also fail "
                             f"self-check — results may be garbage")
                    self.notes.append("raid_columns_unverified")
            else:
                self.notes.append("raid_columns_unverified")
        return True

    def _col(self, cls: str, prop: str) -> Optional[int]:
        ent = self.columns.get(cls, {}).get(prop)
        return ent[0] if ent else None

    # ── raid table rows ───────────────────────────────────────────────────────
    def _decode_raid_row(self, zl: int, blob: int, source: str) -> Dict:
        desc = self.pool.resolve(self.rd.col_mlid(blob, self._col(RAID_CLS, "Desc")))
        return {
            "dungeon_id": self.rd.col_i32(blob, self._col(RAID_CLS, "DungeonId")),
            "difficulty": self.rd.col_i32(blob, self._col(RAID_CLS, "Difficult")),
            "group_id": self.rd.col_i32(blob, self._col(RAID_CLS, "GroupId")),
            "name": self.pool.resolve(self.rd.col_mlid(blob, self._col(RAID_CLS, "Name"))),
            "desc": re.sub(r"<[^>]*>", "", desc or "").strip(),
            "boss_ids": self.rd.col_i32_array(zl, blob, self._col(RAID_CLS, "BossId")),
            "source": source,
            "_zl": zl, "_blob": blob,
        }

    def all_raid_rows(self) -> List[Dict]:
        """Every RaidDungeon row: validated ZLoader walk first (full serialized
        set), heap scan merged for anything the walk missed."""
        seen: Dict[int, Dict] = {}
        n_loader = 0
        for _key, zl, blob in self.rd.iter_rows_via_loader(RAID_CLS):
            n_loader += 1
            r = self._decode_raid_row(zl, blob, "loader")
            if (r["dungeon_id"] or 0) > 0:
                seen.setdefault(blob, r)
        if n_loader == 0:
            self.notes.append("raid_loader_walk_empty")
            self.log("[enum] raid table loader walk yielded no rows; "
                     "falling back to instantiated heap rows only")
            self.fallbacks_used.append("raid_heap_scan_only")
            for _row, zl, blob in self.rd.iter_rows(RAID_CLS):
                r = self._decode_raid_row(zl, blob, "heap")
                if (r["dungeon_id"] or 0) > 0:
                    seen.setdefault(blob, r)
        return sorted(seen.values(),
                      key=lambda r: (r["group_id"] or 0, r["difficulty"] or 0))

    def family_rows(self, dungeon_id: int) -> Tuple[List[Dict], List[Dict]]:
        """(exact-DungeonId rows, GroupId family rows). A raid's difficulties
        live in SIBLING rows sharing GroupId, each with its own DungeonId."""
        rows = self.all_raid_rows()
        exact = [r for r in rows if r["dungeon_id"] == dungeon_id]
        gids = {r["group_id"] for r in exact if r["group_id"]}
        family = [r for r in rows if r["group_id"] in gids] if gids else exact
        return exact, family

    # ── monster / skill lookups ───────────────────────────────────────────────
    def _build_index(self, cls: str) -> Dict[int, Tuple[int, int]]:
        """id -> (zloader, blob) over the FULL table (loader walk first — it has
        every serialized row — heap scan merged on top)."""
        c_id = self._col(cls, "Id")
        out: Dict[int, Tuple[int, int]] = {}
        for key, zl, blob in self.rd.iter_rows_via_loader(cls):
            rid = self.rd.col_i32(blob, c_id)
            if rid is not None and rid > 0:
                out[rid] = (zl, blob)
        if not out:
            self.log(f"[enum] {cls}: loader walk empty — falling back to heap scan")
            self.fallbacks_used.append(f"{cls}_heap_scan_only")
            for _row, zl, blob in self.rd.iter_rows(cls):
                rid = self.rd.col_i32(blob, c_id)
                if rid is not None and rid > 0:
                    out.setdefault(rid, (zl, blob))
        return out

    def monster_index(self) -> Dict[int, Tuple[int, int]]:
        if self._monster_index is None:
            t0 = time.time()
            self._monster_index = self._build_index(MONSTER_CLS)
            self.log(f"[enum] monster table: {len(self._monster_index)} rows "
                     f"({time.time()-t0:.1f}s)")
        return self._monster_index

    def skill_index(self) -> Dict[int, Tuple[int, int]]:
        if self._skill_index is None:
            t0 = time.time()
            self._skill_index = self._build_index(SKILL_CLS)
            self.log(f"[enum] skill table: {len(self._skill_index)} rows "
                     f"({time.time()-t0:.1f}s)")
        return self._skill_index

    def monster_info(self, monster_id: int) -> Optional[Dict]:
        ent = self.monster_index().get(int(monster_id))
        if not ent:
            return None
        zl, blob = ent
        info = {
            "id": int(monster_id),
            "name": self.pool.resolve(self.rd.col_mlid(blob, self._col(MONSTER_CLS, "Name"))),
            "skill_ids": self.rd.col_i32_array(zl, blob, self._col(MONSTER_CLS, "SkillIds")),
        }
        c_born = self._col(MONSTER_CLS, "BornSkillId")
        if c_born is not None:
            info["born_skill_id"] = self.rd.col_i32(blob, c_born)
        else:
            self.notes.append("born_skill_column_unavailable")
        return info

    def skill_name(self, skill_id: int) -> Tuple[str, str]:
        """(name, source). Boss mechanic skills usually have an EMPTY localized
        Name; the designer name (NameDesign, raw string column) is the real
        label then."""
        ent = self.skill_index().get(int(skill_id))
        if not ent:
            return "", "missing"
        zl, blob = ent
        nm = self.pool.resolve(self.rd.col_mlid(blob, self._col(SKILL_CLS, "Name")))
        if nm:
            return nm, "mlstring"
        nd = self.rd.col_string(zl, blob, self._col(SKILL_CLS, "NameDesign"))
        return (nd, "design") if nd else ("", "empty")

    def monsters_by_name(self, terms: List[str]) -> List[Dict]:
        """Fallback (A): full monster-table scan filtered by name substrings."""
        c_name = self._col(MONSTER_CLS, "Name")
        out = []
        for rid, (zl, blob) in self.monster_index().items():
            nm = self.pool.resolve(self.rd.col_mlid(blob, c_name))
            if nm and any(t in nm for t in terms):
                out.append({"id": rid, "name": nm})
        return sorted(out, key=lambda m: m["id"])


# ── apply: BossSkillStore + name-table overlay ───────────────────────────────

def _apply(result: Dict, log=print) -> Dict:
    report: Dict = {}
    from plugins.star_resonance_plugin.engines.boss_skill_store import BossSkillStore, KIND_SKILL
    store = BossSkillStore()
    n_obs = 0
    dungeon_id = int(result["dungeon_id"])
    scene_name = result.get("dungeon_name") or result.get("difficulty_name") or ""
    name_rows: List[Dict] = []
    boss_names: Dict[int, str] = {}
    for ph in result.get("phases", []):
        boss_id = int(ph.get("boss_id") or 0)
        boss_name = ph.get("boss_name") or ""
        if boss_id <= 0:
            continue
        if boss_name:
            boss_names[boss_id] = boss_name
        skills = list(ph.get("skills") or [])
        born = ph.get("born_skill") or {}
        if born.get("id"):
            skills = skills + [dict(born, _born=True)]
        for sk in skills:
            sid = int(sk.get("id") or 0)
            if sid <= 0:
                continue
            sname = sk.get("name") or ""
            tags = ["config"] + (["born"] if sk.get("_born") else [])
            store.observe(scene_key=str(dungeon_id), dungeon_id=dungeon_id,
                          scene_name=scene_name, boss_base_id=boss_id,
                          boss_name=boss_name, obs_id=sid, name=sname,
                          kind=KIND_SKILL, tags=tags)
            n_obs += 1
            if sname:
                name_rows.append({"text": sname, "confidence": "mem",
                                  "primary_match": {"id_space": "skill_id", "id": sid}})
    saved = store.save(force=True)
    report["boss_skill_store"] = {"observations": n_obs, "saved": saved,
                                  "path": store._path}
    log(f"[apply] BossSkillStore: {n_obs} observations -> {store._path} (saved={saved})")

    # name-table overlay (same funnel as tools.tablekit.mem_name_ingest). Skill
    # ids go through the live-rows builder (the classifier refines the skill
    # sub-kind); boss names are injected as kind "monster" directly — the same
    # kind the runtime mem name path records boss base ids under.
    from plugins.star_resonance_plugin.net.tcp_name_cache import build_index_from_live_rows, sanitize_shared_cache
    raw = build_index_from_live_rows(
        {"rows": name_rows}, confidence={"high", "medium", "mem", "tcp", "static"})
    if boss_names:
        bucket = raw.setdefault("names", {}).setdefault("by_kind", {}).setdefault("monster", {})
        for bid, bname in boss_names.items():
            bucket[str(bid)] = {"text": bname, "confidence": "mem"}
    index = sanitize_shared_cache(raw)
    os.makedirs(os.path.dirname(_OVERLAY_CACHE), exist_ok=True)
    with open(_OVERLAY_CACHE, "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")
    from tools.tablekit.hybrid_name_tables import overlay_cache_into_existing_tables
    ov = overlay_cache_into_existing_tables(_OVERLAY_CACHE, write=True)
    changed = {k: v["changed"] for k, v in ov["kinds"].items() if v.get("changed")}
    report["name_overlay"] = {"cache": _OVERLAY_CACHE, "changed": changed,
                              "total_changed": sum(changed.values())}
    log(f"[apply] name overlay: {changed or 'no changes'}")
    return report


# ── CLI ──────────────────────────────────────────────────────────────────────

def main(argv: Optional[List[str]] = None) -> int:
    _utf8_stdout()
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dungeon", type=int, default=13023)
    p.add_argument("--list-difficulties", action="store_true",
                   help="list the dungeon's difficulty rows (Difficult + CN name) and exit")
    p.add_argument("--difficulty", type=int, default=None,
                   help="Difficult value to enumerate (see --list-difficulties)")
    p.add_argument("--phase", type=int, default=3,
                   help="boss phase, 1-based index into BossId[] (default 3)")
    p.add_argument("--all-phases", action="store_true", help="enumerate every BossId entry")
    p.add_argument("--dry-run", action="store_true", help="print + export only (default)")
    p.add_argument("--apply", action="store_true",
                   help="also record into BossSkillStore and overlay the name tables")
    p.add_argument("--name-filter", default="机骸,悖与灾",
                   help="comma-separated CN substrings for the monster-name fallback")
    p.add_argument("--out", default=None, help="exports JSON path")
    args = p.parse_args(argv)

    from mem_probe.il2cpp.static_dps_source import StaticDpsSource
    src = StaticDpsSource()
    _ = src.sr   # lazy open: bundle + process handle
    en = RaidSkillEnumerator(src)
    en.prepare()

    exact, family = en.family_rows(args.dungeon)
    offline_name = _dungeon_offline_name(args.dungeon)
    print(f"\n[enum] dungeon {args.dungeon} {offline_name!r}: {len(exact)} exact "
          f"row(s), {len(family)} GroupId-family difficulty row(s)")
    for r in family:
        mark = "*" if r["dungeon_id"] == args.dungeon else " "
        ids = r["boss_ids"]
        print(f" {mark}DungeonId={r['dungeon_id']:<6} Difficult={r['difficulty']:<2} "
              f"GroupId={r['group_id']:<3} Name={r['name']!r} "
              f"BossId({len(ids)})={ids}")
        if r["desc"]:
            print(f"    desc: {r['desc'][:60]}")
    if not family:
        print("[enum] no RaidDungeon rows matched this DungeonId — nothing to do")
        return 2
    if args.list_difficulties:
        return 0

    if args.difficulty is None:
        print("[enum] pass --difficulty N (see the list above) or --list-difficulties")
        return 2
    sel = [r for r in family if r["difficulty"] == args.difficulty]
    if not sel:
        print(f"[enum] no family row with Difficult={args.difficulty}")
        return 2
    if len(sel) > 1:
        # prefer the requested DungeonId when several family rows share Difficult
        sel.sort(key=lambda r: 0 if r["dungeon_id"] == args.dungeon else 1)
        print(f"[enum] {len(sel)} family rows share Difficult={args.difficulty}; "
              f"using DungeonId={sel[0]['dungeon_id']}")
    row = sel[0]
    boss_ids = list(row["boss_ids"])
    print(f"\n[enum] selected DungeonId={row['dungeon_id']} "
          f"Difficult={row['difficulty']} Name={row['name']!r} BossId={boss_ids}")

    terms = [t.strip() for t in (args.name_filter or "").split(",") if t.strip()]
    phases: List[Tuple[int, int]] = []           # (phase_no, boss_id)
    if args.all_phases:
        phases = [(i + 1, b) for i, b in enumerate(boss_ids)]
    else:
        if 1 <= args.phase <= len(boss_ids):
            phases = [(args.phase, boss_ids[args.phase - 1])]
        else:
            print(f"[enum] BossId has {len(boss_ids)} entries — phase {args.phase} "
                  f"missing; engaging fallbacks (A) name-filter (B) GroupId siblings")
            en.fallbacks_used.append("phase_index_out_of_range")
            cands = en.monsters_by_name(terms) if terms else []
            if cands:
                print(f"  (A) monster-name candidates: "
                      + ", ".join(f"{c['id']}={c['name']}" for c in cands))
            for s in family:
                if s["_blob"] != row["_blob"]:
                    print(f"  (B) sibling DungeonId={s['dungeon_id']} "
                          f"Difficult={s['difficulty']} BossId={s['boss_ids']}")
            phases = [(i + 1, b) for i, b in enumerate(boss_ids)]   # report what exists

    def _skill_entry(sid: int) -> Dict:
        nm, nm_src = en.skill_name(sid)
        return {"id": sid, "name": nm, "name_src": nm_src}

    result: Dict = {
        "dungeon_id": row["dungeon_id"],         # the SELECTED difficulty row's id
        "requested_dungeon_id": args.dungeon,
        "dungeon_name": offline_name or _dungeon_offline_name(row["dungeon_id"]),
        "difficulty": row["difficulty"],
        "difficulty_name": row["name"],
        "group_id": row["group_id"],
        "boss_ids": boss_ids,
        "generated_at": int(time.time()),
        "phases": [],
        "fallbacks_used": en.fallbacks_used,
        "notes": en.notes,
    }

    for phase_no, boss_id in phases:
        info = en.monster_info(boss_id)
        if not info:
            print(f"\n[P{phase_no}] boss_id={boss_id}: NO MonsterTable row found")
            en.notes.append(f"monster_row_missing:{boss_id}")
            result["phases"].append({"phase": phase_no, "boss_id": boss_id,
                                     "boss_name": "", "skills": []})
            continue
        skills = [_skill_entry(sid) for sid in info["skill_ids"]]
        born = None
        bsid = int(info.get("born_skill_id") or 0)
        if bsid > 0:
            born = _skill_entry(bsid)
        print(f"\n[P{phase_no}] boss {boss_id} {info['name']!r} "
              f"skills={len(skills)} born={born and born['id']}")
        for sk in skills:
            tag = "" if sk["name_src"] == "mlstring" else f"  [{sk['name_src']}]"
            print(f"    {sk['id']:<12} {sk['name']}{tag}")
        if born:
            print(f"    {born['id']:<12} {born['name']}  (BornSkillId)")
        ph = {"phase": phase_no, "boss_id": boss_id, "boss_name": info["name"],
              "skills": skills}
        if born:
            ph["born_skill"] = born
        result["phases"].append(ph)

    # cross-check: does any enumerated boss name hit the expected filter?
    if terms and result["phases"]:
        hit = any(any(t in (ph.get("boss_name") or "") for t in terms)
                  for ph in result["phases"])
        if not hit:
            print(f"\n[enum] WARNING: no enumerated boss name matches "
                  f"{terms} — BossId semantics may differ; check --all-phases output")
            en.notes.append("name_filter_no_match")
            cands = en.monsters_by_name(terms)
            if cands:
                print("  monster-name candidates: "
                      + ", ".join(f"{c['id']}={c['name']}" for c in cands))
                result["name_filter_candidates"] = cands

    out_path = args.out or os.path.join(_ROOT, "exports", "boss_raids",
                                        f"raid_{args.dungeon}_skills.json")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"\n[enum] exported -> {out_path}")

    if args.apply:
        result["apply_report"] = _apply(result)
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
            f.write("\n")
    else:
        print("[enum] dry-run: BossSkillStore / name-table overlay NOT written "
              "(--apply to persist)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
