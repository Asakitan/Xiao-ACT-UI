"""Hard unpack — direct value-cluster discovery (no dump.cs, no TCP truth).

Given a set of KNOWN field values (from in-game UI screenshots), scan the
process heap for every value, then look for spatial clusters: locations
where many distinct values cluster within a small window (0x400 bytes).

A high-cluster region IS a struct holding those fields. Walk back to find
the IL2CPP klass_ptr, persist as an anchor.

This is the most reliable algorithm for IL2CPP games where standard
walk-back via UUID/HP fails (e.g. when the entity uses a Dictionary or
ECS-style component layout).

Heavy compute paths: cy_memscan.find_aligned_u32 / find_aligned_u64 (Cython AVX2).

Usage:
    python -m mem_probe.hard_unpack
    python -m mem_probe.hard_unpack --output cluster_report.json
"""
from __future__ import annotations

import argparse
import bisect
import json
import os
import struct
import sys
import threading
import time
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from mem_probe import cy_memscan as _cy

MAX_REGION_SIZE = 256 * 1024 * 1024
CLUSTER_WINDOW = 0x400          # 1KB window for clustering
MIN_DISTINCT_VALUES = 3          # require ≥N distinct values in window


# ───────── Hardcoded ground truth (主人 provided UI data) ─────────

PLAYER_STATS = {
    # Big-int stats (highly unique)
    "max_hp":        535339,    # 生命上限
    "fight_point":   54452,     # 能力评分
    "endurance":     34539,     # 耐力 (hard 5-digit)
    "strength":      6032,      # 力量
    "phys_atk":      4656,      # 物理攻击
    "break_power":   3302,      # 破妄强度
    # Smaller (less unique, still useful)
    "level_base":    60,
    "level_extra":   74,
    # Percentages — try as f32 (most likely encoding) AND as scaled int
    # (e.g. 2095 for 20.95%, or 0.2095f)
    "crit_pct_f32":  0.2095,    # 暴击
    "haste_pct_f32": 0.4198,    # 急速
    "luck_pct_f32":  0.229,     # 幸运
    "mastery_pct_f32": 0.4314,  # 精通
    "all_pct_f32":   0.6004,    # 全能
    # Or as basis points (×10000)
    "crit_bps":      2095,
    "haste_bps":     4198,
    "luck_bps":      2290,
    "mastery_bps":   4314,
    "all_bps":       6004,
}

WEAPON_STATS = {
    "name_tag":      "不朽守望之誓",
    "wear_lv":       60,
    "ilvl":          250,
    "perfection":    100,
    # base stats
    "break_power":   252,
    "phys_atk":      256,
    "strength":      456,
    "endurance":     2052,
    # advanced
    "haste":         5400,
    "mastery":       5400,
    # rare
    "elem_bonus_pct_f32": 0.08,
    "elem_bonus_bps":     800,
}

HELMET_STATS = {
    "name_tag":      "[极]机遇破魇面盔",
    "wear_lv":       60,
    "reforge":       33,
    "ilvl":          240,
    "perfection":    100,
    # base
    "break_power":   201,
    "phys_def":      1260,
    "strength":      432,
    "endurance":     1944,
    # advanced
    "luck":          2370,
    "haste":         1185,
    "mastery":       711,
    # rare
    "phys_def_extra": 320,
}


# ───────── Helpers ─────────

def _hex(v: int) -> str:
    return f"0x{int(v):X}"


def _f32_bits(v) -> int:
    return struct.unpack("<I", struct.pack("<f", float(v)))[0]


def _f64_bits(v) -> int:
    return struct.unpack("<Q", struct.pack("<d", float(v)))[0]


def _ga_module_range(pm) -> Tuple[int, int]:
    for m in pm.list_modules():
        if m.name.lower() == "gameassembly.dll":
            return m.base, m.base + m.size
    return 0, 0


def _build_modules_predicate(pm):
    modules = pm.list_modules()
    mod_ranges = sorted([(m.base, m.base + m.size) for m in modules])
    mod_bases = [r[0] for r in mod_ranges]

    def in_module(addr: int) -> bool:
        i = bisect.bisect_right(mod_bases, addr) - 1
        if i < 0:
            return False
        base, end = mod_ranges[i]
        return base <= addr < end

    return in_module


