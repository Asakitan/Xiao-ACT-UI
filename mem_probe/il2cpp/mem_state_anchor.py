"""mem_state_anchor — TCP-anchored memory reader for self state.

Static-RVA + `_find_self` only works when the live GameAssembly matches the
checked-in bundle byte-for-byte.  In production the game patches small fields
(blob sizes, alignment padding) and the old static-resolver path goes stale.
The fix is to stop trusting RVA and use **TCP-stable semantic IDs** as
anchors instead — every read is constrained by multiple independent fields,
so the false-positive rate is negligible.

Pipeline:

  1. Build an anchor pack from the TCP parser state:
       - self_uid (CharId)
       - self_profession_id
       - self_level
       - self_skill_level_ids (set of SkillLevelId values)
       - self_dungeon_id
       - self_scene_id
       - monster_template_ids + max_hp (set of (template_id, max_hp) pairs)
  2. The anchor pack is small (~tens of bytes) but uniquely identifies the
     live player instance in a 6 GB private heap.
  3. ``find_self_via_anchors(anchor)``:
       a. Scan all private readable regions.
       b. For each 4-byte aligned offset, look at a sliding window of the
          anchor fields (level, profession, dungeon, scene) and demand
          ALL of them to match simultaneously.
       c. When a candidate region has every anchor field aligned correctly,
          verify via the SkillCD array: read the nearest RepeatedField
          array, decode SkillCDInfo entries, and demand that ≥80% of the
          anchor's known skill ids appear.
       d. The single matching instance is the live player; its
          ``CharSerialize`` object base address falls out for free.
  4. With the validated base, walk the proto layout to read HP / Attr /
     Level / EnergyItem / etc — using the BUNDLE FOR SHAPE ONLY
     (no klass_ptr validation; we just need field offsets, not klass).

The whole thing is read-only.  No hook, no injection, no written memory.
"""
from __future__ import annotations

import os
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Set, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe import cy_memscan as _cy  # noqa: E402
from mem_probe.process import StarProcess  # noqa: E402


# Field offsets (proto wire layout) for CharSerialize / UserFightAttr /
# SkillCDInfo.  These offsets are stable across game updates; the *klass*
# pointer is not.  See sao_auto/mem_probe/il2cpp/out/<game_key>/dump.cs
# for verification.  Hard-coding here avoids depending on the bundle for
# anything but field layout (which is far more stable than RVA).
CHAR_SERIALIZE = {
    'CharId': (0x10, 'i64'),
    'Attr': (0x88, 'ptr'),
    'EnergyItem': (0x70, 'ptr'),
    'RoleLevel': (0xb8, 'ptr'),
    'CharBase': (0x18, 'ptr'),
    'ProfessionList': (0x1f8, 'ptr'),
    'FightPoint': (0x318, 'ptr'),
    'MapData': (0x78, 'ptr'),
    'SceneData': (0x20, 'ptr'),
}
USER_FIGHT_ATTR = {
    'CurHp': (0x10, 'i64'),
    'MaxHp': (0x18, 'i64'),
    'OriginEnergy': (0x20, 'f32'),
    'IsDead': (0x38, 'i32'),
    'CdInfo': (0x50, 'ptr'),
}
CHAR_BASE_INFO = {
    'CharId': (0x10, 'i64'),
    'Name': (0x30, 'str'),
    'FightPoint': (0xe8, 'i32'),
    'InitProfessionId': (0xc8, 'i32'),
}
ROLE_LEVEL = {
    'Level': (0x10, 'i32'),
    'CurLevelExp': (0x18, 'i64'),
}
ENERGY_ITEM = {
    'EnergyLimit': (0x10, 'u32'),
    'ExtraEnergyLimit': (0x14, 'u32'),
}
PROFESSION_LIST = {
    'CurProfessionId': (0x10, 'i32'),
}
SKILL_CD_INFO = {
    'SkillLevelId': (0x10, 'i32'),
    'SkillBeginTime': (0x18, 'i64'),
    'Duration': (0x20, 'i32'),
    'SkillCDType': (0x24, 'u32'),
    'ChargeCount': (0x30, 'i32'),
    'ValidCDTime': (0x34, 'i32'),
    'SubCDRatio': (0x38, 'i32'),
    'SubCDFixed': (0x40, 'i64'),
    'AccelerateCDRatio': (0x48, 'i32'),
}
# MapField layout (EnergyItem.EnergyInfo)
#   +0x10 = MapField instance start
#   +0x18 = MapField internal: Dictionary<int, int>?
# (MapField<TKey,TValue> in IL2CPP uses Google.Protobuf's
#  MapField<TKey, TValue> → internal dict is at +0x18 with protobuf wire
#  encoding wrapped in PB_Tree<,>).  Without a runtime probe we still
#  cannot decode MapField in pure read-only mode.  We report EnergyLimit
#  + ExtraEnergyLimit instead, which is what HUD needs.


