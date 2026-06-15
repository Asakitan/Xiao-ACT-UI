# -*- coding: utf-8 -*-
"""enum_boss_buffs - reverse-map a boss's skills to the buff ids those skills cast.

Runtime buff detection keys on BuffItem.BuffBaseId (the buff id a boss carries),
which is NOT the same number as the skill id in SkillTable. The bridge between
them is the BuffTable's own ``SkillId`` column: every buff row records which
skill spawned it. This walks the full BuffTable straight from memory (read-only),
filters rows whose SkillId belongs to the target boss, and aggregates per skill.

Chain:
  MonsterTable   boss row by Id -> SkillIds[] (skill ids 10330001..10330014),
                 BornClientBuffs[] / DeadClientBuffs[] (self-cast buff ids)
  BuffTable      every row -> Id, Name(mlstring)/NameDesign(string), SkillId,
                 Duration*; rows whose SkillId is in the boss skill set are the
                 buffs that boss's skills apply
  SkillTable     skill id -> Name (label for the aggregation header)
  StringPool     mlid -> localized CN string

Column offsets come from table_columns (getter-thunk extraction with versioned
cache + curated literal fallback + live self-check). Class field offsets resolve
through auto_offsets inside MemConfigTableReader. Nothing is hardcoded.

Usage:
  python -m tools.enum_boss_buffs --boss 103309 --dry-run
  python -m tools.enum_boss_buffs --boss 103309 --apply     # last, persists names

--dry-run (default) prints every skill's reverse-mapped buff ids + names and
writes the exports JSON. --apply additionally overlays the buff names onto the
runtime name tables (id_space "buff_id").
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
from mem_probe.il2cpp.mem_string_pool import StringPoolBridge

BUFF_CLS = TABLE_CLASS["buff"]
MONSTER_CLS = TABLE_CLASS["monster"]
SKILL_CLS = TABLE_CLASS["skill"]

# P3 raid boss "悖与灾的机骸·终": SkillIds 10330001..10330014. Mechanic skills
# usually have an empty localized Name, so the designer label is the real name.
DEFAULT_BOSS_ID = 103309
DEFAULT_SKILL_IDS = list(range(10330001, 10330015))

# Mechanic keywords for the name-substring fallback (engaged when SkillId
# reverse-lookup finds nothing): each maps to a boss mechanic the user tracks.
MECHANIC_KEYWORDS = [
    "横扫", "半场", "分摊", "分散", "扩散", "衰减", "死刑", "宣告", "刑",
    "弹球", "压团", "AOE", "aoe", "神之", "刻度", "归途", "因果", "折跃",
    "天穹", "律动", "终焉", "交响", "放逐", "连结", "试炼", "传送", "转",
    "魔方", "炎戒", "归零", "悖", "灾", "机骸",
]

_NAME_TABLES = os.path.join(_ROOT, "assets", "name_tables")
_OVERLAY_CACHE = os.path.join(_NAME_TABLES, "live_boss_buff_name_cache.json")


def _utf8_stdout() -> None:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass


def _strip_tags(s: str) -> str:
    return re.sub(r"<[^>]*>", "", s or "").strip()


class BossBuffEnumerator:
    """Read-only reverse mapping of boss skills to the buffs those skills cast."""

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

    # -- setup ----------------------------------------------------------------
    def prepare(self) -> bool:
        t0 = time.time()
        if not self.pool.build():
            self.log("[enum] string pool NOT located - names will be empty")
            self.notes.append("string_pool_unavailable")
        else:
            self.log(f"[enum] string pool ready ({time.time()-t0:.1f}s)")
        classes = [BUFF_CLS, MONSTER_CLS, SKILL_CLS]
        self.columns = table_columns.load_columns(classes, log=self.log)
        # Validate the extracted buff columns with a targeted live probe instead
        # of the generic self_check downgrade: the curated literal fallback only
        # carries Id+Name and would DROP the SkillId column this tool needs, so a
        # blanket downgrade is destructive here. The getter-thunk extraction is
        # authoritative; we only confirm the id column and the name column both
        # decode sanely (at least one CJK buff name) on the live loader walk.
        cols = self.columns[BUFF_CLS]
        if not self._validate_buff_columns(cols):
            self.log(f"[enum] {BUFF_CLS}: extracted columns did not validate on "
                     f"live rows - results may be unreliable")
            self.notes.append("buff_columns_unverified")
        return True

    def _validate_buff_columns(self, cols: Dict[str, Tuple[int, str]],
                               sample: int = 400) -> bool:
        """Confirm Id(col0,i32)>0 and at least one buff Name/NameDesign resolves
        to a CJK string over a live sample. Returns False only when the buff
        table itself reads as garbage (column extraction badly wrong)."""
        c_id = cols.get("Id", (0, ""))[0]
        c_name = cols.get("Name", (None, ""))[0]
        c_nd = cols.get("NameDesign", (None, ""))[0]
        n = id_ok = cjk = 0
        try:
            for _key, zl, blob in self.rd.iter_rows_via_loader(BUFF_CLS):
                n += 1
                v = self.rd.col_i32(blob, c_id)
                if v is not None and v > 0:
                    id_ok += 1
                if not cjk:
                    nm = self.pool.resolve(self.rd.col_mlid(blob, c_name)) if c_name is not None else ""
                    if not nm and c_nd is not None:
                        nm = self.rd.col_string(zl, blob, c_nd)
                    if nm and any("一" <= ch <= "鿿" for ch in nm):
                        cjk = 1
                if n >= sample:
                    break
        except Exception:
            return False
        return n > 0 and (id_ok / n) >= 0.8 and bool(cjk)

    def _col(self, cls: str, prop: str) -> Optional[int]:
        ent = self.columns.get(cls, {}).get(prop)
        return ent[0] if ent else None

    def column_report(self) -> Dict[str, Dict[str, List]]:
        """For the final report: each used column's (offset, type) + source."""
        out: Dict[str, Dict[str, List]] = {}
        lit = "buff_columns_literal" in self.fallbacks_used
        for cls, props in (
            (BUFF_CLS, ("Id", "Name", "NameDesign", "SkillId", "Duration")),
            (MONSTER_CLS, ("Id", "Name", "SkillIds",
                           "BornClientBuffs", "DeadClientBuffs")),
        ):
            block: Dict[str, List] = {}
            for p in props:
                ent = self.columns.get(cls, {}).get(p)
                if ent:
                    src = "literal" if (cls == BUFF_CLS and lit) else "table_columns"
                    block[p] = [f"0x{ent[0]:X}", ent[1], src]
                else:
                    block[p] = [None, None, "absent"]
            out[cls] = block
        return out

    # -- monster / skill lookups ----------------------------------------------
    def _build_index(self, cls: str) -> Dict[int, Tuple[int, int]]:
        c_id = self._col(cls, "Id")
        out: Dict[int, Tuple[int, int]] = {}
        for _key, zl, blob in self.rd.iter_rows_via_loader(cls):
            rid = self.rd.col_i32(blob, c_id)
            if rid is not None and rid > 0:
                out[rid] = (zl, blob)
        if not out:
            self.log(f"[enum] {cls}: loader walk empty - falling back to heap scan")
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

    def boss_info(self, boss_id: int) -> Optional[Dict]:
        """MonsterTable row for the boss: name, skill ids, self-cast buff ids."""
        ent = self.monster_index().get(int(boss_id))
        if not ent:
            return None
        zl, blob = ent
        info: Dict = {
            "id": int(boss_id),
            "name": self.pool.resolve(self.rd.col_mlid(blob, self._col(MONSTER_CLS, "Name"))),
            "skill_ids": self.rd.col_i32_array(zl, blob, self._col(MONSTER_CLS, "SkillIds")),
        }
        for prop in ("BornClientBuffs", "DeadClientBuffs"):
            c = self._col(MONSTER_CLS, prop)
            info[prop] = self.rd.col_i32_array(zl, blob, c) if c is not None else []
            if c is None:
                self.notes.append(f"monster_column_absent:{prop}")
        return info

    def skill_name(self, skill_id: int) -> Tuple[str, str]:
        """(name, source). Boss mechanic skills often have an empty localized
        Name; NameDesign (raw string column) is the real label then."""
        ent = self.skill_index().get(int(skill_id))
        if not ent:
            return "", "missing"
        zl, blob = ent
        nm = self.pool.resolve(self.rd.col_mlid(blob, self._col(SKILL_CLS, "Name")))
        if nm:
            return nm, "mlstring"
        nd = self.rd.col_string(zl, blob, self._col(SKILL_CLS, "NameDesign"))
        return (nd, "design") if nd else ("", "empty")

    # -- buff table -----------------------------------------------------------
    def _buff_name(self, zl: int, blob: int) -> Tuple[str, str]:
        """(name, source). Prefer the localized Name(mlstring); fall back to the
        raw NameDesign string column (mechanic buffs frequently have no Name)."""
        nm = self.pool.resolve(self.rd.col_mlid(blob, self._col(BUFF_CLS, "Name")))
        if nm:
            return nm, "mlstring"
        c_nd = self._col(BUFF_CLS, "NameDesign")
        nd = self.rd.col_string(zl, blob, c_nd) if c_nd is not None else ""
        return (nd, "design") if nd else ("", "empty")

    def scan_buffs(self) -> List[Dict]:
        """Full BuffTable walk -> list of {id, name, name_src, skill_id, dur?}.

        One pass over the loader's serialized rows (covers buffs the game never
        instantiated). The per-row buff name is resolved lazily by callers, so
        here we capture id + skill_id + the row handle for later naming."""
        c_id = self._col(BUFF_CLS, "Id")
        c_skill = self._col(BUFF_CLS, "SkillId")
        c_dur = self._col(BUFF_CLS, "Duration")  # absent in this schema -> None
        rows: List[Dict] = []
        t0 = time.time()
        for _key, zl, blob in self.rd.iter_rows_via_loader(BUFF_CLS):
            bid = self.rd.col_i32(blob, c_id)
            if bid is None or bid <= 0:
                continue
            sid = self.rd.col_i32(blob, c_skill) if c_skill is not None else None
            row = {"id": bid, "skill_id": sid, "_zl": zl, "_blob": blob}
            if c_dur is not None:
                row["duration"] = self.rd.col_i32(blob, c_dur)
            rows.append(row)
        if not rows:
            self.log(f"[enum] {BUFF_CLS}: loader walk empty - falling back to heap scan")
            self.fallbacks_used.append("buff_heap_scan_only")
            for _row, zl, blob in self.rd.iter_rows(BUFF_CLS):
                bid = self.rd.col_i32(blob, c_id)
                if bid is None or bid <= 0:
                    continue
                sid = self.rd.col_i32(blob, c_skill) if c_skill is not None else None
                row = {"id": bid, "skill_id": sid, "_zl": zl, "_blob": blob}
                if c_dur is not None:
                    row["duration"] = self.rd.col_i32(blob, c_dur)
                rows.append(row)
        self.log(f"[enum] buff table: {len(rows)} rows ({time.time()-t0:.1f}s)")
        return rows

    def name_buff_row(self, row: Dict) -> Dict:
        """Attach the resolved buff name to a scanned row (drops handles)."""
        nm, src = self._buff_name(row["_zl"], row["_blob"])
        out = {"id": row["id"], "name": nm, "name_src": src,
               "skill_id": row.get("skill_id")}
        if "duration" in row:
            out["duration_ms"] = row["duration"]
        return out

    def buff_id_cluster(self, named_rows: List[Dict], pivot_ids: List[int],
                        radius: int = 80) -> Tuple[int, int, List[Dict]]:
        """Boss mechanic buffs are authored as one contiguous id block. Given
        anchor buff ids (e.g. the boss's own self-research buffs), return the
        (lo, hi, rows) window of every named buff inside that block. Empty
        anchors -> empty window."""
        anchors = [b for b in pivot_ids if b and b > 0]
        if not anchors:
            return 0, 0, []
        lo, hi = min(anchors) - radius, max(anchors) + radius
        rows = [b for b in named_rows if lo <= b["id"] <= hi and (b.get("name") or "")]
        return lo, hi, sorted(rows, key=lambda b: b["id"])