# ───────── Stage 1: scan all known values ─────────

@dataclass
class Hit:
    addr: int
    field_name: str
    value: object         # int or float
    encoding: str         # 'i32' | 'i64' | 'f32' | 'f64'


def _build_needles(stats_list: List[Tuple[str, Dict]]) -> Tuple[
        Dict[int, List[Tuple[str, str, object]]],
        Dict[int, List[Tuple[str, str, object]]]]:
    """Return (u32_needles, u64_needles) where each maps bit_pattern → list of
    (group_name.field_name, encoding, original_value)."""
    u32_needles: Dict[int, List] = defaultdict(list)
    u64_needles: Dict[int, List] = defaultdict(list)
    for group_name, stats in stats_list:
        for k, v in stats.items():
            if not isinstance(v, (int, float)) or v == 0:
                continue
            full = f"{group_name}.{k}"
            if isinstance(v, int):
                # i32
                if 0 < v < (1 << 32):
                    u32_needles[v & 0xFFFFFFFF].append((full, "i32", v))
                # i64
                if 0 < v < (1 << 63):
                    u64_needles[v & 0xFFFFFFFFFFFFFFFF].append((full, "i64", v))
            if isinstance(v, float):
                # f32
                f32 = _f32_bits(v)
                u32_needles[f32].append((full, "f32", v))
                # f64
                f64 = _f64_bits(v)
                u64_needles[f64].append((full, "f64", v))
            # For ints, also try as f32 (in case stat is stored as float
            # whose value happens to be an integer like 5400.0)
            if isinstance(v, int) and v > 0 and v < (1 << 24):
                f32 = _f32_bits(v)
                if f32 != (v & 0xFFFFFFFF):
                    u32_needles[f32].append((full, "f32_of_int", v))
    return u32_needles, u64_needles


def scan_all_values(pm, u32_needles: Dict[int, List],
                    u64_needles: Dict[int, List],
                    *, n_workers: int = 8,
                    skip_module_pages: bool = True,
                    in_module_pred=None) -> List[Hit]:
    """Run Cython AVX2 scans for every needle across the heap.

    Returns flat Hit list. May contain many hits per needle (small values like
    100, 60 hit thousands of times) — clustering stage filters those.
    """
    regions = [r for r in pm.iter_regions(only_readable=True, only_private=True)
               if r.size <= MAX_REGION_SIZE]
    hits: List[Hit] = []
    lock = threading.Lock()

    u32_list = list(u32_needles.keys())
    u64_list = list(u64_needles.keys())

    def scan_one(region):
        # skip pages that lie in any loaded module — those are code/static and
        # never the per-character struct
        if skip_module_pages and in_module_pred is not None:
            if in_module_pred(region.base):
                return
        buf = pm.read_bytes(region.base, region.size)
        if buf is None:
            return
        local: List[Hit] = []
        # u64 multi-needle (single AVX2 call for all)
        if u64_list:
            r64 = _cy.find_aligned_u64_in_set(buf, u64_list)
            for off, val in r64:
                for full, enc, orig in u64_needles[int(val)]:
                    local.append(Hit(addr=region.base + off,
                                     field_name=full, value=orig, encoding=enc))
        # u32 single per needle
        for n in u32_list:
            r32 = _cy.find_aligned_u32(buf, n)
            for off in r32:
                for full, enc, orig in u32_needles[n]:
                    local.append(Hit(addr=region.base + off,
                                     field_name=full, value=orig, encoding=enc))
        if local:
            with lock:
                hits.extend(local)

    if n_workers > 1:
        with ThreadPoolExecutor(max_workers=n_workers) as ex:
            list(ex.map(scan_one, regions))
    else:
        for r in regions:
            scan_one(r)
    return hits


# ───────── Stage 2: cluster hits by spatial proximity ─────────

@dataclass
class Cluster:
    center_addr: int
    span_lo: int
    span_hi: int
    hits: List[Hit] = field(default_factory=list)

    @property
    def n_distinct_fields(self) -> int:
        return len({h.field_name for h in self.hits})

    @property
    def n_distinct_values(self) -> int:
        return len({(h.field_name.split(".", 1)[-1], h.value) for h in self.hits})