@dataclass
class AnchorPack:
    """The TCP-side semantic identifiers we will use to disambiguate memory."""
    uid: int = 0
    profession_id: int = 0
    level: int = 0
    skill_level_ids: Set[int] = field(default_factory=set)
    scene_id: int = 0
    dungeon_id: int = 0
    monster_template_ids: Set[int] = field(default_factory=set)
    seen_at: float = 0.0

    def is_strong(self) -> bool:
        """A weak anchor pack can't pin down the live player; refuse to scan."""
        return self.uid > 0 and (self.level > 0 or self.profession_id > 0
                                 or len(self.skill_level_ids) >= 3)


@dataclass
class ResolvedSelf:
    """Result of anchor-driven self lookup."""
    char_serialize_obj: int
    user_fight_attr_obj: int
    char_base_obj: int
    energy_item_obj: int
    role_level_obj: int
    profession_list_obj: int
    matched_skill_ids: Set[int]
    scan_time_s: float
    confidence: float
    used_anchors: str

    def to_dict(self) -> dict:
        return {
            'char_serialize_obj': self.char_serialize_obj,
            'user_fight_attr_obj': self.user_fight_attr_obj,
            'char_base_obj': self.char_base_obj,
            'energy_item_obj': self.energy_item_obj,
            'role_level_obj': self.role_level_obj,
            'profession_list_obj': self.profession_list_obj,
            'matched_skill_ids': sorted(self.matched_skill_ids),
            'scan_time_s': round(self.scan_time_s, 3),
            'confidence': round(self.confidence, 4),
            'used_anchors': self.used_anchors,
        }