# -- apply: name-table overlay ------------------------------------------------

def _apply(result: Dict, log=print) -> Dict:
    """Overlay every reverse-mapped buff name onto the runtime name tables.

    Same funnel as tools.tablekit.mem_name_ingest: buff ids go through the
    live-rows builder under id_space "buff_id" (the classifier refines the buff
    sub-kind), then overlay_cache_into_existing_tables writes <kind>.json."""
    report: Dict = {}
    name_rows: List[Dict] = []
    seen: set = set()

    def _add(b: Dict) -> None:
        bid = int(b.get("id") or 0)
        nm = b.get("name") or ""
        if bid <= 0 or not nm or bid in seen:
            return
        seen.add(bid)
        name_rows.append({"text": nm, "confidence": "mem",
                          "primary_match": {"id_space": "buff_id", "id": bid}})

    for sk in result.get("skills", []):
        for b in sk.get("buffs", []):
            _add(b)
    for b in result.get("born_buffs", []) + result.get("dead_buffs", []):
        _add(b)
    # the id-cluster carries the real per-mechanic buff names for raid bosses
    for b in (result.get("mechanic_cluster", {}) or {}).get("buffs", []):
        _add(b)

    from plugins.star_resonance_plugin.net.tcp_name_cache import build_index_from_live_rows, sanitize_shared_cache
    raw = build_index_from_live_rows(
        {"rows": name_rows}, confidence={"high", "medium", "mem", "tcp", "static"})
    index = sanitize_shared_cache(raw)
    os.makedirs(os.path.dirname(_OVERLAY_CACHE), exist_ok=True)
    with open(_OVERLAY_CACHE, "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")
    from tools.tablekit.hybrid_name_tables import overlay_cache_into_existing_tables
    ov = overlay_cache_into_existing_tables(_OVERLAY_CACHE, write=True)
    changed = {k: v["changed"] for k, v in ov["kinds"].items() if v.get("changed")}
    report["name_overlay"] = {"cache": _OVERLAY_CACHE, "rows": len(name_rows),
                              "changed": changed,
                              "total_changed": sum(changed.values())}
    log(f"[apply] name overlay: {changed or 'no changes'} "
        f"({len(name_rows)} buff names)")
    return report