def cluster_hits(hits: List[Hit], *, window: int = CLUSTER_WINDOW,
                 min_distinct: int = MIN_DISTINCT_VALUES) -> List[Cluster]:
    """Sort hits by address, sweep windows, group hits within `window` bytes
    where ≥ min_distinct distinct field names co-occur.
    """
    hits_sorted = sorted(hits, key=lambda h: h.addr)
    clusters: List[Cluster] = []
    i = 0
    n = len(hits_sorted)
    while i < n:
        j = i
        while j < n and hits_sorted[j].addr - hits_sorted[i].addr <= window:
            j += 1
        window_hits = hits_sorted[i:j]
        distinct = {h.field_name for h in window_hits}
        if len(distinct) >= min_distinct:
            clusters.append(Cluster(
                center_addr=window_hits[0].addr,
                span_lo=window_hits[0].addr,
                span_hi=window_hits[-1].addr,
                hits=window_hits,
            ))
            # Advance past this window to avoid double-counting
            i = j
        else:
            i += 1
    # Score and sort
    clusters.sort(key=lambda c: -c.n_distinct_fields)
    return clusters


# ───────── Stage 3: walk back to klass_ptr ─────────

def walk_back_to_klass(pm, addr: int, ga_base: int, ga_end: int, in_module,
                       *, max_walk: int = 0x40000) -> List[Tuple[int, int]]:
    """Find candidate (obj_base, klass_ptr) by walking back from addr.

    Reads a single contiguous block (one RPM), then walks the buffer in
    8-byte steps. Default max_walk 0x40000 (256KB) covers very deep
    nested structures (e.g. equipment in a List<List<Item>>).
    """
    results: List[Tuple[int, int]] = []
    aligned = addr - (addr % 8)
    region_start = max(0x10000, aligned - max_walk)
    region_size = aligned - region_start
    if region_size < 8:
        return results
    buf = pm.read_bytes(region_start, region_size)
    if not buf:
        return results
    n_slots = len(buf) // 8
    for i in range(n_slots):
        kp_bytes = buf[i * 8:i * 8 + 8]
        kp = int.from_bytes(kp_bytes, "little")
        if ga_end:
            if not (ga_base <= kp < ga_end):
                continue
        else:
            if not in_module(kp):
                continue
        ob = region_start + i * 8
        results.append((ob, kp))
    results.sort(key=lambda r: abs(r[0] - addr))
    return results[:8]


def diagnose_cluster_neighborhood(pm, addr: int, in_module,
                                  *, before: int = 0x100,
                                  after: int = 0x40) -> Dict:
    """Dump nearby u64 values: anything that looks like a user-space pointer
    or sits in a known module. Helps identify struct boundaries when no
    GA-klass is found via walk_back.
    """
    aligned = addr - (addr % 8)
    region_start = max(0x10000, aligned - before)
    region_end = aligned + after
    size = region_end - region_start
    buf = pm.read_bytes(region_start, size)
    if not buf:
        return {"error": "read failed"}
    user_ptrs: List[Tuple[int, int, str]] = []  # (rel_off, value, type)
    n_slots = len(buf) // 8
    for i in range(n_slots):
        v = int.from_bytes(buf[i * 8:i * 8 + 8], "little")
        if not (0x10000 <= v <= 0x7FFFFFFFFFFF):
            continue
        rel = (region_start + i * 8) - addr
        kind = "module" if in_module(v) else "heap"
        user_ptrs.append((rel, v, kind))
    return {
        "addr": _hex(addr),
        "scanned": [f"{_hex(region_start)}..{_hex(region_end)}"],
        "n_user_ptrs": len(user_ptrs),
        "ptrs": [
            {"rel_off": r, "value": _hex(v), "kind": k}
            for r, v, k in user_ptrs[:24]
        ],
    }


# ───────── Stage 4: full pipeline ─────────

