"""CLI: hard-unpack monster discovery — value-cluster + 256KB walk-back.

The pure-walk-back-from-uuid approach failed because monster data isn't
adjacent to the uuid field in a 1-deref-deep struct. Use the same value-
cluster technique that worked for player_runtime: scan all (uuid, hp,
max_hp) values, find spatial clusters, walk back 256KB to find klass.

Cross-monster convergence on klass_ptr → MonsterEntity klass.

Heavy compute: cy_memscan.find_aligned_u32 / u64 (Cython AVX2).
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
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from mem_probe import cy_memscan as _cy
from mem_probe.locator import ANCHORS_PATH
from mem_probe.entity_discovery import MonsterTruth
from mem_probe.hard_unpack import (
    walk_back_to_klass, _ga_module_range, _build_modules_predicate,
    _hex, _f32_bits, _f64_bits, MAX_REGION_SIZE, CLUSTER_WINDOW,
)


# ───────── TCP collector ─────────

class _MonsterCollector:
    def __init__(self):
        self._lock = threading.Lock()
        self._by_uuid: Dict[int, MonsterTruth] = {}
        self._n = 0

    def __call__(self, payload: dict) -> None:
        try:
            uuid = int(payload.get("uuid") or 0)
            hp = int(payload.get("hp") or 0)
            max_hp = int(payload.get("max_hp") or 0)
            name = str(payload.get("name") or "")
        except Exception:
            return
        if uuid <= 0 or max_hp <= 0:
            return
        with self._lock:
            self._n += 1
            prev = self._by_uuid.get(uuid)
            if prev is None or max_hp > prev.max_hp:
                self._by_uuid[uuid] = MonsterTruth(uuid, hp, max_hp, name)

    def snapshot(self) -> List[MonsterTruth]:
        with self._lock:
            return list(self._by_uuid.values())

    def n(self) -> int:
        with self._lock:
            return len(self._by_uuid)


# ───────── Hard-unpack monster scan ─────────

def _scan_per_monster(pm, truth: MonsterTruth) -> List[Tuple[int, str]]:
    """Scan heap for all (uuid, hp, max_hp) bit patterns of one monster.
    Returns list of (abs_addr, value_kind) where value_kind ∈
    {'uuid_u64', 'hp_u32', 'hp_u64', 'hp_f32', 'maxhp_u32', 'maxhp_u64',
     'maxhp_f32'}.
    """
    u32_set: List[Tuple[int, str]] = []  # (bits, kind)
    u64_set: List[Tuple[int, str]] = []
    u64_set.append((truth.uuid & 0xFFFFFFFFFFFFFFFF, "uuid_u64"))
    if 0 < truth.hp < (1 << 32):
        u32_set.append((truth.hp & 0xFFFFFFFF, "hp_u32"))
    if 0 < truth.hp < (1 << 63):
        u64_set.append((truth.hp & 0xFFFFFFFFFFFFFFFF, "hp_u64"))
    if 0 < truth.hp < (1 << 24):
        f = _f32_bits(truth.hp)
        if f != (truth.hp & 0xFFFFFFFF):
            u32_set.append((f, "hp_f32"))
    if 0 < truth.max_hp < (1 << 32):
        u32_set.append((truth.max_hp & 0xFFFFFFFF, "maxhp_u32"))
    if 0 < truth.max_hp < (1 << 63):
        u64_set.append((truth.max_hp & 0xFFFFFFFFFFFFFFFF, "maxhp_u64"))
    if 0 < truth.max_hp < (1 << 24):
        f = _f32_bits(truth.max_hp)
        if f != (truth.max_hp & 0xFFFFFFFF):
            u32_set.append((f, "maxhp_f32"))

    regions = [r for r in pm.iter_regions(only_readable=True, only_private=True)
               if r.size <= MAX_REGION_SIZE]
    hits: List[Tuple[int, str]] = []
    lock = threading.Lock()

    def scan_one(region):
        buf = pm.read_bytes(region.base, region.size)
        if buf is None:
            return
        local: List[Tuple[int, str]] = []
        # u64 multi-needle
        if u64_set:
            r64 = _cy.find_aligned_u64_in_set(buf, [n for n, _ in u64_set])
            kind_map = {n: k for n, k in u64_set}
            for off, val in r64:
                local.append((region.base + off, kind_map[int(val)]))
        # u32 single-needle per value
        for needle, kind in u32_set:
            r32 = _cy.find_aligned_u32(buf, needle)
            for off in r32:
                local.append((region.base + off, kind))
        if local:
            with lock:
                hits.extend(local)

    with ThreadPoolExecutor(max_workers=8) as ex:
        list(ex.map(scan_one, regions))
    return hits


def _cluster_per_monster(hits: List[Tuple[int, str]],
                         *, window: int = CLUSTER_WINDOW,
                         min_kinds: int = 2) -> List[dict]:
    """Find spatial clusters where ≥min_kinds distinct value-kinds co-occur."""
    hits_sorted = sorted(hits, key=lambda h: h[0])
    clusters: List[dict] = []
    i = 0
    n = len(hits_sorted)
    while i < n:
        j = i
        while j < n and hits_sorted[j][0] - hits_sorted[i][0] <= window:
            j += 1
        win = hits_sorted[i:j]
        kinds = {k for _, k in win}
        if len(kinds) >= min_kinds:
            clusters.append({
                "span_lo": win[0][0],
                "span_hi": win[-1][0],
                "kinds": kinds,
                "hits": win,
            })
            i = j
        else:
            i += 1
    return clusters


def discover_monster_klass_hard(pm, truths: List[MonsterTruth],
                                *, verbose: bool = True) -> Optional[dict]:
    """Run hard-unpack-style discovery on monster TCP truth.

    For each monster:
      1. Scan all bit patterns (uuid + hp + max_hp + 4 encodings)
      2. Find clusters where ≥2 distinct kinds co-occur in 1KB window
      3. Walk back 256KB to find klass_ptr in GA module
    Cross-monster convergence: same klass_ptr seen for ≥2 monsters → winner.
    """
    t0 = time.time()
    in_module = _build_modules_predicate(pm)
    ga_base, ga_end = _ga_module_range(pm)
    if verbose:
        print(f"[hard-monster] GA: 0x{ga_base:X}–0x{ga_end:X}")
        print(f"[hard-monster] scanning {len(truths)} monsters ...")

    # klass_ptr → list of {monster_uuid, obj_addr, max_hp_off, hp_off, hp_encoding, uuid_off}
    klass_votes: Dict[int, List[dict]] = defaultdict(list)
    diag_per_monster: Dict[int, dict] = {}

    for truth in truths:
        t_one = time.time()
        hits = _scan_per_monster(pm, truth)
        clusters = _cluster_per_monster(hits, window=CLUSTER_WINDOW, min_kinds=2)
        diag_per_monster[truth.uuid] = {
            "n_hits": len(hits),
            "n_clusters": len(clusters),
        }
        if verbose:
            print(f"  [uuid={truth.uuid} hp={truth.hp}/{truth.max_hp}] "
                  f"{len(hits)} hits → {len(clusters)} clusters "
                  f"({time.time()-t_one:.2f}s)")

        for c in clusters:
            kc = walk_back_to_klass(pm, c["span_lo"], ga_base, ga_end, in_module,
                                    max_walk=0x40000)
            if not kc:
                continue
            obj_base, klass_ptr = kc[0]
            # Compute offsets for uuid / hp / max_hp within obj
            offsets: Dict[str, int] = {}
            for addr, kind in c["hits"]:
                rel = addr - obj_base
                # Take the FIRST instance of each kind within this cluster
                if kind not in offsets:
                    offsets[kind] = rel
            if "uuid_u64" not in offsets:
                continue  # require uuid presence
            klass_votes[klass_ptr].append({
                "monster_uuid": truth.uuid,
                "obj_addr": obj_base,
                "uuid_off": offsets.get("uuid_u64"),
                "hp_off_u32": offsets.get("hp_u32"),
                "hp_off_u64": offsets.get("hp_u64"),
                "hp_off_f32": offsets.get("hp_f32"),
                "maxhp_off_u32": offsets.get("maxhp_u32"),
                "maxhp_off_u64": offsets.get("maxhp_u64"),
                "maxhp_off_f32": offsets.get("maxhp_f32"),
                "kinds": list(c["kinds"]),
            })

    if verbose:
        print(f"\n[hard-monster] DIAG:")
        for uuid, d in diag_per_monster.items():
            print(f"   uuid={uuid}  hits={d['n_hits']}  clusters={d['n_clusters']}")
        print(f"[hard-monster] {len(klass_votes)} distinct klass_ptr candidates")

    # Convergence: pick klass that appears for ≥2 distinct monsters
    best_klass: Optional[int] = None
    best_records: List[dict] = []
    best_uuids: set = set()
    for kp, recs in klass_votes.items():
        uuids = {r["monster_uuid"] for r in recs}
        if len(uuids) > len(best_uuids):
            best_klass = kp
            best_uuids = uuids
            best_records = recs

    if best_klass is None or len(best_uuids) < 2:
        if verbose:
            print(f"[hard-monster] no klass converged across ≥2 monsters "
                  f"(best={len(best_uuids)} uuids)")
            top = sorted(klass_votes.items(),
                         key=lambda kv: -len({r['monster_uuid'] for r in kv[1]}))[:5]
            for kp, recs in top:
                uuids = {r["monster_uuid"] for r in recs}
                print(f"   klass=0x{kp:X}  monsters={len(uuids)}  records={len(recs)}")
        return None

    # Pick the most common (uuid_off, max_hp_off, hp_off) signature
    sig_counter: Counter = Counter()
    for r in best_records:
        # Prefer i32 max_hp (most common for IL2CPP), fallback to others
        max_off = (r.get("maxhp_off_u32") or r.get("maxhp_off_f32")
                   or r.get("maxhp_off_u64") or -1)
        hp_off = (r.get("hp_off_u32") or r.get("hp_off_f32")
                  or r.get("hp_off_u64") or -1)
        if r.get("maxhp_off_u32"):
            enc = "i32"; width = 4
        elif r.get("maxhp_off_f32"):
            enc = "f32"; width = 4
        elif r.get("maxhp_off_u64"):
            enc = "i64"; width = 8
        else:
            continue
        sig_counter[(r["uuid_off"], max_off, hp_off, enc, width)] += 1

    if not sig_counter:
        if verbose:
            print(f"[hard-monster] klass found but no (uuid_off, max_hp_off) signature")
        return None

    (uuid_off, max_off, hp_off, enc, width), n_supporting = sig_counter.most_common(1)[0]
    elapsed = time.time() - t0

    if verbose:
        print(f"\n[hard-monster] WINNER:")
        print(f"   monster_klass_ptr = 0x{best_klass:X}")
        print(f"   uuid_off          = 0x{uuid_off:X}")
        print(f"   max_hp_off        = 0x{max_off:X}")
        print(f"   hp_off            = "
              f"{'0x%X' % hp_off if hp_off >= 0 and hp_off != max_off else 'unknown (hp==max_hp)'}")
        print(f"   hp_encoding       = {enc} (width={width})")
        print(f"   convergent monsters = {len(best_uuids)}")
        print(f"   sig support       = {n_supporting}/{len(best_records)}")
        print(f"   elapsed           = {elapsed:.2f}s")

    return {
        "monster_klass_ptr": _hex(best_klass),
        "uuid_off": int(uuid_off),
        "attr_slot_off": -1,  # FLAT: hp/max_hp directly in obj body
        "hp_off": int(hp_off) if (hp_off >= 0 and hp_off != max_off) else -1,
        "max_hp_off": int(max_off),
        "hp_width": int(width),
        "hp_encoding": enc,
        "discovered_via": "hard_unpack_value_cluster",
        "convergent_monsters": len(best_uuids),
        "sig_support": n_supporting,
        "support": [
            {"uuid": int(r["monster_uuid"]), "obj_addr": _hex(r["obj_addr"])}
            for r in best_records[:8]
        ],
        "elapsed_s": elapsed,
    }


def persist_entity_anchor(anchors_path: str, payload: dict) -> None:
    try:
        with open(anchors_path, "r", encoding="utf-8") as f:
            anchors = json.load(f)
    except (OSError, json.JSONDecodeError):
        anchors = {}
    sl = anchors.setdefault("smart_locator", {})
    nested = sl.setdefault("anchors", {})
    nested["entity_collection"] = payload
    sl["schema_version"] = max(2, int(sl.get("schema_version", 0) or 0))
    sl["entity_collection_set_at"] = time.time()
    with open(anchors_path, "w", encoding="utf-8") as f:
        json.dump(anchors, f, ensure_ascii=False, indent=2)


# ───────── CLI ─────────

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Hard-unpack monster discovery (value-cluster + 256KB walk-back)")
    ap.add_argument("--min-monsters", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--anchors", default=ANCHORS_PATH)
    ap.add_argument("--print-every", type=float, default=2.0)
    args = ap.parse_args(argv)

    from game_state import GameStateManager  # type: ignore
    from packet_bridge import PacketBridge  # type: ignore

    state_mgr = GameStateManager()
    collector = _MonsterCollector()
    bridge = PacketBridge(state_mgr, on_monster_update=collector,
                          on_damage=None, on_boss_event=None,
                          on_scene_change=None, data_source='tcp')
    print(f"[hard-monster] starting PacketBridge — go fight ≥{args.min_monsters} monsters")
    bridge.start()

    deadline = time.time() + args.timeout
    last_print = 0.0
    try:
        while time.time() < deadline:
            n = collector.n()
            now = time.time()
            if now - last_print > args.print_every:
                last_print = now
                print(f"[hard-monster] {n} distinct UUIDs ({int(deadline-now)}s left)")
            if n >= args.min_monsters:
                time.sleep(1.5)
                break
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass

    truths = collector.snapshot()
    if len(truths) < 2:
        print(f"[hard-monster] FAIL: only {len(truths)} monsters collected")
        bridge.stop()
        return 2

    print(f"[hard-monster] gathered {len(truths)} monsters; stopping TCP ...")
    bridge.stop()
    time.sleep(0.5)

    from mem_probe.locator import SmartLocator
    sl = SmartLocator()
    pm = sl.pm
    print(f"[hard-monster] attached pid={pm.pid}")

    result = discover_monster_klass_hard(pm, truths)
    if result is None:
        print(f"[hard-monster] discovery failed")
        return 3

    persist_entity_anchor(args.anchors, result)
    print(f"\n[hard-monster] persisted entity_collection anchor → {args.anchors}")
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