# -- CLI ----------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    _utf8_stdout()
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--boss", type=int, default=DEFAULT_BOSS_ID)
    p.add_argument("--skill-ids", default=None,
                   help="comma-separated skill ids to reverse-map (default: the "
                        "boss's own MonsterTable.SkillIds, or 10330001..10330014)")
    p.add_argument("--dry-run", action="store_true",
                   help="print + export only (default)")
    p.add_argument("--apply", action="store_true",
                   help="also overlay the buff names onto the runtime name tables")
    p.add_argument("--out", default=None, help="exports JSON path")
    args = p.parse_args(argv)

    from mem_probe.il2cpp.static_dps_source import StaticDpsSource
    src = StaticDpsSource()
    _ = src.sr   # lazy open: bundle + process handle
    en = BossBuffEnumerator(src)
    en.prepare()

    # boss row -> its real skill id set (CLI override wins, else MonsterTable,
    # else the known P3 literal range)
    info = en.boss_info(args.boss)
    if info:
        print(f"\n[enum] boss {args.boss} {info['name']!r}: "
              f"{len(info['skill_ids'])} skill ids, "
              f"{len(info['BornClientBuffs'])} born buff(s), "
              f"{len(info['DeadClientBuffs'])} dead buff(s)")
    else:
        print(f"\n[enum] boss {args.boss}: NO MonsterTable row found "
              f"(using literal skill range)")
        en.notes.append(f"monster_row_missing:{args.boss}")

    if args.skill_ids:
        skill_ids = [int(x) for x in args.skill_ids.split(",") if x.strip()]
    elif info and info["skill_ids"]:
        skill_ids = list(info["skill_ids"])
    else:
        skill_ids = list(DEFAULT_SKILL_IDS)
    skill_set = set(skill_ids)
    print(f"[enum] reverse-mapping SkillId in {sorted(skill_set)}")

    # full BuffTable scan, then filter by SkillId membership
    buff_rows = en.scan_buffs()
    n_with_skill = sum(1 for r in buff_rows if (r.get("skill_id") or 0) > 0)
    print(f"[enum] {n_with_skill}/{len(buff_rows)} buff rows carry a SkillId > 0")

    by_skill: Dict[int, List[Dict]] = {sid: [] for sid in skill_ids}
    for r in buff_rows:
        sid = r.get("skill_id")
        if sid in skill_set:
            by_skill[sid].append(en.name_buff_row(r))

    # resolve every skill's name up front (header labels + name-equality bridge)
    skill_labels: Dict[int, Tuple[str, str]] = {sid: en.skill_name(sid) for sid in skill_ids}

    # bridge 2 (name equality): a buff whose Name equals a boss skill's Name is
    # that skill's effect, even when the SkillId back-ref column is empty.
    name_to_skill: Dict[str, int] = {}
    for sid, (sname, _src) in skill_labels.items():
        if sname:
            name_to_skill.setdefault(sname, sid)
    named_all: List[Dict] = [en.name_buff_row(r) for r in buff_rows]
    for b in named_all:
        sid = name_to_skill.get(b.get("name") or "")
        if sid is not None and b not in by_skill[sid]:
            entry = dict(b)
            entry["match"] = "name_eq"
            by_skill[sid].append(entry)

    skills_out: List[Dict] = []
    total_hits = 0
    for sid in skill_ids:
        sname, ssrc = skill_labels[sid]
        # de-dup buffs collected via SkillId back-ref + name equality
        seen_ids: set = set()
        buffs: List[Dict] = []
        for b in sorted(by_skill[sid], key=lambda b: b["id"]):
            if b["id"] in seen_ids:
                continue
            seen_ids.add(b["id"])
            buffs.append(b)
        total_hits += len(buffs)
        tag = "" if ssrc in ("mlstring", "design") else f"  [{ssrc}]"
        print(f"\n[skill {sid}] {sname}{tag}  -> {len(buffs)} buff(s)")
        for b in buffs:
            bt = "" if b["name_src"] == "mlstring" else f"  [{b['name_src']}]"
            mt = f"  ({b['match']})" if b.get("match") else ""
            dur = f"  dur={b['duration_ms']}" if "duration_ms" in b else ""
            print(f"    buff {b['id']:<10} {b['name']}{bt}{mt}{dur}")
        skills_out.append({
            "skill_id": sid, "skill_name": sname, "skill_name_src": ssrc,
            "buff_ids": [b["id"] for b in buffs], "buffs": buffs,
        })

    # boss self-cast buffs (BornClientBuffs / DeadClientBuffs) -> named
    born_buffs: List[Dict] = []
    dead_buffs: List[Dict] = []
    if info:
        buff_by_id = {r["id"]: r for r in buff_rows}
        for prop, dst in (("BornClientBuffs", born_buffs),
                          ("DeadClientBuffs", dead_buffs)):
            for bid in info.get(prop, []):
                row = buff_by_id.get(bid)
                if row:
                    dst.append(en.name_buff_row(row))
                else:
                    dst.append({"id": bid, "name": "", "name_src": "missing_row",
                                "skill_id": None})
        if born_buffs or dead_buffs:
            print(f"\n[enum] boss self-cast buffs: "
                  f"born={[b['id'] for b in born_buffs]} "
                  f"dead={[b['id'] for b in dead_buffs]}")
            for b in born_buffs + dead_buffs:
                print(f"    buff {b['id']:<10} {b['name']}  [{b['name_src']}]")

    # bridge 3 (id cluster): boss mechanic buffs are authored as one contiguous
    # id block. Anchor it on the boss's self-research buffs (Born/Dead client
    # buffs) and/or any name-equality hits, then dump every named buff inside
    # that window - this is the authoritative per-mechanic candidate table when
    # the SkillId back-ref is empty (it is, for raid mechanic buffs).
    anchors = ([b["id"] for b in born_buffs + dead_buffs if b.get("id")]
               + [b["id"] for sk in skills_out for b in sk["buffs"]
                  if b.get("match") == "name_eq"])
    cluster_lo, cluster_hi, cluster_rows = en.buff_id_cluster(named_all, anchors)
    if cluster_rows:
        print(f"\n[enum] mechanic buff id-cluster {cluster_lo}..{cluster_hi} "
              f"(anchored on {sorted(set(anchors))}): {len(cluster_rows)} named buff(s)")
        for b in cluster_rows:
            print(f"    buff {b['id']:<10} {b['name']}")

    # fallback: scan buff names for the user's mechanic keywords so there is a
    # candidate table even if the id cluster is empty. Always run it as a
    # cross-check (cheap); flagged in fallbacks_used only when it is the ONLY hit.
    keyword_candidates: List[Dict] = []
    for b in named_all:
        nm = b["name"] or ""
        hits = [k for k in MECHANIC_KEYWORDS if k and k in nm]
        if hits:
            kb = dict(b)
            kb["matched_keywords"] = sorted(set(hits))
            keyword_candidates.append(kb)
    keyword_candidates.sort(key=lambda b: b["id"])
    if total_hits == 0 and not cluster_rows:
        en.fallbacks_used.append("skillid_reverse_empty")
    if total_hits == 0:
        print(f"\n[enum] SkillId back-ref found 0 buffs for these skills; "
              f"keyword fallback: {len(keyword_candidates)} candidate buff(s)")
        for b in keyword_candidates[:200]:
            print(f"    buff {b['id']:<10} {b['name']}  kw={b['matched_keywords']}")

    result: Dict = {
        "boss_id": args.boss,
        "boss_name": (info or {}).get("name", ""),
        "skill_ids": skill_ids,
        "generated": int(time.time()),
        "column_offsets": en.column_report(),
        "skills": skills_out,
        "born_buffs": born_buffs,
        "dead_buffs": dead_buffs,
        "mechanic_cluster": {
            "id_lo": cluster_lo, "id_hi": cluster_hi,
            "anchors": sorted(set(anchors)),
            "buffs": cluster_rows,
        },
        "keyword_candidates": keyword_candidates,
        "fallbacks_used": en.fallbacks_used,
        "notes": en.notes,
        "summary": {
            "total_skill_buff_hits": total_hits,
            "buff_rows_scanned": len(buff_rows),
            "buff_rows_with_skillid": n_with_skill,
            "mechanic_cluster_size": len(cluster_rows),
            "keyword_candidate_size": len(keyword_candidates),
        },
    }

    out_path = args.out or os.path.join(_ROOT, "exports", "boss_raids",
                                        f"raid_{args.boss}_buffs.json")
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
        print("[enum] dry-run: name-table overlay NOT written (--apply to persist)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
