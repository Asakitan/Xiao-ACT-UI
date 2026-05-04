"""Per-monster value-cluster discovery — uses ALL TCP attrs (HP, extinction,
template_id, season_level, breaking_stage) as cluster anchors.

Premise: this game's IL2CPP klass table is anti-cheat-protected and entity
data lives in C++ Panda engine memory (not standard Il2CppClass instances).
Walking back from value clusters can't reliably ID a klass because objects
don't have IL2CPP class headers in heap.

Solution: per-monster absolute-address tracking. Each monster has a uuid;
that uuid is in memory at some location with adjacent fields (hp, extinction,
etc.). We use ALL TCP-known attrs as cluster anchors to disambiguate the
real monster object from coincidence hits, then read fields at relative
offsets each tick.

Heavy compute: cy_memscan AVX2 (find_aligned_u64_in_set, find_aligned_u32).
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import threading
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from mem_probe import cy_memscan as _cy
from mem_probe.locator import ANCHORS_PATH

MAX_REGION_SIZE = 256 * 1024 * 1024
CLUSTER_WINDOW = 0x800   # 2KB — wider than player; entities seem larger


@dataclass
class FullMonsterTruth:
    """All attrs we know from TCP for a single monster."""
    uuid: int                  # i64 unique
    hp: int = 0
    max_hp: int = 0
    template_id: int = 0       # AttrType.ID
    season_level: int = 0
    extinction: int = 0
    max_extinction: int = 0
    breaking_stage: int = 0
    stunned: int = 0
    max_stunned: int = 0
    name: str = ""

    def value_set_u32(self) -> List[Tuple[int, str]]:
        """Return list of (i32_bit_pattern, label) for all non-zero stats."""
        out = []
        for label, v in [
            ("hp", self.hp), ("max_hp", self.max_hp),
            ("template_id", self.template_id),
            ("season_level", self.season_level),
            ("extinction", self.extinction),
            ("max_extinction", self.max_extinction),
            ("breaking_stage", self.breaking_stage),
            ("stunned", self.stunned),
            ("max_stunned", self.max_stunned),
        ]:
            if v <= 0:
                continue
            # i32
            if v < (1 << 32):
                out.append((v & 0xFFFFFFFF, f"{label}_i32"))
            # f32 if value fits f32 precision
            if v < (1 << 24):
                f32 = struct.unpack("<I", struct.pack("<f", float(v)))[0]
                if f32 != (v & 0xFFFFFFFF):
                    out.append((f32, f"{label}_f32"))
        return out


def _hex(v: int) -> str:
    return f"0x{v:X}"


def _scan_per_monster_full(pm, truth: FullMonsterTruth) -> List[Tuple[int, str]]:
    """Scan heap for all bit patterns of one monster. Return [(addr, label), ...].
    Cython AVX2 multi-needle for u64; single-needle u32 per value.
    """
    u64_needles = [(truth.uuid & 0xFFFFFFFFFFFFFFFF, "uuid_u64")]
    u32_needles = truth.value_set_u32()
    if not u64_needles and not u32_needles:
        return []

    regions = [r for r in pm.iter_regions(only_readable=True, only_private=True)
               if r.size <= MAX_REGION_SIZE]
    out: List[Tuple[int, str]] = []
    lock = threading.Lock()

    def scan_one(region):
        buf = pm.read_bytes(region.base, region.size)
        if buf is None:
            return
        local: List[Tuple[int, str]] = []
        # u64 multi-needle
        if u64_needles:
            kind_map = {n: k for n, k in u64_needles}
            r64 = _cy.find_aligned_u64_in_set(buf, [n for n, _ in u64_needles])
            for off, val in r64:
                local.append((region.base + off, kind_map[int(val)]))
        # u32 single per needle, dedupe needles to avoid redundant scans
        seen_needles = set()
        for n, label in u32_needles:
            if n in seen_needles:
                continue
            seen_needles.add(n)
            r32 = _cy.find_aligned_u32(buf, n)
            # Map back to ALL labels matching this needle (e.g. hp == max_hp)
            matching = [lab for nv, lab in u32_needles if nv == n]
            for off in r32:
                for lab in matching:
                    local.append((region.base + off, lab))
        if local:
            with lock:
                out.extend(local)

    with ThreadPoolExecutor(max_workers=8) as ex:
        list(ex.map(scan_one, regions))
    return out


def _cluster_hits(hits: List[Tuple[int, str]],
                  *, window: int = CLUSTER_WINDOW,
                  min_distinct_kinds: int = 3) -> List[dict]:
    """Find clusters where ≥ min_distinct_kinds distinct labels co-occur."""
    hits_sorted = sorted(hits, key=lambda h: h[0])
    n = len(hits_sorted)
    clusters = []
    i = 0
    while i < n:
        j = i
        while j < n and hits_sorted[j][0] - hits_sorted[i][0] <= window:
            j += 1
        win = hits_sorted[i:j]
        labels = {lab for _, lab in win}
        if len(labels) >= min_distinct_kinds:
            clusters.append({
                "span_lo": win[0][0],
                "span_hi": win[-1][0],
                "labels": labels,
                "hits": win,
            })
            i = j
        else:
            i += 1
    return clusters


def discover_monster_per_uuid(pm, truths: List[FullMonsterTruth],
                              *, verbose: bool = True) -> List[dict]:
    """Per-monster absolute-address discovery.

    For each monster's truths:
      1. Cython scan all (uuid, hp, ..., extinction) bit patterns
      2. Find clusters where ≥3 distinct attrs co-occur in 2KB window
      3. The strongest cluster (most distinct labels) IS the monster's
         in-memory representation
      4. Compute relative offsets: hp_off = hp_addr - cluster_obj_base, etc.

    Returns list of dicts (one per monster) with discovered offsets.
    """
    results: List[dict] = []
    for truth in truths:
        t0 = time.time()
        hits = _scan_per_monster_full(pm, truth)
        clusters = _cluster_hits(hits, window=CLUSTER_WINDOW, min_distinct_kinds=3)
        if verbose:
            print(f"[per-monster] uuid={truth.uuid} hp={truth.hp} ext={truth.extinction}/{truth.max_extinction} "
                  f"tid={truth.template_id} → {len(hits)} hits, {len(clusters)} clusters "
                  f"({time.time()-t0:.2f}s)")
        if not clusters:
            results.append({"uuid": truth.uuid, "found": False})
            continue
        # Pick cluster with most distinct labels
        clusters.sort(key=lambda c: -len(c["labels"]))
        best = clusters[0]
        # uuid offset is the anchor — the obj_base IS the address where uuid lives
        # (we don't walk back to a klass header — there isn't one in this game)
        uuid_addr = None
        for addr, lab in best["hits"]:
            if lab == "uuid_u64":
                uuid_addr = addr
                break
        if uuid_addr is None:
            results.append({"uuid": truth.uuid, "found": False, "reason": "no uuid hit in best cluster"})
            continue
        # For each label, compute relative offset from uuid
        offsets: Dict[str, int] = {}
        for addr, lab in best["hits"]:
            if lab not in offsets:  # take first per label
                offsets[lab] = addr - uuid_addr
        if verbose:
            print(f"   WINNER cluster span={_hex(best['span_lo'])}..{_hex(best['span_hi'])} "
                  f"({len(best['labels'])} labels):")
            for lab in sorted(best["labels"]):
                rel = offsets.get(lab, "?")
                rel_str = f"{rel:+d}" if isinstance(rel, int) else "?"
                print(f"     {lab:<24} rel_to_uuid={rel_str}")
        results.append({
            "uuid": truth.uuid,
            "found": True,
            "uuid_addr": _hex(uuid_addr),
            "cluster_span": [_hex(best["span_lo"]), _hex(best["span_hi"])],
            "n_distinct_labels": len(best["labels"]),
            "offsets_from_uuid": offsets,
        })
    return results


def _aggregate_offsets(per_monster: List[dict]) -> Dict[str, int]:
    """Across multiple monsters, find offsets that AGREE across most monsters.

    A label whose rel_to_uuid is the same for ≥2 monsters is a real field.
    """
    votes: Dict[Tuple[str, int], int] = defaultdict(int)
    for entry in per_monster:
        if not entry.get("found"):
            continue
        for lab, off in entry.get("offsets_from_uuid", {}).items():
            votes[(lab, off)] += 1
    # Keep majority winner per label
    by_label: Dict[str, List[Tuple[int, int]]] = defaultdict(list)
    for (lab, off), n in votes.items():
        by_label[lab].append((off, n))
    out: Dict[str, int] = {}
    for lab, candidates in by_label.items():
        candidates.sort(key=lambda c: -c[1])
        if candidates[0][1] >= 2:
            out[lab] = candidates[0][0]
    return out


# ───────── TCP collector that captures ALL attrs ─────────

class _FullMonsterCollector:
    def __init__(self):
        self._lock = threading.Lock()
        self._by_uuid: Dict[int, FullMonsterTruth] = {}
        self._n = 0

    def __call__(self, payload: dict) -> None:
        try:
            uuid = int(payload.get("uuid") or 0)
            if uuid <= 0:
                return
            with self._lock:
                self._n += 1
                t = self._by_uuid.get(uuid)
                if t is None:
                    t = FullMonsterTruth(uuid=uuid)
                    self._by_uuid[uuid] = t
                # Update with whatever fields are present (max takes precedence)
                hp = int(payload.get("hp") or 0)
                if hp > 0 and hp != t.hp:
                    t.hp = hp
                mh = int(payload.get("max_hp") or 0)
                if mh > 0 and mh > t.max_hp:
                    t.max_hp = mh
                tid = int(payload.get("template_id") or 0)
                if tid > 0:
                    t.template_id = tid
                sl = int(payload.get("season_level") or 0)
                if sl > 0:
                    t.season_level = sl
                ext = int(payload.get("extinction") or 0)
                if ext > 0:
                    t.extinction = ext
                me = int(payload.get("max_extinction") or 0)
                if me > 0 and me > t.max_extinction:
                    t.max_extinction = me
                bs = int(payload.get("breaking_stage") or -1)
                if bs >= 0:
                    t.breaking_stage = bs
                stun = int(payload.get("stunned") or 0)
                if stun > 0:
                    t.stunned = stun
                mst = int(payload.get("max_stunned") or 0)
                if mst > 0 and mst > t.max_stunned:
                    t.max_stunned = mst
                nm = str(payload.get("name") or "")
                if nm:
                    t.name = nm
        except Exception:
            return

    def snapshot(self) -> List[FullMonsterTruth]:
        with self._lock:
            return list(self._by_uuid.values())

    def n_distinct(self) -> int:
        with self._lock:
            return len(self._by_uuid)

    def n_updates(self) -> int:
        with self._lock:
            return self._n


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Per-monster value-cluster discovery")
    ap.add_argument("--min-monsters", type=int, default=3)
    ap.add_argument("--collect-time", type=float, default=15.0,
                    help="Seconds to collect attrs from TCP after first monster appears")
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--anchors", default=ANCHORS_PATH)
    args = ap.parse_args(argv)

    from game_state import GameStateManager  # type: ignore
    from packet_bridge import PacketBridge  # type: ignore

    state_mgr = GameStateManager()
    collector = _FullMonsterCollector()
    bridge = PacketBridge(state_mgr, on_monster_update=collector,
                          on_damage=None, on_boss_event=None,
                          on_scene_change=None, data_source='tcp')
    print(f"[per-monster] starting TCP collection — fight ≥{args.min_monsters} monsters")
    bridge.start()

    deadline = time.time() + args.timeout
    first_monster_at = 0.0
    last_print = 0.0
    try:
        while time.time() < deadline:
            n = collector.n_distinct()
            now = time.time()
            if now - last_print > 2.0:
                last_print = now
                print(f"[per-monster] {n} distinct UUIDs, {collector.n_updates()} updates "
                      f"({int(deadline-now)}s left)")
            if n >= args.min_monsters:
                if first_monster_at == 0.0:
                    first_monster_at = now
                # Keep collecting for collect_time to gather more attrs
                if now - first_monster_at >= args.collect_time:
                    break
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass

    truths = collector.snapshot()
    print(f"[per-monster] collected {len(truths)} monsters; sample:")
    for t in truths[:6]:
        print(f"   uuid={t.uuid} hp={t.hp}/{t.max_hp} ext={t.extinction}/{t.max_extinction} "
              f"tid={t.template_id} sl={t.season_level} bs={t.breaking_stage} stun={t.stunned}")
    if len(truths) < 2:
        print("[per-monster] not enough monsters")
        bridge.stop()
        return 2

    print("[per-monster] stopping TCP, running memory scan ...")
    bridge.stop()
    time.sleep(0.5)

    from mem_probe.locator import SmartLocator
    sl = SmartLocator()
    pm = sl.pm
    print(f"[per-monster] attached pid={pm.pid}")

    results = discover_monster_per_uuid(pm, truths)
    aggregated = _aggregate_offsets(results)
    print(f"\n[per-monster] aggregated offsets (≥2 monsters agree):")
    if not aggregated:
        print("   (none — clusters didn't converge across monsters)")
    for lab, off in sorted(aggregated.items(), key=lambda kv: kv[1]):
        sign = "+" if off >= 0 else "-"
        print(f"   {lab:<24} = uuid {sign}{abs(off):#X}")

    # Persist as entity_collection anchor (per-monster mode)
    try:
        with open(args.anchors, "r", encoding="utf-8") as f:
            anchors = json.load(f)
    except (OSError, json.JSONDecodeError):
        anchors = {}
    sl_blk = anchors.setdefault("smart_locator", {})
    nested = sl_blk.setdefault("anchors", {})
    nested["entity_collection"] = {
        "mode": "per_monster_uuid_anchor",
        "discovered_via": "value_cluster_per_monster",
        "discovered_at": time.time(),
        "n_monsters_discovered": sum(1 for r in results if r.get("found")),
        "field_offsets_from_uuid": aggregated,
        "per_monster_results": [
            {k: v for k, v in r.items() if k != "offsets_from_uuid"}
            for r in results
        ],
    }
    sl_blk["schema_version"] = max(2, int(sl_blk.get("schema_version", 0) or 0))
    sl_blk["entity_collection_set_at"] = time.time()
    with open(args.anchors, "w", encoding="utf-8") as f:
        json.dump(anchors, f, ensure_ascii=False, indent=2)
    print(f"\n[per-monster] persisted entity_collection → {args.anchors}")
    return 0 if aggregated else 3


if __name__ == "__main__":
    sys.exit(main())
