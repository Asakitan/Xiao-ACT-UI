"""Value-anchor monster/entity klass discovery — no dump.cs required.

Algorithm (no IL2CPP base address needed; klass pointers are absolute):

  1. Capture monster ground truth from TCP: a list of (uuid, hp, max_hp).
     We need ≥2 distinct UUIDs to converge on the right klass.
  2. Single Cython AVX2 pass scans every committed private region for any
     i64 == any UUID in the set (find_aligned_u64_in_set, multi-needle).
     This is the only "linear over the whole heap" step.
  3. For each (uuid_addr, uuid) hit, try a small set of UID-field offsets
     K1 ∈ {0x10, 0x18, 0x20, 0x28, 0x30}; obj_base = uuid_addr - K1.
     - Read 8 bytes at obj_base → must be a valid IL2CPP klass ptr (in GA).
     - Read 0x100-byte body once.
     - Inside body, scan i32 == hp and i32 == max_hp at 4-byte alignment
       (Cython narrow_u32_batch). Try i64 too (long encoding).
     - Adjacency check: hp/max_hp within ±0x10 of each other.
  4. Cross-monster convergence: for each (klass_ptr, uuid_off, hp_off,
     max_hp_off, hp_width) tuple, count how many distinct UUIDs produced it.
     The tuple with the highest count (≥2) wins — that's MonsterEntity.
  5. Persist to anchors.json under
     `smart_locator.anchors.entity_collection`:
        monster_klass_ptr, uuid_off, hp_off, max_hp_off, hp_width

The actual entity TABLE (a List<MonsterEntity> / Dictionary) is *not*
required — the entity_watcher can iterate by Cython-scanning for
`klass_ptr == monster_klass_ptr` each tick (cheap because aligned u64
scan over the heap is sub-second with AVX2). This is more robust than
chasing a List<T>'s _items+count pair.

Public surface:
    MonsterTruth(uuid, hp, max_hp, name)
    discover_monster_klass(pm, truths) -> dict | None
    persist_entity_anchor(anchors_path, payload) -> None
"""
from __future__ import annotations

import bisect
import json
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from mem_probe import cy_memscan as _cy

# Tuning constants — keep small; we only need ~2 distinct UUIDs
DEFAULT_BODY_BYTES = 0x300        # widened: HP fields can be deeper than 0x100
DEFAULT_UID_OFF_TRIES = (
    0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58,
    0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8,
    0xB0, 0xB8, 0xC0,
)
DEFAULT_HP_PAIR_RADIUS = 0x40     # cur ↔ max distance can be up to 64 bytes
MAX_REGION_SIZE = 256 * 1024 * 1024  # skip > 256MB regions (giant alloc; rarely valuable)


@dataclass
class MonsterTruth:
    uuid: int
    hp: int
    max_hp: int
    name: str = ""

    def is_usable(self) -> bool:
        return self.uuid > 0 and self.hp > 0 and self.max_hp > 0


def _hex(v: int) -> str:
    return f"0x{int(v):X}"


def _modules_in_module_check(pm) -> Callable[[int], bool]:
    """Build a fast `addr ∈ any_loaded_module` predicate."""
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


def _ga_module_range(pm) -> Tuple[int, int]:
    """Return (base, end) of GameAssembly.dll, or (0,0) if not found."""
    for m in pm.list_modules():
        if m.name.lower() == "gameassembly.dll":
            return m.base, m.base + m.size
    return 0, 0


# ───────── Cython-backed full-heap multi-UUID scan ─────────

def _scan_uuids_full_heap(pm, uuids: List[int], *,
                          n_workers: int = 8) -> List[Tuple[int, int]]:
    """Full-heap parallel scan for any i64 ∈ {uuids}. Cython AVX2 inside.

    Returns flat list of (abs_addr, uuid_value) hits.
    """
    needles = [int(u) & 0xFFFFFFFFFFFFFFFF for u in uuids if u > 0]
    if not needles:
        return []

    regions = [r for r in pm.iter_regions(only_readable=True, only_private=True)
               if r.size <= MAX_REGION_SIZE]
    results: List[Tuple[int, int]] = []
    lock = threading.Lock()

    def scan_one(region):
        buf = pm.read_bytes(region.base, region.size)
        if buf is None:
            return
        hits = _cy.find_aligned_u64_in_set(buf, needles)
        if not hits:
            return
        with lock:
            for off, val in hits:
                results.append((region.base + off, int(val)))

    if n_workers > 1:
        with ThreadPoolExecutor(max_workers=n_workers) as ex:
            list(ex.map(scan_one, regions))
    else:
        for r in regions:
            scan_one(r)
    return results