def run_hard_unpack(pm, *,
                    player_stats: Optional[Dict] = None,
                    weapon_stats: Optional[Dict] = None,
                    helmet_stats: Optional[Dict] = None,
                    verbose: bool = True) -> Dict:
    if player_stats is None:
        player_stats = PLAYER_STATS
    if weapon_stats is None:
        weapon_stats = WEAPON_STATS
    if helmet_stats is None:
        helmet_stats = HELMET_STATS

    t0 = time.time()
    in_module = _build_modules_predicate(pm)
    ga_base, ga_end = _ga_module_range(pm)
    if verbose:
        print(f"[hard-unpack] GA: 0x{ga_base:X}–0x{ga_end:X}")

    groups = [("player", player_stats),
              ("weapon", weapon_stats),
              ("helmet", helmet_stats)]
    # Drop string fields (name_tag) — strings need different handling
    groups = [(g, {k: v for k, v in s.items() if isinstance(v, (int, float))})
              for g, s in groups]

    u32_needles, u64_needles = _build_needles(groups)
    if verbose:
        print(f"[hard-unpack] needles: {len(u32_needles)} u32, "
              f"{len(u64_needles)} u64 patterns")

    t_scan = time.time()
    hits = scan_all_values(pm, u32_needles, u64_needles,
                           in_module_pred=in_module)
    if verbose:
        print(f"[hard-unpack] scan: {len(hits)} total hits ({time.time()-t_scan:.2f}s)")

    # Per-group clustering
    out: Dict[str, dict] = {"groups": {}}
    for group_name, _stats in groups:
        group_hits = [h for h in hits if h.field_name.startswith(f"{group_name}.")]
        clusters = cluster_hits(group_hits)
        if verbose:
            print(f"\n[hard-unpack] {group_name}: {len(group_hits)} hits → "
                  f"{len(clusters)} clusters")
        # Report top clusters
        cluster_reports = []
        for ci, c in enumerate(clusters[:6]):
            kc = walk_back_to_klass(pm, c.span_lo, ga_base, ga_end, in_module)
            # Always probe neighborhood to see ptr structure (even if no GA-klass)
            neighborhood = diagnose_cluster_neighborhood(pm, c.span_lo, in_module)
            klass_str = (f"klass=0x{kc[0][1]:X} obj=0x{kc[0][0]:X} "
                         f"(walk_back=0x{c.span_lo - kc[0][0]:X})"
                         if kc else f"klass=? (no GA-ptr in 64KB before; "
                                    f"{neighborhood['n_user_ptrs']} user-ptrs nearby)")
            if verbose:
                print(f"  cluster #{ci}: span=0x{c.span_lo:X}..0x{c.span_hi:X} "
                      f"({c.n_distinct_fields} fields, {len(c.hits)} hits)  {klass_str}")
                for h in sorted(c.hits, key=lambda x: x.addr)[:20]:
                    rel = h.addr - (kc[0][0] if kc else c.span_lo)
                    print(f"     +0x{rel:04X}  {h.field_name:<24} = {h.value!r:<10} ({h.encoding})")
                # If no klass found, show pointers near cluster — helps find parent
                if not kc and ci < 3:
                    print(f"     -- nearby user-space pointers (rel to span_lo):")
                    for p in neighborhood["ptrs"][:8]:
                        print(f"        rel={p['rel_off']:+5d}  {p['value']}  ({p['kind']})")
            cluster_reports.append({
                "span_lo": _hex(c.span_lo),
                "span_hi": _hex(c.span_hi),
                "n_distinct_fields": c.n_distinct_fields,
                "n_hits": len(c.hits),
                "owner_candidates": [
                    {"obj_addr": _hex(ob), "klass_ptr": _hex(kp),
                     "walk_back": c.span_lo - ob}
                    for ob, kp in kc[:3]
                ],
                "neighborhood_ptrs": neighborhood,
                "fields": [
                    {
                        "name": h.field_name,
                        "addr": _hex(h.addr),
                        "rel_off": (h.addr - kc[0][0]) if kc else (h.addr - c.span_lo),
                        "value": h.value if isinstance(h.value, int) else float(h.value),
                        "encoding": h.encoding,
                    }
                    for h in sorted(c.hits, key=lambda x: x.addr)[:32]
                ],
            })
        # Cross-cluster owner convergence: an obj_addr that owns ≥2 clusters
        # is almost certainly the real parent struct.
        owner_votes: Counter = Counter()
        owner_klass: Dict[int, int] = {}
        for cr in cluster_reports:
            for oc in cr["owner_candidates"]:
                ob = int(oc["obj_addr"], 16)
                kp = int(oc["klass_ptr"], 16)
                owner_votes[ob] += 1
                owner_klass[ob] = kp
        convergent: List[dict] = []
        for ob, n in owner_votes.most_common(8):
            if n >= 1:  # report all
                convergent.append({
                    "obj_addr": _hex(ob),
                    "klass_ptr": _hex(owner_klass[ob]),
                    "n_clusters": n,
                })

        if verbose and convergent:
            print(f"  [convergence] obj_addrs that own ≥1 cluster:")
            for c in convergent[:5]:
                print(f"     {c['obj_addr']}  klass={c['klass_ptr']}  "
                      f"clusters={c['n_clusters']}")

        out["groups"][group_name] = {
            "n_hits": len(group_hits),
            "clusters": cluster_reports,
            "convergent_owners": convergent,
        }

    # ── Auto-persist the strongest player owner to anchors.json ──
    player_owners = out["groups"].get("player", {}).get("convergent_owners", [])
    if player_owners and player_owners[0]["n_clusters"] >= 2:
        best = player_owners[0]
        anchors_path = os.path.join(_HERE, "anchors.json")
        try:
            with open(anchors_path, "r", encoding="utf-8") as f:
                anchors = json.load(f)
        except (OSError, json.JSONDecodeError):
            anchors = {}
        sl = anchors.setdefault("smart_locator", {})
        nested = sl.setdefault("anchors", {})
        nested["player_runtime"] = {
            "obj_addr": best["obj_addr"],
            "klass_ptr": best["klass_ptr"],
            "n_clusters_supporting": best["n_clusters"],
            "discovered_via": "hard_unpack_value_cluster",
            "discovered_at": time.time(),
            "field_clusters": [
                {
                    "span_lo": cr["span_lo"],
                    "n_distinct_fields": cr["n_distinct_fields"],
                    "rel_off_to_owner": (int(cr["span_lo"], 16) - int(best["obj_addr"], 16)),
                    "fields": [
                        {"name": f["name"], "rel_off_owner": (int(f["addr"], 16) - int(best["obj_addr"], 16)),
                         "value": f["value"], "encoding": f["encoding"]}
                        for f in cr["fields"][:16]
                    ],
                }
                for cr in out["groups"]["player"]["clusters"][:5]
                if any(int(oc["obj_addr"], 16) == int(best["obj_addr"], 16)
                       for oc in cr["owner_candidates"])
            ],
        }
        sl["schema_version"] = max(2, int(sl.get("schema_version", 0) or 0))
        sl["player_runtime_set_at"] = time.time()
        with open(anchors_path, "w", encoding="utf-8") as f:
            json.dump(anchors, f, ensure_ascii=False, indent=2)
        if verbose:
            print(f"\n[hard-unpack] PERSISTED player_runtime anchor → {anchors_path}")
            print(f"   obj_addr  = {best['obj_addr']}")
            print(f"   klass_ptr = {best['klass_ptr']}")
            print(f"   supporting clusters = {best['n_clusters']}")

    out["elapsed_s"] = time.time() - t0
    out["pid"] = pm.pid
    return out


# ───────── CLI ─────────

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Hard-unpack: value-cluster IL2CPP discovery")
    ap.add_argument("--output", default=os.path.join(_HERE, "hard_unpack_report.json"),
                    help="JSON report output path")
    ap.add_argument("--verbose", action="store_true", default=True)
    args = ap.parse_args(argv)

    print("[hard-unpack] attaching to Star.exe ...")
    from mem_probe.locator import SmartLocator
    sl = SmartLocator()
    pm = sl.pm
    print(f"[hard-unpack] attached pid={pm.pid}")

    report = run_hard_unpack(pm, verbose=args.verbose)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2, default=str)
    print(f"\n[hard-unpack] full report → {args.output}")
    print(f"[hard-unpack] total elapsed: {report['elapsed_s']:.2f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
