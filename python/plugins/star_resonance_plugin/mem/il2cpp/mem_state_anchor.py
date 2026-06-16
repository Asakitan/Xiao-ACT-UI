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
from typing import Dict, Iterable, Iterator, List, Optional, Set, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe import cy_memscan as _cy  # noqa: E402
from mem_probe.process import GameProcess as StarProcess  # noqa: E402
from plugins.star_resonance_plugin.mem.cy_combat import find_skill_cd_arrays_in_blob as _find_skill_cd  # noqa: E402


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
# SceneData (CharSerialize.SceneData -> current map/scene); TypeDefIndex 11418.
SCENE_DATA = {
    'MapId': (0x10, 'u32'),
    'LevelMapId': (0x40, 'u32'),
    'LineId': (0x80, 'u32'),
}

# Conservative self-player plausibility guardrails used when TCP only gives a
# uid.  These are deliberately broad enough for future gear/level growth but
# narrow enough to reject arbitrary u64 hits that happen to equal the uid.
MAX_SELF_HP = 100_000_000
MAX_SELF_LEVEL = 200
MAX_SELF_PROFESSION_ID = 64
MAX_SELF_NAME_LEN = 64
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

    def has_semantic_detail(self) -> bool:
        """Return True when TCP has more than just the uid."""
        return self.level > 0 or self.profession_id > 0 or bool(self.skill_level_ids)


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
    # Half-width of the bounded re-acquisition window around the last-known base.
    # A 4 MB half means a single <=8 MB read, sub-ms in the Cython scanner.
    WINDOW_HALF_BYTES = 4 * 1024 * 1024

    def __init__(self, process: Optional[StarProcess] = None, max_scan_regions_mb: int = 0,
                 *, resolver=None):
        self.pm = process or StarProcess()
        # auto-offset: override each {field:(offset,type)} layout's offset with the
        # dump-resolved offset (by name), keeping the type tag and the literal as
        # fallback. The anchor SCAN still uses TCP semantic ids; this only makes the
        # post-scan proto field reads self-heal on a game patch.
        self._init_layout(resolver)
        self._cached_regions = None
        self._cached_ranked = None
        self._cache_ts = 0.0
        self._regen_ttl = 5.0
        self._scan_scratch = None        # reused zero-copy uid-scan buffer
        self._resolved_cache: Optional[ResolvedSelf] = None
        self._resolved_cache_uid: int = 0
        self._resolved_cache_ts: float = 0.0
        # klass-sentinel SESSION cache (in-memory only, never persisted to disk;
        # the base address changes every game launch — see hybrid-base-discovery
        # skill: no blindly-trusted cross-restart anchors).
        self._resolved_cache_klass: int = 0
        self._resolved_cache_pid: int = 0
        self._resolved_cache_ga_base: int = 0
        # Window-scan hints survive a cache drop so a moved object can be re-found
        # cheaply near its previous neighbourhood before escalating to a full scan.
        self._last_known_base: int = 0
        self._last_known_uid: int = 0
        self.max_scan_regions_mb = max(0, int(max_scan_regions_mb or 0))
        self.last_region_scan_bytes: int = 0
        self.last_region_scan_limited: bool = False
        # Telemetry for the data-source health panel.
        self.last_scan_mode: str = "none"   # cache-hit | window-hit | full-scan | miss
        self.last_scan_time_s: float = 0.0
        self.last_confidence: float = 0.0

    def _init_layout(self, resolver) -> None:
        """Resolve the proto field layouts by name (live memory -> dump -> literal)."""
        from plugins.star_resonance_plugin.mem.il2cpp import auto_offsets as _ao

        def _R(literal_map, class_name):
            out = {}
            for fname, (off, typ) in literal_map.items():
                g = _ao.offset(resolver, class_name, fname)
                use = g if (g is not None and (g != 0 or off == 0)) else off
                out[fname] = (int(use), typ)
            return out

        self.CHAR_SERIALIZE = _R(CHAR_SERIALIZE, "Zproto.CharSerialize")
        self.USER_FIGHT_ATTR = _R(USER_FIGHT_ATTR, "Zproto.UserFightAttr")
        self.CHAR_BASE_INFO = _R(CHAR_BASE_INFO, "Zproto.CharBaseInfo")
        self.ROLE_LEVEL = _R(ROLE_LEVEL, "Zproto.RoleLevel")
        self.ENERGY_ITEM = _R(ENERGY_ITEM, "Zproto.EnergyItem")
        self.PROFESSION_LIST = _R(PROFESSION_LIST, "Zproto.ProfessionList")
        self.SKILL_CD_INFO = _R(SKILL_CD_INFO, "Zproto.SkillCDInfo")
        self.SCENE_DATA = _R(SCENE_DATA, "Zproto.SceneData")

    def close(self):
        try:
            self.pm.close()
        except Exception:
            pass

    def _ga_base(self) -> int:
        """GameAssembly/main-module base for the current process (0 on failure).

        Part of the session-cache invalidation set: a base change means the game
        relaunched (ASLR), so any cached object address is stale.
        """
        try:
            mod = self.pm.main_module()
            return int(getattr(mod, "base", 0) or 0)
        except Exception:
            return 0

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
            self._cached_ranked = None
            self.last_region_scan_bytes = total_bytes
            self.last_region_scan_limited = limited
            self._cache_ts = now
        return self._cached_regions

    @staticmethod
    def _score_region(region) -> float:
        """Heuristic score: 'most-likely IL2CPP GC heap' first.

        bdwgc super-blocks are large, committed, private, read-write regions.
        Pure arithmetic on already-enumerated fields — no extra RPC. Higher =
        scanned earlier; full-heap coverage is preserved (ranking only reorders).
        """
        size = max(0, int(getattr(region, "size", 0) or 0))
        if bool(getattr(region, "is_image", False)):
            return 0.0
        # Size band dominates (cap at 256 MB so a few huge regions don't starve).
        size_score = min(size, 256 * 1024 * 1024) / float(256 * 1024 * 1024)
        score = size_score * 3.0
        if size >= 4 * 1024 * 1024:
            score += 0.5
        # Protection: the managed heap is read-write.
        protect = int(getattr(region, "protect", 0) or 0) & 0xFF
        if protect == 0x04:            # PAGE_READWRITE
            prot_factor = 1.0
        elif protect in (0x02, 0x08):  # READONLY / WRITECOPY
            prot_factor = 0.1
        elif protect & 0xF0:           # any EXECUTE_* — code, not heap
            prot_factor = 0.0001
        else:
            prot_factor = 0.3
        score *= prot_factor
        if size < 64 * 1024:           # tiny: stacks/TLS, deprioritise (not excluded)
            score *= 0.25
        return score

    def _ranked_regions(self):
        """`_regions()` ordered GC-heap-first; cached under the same TTL gate.

        Stable sort → tie-break stays address order. The owning-region search
        and full-heap fallback both still visit every region; this only changes
        the order so a strong-anchor caller validates-and-stops sooner.
        """
        if self._cached_ranked is None:
            self._cached_ranked = sorted(
                list(self._regions()), key=self._score_region, reverse=True
            )
        return self._cached_ranked

    def _owning_region(self, addr: int):
        """Return the cached region containing ``addr`` (or None)."""
        if not addr:
            return None
        for region in self._regions():
            base = int(getattr(region, "base", 0) or 0)
            size = int(getattr(region, "size", 0) or 0)
            if base <= addr < base + size:
                return region
        return None

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

    @staticmethod
    def _plausible_hp(cur: Optional[int], mx: Optional[int]) -> bool:
        if cur is None or mx is None:
            return False
        try:
            cur_i = int(cur)
            mx_i = int(mx)
        except Exception:
            return False
        return 0 <= cur_i <= mx_i <= MAX_SELF_HP and mx_i > 0

    @staticmethod
    def _plausible_level(level: Optional[int]) -> bool:
        try:
            lv = int(level or 0)
        except Exception:
            return False
        return 1 <= lv <= MAX_SELF_LEVEL

    @staticmethod
    def _plausible_profession_id(profession_id: Optional[int]) -> bool:
        try:
            pid = int(profession_id or 0)
        except Exception:
            return False
        return 1 <= pid <= MAX_SELF_PROFESSION_ID

    @staticmethod
    def _plausible_char_name(name: Optional[str]) -> bool:
        if not isinstance(name, str):
            return False
        text = name.strip()
        if not text or len(text) > MAX_SELF_NAME_LEN:
            return False
        return all(ch.isprintable() for ch in text)

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

    def _iter_uid_candidates(self, anchor: AnchorPack, max_hits: int = 4096) -> Iterator[int]:
        """Yield candidate ``CharSerialize`` bases by TCP-confirmed uid, lazily.

        ``CharSerialize.CharId`` is at ``+0x10``.  We scan readable private
        regions with the Cython u64 scanner, then subtract that field offset.
        Yielding region-by-region lets a strong-anchor caller validate-and-stop
        the moment self is found, instead of always sweeping the whole heap to
        collect every uid collision first.
        """
        if anchor.uid <= 0:
            return
        emitted = 0
        needle = int(anchor.uid) & 0xFFFFFFFFFFFFFFFF
        read_into = getattr(self.pm, "read_bytes_into", None)
        if self._scan_scratch is None or len(self._scan_scratch) < self.UID_SCAN_CHUNK:
            self._scan_scratch = bytearray(self.UID_SCAN_CHUNK)
        for region in self._ranked_regions():
            base = region.base
            size = region.size
            if size < 0x80:
                continue
            off = 0
            while off < size:
                n = min(self.UID_SCAN_CHUNK, size - off)
                if read_into is not None:
                    got = read_into(base + off, self._scan_scratch, n)
                    if got <= 0:
                        off += n
                        continue
                    buf = memoryview(self._scan_scratch)[:got]
                else:
                    blob = self._read(base + off, n)
                    if blob is None:
                        off += n
                        continue
                    buf = blob
                    got = n
                remaining = max_hits - emitted
                if remaining <= 0:
                    return
                for hit_off in _cy.find_aligned_u64(buf, needle, remaining):
                    cs_base = base + off + int(hit_off) - self.CHAR_SERIALIZE['CharId'][0]
                    if cs_base >= base:
                        yield cs_base
                        emitted += 1
                        if emitted >= max_hits:
                            return
                off += n

    def _scan_for_uid_candidates(self, anchor: AnchorPack, max_hits: int = 4096) -> List[int]:
        """Eagerly collect all uid candidates (used by the weak-anchor path)."""
        return list(self._iter_uid_candidates(anchor, max_hits))

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
        rf = self._read_u64(attr + self.USER_FIGHT_ATTR['CdInfo'][0])
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
        sid_off = self.SKILL_CD_INFO['SkillLevelId'][0]
        dur_off = self.SKILL_CD_INFO['Duration'][0]
        # Decode the element pointers locally, then read every (SkillLevelId,
        # Duration) pair in ONE batched nogil RPM call instead of 2 single reads
        # per element (this runs on every 2 Hz cache-hit revalidation).
        ptrs = []
        import struct
        for i in range(count):
            ptr = struct.unpack_from('<Q', ptr_blob, i * 8)[0]
            if self._plausible_ptr(ptr):
                ptrs.append(ptr)
        if not ptrs:
            return set()
        matched: Set[int] = set()
        batch = None
        reader = getattr(self.pm, "read_u32_many", None)
        if callable(reader):
            try:
                addrs = []
                for p in ptrs:
                    addrs.append(p + sid_off)
                    addrs.append(p + dur_off)
                batch = reader(addrs)
            except Exception:
                batch = None
        if batch is not None and len(batch) == 2 * len(ptrs):
            for i in range(len(ptrs)):
                sv = batch[2 * i]
                if sv is None:
                    continue
                sid = sv - 0x100000000 if sv >= 0x80000000 else sv
                if sid in anchor_skills:
                    dv = batch[2 * i + 1]
                    if dv is None:
                        continue
                    dur = dv - 0x100000000 if dv >= 0x80000000 else dv
                    if 0 <= dur <= 600_000:
                        matched.add(int(sid))
            return matched
        for ptr in ptrs:
            sid = self._read_i32(ptr + sid_off)
            if sid in anchor_skills:
                dur = self._read_i32(ptr + dur_off)
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
                cands = _find_skill_cd(
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
                # auto-offset (hoisted out of the per-candidate loop): SkillCDInfo
                # SkillLevelId / Duration, dump-resolved with literal fallback.
                _off_sid = self.SKILL_CD_INFO['SkillLevelId'][0]
                _off_dur = self.SKILL_CD_INFO['Duration'][0]
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
                    sid = self._read_i32(elem_ptr + _off_sid)
                    dur = self._read_i32(elem_ptr + _off_dur)
                    if sid is None or dur is None or dur < 0 or dur > 600_000:
                        continue
                    if sid not in anchor_skill_set:
                        continue
                    results.append((arr_ptr, {int(sid)}))
                off += chunk
        return results

    def _build_resolved(self, cs_base: int, info: dict, all_matched: Set[int],
                        anchor_skills: Set[int], t0: float) -> ResolvedSelf:
        conf = info['confidence']
        if anchor_skills:
            skill_conf = min(1.0, len(all_matched) / max(1, min(32, len(anchor_skills))))
            conf = min(1.0, (conf * 0.65) + (skill_conf * 0.35))
        return ResolvedSelf(
            char_serialize_obj=cs_base,
            user_fight_attr_obj=info['ufa'],
            char_base_obj=info['cb'],
            energy_item_obj=info['ei'],
            role_level_obj=info['rl'],
            profession_list_obj=info['pl'],
            matched_skill_ids=all_matched,
            scan_time_s=time.time() - t0,
            confidence=conf,
            used_anchors=info['used'],
        )

    def _validate_candidate(self, cs_base: int, anchor: AnchorPack,
                            anchor_skills: Set[int], min_skill_matches: int,
                            t0: float) -> Optional[ResolvedSelf]:
        """Validate one CharSerialize candidate; shared by full + window scans."""
        ok, info = self._validate_self(cs_base, anchor)
        if not ok:
            return None
        all_matched = self._read_skill_matches_from_attr(info['ufa'], anchor_skills)
        if len(all_matched) < min_skill_matches:
            return None
        return self._build_resolved(cs_base, info, all_matched, anchor_skills, t0)

    # ---------- klass-sentinel SESSION cache (in-memory, PID-scoped) ----------

    def _drop_cache(self) -> None:
        self._resolved_cache = None
        self._resolved_cache_uid = 0
        self._resolved_cache_klass = 0
        # NOTE: keep _last_known_base/_last_known_uid as a window-scan hint.

    def _remember(self, anchor: AnchorPack, resolved: ResolvedSelf) -> None:
        """Store the resolved self into the session cache + klass sentinel.

        Honors the "no klass → no anchor" rule: if obj+0 is not a plausible heap
        pointer we keep only the window hint and refuse to cache a base we cannot
        validate. Never persisted to disk — the base is session-volatile (ASLR).
        """
        cs = int(resolved.char_serialize_obj or 0)
        self._last_known_base = cs
        self._last_known_uid = int(anchor.uid or 0)
        klass = self._read_u64(cs)
        if not self._plausible_ptr(klass):
            self._drop_cache()
            return
        self._resolved_cache = resolved
        self._resolved_cache_uid = int(anchor.uid or 0)
        self._resolved_cache_ts = time.time()
        self._resolved_cache_klass = int(klass)
        try:
            self._resolved_cache_pid = int(getattr(self.pm, "pid", 0) or 0)
        except Exception:
            self._resolved_cache_pid = 0
        self._resolved_cache_ga_base = self._ga_base()

    def _cache_check(self, anchor: AnchorPack, anchor_skills: Set[int],
                     min_skill_matches: int, t0: float) -> Optional[ResolvedSelf]:
        cached = self._resolved_cache
        if cached is None or self._resolved_cache_uid != int(anchor.uid or 0):
            return None
        # PID guard — a relaunch is a different process.
        if self._resolved_cache_pid:
            try:
                if int(getattr(self.pm, "pid", 0) or 0) != self._resolved_cache_pid:
                    self._drop_cache(); return None
            except Exception:
                self._drop_cache(); return None
        # ga_base guard — module rebased (ASLR / relaunch).
        if self._resolved_cache_ga_base and self._ga_base() != self._resolved_cache_ga_base:
            self._drop_cache(); return None
        # KLASS SENTINEL — obj+0 must still equal the klass captured at resolve.
        klass = self._read_u64(cached.char_serialize_obj)
        if not klass or int(klass) != self._resolved_cache_klass:
            self._drop_cache(); return None
        # Semantic re-validation (CharId / HP / level / profession) + skill match.
        ok, info = self._validate_self(cached.char_serialize_obj, anchor)
        if not ok:
            self._drop_cache(); return None
        matched = self._read_skill_matches_from_attr(info['ufa'], anchor_skills)
        if anchor_skills and not matched:
            self._drop_cache(); return None
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
        self._last_known_base = int(cached.char_serialize_obj or 0)
        return fresh

    def reacquire_self(self, anchor: AnchorPack, hint_addr: int, *,
                       window_bytes: Optional[int] = None) -> Optional[ResolvedSelf]:
        """Bounded re-acquisition: scan a window around ``hint_addr``.

        Used when the session cache is dropped (object moved within the heap) so
        self is re-found cheaply near its previous neighbourhood before paying a
        full ranked scan. Read-only; returns the first validated candidate, else
        None (caller escalates to the ranked full scan).
        """
        if anchor.uid <= 0 or not self._plausible_ptr(hint_addr):
            return None
        region = self._owning_region(hint_addr)
        if region is None:
            return None
        half = int(window_bytes if window_bytes else self.WINDOW_HALF_BYTES)
        r_base = int(region.base)
        r_end = r_base + int(region.size)
        lo = max(r_base, hint_addr - half) & ~0x7
        hi = min(r_end, hint_addr + half)
        n = hi - lo
        if n < 0x80:
            return None
        blob = self._read(lo, n)
        if blob is None:
            return None
        anchor_skills = set(anchor.skill_level_ids)
        min_skill_matches = 0
        if anchor_skills:
            min_skill_matches = min(8, max(2, len(anchor_skills) // 32))
        t0 = time.time()
        needle = int(anchor.uid) & 0xFFFFFFFFFFFFFFFF
        for hit_off in _cy.find_aligned_u64(blob, needle, 256):
            cs_base = lo + int(hit_off) - self.CHAR_SERIALIZE['CharId'][0]
            if cs_base < r_base:
                continue
            resolved = self._validate_candidate(cs_base, anchor, anchor_skills,
                                                 min_skill_matches, t0)
            if resolved is not None:
                return resolved
        return None

    def _finish_scan(self, anchor: AnchorPack, resolved: ResolvedSelf,
                     mode: str, t0: float) -> ResolvedSelf:
        self._remember(anchor, resolved)
        self.last_scan_mode = mode
        self.last_scan_time_s = time.time() - t0
        self.last_confidence = float(getattr(resolved, "confidence", 0.0) or 0.0)
        return resolved

    def find_self(self, anchor: AnchorPack) -> Optional[ResolvedSelf]:
        """Find the live player via anchor-driven scanning.

        Order: (1) klass-sentinel session cache, (2) bounded window re-scan
        around the last-known base, (3) GC-heap-first ranked full scan. The uid
        (``CharSerialize.CharId`` at +0x10) is the first hard anchor; each
        candidate is validated through object pointers and ``CdInfo`` skill ids.
        No static RVA/klass trust; no persisted base (session-volatile, ASLR).
        """
        if anchor.uid <= 0:
            self.last_scan_mode = "miss"
            return None
        t0 = time.time()
        anchor_skills = set(anchor.skill_level_ids)
        strong_anchor = anchor.is_strong()
        min_skill_matches = 0
        if anchor_skills:
            min_skill_matches = min(8, max(2, len(anchor_skills) // 32))

        # 1) session cache (klass sentinel + PID/ga_base guards)
        hit = self._cache_check(anchor, anchor_skills, min_skill_matches, t0)
        if hit is not None:
            return self._finish_scan(anchor, hit, "cache-hit", t0)

        # 2) bounded window re-acquisition around the last-known base
        if self._last_known_base and self._last_known_uid == int(anchor.uid or 0):
            win = self.reacquire_self(anchor, self._last_known_base)
            if win is not None:
                return self._finish_scan(anchor, win, "window-hit", t0)

        # 3) GC-heap-first ranked full scan
        if strong_anchor:
            # uid is a plain i64 so a multi-GB heap has thousands of collisions,
            # but a strong anchor's uid+hp+level+profession gate is unique to the
            # live self, so we scan lazily and return on the FIRST validated hit.
            for cs_base in self._iter_uid_candidates(anchor):
                resolved = self._validate_candidate(cs_base, anchor, anchor_skills,
                                                     min_skill_matches, t0)
                if resolved is not None:
                    return self._finish_scan(anchor, resolved, "full-scan", t0)
            self.last_scan_mode = "miss"
            self.last_scan_time_s = time.time() - t0
            return None

        # Weak (uid-only) anchor: no hard level/profession gate, so keep the most
        # plausible full snapshot rather than the first collision.
        best: Optional[ResolvedSelf] = None
        for cs_base in self._iter_uid_candidates(anchor):
            resolved = self._validate_candidate(cs_base, anchor, anchor_skills,
                                                 min_skill_matches, t0)
            if resolved is None:
                continue
            if best is None or resolved.confidence > best.confidence:
                best = resolved
                if resolved.confidence >= 0.95:
                    break
        if best is not None:
            return self._finish_scan(anchor, best, "full-scan", t0)
        self.last_scan_mode = "miss"
        self.last_scan_time_s = time.time() - t0
        return None

    def _validate_self(self, cs_base: int, anchor: AnchorPack) -> Tuple[bool, dict]:
        """Cross-validate candidate CharSerialize base via field readback."""
        info = {'ufa': 0, 'cb': 0, 'ei': 0, 'rl': 0, 'pl': 0, 'used': '', 'confidence': 0.0}
        checks = 0
        passed = 0
        plausibility_score = 0
        plausibility_used: List[str] = []
        # auto-offset: dump-resolved field offsets (literal fallback)
        CS, UFA, CBI, RL, PL = (self.CHAR_SERIALIZE, self.USER_FIGHT_ATTR,
                                self.CHAR_BASE_INFO, self.ROLE_LEVEL, self.PROFESSION_LIST)
        # CharId
        if anchor.uid > 0:
            checks += 1
            cid = self._read_i64(cs_base + CS['CharId'][0])
            if cid == anchor.uid:
                passed += 1
            else:
                return False, info
        # Attr pointer must be a real heap pointer
        attr = self._read_u64(cs_base + CS['Attr'][0])
        if not attr or attr < 0x1000:
            return False, info
        info['ufa'] = attr
        # UserFightAttr HP must be internally sane.  MaxHp alone is not enough:
        # uid-only scans can find arbitrary integers with an adjacent positive
        # qword that looks like MaxHp.
        cur = self._read_i64(attr + UFA['CurHp'][0])
        mx = self._read_i64(attr + UFA['MaxHp'][0])
        if not self._plausible_hp(cur, mx):
            return False, info
        checks += 1
        passed += 1
        plausibility_score += 2
        plausibility_used.append('hp')
        dead = self._read_i32(attr + UFA['IsDead'][0])
        if dead is not None:
            if int(dead) not in (0, 1):
                return False, info
            plausibility_score += 1
            plausibility_used.append('dead')
        # CharBase pointer
        cb = self._read_u64(cs_base + CS['CharBase'][0])
        if self._plausible_ptr(cb):
            info['cb'] = cb
            cb_uid = self._read_i64(cb + CBI['CharId'][0])
            if anchor.uid > 0 and cb_uid == anchor.uid:
                plausibility_score += 2
                plausibility_used.append('char_base.uid')
            name = self._read_il2cpp_string(cb + CBI['Name'][0])
            if self._plausible_char_name(name):
                plausibility_score += 2
                plausibility_used.append('name')
            init_pid = self._read_i32(cb + CBI['InitProfessionId'][0])
            if self._plausible_profession_id(init_pid):
                plausibility_score += 1
                plausibility_used.append('char_base.prof')
        # EnergyItem pointer
        ei = self._read_u64(cs_base + CS['EnergyItem'][0])
        if self._plausible_ptr(ei):
            info['ei'] = ei
        # RoleLevel
        rl = self._read_u64(cs_base + CS['RoleLevel'][0])
        if self._plausible_ptr(rl):
            info['rl'] = rl
            lv_probe = self._read_i32(rl + RL['Level'][0])
            if self._plausible_level(lv_probe):
                plausibility_score += 1
                plausibility_used.append('level')
        # ProfessionList
        pl = self._read_u64(cs_base + CS['ProfessionList'][0])
        if self._plausible_ptr(pl):
            info['pl'] = pl
            cur_pid = self._read_i32(pl + PL['CurProfessionId'][0])
            if self._plausible_profession_id(cur_pid):
                plausibility_score += 1
                plausibility_used.append('profession')
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
        if not anchor.has_semantic_detail():
            # Uid-only mode is allowed only when the object graph itself looks
            # like a real self player: HP is sane and enough identity fields are
            # readable.  This rejected live false positives with garbage HP,
            # empty names, huge levels, and bogus profession ids.
            if plausibility_score < 7:
                return False, info
        info['used'] = (
            f'uid={anchor.uid}, level={anchor.level}, prof={anchor.profession_id}, '
            f'skills={len(anchor.skill_level_ids)}, plausible={"|".join(plausibility_used)}'
        )
        anchor_conf = (passed / checks) if checks else 0.0
        plausibility_conf = min(1.0, plausibility_score / 10.0)
        info['confidence'] = max(anchor_conf, plausibility_conf)
        return True, info

    # ---------- field readers (assume ResolvedSelf is valid) ----------

    def read_self_snapshot(self, resolved: ResolvedSelf) -> dict:
        out: dict = {}
        cs = resolved.char_serialize_obj
        attr = resolved.user_fight_attr_obj
        CS, UFA = self.CHAR_SERIALIZE, self.USER_FIGHT_ATTR
        # uid / hp (auto-offset: dump-resolved field offsets, literal fallback)
        out['uid'] = self._read_i64(cs + CS['CharId'][0])
        cur = self._read_i64(attr + UFA['CurHp'][0])
        mx = self._read_i64(attr + UFA['MaxHp'][0])
        out['cur_hp'] = cur
        out['max_hp'] = mx
        out['is_dead'] = self._read_i32(attr + UFA['IsDead'][0])
        out['origin_energy'] = self._read_f32(attr + UFA['OriginEnergy'][0])
        # CharBase
        if resolved.char_base_obj:
            cb = resolved.char_base_obj
            CBI = self.CHAR_BASE_INFO
            out['name'] = self._read_il2cpp_string(cb + CBI['Name'][0]) or ''
            out['fight_point'] = self._read_i32(cb + CBI['FightPoint'][0])
            out['init_profession_id'] = self._read_i32(cb + CBI['InitProfessionId'][0])
        # RoleLevel
        if resolved.role_level_obj:
            rl = resolved.role_level_obj
            RL = self.ROLE_LEVEL
            out['level_base'] = self._read_i32(rl + RL['Level'][0])
            out['season_exp'] = self._read_i64(rl + RL['CurLevelExp'][0])
        # EnergyItem (limit only — MapField deferred)
        if resolved.energy_item_obj:
            ei = resolved.energy_item_obj
            EI = self.ENERGY_ITEM
            out['energy_limit'] = self._read_u32(ei + EI['EnergyLimit'][0])
            out['extra_energy_limit'] = self._read_u32(ei + EI['ExtraEnergyLimit'][0])
        # ProfessionList.CurProfessionId
        if resolved.profession_list_obj:
            pl = resolved.profession_list_obj
            cur_pid = self._read_i32(pl + self.PROFESSION_LIST['CurProfessionId'][0])
            if cur_pid and cur_pid > 0:
                out['profession_id'] = cur_pid
        # SceneData (current map/scene). Name resolved app-side via the offline
        # scene/dungeon table -> no TCP needed.
        sd = self._read_u64(cs + CS['SceneData'][0])
        if sd:
            SD = self.SCENE_DATA
            mid = self._read_u32(sd + SD['MapId'][0]) or 0
            if mid:
                out['scene_map_id'] = int(mid)
            lvl = self._read_u32(sd + SD['LevelMapId'][0]) or 0
            if lvl:
                out['scene_level_map_id'] = int(lvl)
            line = self._read_u32(sd + SD['LineId'][0]) or 0
            if line:
                out['scene_line_id'] = int(line)
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