def _scan_values_full_heap(pm, u32_needles: List[int], u64_needles: List[int],
                           *, n_workers: int = 8) -> Dict[int, List[Tuple[int, str]]]:
    """Full-heap multi-needle scan for u32 + u64 values. Cython AVX2 inside.

    Returns dict: needle_value → list of (abs_addr, 'u32'|'u64')
    """
    out: Dict[int, List[Tuple[int, str]]] = {}
    if not u32_needles and not u64_needles:
        return out

    regions = [r for r in pm.iter_regions(only_readable=True, only_private=True)
               if r.size <= MAX_REGION_SIZE]
    lock = threading.Lock()

    def scan_one(region):
        buf = pm.read_bytes(region.base, region.size)
        if buf is None:
            return
        # u64 multi-needle (single Cython call for all u64 values)
        if u64_needles:
            hits64 = _cy.find_aligned_u64_in_set(
                buf, [int(n) & 0xFFFFFFFFFFFFFFFF for n in u64_needles])
            if hits64:
                with lock:
                    for off, val in hits64:
                        out.setdefault(int(val), []).append(
                            (region.base + off, "u64"))
        # u32 single-needle per value (no multi-needle u32 in cy_memscan)
        for n in u32_needles:
            hits32 = _cy.find_aligned_u32(buf, int(n) & 0xFFFFFFFF)
            if hits32:
                with lock:
                    for off in hits32:
                        out.setdefault(int(n), []).append(
                            (region.base + off, "u32"))

    if n_workers > 1:
        with ThreadPoolExecutor(max_workers=n_workers) as ex:
            list(ex.map(scan_one, regions))
    else:
        for r in regions:
            scan_one(r)
    return out


# ───────── Per-candidate body analysis (Cython narrow + small Python join) ─────────

def _f32_bits(v: int) -> int:
    """Reinterpret int v as float32 → return raw u32 bit pattern."""
    import struct
    return struct.unpack("<I", struct.pack("<f", float(v)))[0]


def _f64_bits(v: int) -> int:
    """Reinterpret int v as float64 → return raw u64 bit pattern."""
    import struct
    return struct.unpack("<Q", struct.pack("<d", float(v)))[0]