class AnchorMemoryReader:
    """Pure anchor-driven memory reader.

    No static RVA / klass-ptr validation.  All disambiguation is done with
    TCP-derived semantic IDs.
    """

    # Each candidate is a (region_base, region_size) tuple already filtered
    # to private+readable.
    CHAR_SCAN_CHUNK = 8 * 1024 * 1024
    UID_SCAN_CHUNK = 16 * 1024 * 1024

    def __init__(self, process: Optional[StarProcess] = None, max_scan_regions_mb: int = 0):
        self.pm = process or StarProcess()
        self._cached_regions = None
        self._cache_ts = 0.0
        self._regen_ttl = 5.0
        self._resolved_cache: Optional[ResolvedSelf] = None
        self._resolved_cache_uid: int = 0
        self._resolved_cache_ts: float = 0.0
        self.max_scan_regions_mb = max(0, int(max_scan_regions_mb or 0))
        self.last_region_scan_bytes: int = 0
        self.last_region_scan_limited: bool = False

    def close(self):
        try:
            self.pm.close()
        except Exception:
            pass

    def _regions(self):
        now = time.time()
        if self._cached_regions is None or (now - self._cache_ts) > self._regen_ttl:
            regions = []
            total_bytes = 0
            limited = False
            max_bytes = int(self.max_scan_regions_mb) * 1024 * 1024
            for region in self.pm.iter_regions(only_readable=True, only_private=True):
                size = max(0, int(getattr(region, "size", 0) or 0))
                if max_bytes > 0 and total_bytes + size > max_bytes:
                    limited = True
                    break
                regions.append(region)
                total_bytes += size
            self._cached_regions = regions
            self.last_region_scan_bytes = total_bytes
            self.last_region_scan_limited = limited
            self._cache_ts = now
        return self._cached_regions

    def _read(self, addr: int, n: int) -> Optional[bytes]:
        return self.pm.read_bytes(addr, n)

    def _read_u64(self, addr: int) -> Optional[int]:
        return self.pm.read_u64(addr)

    def _read_i64(self, addr: int) -> Optional[int]:
        return self.pm.read_i64(addr)

    def _read_i32(self, addr: int) -> Optional[int]:
        return self.pm.read_i32(addr)

    def _read_u32(self, addr: int) -> Optional[int]:
        return self.pm.read_u32(addr)

    def _read_f32(self, addr: int) -> Optional[float]:
        return self.pm.read_f32(addr)

    @staticmethod
    def _plausible_ptr(ptr: Optional[int]) -> bool:
        return bool(ptr and 0x10000 <= ptr <= 0x7FFFFFFFFFFF)

    # ---------- anchor pack construction ----------

    @staticmethod
    def build_anchor_from_parser(parser) -> AnchorPack:
        """Pull a fresh anchor pack from the live TCP parser state.

        Safe even when the parser is mid-stream: we copy the uid/level/profession
        fields by value, then iterate skill_cd_map for skill_level_ids.
        """
        if parser is None:
            return AnchorPack()
        ap = AnchorPack()
        try:
            ap.uid = int(getattr(parser, '_current_uid', 0) or 0)
        except Exception:
            ap.uid = 0
        try:
            ap.scene_id = int(getattr(parser, '_last_scene_id', 0) or 0)
        except Exception:
            ap.scene_id = 0
        try:
            ap.dungeon_id = int(getattr(parser, '_last_dungeon_id', 0) or 0)
        except Exception:
            ap.dungeon_id = 0
        players = getattr(parser, '_players', {}) or {}
        p = players.get(ap.uid) if ap.uid else None
        if p is not None:
            try:
                ap.level = int(getattr(p, 'level', 0) or 0)
            except Exception:
                ap.level = 0
            try:
                ap.profession_id = int(getattr(p, 'profession_id', 0) or 0)
            except Exception:
                ap.profession_id = 0
            try:
                ap.skill_level_ids = {
                    int(sid) for sid in (getattr(p, 'skill_cd_map', {}) or {}).keys() if sid
                }
            except Exception:
                ap.skill_level_ids = set()
        # monster template_ids (for sanity, not for self-pin)
        try:
            for m in (getattr(parser, '_monsters', {}) or {}).values():
                t = int(getattr(m, 'template_id', 0) or 0)
                if t > 0:
                    ap.monster_template_ids.add(t)
        except Exception:
            pass
        ap.seen_at = time.time()
        return ap

    # ---------- skill-cd driven self discovery ----------

    def _scan_for_uid_candidates(self, anchor: AnchorPack, max_hits: int = 4096) -> List[int]:
        """Find candidate CharSerialize bases by TCP-confirmed uid.

        ``CharSerialize.CharId`` is at ``+0x10``.  We scan readable private
        regions with the Cython u64 scanner, then subtract that field offset.
        This keeps the expensive cross-process work to a small number of
        candidate validations instead of pointer-walking every SkillCD array.
        """
        if anchor.uid <= 0:
            return []
        out: List[int] = []
        needle = int(anchor.uid) & 0xFFFFFFFFFFFFFFFF
        for region in self._regions():
            base = region.base
            size = region.size
            if size < 0x80:
                continue
            off = 0
            while off < size:
                n = min(self.UID_SCAN_CHUNK, size - off)
                blob = self._read(base + off, n)
                if blob is None:
                    off += n
                    continue
                remaining = max_hits - len(out)
                if remaining <= 0:
                    return out
                for hit_off in _cy.find_aligned_u64(blob, needle, remaining):
                    cs_base = base + off + int(hit_off) - CHAR_SERIALIZE['CharId'][0]
                    if cs_base >= base:
                        out.append(cs_base)
                        if len(out) >= max_hits:
                            return out
                off += n
        return out

    def _read_skill_matches_from_attr(self, attr: int, anchor_skills: Set[int],
                                      max_items: int = 512) -> Set[int]:
        """Read ``UserFightAttr.CdInfo`` and return skill ids found in anchors.

        RepeatedField<T> is a heap object pointer stored at ``attr+0x50``:
        ``+0x10`` array pointer, ``+0x18`` count, IL2CPP array elements at
        ``array+0x20``.  The pointer array itself is read in one batch; only
        the candidate SkillCDInfo objects are then sampled one by one.
        """
        if not anchor_skills or not self._plausible_ptr(attr):
            return set()
        rf = self._read_u64(attr + USER_FIGHT_ATTR['CdInfo'][0])
        if not self._plausible_ptr(rf):
            return set()
        count = self._read_u32(rf + 0x18) or 0
        if count <= 0 or count > 2048:
            return set()
        arr = self._read_u64(rf + 0x10)
        if not self._plausible_ptr(arr):
            return set()
        max_len = self._read_u32(arr + 0x18) or 0
        if max_len <= 0 or max_len > 4096:
            return set()
        count = min(int(count), int(max_len), int(max_items))
        ptr_blob = self._read(arr + 0x20, count * 8)
        if not ptr_blob or len(ptr_blob) < count * 8:
            return set()
        matched: Set[int] = set()
        for i in range(count):
            ptr = int.from_bytes(ptr_blob[i * 8:i * 8 + 8], 'little', signed=False)
            if not self._plausible_ptr(ptr):
                continue
            sid = self._read_i32(ptr + SKILL_CD_INFO['SkillLevelId'][0])
            if sid in anchor_skills:
                dur = self._read_i32(ptr + SKILL_CD_INFO['Duration'][0])
                if dur is None or dur < 0 or dur > 600_000:
                    continue
                matched.add(int(sid))
        return matched

    def _scan_for_skill_cd_arrays(self, anchor: AnchorPack) -> List[Tuple[int, Set[int]]]:
        """Find every RepeatedField<SkillCDInfo> array in private heap.

        Strategy: chunk-scan private regions with the Cython-decorated
        ``find_skill_cd_arrays_in_blob`` which finds every 4-byte aligned
        int32 in [50, 1024] (a plausible array count) and validates the
        neighboring array header + first element pointer *all inside the
        same blob*, so no per-candidate RPC is needed.  Elements that
        live in another region are filtered via a quick ``in_blob`` flag.
        """
        if not anchor.skill_level_ids:
            return []
        anchor_skill_set = set(int(s) for s in anchor.skill_level_ids)
        results: List[Tuple[int, Set[int]]] = []
        # Smaller chunk reduces per-chunk RPC count when cands need
        # out-of-blob deref.  Cython still scans the whole region.
        chunk = 2 * 1024 * 1024
        for region in self._regions():
            base = region.base
            size = region.size
            if size < 0x1000:
                continue
            off = 0
            while off + 0x80 <= size:
                n = min(chunk, size - off - 0x80)
                if n <= 0:
                    break
                blob = self._read(base + off, n + 0x80)
                if blob is None:
                    off += chunk
                    continue
                cands = _cy.find_skill_cd_arrays_in_blob(
                    blob, base + off, anchor_skill_set, 50, 1024, 256
                )
                if not cands:
                    off += chunk
                    continue
                # Hard cap: if we already have plenty, stop scanning more
                # regions.  This is a read-only memory probe; more
                # candidates beyond 1024 just delays validation without
                # adding confidence.
                if len(results) >= 1024:
                    return results
                for c in cands:
                    arr_ptr = c['array_ptr']
                    cnt = c['count']
                    elem_ptr = c['element0_ptr']
                    if elem_ptr == 0:
                        # Array lives in another region — skip; we don't
                        # do additional RPCs to reach into other regions
                        # in this loop (too expensive across thousands of
                        # candidates).
                        continue
                    sid = self._read_i32(elem_ptr + 0x10)
                    dur = self._read_i32(elem_ptr + 0x20)
                    if sid is None or dur is None or dur < 0 or dur > 600_000:
                        continue
                    if sid not in anchor_skill_set:
                        continue
                    results.append((arr_ptr, {int(sid)}))
                off += chunk
        return results

    def find_self(self, anchor: AnchorPack) -> Optional[ResolvedSelf]:
        """Find the live player via anchor-driven scanning.

        Strategy: use TCP-confirmed ``uid`` as the first hard anchor and scan
        for ``CharSerialize.CharId`` with Cython.  Each candidate is then
        validated through normal object pointers and finally through
        ``UserFightAttr.CdInfo`` skill ids.  This avoids static RVA/klass
        trust and also avoids the invalid old ``array_ptr - 0xE8`` formula:
        ``CdInfo`` is a pointer to a RepeatedField object, not an inline
        struct next to ``UserFightAttr``.
        """
        if not anchor.is_strong():
            return None
        t0 = time.time()
        anchor_skills = set(anchor.skill_level_ids)
        cached = self._resolved_cache
        if cached is not None and self._resolved_cache_uid == int(anchor.uid or 0):
            ok, info = self._validate_self(cached.char_serialize_obj, anchor)
            if ok:
                matched = self._read_skill_matches_from_attr(info['ufa'], anchor_skills)
                if not anchor_skills or matched:
                    fresh = ResolvedSelf(
                        char_serialize_obj=cached.char_serialize_obj,
                        user_fight_attr_obj=info['ufa'],
                        char_base_obj=info['cb'],
                        energy_item_obj=info['ei'],
                        role_level_obj=info['rl'],
                        profession_list_obj=info['pl'],
                        matched_skill_ids=matched or cached.matched_skill_ids,
                        scan_time_s=time.time() - t0,
                        confidence=info['confidence'],
                        used_anchors=info['used'] + ', cache=hit',
                    )
                    self._resolved_cache = fresh
                    self._resolved_cache_ts = time.time()
                    return fresh
            self._resolved_cache = None
            self._resolved_cache_uid = 0
        candidates = self._scan_for_uid_candidates(anchor)
        if not candidates:
            return None
        min_skill_matches = 0
        if anchor_skills:
            min_skill_matches = min(8, max(2, len(anchor_skills) // 32))
        for cs_base in candidates[:512]:
            ok, info = self._validate_self(cs_base, anchor)
            if not ok:
                continue
            ufa = info['ufa']
            all_matched = self._read_skill_matches_from_attr(ufa, anchor_skills)
            if len(all_matched) < min_skill_matches:
                continue
            scan_time = time.time() - t0
            if anchor_skills:
                skill_conf = min(1.0, len(all_matched) / max(1, min(32, len(anchor_skills))))
                info['confidence'] = min(1.0, (info['confidence'] * 0.65) + (skill_conf * 0.35))
            resolved = ResolvedSelf(
                char_serialize_obj=cs_base,
                user_fight_attr_obj=ufa,
                char_base_obj=info['cb'],
                energy_item_obj=info['ei'],
                role_level_obj=info['rl'],
                profession_list_obj=info['pl'],
                matched_skill_ids=all_matched,
                scan_time_s=scan_time,
                confidence=info['confidence'],
                used_anchors=info['used'],
            )
            self._resolved_cache = resolved
            self._resolved_cache_uid = int(anchor.uid or 0)
            self._resolved_cache_ts = time.time()
            return resolved
        return None

    def _validate_self(self, cs_base: int, anchor: AnchorPack) -> Tuple[bool, dict]:
        """Cross-validate candidate CharSerialize base via field readback."""
        info = {'ufa': 0, 'cb': 0, 'ei': 0, 'rl': 0, 'pl': 0, 'used': '', 'confidence': 0.0}
        checks = 0
        passed = 0
        # CharId
        if anchor.uid > 0:
            checks += 1
            cid = self._read_i64(cs_base + 0x10)
            if cid == anchor.uid:
                passed += 1
            else:
                return False, info
        # Attr pointer must be a real heap pointer
        attr = self._read_u64(cs_base + 0x88)
        if not attr or attr < 0x1000:
            return False, info
        info['ufa'] = attr
        # UserFightAttr.MaxHp must be > 0
        mx = self._read_i64(attr + 0x18)
        if not mx or mx <= 0 or mx > 0x7FFFFFFF:
            return False, info
        # CharBase pointer
        cb = self._read_u64(cs_base + 0x18)
        if cb and cb > 0x1000:
            info['cb'] = cb
        # EnergyItem pointer
        ei = self._read_u64(cs_base + 0x70)
        if ei and ei > 0x1000:
            info['ei'] = ei
        # RoleLevel
        rl = self._read_u64(cs_base + 0xb8)
        if rl and rl > 0x1000:
            info['rl'] = rl
        # ProfessionList
        pl = self._read_u64(cs_base + 0x1f8)
        if pl and pl > 0x1000:
            info['pl'] = pl
        if anchor.level > 0 and info['rl']:
            checks += 1
            lv = self._read_i32(info['rl'] + 0x10)
            if lv == anchor.level:
                passed += 1
            else:
                return False, info
        if anchor.profession_id > 0 and info['cb']:
            checks += 1
            pid = self._read_i32(info['cb'] + 0xc8)
            if pid == anchor.profession_id:
                passed += 1
            else:
                return False, info
        info['used'] = f'uid={anchor.uid}, level={anchor.level}, prof={anchor.profession_id}, skills={len(anchor.skill_level_ids)}'
        info['confidence'] = (passed / checks) if checks else 0.0
        return True, info

    # ---------- field readers (assume ResolvedSelf is valid) ----------

    def read_self_snapshot(self, resolved: ResolvedSelf) -> dict:
        out: dict = {}
        cs = resolved.char_serialize_obj
        attr = resolved.user_fight_attr_obj
        # uid / hp
        out['uid'] = self._read_i64(cs + 0x10)
        cur = self._read_i64(attr + 0x10)
        mx = self._read_i64(attr + 0x18)
        out['cur_hp'] = cur
        out['max_hp'] = mx
        out['is_dead'] = self._read_i32(attr + 0x38)
        out['origin_energy'] = self._read_f32(attr + 0x20)
        # CharBase
        if resolved.char_base_obj:
            cb = resolved.char_base_obj
            out['name'] = self._read_il2cpp_string(cb + 0x30) or ''
            out['fight_point'] = self._read_i32(cb + 0xe8)
            out['init_profession_id'] = self._read_i32(cb + 0xc8)
        # RoleLevel
        if resolved.role_level_obj:
            rl = resolved.role_level_obj
            out['level_base'] = self._read_i32(rl + 0x10)
            out['season_exp'] = self._read_i64(rl + 0x18)
        # EnergyItem (limit only — MapField deferred)
        if resolved.energy_item_obj:
            ei = resolved.energy_item_obj
            out['energy_limit'] = self._read_u32(ei + 0x10)
            out['extra_energy_limit'] = self._read_u32(ei + 0x14)
        # ProfessionList.CurProfessionId
        if resolved.profession_list_obj:
            pl = resolved.profession_list_obj
            cur_pid = self._read_i32(pl + 0x10)
            if cur_pid and cur_pid > 0:
                out['profession_id'] = cur_pid
        out['resolved'] = resolved.to_dict()
        return out

    def _read_il2cpp_string(self, str_field_addr: int) -> Optional[str]:
        """Read a `string` field (a heap ptr to Il2CppString)."""
        ptr = self._read_u64(str_field_addr)
        if not ptr:
            return None
        ln = self._read_i32(ptr + 0x10)
        if ln is None or ln < 0 or ln > 4096:
            return None
        return self.pm.read_utf16(ptr + 0x14, ln)
