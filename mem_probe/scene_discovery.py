"""Value-anchor scene_manager / SceneInfo klass discovery.

Algorithm (no IL2CPP base address needed):

  1. Capture (scene_uuid, dungeon_id) from TCP SyncDungeonData. SceneUuid is
     int64 and unique per dungeon instance — strong anchor.
  2. Cython AVX2 scan i64 == scene_uuid across heap. Few hits (typically <50).
  3. For each hit, try a small set of UID/scene_uuid field offsets to derive
     obj_base. Read 0x100-byte body once.
  4. Verify klass_ptr at obj_base is in GameAssembly.dll. Then scan body for
     dungeon_id (if available) and i32-aligned candidates as scene_id /
     layer.
  5. Use multi-field convergence (klass_ptr, scene_uuid_off, dungeon_id_off)
     — best signature wins.

Persists to anchors.json under `smart_locator.anchors.scene_manager`:
    obj_addr, klass_ptr, scene_uuid_off, dungeon_id_off, scene_id_off,
    layer_off, hp_width.

Note: with only one ground-truth scene_uuid per session this discovery has
weaker disambiguation than monster discovery (which converges across N
UUIDs). We rely on klass_ptr + dungeon_id co-presence.
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

DEFAULT_BODY_BYTES = 0x180
DEFAULT_UID_OFF_TRIES = (0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50)
MAX_REGION_SIZE = 256 * 1024 * 1024


@dataclass
class SceneTruth:
    scene_uuid: int          # int64, unique per dungeon instance — primary anchor
    dungeon_id: int = 0      # int32, secondary — used for body validation
    scene_id: int = 0        # int32, optional — usually map id (e.g. 100, 200)
    layer: int = 0           # int32, optional — sub-area within scene
    difficulty: int = 0      # int32, optional

    def is_usable(self) -> bool:
        return self.scene_uuid > 0


def _hex(v: int) -> str:
    return f"0x{int(v):X}"


def _ga_module_range(pm) -> Tuple[int, int]:
    for m in pm.list_modules():
        if m.name.lower() == "gameassembly.dll":
            return m.base, m.base + m.size
    return 0, 0


def _scan_uuid_full_heap(pm, scene_uuid: int, *,
                         n_workers: int = 8) -> List[int]:
    """Cython AVX2 scan for i64 == scene_uuid. Returns absolute addresses."""
    needle = int(scene_uuid) & 0xFFFFFFFFFFFFFFFF
    regions = [r for r in pm.iter_regions(only_readable=True, only_private=True)
               if r.size <= MAX_REGION_SIZE]
    results: List[int] = []
    lock = threading.Lock()

    def scan_one(region):
        buf = pm.read_bytes(region.base, region.size)
        if buf is None:
            return
        offs = _cy.find_aligned_u64(buf, needle)
        if offs:
            with lock:
                for off in offs:
                    results.append(region.base + off)

    if n_workers > 1:
        with ThreadPoolExecutor(max_workers=n_workers) as ex:
            list(ex.map(scan_one, regions))
    else:
        for r in regions:
            scan_one(r)
    return results


def _find_i32_in_body(body: bytes, value: int) -> List[int]:
    """Return all 4-byte aligned offsets in body where i32 == value."""
    if value <= 0 or value > 0xFFFFFFFF:
        return []
    mask = _cy.narrow_u32_batch(body, int(value) & 0xFFFFFFFF)
    return [i * 4 for i, b in enumerate(mask) if b]


def discover_scene_klass(pm, truth: SceneTruth, *,
                         verbose: bool = True) -> Optional[dict]:
    """Find the SceneInfo / SceneManager klass + scene_uuid offset.

    Args:
        pm: StarProcess instance.
        truth: SceneTruth with at least scene_uuid set; dungeon_id is highly
               recommended for disambiguation.

    Returns:
        dict suitable for anchors.json scene_manager block, or None.
    """
    t0 = time.time()
    if not truth.is_usable():
        if verbose:
            print(f"[scene-discovery] truth missing scene_uuid")
        return None
    if verbose:
        print(f"[scene-discovery] anchor: scene_uuid={truth.scene_uuid} "
              f"dungeon_id={truth.dungeon_id} scene_id={truth.scene_id}")

    ga_base, ga_end = _ga_module_range(pm)
    if not ga_end and verbose:
        print(f"[scene-discovery] WARN: GameAssembly.dll not found")

    t_scan = time.time()
    hits = _scan_uuid_full_heap(pm, truth.scene_uuid, n_workers=8)
    if verbose:
        print(f"[scene-discovery] scene_uuid scan: {len(hits)} hits "
              f"({time.time()-t_scan:.2f}s, AVX2)")
    if not hits:
        return None

    # Per-candidate analysis: walk back, validate klass, find dungeon_id offset.
    # We collect all (klass_ptr, scene_uuid_off, dungeon_id_off) signatures
    # and pick the one that has a klass in GA AND (if available) finds
    # dungeon_id within the body.
    candidates: List[dict] = []
    for uid_addr in hits:
        for k1 in DEFAULT_UID_OFF_TRIES:
            obj_base = uid_addr - k1
            if obj_base < 0x10000:
                continue
            klass_blob = pm.read_bytes(obj_base, 8)
            if not klass_blob:
                continue
            klass_ptr = int.from_bytes(klass_blob, "little")
            if ga_end:
                if not (ga_base <= klass_ptr < ga_end):
                    continue
            body = pm.read_bytes(obj_base, DEFAULT_BODY_BYTES)
            if not body or len(body) < 0x40:
                continue

            # Try to find dungeon_id (i32) in body — narrows the right object
            dungeon_offs = _find_i32_in_body(body, truth.dungeon_id) if truth.dungeon_id else []
            scene_id_offs = _find_i32_in_body(body, truth.scene_id) if truth.scene_id else []
            difficulty_offs = _find_i32_in_body(body, truth.difficulty) if truth.difficulty else []
            layer_offs = _find_i32_in_body(body, truth.layer) if truth.layer else []

            # Score this candidate: how many ground-truth fields did we find?
            score = (
                (1 if dungeon_offs else 0)
                + (1 if scene_id_offs else 0)
                + (1 if difficulty_offs else 0)
                + (1 if layer_offs else 0)
            )

            candidates.append({
                "obj_addr": obj_base,
                "klass_ptr": klass_ptr,
                "scene_uuid_off": k1,
                "dungeon_id_offs": dungeon_offs,
                "scene_id_offs": scene_id_offs,
                "difficulty_offs": difficulty_offs,
                "layer_offs": layer_offs,
                "score": score,
            })

    if not candidates:
        if verbose:
            print(f"[scene-discovery] no candidate body had a GA-klass")
        return None

    # Pick the highest-scoring candidate. Tie-break: closest k1 to 0x10
    # (CharSerialize.SceneData layout; scene_uuid usually first field).
    candidates.sort(key=lambda c: (-c["score"], abs(c["scene_uuid_off"] - 0x10)))
    best = candidates[0]
    if best["score"] == 0 and len(candidates) > 1:
        if verbose:
            print(f"[scene-discovery] WARN: no body-side validation hits, "
                  f"picking highest-K1-quality candidate")

    klass_ptr = best["klass_ptr"]
    scene_uuid_off = best["scene_uuid_off"]
    obj_addr = best["obj_addr"]
    dungeon_id_off = (best["dungeon_id_offs"][0]
                      if best["dungeon_id_offs"] else -1)
    scene_id_off = (best["scene_id_offs"][0]
                    if best["scene_id_offs"] else -1)
    layer_off = (best["layer_offs"][0]
                 if best["layer_offs"] else -1)

    elapsed = time.time() - t0
    if verbose:
        print(f"[scene-discovery] WINNER (score={best['score']}/4):")
        print(f"   obj_addr        = 0x{obj_addr:X}")
        print(f"   klass_ptr       = 0x{klass_ptr:X}")
        print(f"   scene_uuid_off  = 0x{scene_uuid_off:X}")
        print(f"   dungeon_id_off  = {('0x%X' % dungeon_id_off) if dungeon_id_off >= 0 else 'unknown'}")
        print(f"   scene_id_off    = {('0x%X' % scene_id_off) if scene_id_off >= 0 else 'unknown'}")
        print(f"   layer_off       = {('0x%X' % layer_off) if layer_off >= 0 else 'unknown'}")
        print(f"   total cands     = {len(candidates)}")
        print(f"   elapsed         = {elapsed:.2f}s")

    return {
        "obj_addr": _hex(obj_addr),
        "klass_ptr": _hex(klass_ptr),
        "scene_uuid_off": int(scene_uuid_off),
        "dungeon_id_off": int(dungeon_id_off),
        "scene_id_off": int(scene_id_off),
        "layer_off": int(layer_off),
        "elapsed_s": elapsed,
        "score": int(best["score"]),
        "n_candidates": len(candidates),
    }


def persist_scene_anchor(anchors_path: str, payload: dict) -> None:
    """Write scene_manager block into anchors.json (schema v2)."""
    try:
        with open(anchors_path, "r", encoding="utf-8") as f:
            anchors = json.load(f)
    except (OSError, json.JSONDecodeError):
        anchors = {}
    sl = anchors.setdefault("smart_locator", {})
    nested = sl.setdefault("anchors", {})
    nested["scene_manager"] = payload
    sl["schema_version"] = max(2, int(sl.get("schema_version", 0) or 0))
    sl["scene_manager_set_at"] = time.time()
    with open(anchors_path, "w", encoding="utf-8") as f:
        json.dump(anchors, f, ensure_ascii=False, indent=2)


__all__ = [
    "SceneTruth",
    "discover_scene_klass",
    "persist_scene_anchor",
]