def _analyze_body(body: bytes, hp: int, max_hp: int) -> List[Tuple[int, int, int, int]]:
    """Inside a candidate's body, find aligned (cur_hp, max_hp) signatures.

    Returns list of (cur_off, max_off, hp_width_bytes, encoding_id).

    Encoding IDs:
       32 = i32 pair      | 132 = i32 max-only
       64 = i64 pair      | 164 = i64 max-only
      132f = f32 pair     | 1132 = f32 max-only
      164f = f64 pair     | 1164 = f64 max-only

    When hp == max_hp the algorithm can't distinguish cur from max so it emits
    single-slot signatures keyed on max_hp alone. Cross-UUID convergence still
    picks the real max_hp offset because the same offset keeps winning across
    distinct monsters.
    """
    results: List[Tuple[int, int, int, int]] = []
    same = (hp == max_hp and hp > 0)

    def _scan_pair(width: int, hp_needle: int, max_needle: int,
                   pair_id: int, single_id: int) -> None:
        """Generic narrow→pair using whatever width's batch kernel."""
        kernel = _cy.narrow_u32_batch if width == 4 else _cy.narrow_u64_batch
        max_mask = kernel(body, max_needle)
        max_offs = [i * width for i, b in enumerate(max_mask) if b]
        if hp_needle != max_needle and hp_needle is not None:
            hp_mask = kernel(body, hp_needle)
            hp_offs = [i * width for i, b in enumerate(hp_mask) if b]
            for cur_off in hp_offs:
                for max_off in max_offs:
                    if (abs(cur_off - max_off) <= DEFAULT_HP_PAIR_RADIUS
                            and cur_off != max_off):
                        results.append((cur_off, max_off, width, pair_id))
        for max_off in max_offs:
            results.append((-1, max_off, width, single_id))

    # ── i32 path ──
    if 0 < max_hp <= 0xFFFFFFFF:
        _scan_pair(
            4,
            (hp & 0xFFFFFFFF) if (0 < hp <= 0xFFFFFFFF and not same) else None,
            max_hp & 0xFFFFFFFF,
            pair_id=32, single_id=132,
        )

    # ── i64 path ──
    if 0 < max_hp <= 0xFFFFFFFFFFFFFFFF:
        _scan_pair(
            8,
            (hp & 0xFFFFFFFFFFFFFFFF) if (hp > 0 and not same) else None,
            max_hp & 0xFFFFFFFFFFFFFFFF,
            pair_id=64, single_id=164,
        )

    # ── f32 path: reinterpret integer hp/max_hp as float bits ──
    # Many Unity-IL2CPP games store entity HP as float. We convert the int
    # truth to the IEEE-754 32-bit pattern and scan u32 == that pattern.
    # Encoding IDs: 232=f32 pair, 1132=f32 max-only.
    if max_hp > 0 and max_hp < (1 << 24):  # f32 mantissa precision is 24 bits
        max_f32 = _f32_bits(max_hp)
        if max_f32 != (max_hp & 0xFFFFFFFF):  # only if differs from i32
            _scan_pair(
                4,
                _f32_bits(hp) if (hp > 0 and not same) else None,
                max_f32,
                pair_id=232, single_id=1132,
            )

    # ── f64 path: reinterpret as double — 264 pair, 1164 max-only ──
    if max_hp > 0 and max_hp < (1 << 53):
        max_f64 = _f64_bits(max_hp)
        if max_f64 != (max_hp & 0xFFFFFFFFFFFFFFFF):
            _scan_pair(
                8,
                _f64_bits(hp) if (hp > 0 and not same) else None,
                max_f64,
                pair_id=264, single_id=1164,
            )

    return results


# ───────── HP-Forward fallback ─────────

def _walk_back_to_klass_in_ga(pm, hp_addr: int, ga_base: int, ga_end: int,
                              in_module, *, max_walk: int = 0x40000) -> List[Tuple[int, int]]:
    """Given an address where max_hp lives, walk back looking for a klass_ptr
    in GameAssembly.dll. Single RPM + buffer scan (much faster than per-step).

    Default max_walk 0x40000 (256KB) — covers large arrays of structs and
    deeply-nested IL2CPP class layouts (proven necessary for player_runtime).

    Returns list of (obj_base, klass_ptr), closest to hp_addr first.
    """
    results: List[Tuple[int, int]] = []
    aligned = hp_addr - (hp_addr % 8)
    region_start = max(0x10000, aligned - max_walk)
    region_size = aligned - region_start
    if region_size < 8:
        return results
    buf = pm.read_bytes(region_start, region_size)
    if not buf:
        return results
    n_slots = len(buf) // 8
    for i in range(n_slots):
        kp = int.from_bytes(buf[i * 8:i * 8 + 8], "little")
        if ga_end:
            if not (ga_base <= kp < ga_end):
                continue
        else:
            if not in_module(kp):
                continue
        results.append((region_start + i * 8, kp))
    results.sort(key=lambda r: abs(r[0] - hp_addr))
    return results[:8]


def _discover_via_hp_forward(pm, truths_unique: List[MonsterTruth],
                             ga_base: int, ga_end: int, in_module,
                             *, verbose: bool = True) -> Optional[dict]:
    """Inverse algorithm: find max_hp values in memory, walk back to owner obj.

    Used when uid-forward fails (HP container not reachable from monster's
    direct ptr slots). max_hp is rare (i32 unique-ish), so this is reliable
    even when HP lives 2+ derefs deep.

    Convergence: signatures (klass_ptr, max_hp_off, encoding) seen across ≥2
    different monsters win.
    """
    import struct as _struct

    # Build needle sets across encodings
    u32_needles: set = set()
    u64_needles: set = set()
    needle_to_truth: Dict[int, List[Tuple[MonsterTruth, str]]] = {}

    def _add(value: int, truth: MonsterTruth, enc: str, width: int):
        if width == 4:
            u32_needles.add(value & 0xFFFFFFFF)
        else:
            u64_needles.add(value & 0xFFFFFFFFFFFFFFFF)
        needle_to_truth.setdefault(value, []).append((truth, enc))

    for t in truths_unique:
        mh = t.max_hp
        # i32
        if 0 < mh < (1 << 32):
            _add(mh, t, "i32", 4)
        # i64
        if 0 < mh < (1 << 63):
            _add(mh, t, "i64", 8)
        # f32
        if 0 < mh < (1 << 24):
            f32 = _f32_bits(mh)
            if f32 != (mh & 0xFFFFFFFF):
                _add(f32, t, "f32", 4)
        # f64
        if 0 < mh < (1 << 53):
            f64 = _f64_bits(mh)
            if f64 != (mh & 0xFFFFFFFFFFFFFFFF):
                _add(f64, t, "f64", 8)

    if verbose:
        print(f"[hp-forward] needles: {len(u32_needles)} u32, {len(u64_needles)} u64")

    t_scan = time.time()
    hits = _scan_values_full_heap(
        pm, list(u32_needles), list(u64_needles), n_workers=8)
    n_hits = sum(len(v) for v in hits.values())
    if verbose:
        print(f"[hp-forward] full-heap scan: {n_hits} total hits "
              f"across {len(hits)} distinct values ({time.time()-t_scan:.2f}s)")
    if not hits:
        return None

    # For each hit, walk back to find a klass_ptr in GA, then check if a
    # monster UUID is in obj body (or 1-deref deep). Track sigs as
    # (klass_ptr, max_hp_off, encoding).
    votes: Dict[Tuple[int, int, str], List[dict]] = {}
    diag_max_hp_with_owner: Dict[int, int] = {t.max_hp: 0 for t in truths_unique}
    # Cap hits per needle to avoid blowup if max_hp is also a popular int
    PER_NEEDLE_CAP = 200
    for needle_val, hit_list in hits.items():
        for truth, enc in needle_to_truth.get(needle_val, []):
            for abs_addr, width_kind in hit_list[:PER_NEEDLE_CAP]:
                width = 4 if width_kind == "u32" else 8
                # Validate encoding alignment (f32/f64 pattern)
                # (encoding-specific filtering already implicit via needle sets)
                owner_cands = _walk_back_to_klass_in_ga(
                    pm, abs_addr, ga_base, ga_end, in_module, max_walk=0x300)
                for obj_base, klass_ptr in owner_cands:
                    max_hp_off = abs_addr - obj_base
                    # We want a "reasonable" max_hp offset (not 0, not too big)
                    if max_hp_off < 8 or max_hp_off > 0x300:
                        continue
                    # Now verify this is a real monster owner — its body
                    # should contain ≥1 monster UUID (own or sibling).
                    body_size = max(0x80, max_hp_off + 0x40)
                    body = pm.read_bytes(obj_base, body_size)
                    if not body:
                        continue
                    # Look for ANY monster UUID in body or 1-deref attr bodies
                    found_uuid: Optional[int] = None
                    for t in truths_unique:
                        u_mask = _cy.narrow_u64_batch(
                            body, int(t.uuid) & 0xFFFFFFFFFFFFFFFF)
                        if any(u_mask):
                            found_uuid = t.uuid
                            break
                    if found_uuid is None:
                        # Try 1-level deref of ptr slots
                        # (max ~32 slots scanned to keep cost bounded)
                        for slot_off in range(8, min(len(body), 0x100), 8):
                            ptr_blob = body[slot_off:slot_off + 8]
                            attr_ptr = int.from_bytes(ptr_blob, "little")
                            if not (0x10000 <= attr_ptr <= 0x7FFFFFFFFFFF):
                                continue
                            ab = pm.read_bytes(attr_ptr, 0x100)
                            if not ab:
                                continue
                            for t in truths_unique:
                                if any(_cy.narrow_u64_batch(
                                        ab, int(t.uuid) & 0xFFFFFFFFFFFFFFFF)):
                                    found_uuid = t.uuid
                                    break
                            if found_uuid is not None:
                                break
                    if found_uuid is None:
                        continue
                    sig = (klass_ptr, max_hp_off, enc)
                    votes.setdefault(sig, []).append({
                        "uuid": found_uuid,
                        "obj_addr": obj_base,
                        "max_hp_addr": abs_addr,
                        "klass_ptr": klass_ptr,
                    })
                    diag_max_hp_with_owner[truth.max_hp] += 1

    if verbose:
        print(f"[hp-forward] DIAG per-max_hp (owner objs found):")
        for mh, n in diag_max_hp_with_owner.items():
            print(f"   max_hp={mh}: {n}")

    if not votes:
        return None

    # Pick best signature: most distinct max_hp values supporting it
    best_sig: Optional[Tuple[int, int, str]] = None
    best_distinct_max_hp: set = set()
    best_records: List[dict] = []
    for sig, recs in votes.items():
        # Use distinct UUIDs from records (UUIDs are tied to truths' max_hp)
        max_hps_supporting = {
            t.max_hp for t in truths_unique if t.uuid in {r["uuid"] for r in recs}
        }
        if len(max_hps_supporting) > len(best_distinct_max_hp):
            best_sig = sig
            best_distinct_max_hp = max_hps_supporting
            best_records = recs

    if best_sig is None or len(best_distinct_max_hp) < 2:
        if verbose:
            print(f"[hp-forward] no convergent signature "
                  f"(best: {len(best_distinct_max_hp)} max_hp values)")
            top5 = sorted(votes.items(), key=lambda kv: -len(kv[1]))[:5]
            for sig, recs in top5:
                kp, mo, e = sig
                uuids = {r["uuid"] for r in recs}
                print(f"   klass=0x{kp:X} max+0x{mo:X} {e} "
                      f"records={len(recs)} uuids={len(uuids)}")
        return None

    klass_ptr, max_hp_off, enc = best_sig
    hp_width = 4 if enc in ("i32", "f32") else 8
    if verbose:
        print(f"[hp-forward] WINNER:")
        print(f"   monster_klass_ptr = 0x{klass_ptr:X}")
        print(f"   max_hp_off        = 0x{max_hp_off:X}  (within OWNER obj, not entity!)")
        print(f"   hp_encoding       = {enc}")
        print(f"   convergent max_hp values = {len(best_distinct_max_hp)}")

    return {
        "monster_klass_ptr": _hex(klass_ptr),
        "uuid_off": -1,                # uuid not adjacent — discovery via this path
        "attr_slot_off": -1,            # owner obj IS the attr container
        "hp_off": -1,                   # hp == max_hp at discovery
        "max_hp_off": int(max_hp_off),
        "hp_width": int(hp_width),
        "hp_encoding": enc,
        "discovered_via": "hp_forward",
        "support": [
            {"uuid": int(r["uuid"]),
             "obj_addr": _hex(r["obj_addr"]),
             "max_hp_addr": _hex(r["max_hp_addr"])}
            for r in best_records[:8]
        ],
        "convergent_max_hp_values": len(best_distinct_max_hp),
    }


# ───────── Main discovery routine ─────────

def discover_monster_klass(pm, truths: List[MonsterTruth],
                           *, verbose: bool = True) -> Optional[dict]:
    """Find the MonsterEntity klass + offsets via cross-UUID convergence.

    Args:
        pm:       StarProcess instance.
        truths:   list of MonsterTruth with at least 2 distinct usable entries.
        verbose:  print progress.

    Returns:
        dict with discovered anchor fields, or None if no convergent klass:
            {
              "monster_klass_ptr": "0x7FFE...",
              "uuid_off": 0x10,
              "hp_off": 0x40, "max_hp_off": 0x48, "hp_width": 8,
              "support": [{"uuid": ..., "obj_addr": "0x..."}, ...],
              "elapsed_s": ...,
            }
    """
    t0 = time.time()
    usable = [t for t in truths if t.is_usable()]
    if len(usable) < 2:
        if verbose:
            print(f"[entity-discovery] need ≥2 usable monster truths; got {len(usable)}")
        return None

    # Collapse duplicate UUIDs (keep the one with highest hp/max_hp pair)
    by_uuid: Dict[int, MonsterTruth] = {}
    for t in usable:
        prev = by_uuid.get(t.uuid)
        if prev is None or (t.max_hp > prev.max_hp):
            by_uuid[t.uuid] = t
    truths_unique = list(by_uuid.values())
    if verbose:
        print(f"[entity-discovery] {len(truths_unique)} distinct monster UUIDs:")
        for t in truths_unique[:8]:
            print(f"   uuid={t.uuid} hp={t.hp}/{t.max_hp} name={t.name!r}")

    # Stage 1: full-heap UUID scan (Cython AVX2)
    in_module = _modules_in_module_check(pm)
    ga_base, ga_end = _ga_module_range(pm)
    if verbose:
        if ga_end:
            print(f"[entity-discovery] GA: 0x{ga_base:X}–0x{ga_end:X}")
        else:
            print(f"[entity-discovery] WARN: GameAssembly.dll not found in modules; "
                  f"klass sanity uses any-module check")

    uuid_set = [t.uuid for t in truths_unique]
    t_scan = time.time()
    hits = _scan_uuids_full_heap(pm, uuid_set, n_workers=8)
    if verbose:
        print(f"[entity-discovery] uuid scan: {len(hits)} total hits "
              f"({time.time()-t_scan:.2f}s, AVX2 multi-needle)")
    if not hits:
        return None

    # Group hits by uuid
    hits_by_uuid: Dict[int, List[int]] = {u: [] for u in uuid_set}
    for addr, val in hits:
        if val in hits_by_uuid:
            hits_by_uuid[val].append(addr)

    # Stage 2: per-candidate analysis. Two paths:
    #   FLAT:    signature = (klass, uid_off, -1, cur, max, w)  → hp lives in obj body
    #   NESTED:  signature = (klass, uid_off, attr_slot_off, cur, max, w) → hp lives
    #            in deref(obj+attr_slot_off) attr body. Same as SELF's
    #            char_serialize→user_fight_attr layout.
    #
    # For speed: cap UUID hits at 500/uuid; cap nested attr_slot tries to ptr
    # slots whose deref klass ALSO falls in GameAssembly.dll.
    HIT_CAP = 500
    NESTED_ATTR_BODY_BYTES = 0x200
    # Signature: (klass, uid_off, attr_slot_off, cur_off, max_off, width, enc_id)
    votes: Dict[Tuple[int, int, int, int, int, int, int], List[dict]] = {}
    diag_uuid_with_ga_klass: Dict[int, int] = {u: 0 for u in uuid_set}
    diag_uuid_flat_match: Dict[int, int] = {u: 0 for u in uuid_set}
    diag_uuid_nested_match: Dict[int, int] = {u: 0 for u in uuid_set}
    diag_uuid_attr_ptr_cands: Dict[int, int] = {u: 0 for u in uuid_set}

    def _ptr_in_user_space(p: int) -> bool:
        return 0x10000 <= p <= 0x7FFFFFFFFFFF

    for uuid_val, addrs in hits_by_uuid.items():
        if not addrs:
            continue
        truth = by_uuid[uuid_val]
        for uid_addr in addrs[:HIT_CAP]:
            for k1 in DEFAULT_UID_OFF_TRIES:
                obj_base = uid_addr - k1
                if obj_base < 0x10000:
                    continue
                # 1) klass ptr sanity at obj_base
                klass_blob = pm.read_bytes(obj_base, 8)
                if not klass_blob:
                    continue
                klass_ptr = int.from_bytes(klass_blob, "little")
                if ga_end:
                    if not (ga_base <= klass_ptr < ga_end):
                        continue
                else:
                    if not in_module(klass_ptr):
                        continue
                diag_uuid_with_ga_klass[uuid_val] += 1

                body = pm.read_bytes(obj_base, DEFAULT_BODY_BYTES)
                if not body or len(body) < 0x40:
                    continue

                # ── FLAT path: hp/max_hp directly in obj body ──
                pairs = _analyze_body(body, truth.hp, truth.max_hp)
                if pairs:
                    diag_uuid_flat_match[uuid_val] += 1
                for cur_off, max_off, hp_width, enc in pairs:
                    # attr_slot_off = -1 means "hp is in obj body, not nested"
                    sig = (klass_ptr, k1, -1, cur_off, max_off, hp_width, enc)
                    votes.setdefault(sig, []).append({
                        "uuid": uuid_val,
                        "obj_addr": obj_base,
                        "klass_ptr": klass_ptr,
                    })

                # ── NESTED path: hp/max_hp in deref(obj_base + attr_slot_off) ──
                # Walk every 8-byte aligned ptr slot in body; deref each;
                # check the deref'd klass is also in GA; cython-scan that body.
                for slot_off in range(8, min(len(body), 0x200), 8):
                    if slot_off == k1:
                        continue  # this is the UID slot
                    ptr_blob = body[slot_off:slot_off + 8]
                    if len(ptr_blob) < 8:
                        break
                    attr_ptr = int.from_bytes(ptr_blob, "little")
                    if not _ptr_in_user_space(attr_ptr):
                        continue
                    attr_body = pm.read_bytes(attr_ptr, NESTED_ATTR_BODY_BYTES)
                    if not attr_body or len(attr_body) < 0x20:
                        continue
                    attr_klass = int.from_bytes(attr_body[0:8], "little")
                    if ga_end:
                        if not (ga_base <= attr_klass < ga_end):
                            continue
                    else:
                        if not in_module(attr_klass):
                            continue
                    diag_uuid_attr_ptr_cands[uuid_val] += 1
                    nested_pairs = _analyze_body(attr_body, truth.hp, truth.max_hp)
                    if nested_pairs:
                        diag_uuid_nested_match[uuid_val] += 1
                    for cur_off, max_off, hp_width, enc in nested_pairs:
                        sig = (klass_ptr, k1, slot_off, cur_off, max_off, hp_width, enc)
                        votes.setdefault(sig, []).append({
                            "uuid": uuid_val,
                            "obj_addr": obj_base,
                            "klass_ptr": klass_ptr,
                            "attr_obj": attr_ptr,
                            "attr_klass": attr_klass,
                        })

    if verbose:
        print(f"[entity-discovery] DIAG per-uuid:")
        for u in uuid_set:
            print(f"   uuid={u}  ga_klass={diag_uuid_with_ga_klass.get(u,0)}  "
                  f"flat_match={diag_uuid_flat_match.get(u,0)}  "
                  f"attr_cands={diag_uuid_attr_ptr_cands.get(u,0)}  "
                  f"nested_match={diag_uuid_nested_match.get(u,0)}")

    if not votes:
        if verbose:
            print(f"[entity-discovery] uid-forward path found nothing; "
                  f"falling back to HP-FORWARD ...")
        hp_result = _discover_via_hp_forward(
            pm, truths_unique, ga_base, ga_end, in_module, verbose=verbose,
        )
        if hp_result is not None:
            hp_result["elapsed_s"] = time.time() - t0
            return hp_result
        if verbose:
            print(f"[entity-discovery] HP-forward also failed")
        return None

    # Stage 3: pick the winner — most distinct UUIDs supporting it.
    # Quality preference (highest first):
    #   - paired (cur_off >= 0) > max-only (cur_off == -1)
    #   - nested (attr_slot_off >= 0) preferred when flat fails for ≥1 UUID,
    #     else flat preferred (simpler).
    #   - integer encoding > float (integer is exact; float can round).
    def _sig_quality(sig):
        _kp, _uo, atso, co, _mo, _w, enc = sig
        paired = (1 if co >= 0 else 0)
        nested = (1 if atso >= 0 else 0)
        # Encoding IDs:
        #   32/64        = i32/i64 paired
        #   132/164      = i32/i64 max-only
        #   232/264      = f32/f64 paired
        #   1132/1164    = f32/f64 max-only
        is_int = 1 if enc in (32, 64, 132, 164) else 0
        return paired * 4 + nested * 2 + is_int
    best_sig: Optional[Tuple[int, int, int, int, int, int, int]] = None
    best_uuids: set = set()
    best_records: List[dict] = []
    best_quality = -1
    for sig, records in votes.items():
        distinct = {r["uuid"] for r in records}
        q = _sig_quality(sig)
        if (len(distinct) > len(best_uuids)
                or (len(distinct) == len(best_uuids) and q > best_quality)):
            best_sig = sig
            best_uuids = distinct
            best_records = records
            best_quality = q

    if best_sig is None or len(best_uuids) < 2:
        if verbose:
            print(f"[entity-discovery] no signature converged across ≥2 UUIDs "
                  f"(best had {len(best_uuids)} UUID(s))")
            top5 = sorted(votes.items(),
                          key=lambda kv: -len({r['uuid'] for r in kv[1]}))[:5]
            for sig, recs in top5:
                kp, uo, atso, co, mo, w, enc = sig
                uids = {r["uuid"] for r in recs}
                atso_str = f"attr+0x{atso:X}→" if atso >= 0 else "[flat]"
                co_str = f"hp+0x{co:X}" if co >= 0 else "hp+?"
                enc_str = {32:'i32', 64:'i64', 132:'i32-max-only',
                           164:'i64-max-only', 232:'f32', 264:'f64',
                           1132:'f32-max-only', 1164:'f64-max-only'}.get(enc, f'enc{enc}')
                print(f"   klass=0x{kp:X} uid+0x{uo:X} {atso_str}{co_str}/max+0x{mo:X} "
                      f"{enc_str} support={len(uids)} uuids")
        return None

    klass_ptr, uuid_off, attr_slot_off, cur_off, max_off, hp_width, encoding = best_sig
    enc_name = {
        32: "i32", 64: "i64", 132: "i32", 164: "i64",
        232: "f32", 264: "f64", 1132: "f32", 1164: "f64",
    }.get(encoding, f"enc{encoding}")
    elapsed = time.time() - t0
    nested = (attr_slot_off >= 0)
    if verbose:
        print(f"[entity-discovery] WINNER ({'NESTED attr' if nested else 'FLAT body'}):")
        print(f"   monster_klass_ptr = 0x{klass_ptr:X}")
        print(f"   uuid_off          = 0x{uuid_off:X}")
        if nested:
            print(f"   attr_slot_off     = 0x{attr_slot_off:X}  (deref → attr obj)")
        print(f"   cur_hp_off        = "
              f"{('0x%X' % cur_off) if cur_off >= 0 else 'unknown (max-only; resolves after first damage)'}")
        print(f"   max_hp_off        = 0x{max_off:X}")
        print(f"   hp_encoding       = {enc_name} (width=i{hp_width*8} bytes)")
        print(f"   convergent UUIDs  = {len(best_uuids)}/{len(truths_unique)}")
        print(f"   elapsed           = {elapsed:.2f}s")

    payload = {
        "monster_klass_ptr": _hex(klass_ptr),
        "uuid_off": int(uuid_off),
        "attr_slot_off": int(attr_slot_off),  # -1 means flat (hp in obj body)
        "hp_off": int(cur_off),               # -1 means hp==max_hp at discovery
        "max_hp_off": int(max_off),
        "hp_width": int(hp_width),
        "hp_encoding": enc_name,              # 'i32' | 'i64' | 'f32' | 'f64'
        "support": [
            {
                "uuid": int(r["uuid"]),
                "obj_addr": _hex(r["obj_addr"]),
                **({"attr_obj": _hex(r["attr_obj"])} if "attr_obj" in r else {}),
            }
            for r in best_records[:8]
        ],
        "elapsed_s": elapsed,
        "convergent_uuids": len(best_uuids),
    }
    return payload


# ───────── Persistence ─────────

def persist_entity_anchor(anchors_path: str, payload: dict) -> None:
    """Write the entity_collection block into anchors.json (schema v2)."""
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


__all__ = [
    "MonsterTruth",
    "discover_monster_klass",
    "persist_entity_anchor",
]
