# -*- coding: utf-8 -*-
"""mem_entity_combat - read live combat HP/state from a ZEntity (read-only).

The displayed monster/boss HP lives in the entity's attribute cache:
  ZEntity.attrs_ (@+0x48) -> ZAttrCollection.cacheSlim_._values (@+0x20)
    -> ValueTuple<uint, object[]>[]  (element0.Item2 @ array+0x28 is the object[])
  each object[] slot is a typed ZAttr<T>:
    ZAttr<T> = { bool isDefault_; T value_; object bindWatchers_ } after the 0x10
    il2cpp header -> value_ @ obj+0x14 (Int/Float/Bool) or obj+0x18 (Long/String/ref).

attr_id -> value is decoded structurally from the Burst _indexPart (@attrs+0x18):
KeySegment*[32] (8 attr ids + count each) parallel to int*[32] valueIndices, then the
value index pages into _values (32/page). read_attr_map() returns the full {attr_id:
value} map, so HP/breaking/overdrive/stun/cast are read deterministically by attr id
(== packet_parser.enums.AttrType). The HP invariant remains a fallback in read_hp.

All offsets are field offsets within IL2CPP objects (stable across base/ASLR); the
only version-sensitive inputs are the class layout, taken from the live dump.
"""
from __future__ import annotations
import struct
from typing import Dict, List, Optional, Tuple

try:
    from mem_probe import cy_memscan as _cymem
    _HAS_BATCH = bool(_cymem.has_batch_read())
    _HAS_FULLDECODE = bool(_cymem.has_full_combat_decode())
except Exception:
    _cymem = None
    _HAS_BATCH = False
    _HAS_FULLDECODE = False

# ZEntity / ZAttrCollection field offsets (verified vs dump fdc7111b / 8144ccfd)
ENT_ATTRS_OFF = 0x48        # ZEntity.attrs_ -> ZAttrCollection
COLL_INDEXPART_OFF = 0x18   # ZAttrCacheSlim._indexPart (native Burst index)
COLL_VALUES_OFF = 0x20      # ZAttrCacheSlim._values (ValueTuple<uint,object[]>[])
VALUES_ELEM0_ARR_OFF = 0x28 # _values[0].Item2 (object[]) = values+0x20 + 8
ARRAY_LEN_OFF = 0x18
ARRAY_ELEMS_OFF = 0x20
ATTR_VAL4_OFF = 0x14        # ZAttr<int/float/bool>.value_
ATTR_VAL8_OFF = 0x18        # ZAttr<long/string/ref>.value_
CLASS_NAME_OFF = 0x10       # Il2CppClass.name (char*)

# ZAttrCacheSlim._indexPart layout (from dump.cs struct ZAttrCacheSlim, TypeDefIndex
# 32284): SegmentSize=8, SegmentCount=32, ValueSegmentSize=32. The indexPart is a
# native Burst block; KeySegment*[32] starts at +0x10 (each KeySegment = uint Keys[8]
# @0x0 + int Count @0x20), and the parallel int*[32] valueIndices at +0x110. Lookup:
# attr_id -> (seg,pos) in KeySegments -> vIdx = valueIndices[seg][pos] -> value lives in
# _values[vIdx>>5].Item2[vIdx&31] (ValueSegmentSize=32 paging). Verified live vs HP.
INDEX_KEYSEG_OFF = 0x10
INDEX_VALIDX_OFF = 0x110
INDEX_SEG_COUNT = 32
INDEX_SEG_SIZE = 8
KEYSEG_COUNT_OFF = 0x20
VALUES_TUPLE_STRIDE = 0x10   # sizeof ValueTuple<uint, object[]> in the array
VALUES_TUPLE_ARR_OFF = 0x8   # tuple.Item2 (object[]) within the element
VALUE_PAGE_SIZE = 32         # ValueSegmentSize

# Il2CppString layout (x64): length @+0x10 (int32), UTF-16LE chars @+0x14.
STR_LEN_OFF = 0x10
STR_CHARS_OFF = 0x14
STR_MAX_CHARS = 64           # names are short; cap rejects non-string pointers

# attr ids (== packet_parser.enums.AttrType)
A_NAME = 1                   # AttrType.NAME -> the entity's resolved display-name string
A_HP, A_MAX_HP = 11310, 11320
A_MAX_EXT, A_EXT, A_MAX_STUN, A_STUN = 440, 441, 442, 443
A_OVERDRIVE, A_BREAK_STAGE = 444, 455
A_STOP_TICKING = 453         # AttrStopBreakingBarTickingFlag (break recovery frozen)
A_SKILL_ID = 100             # current cast skill id (present only while casting)
# attrs read every tick for the combat snapshot — only these are resolved (not all 107)
COMBAT_ATTR_IDS = (A_HP, A_MAX_HP, A_BREAK_STAGE, A_OVERDRIVE, A_STUN, A_EXT,
                   A_MAX_EXT, A_STOP_TICKING, A_SKILL_ID)
_TYPE_CHAR = {"LongAttr": "L", "IntAttr": "I", "FloatAttr": "F", "BoolAttr": "B"}

MAX_HP_PLAUSIBLE = 5_000_000_000   # exclude server-time longs (~1.7e12)


def _plaus(p: Optional[int]) -> bool:
    return bool(p and 0x10000 <= p <= 0x7FFFFFFFFFFF)


class EntityCombatReader:
    """Reads HP/state from a ZEntity's attribute cache. ``pm`` needs read_u64/i64/
    u32/i32/bytes."""

    def __init__(self, pm, *, resolver=None):
        self.pm = pm
        # auto-offset: resolve ZEntity.attrs_ + the ZAttrCacheSlim base (cacheSlim_)
        # by name from the dump, literal fallback. _indexPart/_values are the Burst
        # struct's inline fields (inner deltas 0x0/0x8), kept structural. The cython
        # fast path (read_entity_combat_many) carries its own offsets; these drive the
        # Python paths (read_attr_map / read_name_attr / read_combat -> boss-cast loop).
        from mem_probe.il2cpp import auto_offsets as _ao
        ent = _ao.resolve(resolver, "Panda.ZGame.ZEntity", {
            "off_ent_attrs": ("attrs_", ENT_ATTRS_OFF),
        })
        coll = _ao.resolve(resolver, "Panda.ZGame.ZAttrCollection", {
            "cacheslim": ("cacheSlim_", COLL_INDEXPART_OFF),
        })
        self.off_ent_attrs = ent["off_ent_attrs"]
        cs = coll["cacheslim"]
        self.off_coll_indexpart = cs + (COLL_INDEXPART_OFF - COLL_INDEXPART_OFF)  # _indexPart @ inner 0x0
        self.off_coll_values = cs + (COLL_VALUES_OFF - COLL_INDEXPART_OFF)        # _values   @ inner 0x8
        self._klass_name: Dict[int, str] = {}     # klass ptr -> name (session cache)
        # per-entity attr layout cache keyed by _indexPart ptr: {attr_id: (vidx, type)}.
        # The Burst index decode is the expensive part (~hundreds of reads); it is
        # stable for an entity, so decode once and only re-read the few values/tick.
        self._layout_cache: Dict[int, Dict[int, tuple]] = {}

    def _kname(self, kp: int) -> str:
        if kp in self._klass_name:
            return self._klass_name[kp]
        nm = ""
        if _plaus(kp):
            np = self.pm.read_u64(kp + CLASS_NAME_OFF)
            if _plaus(np):
                b = self.pm.read_bytes(np, 40)
                if b:
                    nm = b.split(b"\x00", 1)[0].decode("utf-8", "replace")
        self._klass_name[kp] = nm
        return nm

    def _object_array(self, ent_addr: int):
        """Return (arr_addr, length) of the entity's cacheSlim object[] (or (0,0))."""
        attrs = self.pm.read_u64(ent_addr + self.off_ent_attrs)
        if not _plaus(attrs):
            return 0, 0, 0
        vals = self.pm.read_u64(attrs + self.off_coll_values)
        if not _plaus(vals):
            return 0, 0, attrs
        arr = self.pm.read_u64(vals + VALUES_ELEM0_ARR_OFF)
        if not _plaus(arr):
            return 0, 0, attrs
        n = self.pm.read_u32(arr + ARRAY_LEN_OFF) or 0
        return arr, min(int(n), 256), attrs

    def read_typed_attrs(self, ent_addr: int) -> List[Tuple[int, str, object]]:
        """Return [(slot, type_name, value), ...] for the entity's attr objects."""
        arr, n, _ = self._object_array(ent_addr)
        out: List[Tuple[int, str, object]] = []
        for j in range(n):
            o = self.pm.read_u64(arr + ARRAY_ELEMS_OFF + j * 8)
            if not _plaus(o):
                continue
            t = self._kname(self.pm.read_u64(o))
            if t == "LongAttr":
                out.append((j, t, self.pm.read_i64(o + ATTR_VAL8_OFF)))
            elif t == "IntAttr":
                out.append((j, t, self.pm.read_i32(o + ATTR_VAL4_OFF)))
            elif t == "FloatAttr":
                raw = self.pm.read_bytes(o + ATTR_VAL4_OFF, 4)
                out.append((j, t, struct.unpack("<f", raw)[0] if raw else 0.0))
            elif t == "BoolAttr":
                raw = self.pm.read_bytes(o + ATTR_VAL4_OFF, 1)
                out.append((j, t, bool(raw[0]) if raw else False))
        return out

    def _attr_value(self, o: int):
        """Decode a ZAttr<T> object to its scalar value (or None for ref types)."""
        if not _plaus(o):
            return None
        t = self._kname(self.pm.read_u64(o))
        if t == "LongAttr":
            return self.pm.read_i64(o + ATTR_VAL8_OFF)
        if t == "IntAttr":
            return self.pm.read_i32(o + ATTR_VAL4_OFF)
        if t == "FloatAttr":
            raw = self.pm.read_bytes(o + ATTR_VAL4_OFF, 4)
            return struct.unpack("<f", raw)[0] if raw else 0.0
        if t == "BoolAttr":
            raw = self.pm.read_bytes(o + ATTR_VAL4_OFF, 1)
            return bool(raw[0]) if raw else False
        return None

    def _value_at(self, vals: int, vidx: int):
        """_values[vidx>>5].Item2[vidx&31] -> scalar attr value (paged by 32)."""
        if vidx < 0:
            return None
        arrp = self.pm.read_u64(vals + ARRAY_ELEMS_OFF
                                + (vidx >> 5) * VALUES_TUPLE_STRIDE + VALUES_TUPLE_ARR_OFF)
        if not _plaus(arrp):
            return None
        o = self.pm.read_u64(arrp + ARRAY_ELEMS_OFF + (vidx & 31) * 8)
        return self._attr_value(o)

    def read_attr_map(self, ent_addr: int) -> Dict[int, object]:
        """Decode the entity's full {attr_id: value} map from the Burst index.

        Walks the KeySegment*[32] keys (8 attr ids + count each) parallel to the
        int*[32] valueIndices, then resolves each value index into the paged _values.
        Deterministic (attr_id-keyed) -- the structural decode of ZAttrCacheSlim.
        """
        attrs = self.pm.read_u64(ent_addr + self.off_ent_attrs)
        if not _plaus(attrs):
            return {}
        ip = self.pm.read_u64(attrs + self.off_coll_indexpart)
        vals = self.pm.read_u64(attrs + self.off_coll_values)
        if not (_plaus(ip) and _plaus(vals)):
            return {}
        out: Dict[int, object] = {}
        for k in range(INDEX_SEG_COUNT):
            segp = self.pm.read_u64(ip + INDEX_KEYSEG_OFF + k * 8)
            if not _plaus(segp):
                continue
            cnt = self.pm.read_u32(segp + KEYSEG_COUNT_OFF) or 0
            if cnt <= 0 or cnt > INDEX_SEG_SIZE:
                continue
            vip = self.pm.read_u64(ip + INDEX_VALIDX_OFF + k * 8)
            if not _plaus(vip):
                continue
            for pos in range(cnt):
                key = self.pm.read_u32(segp + pos * 4)
                vidx = self.pm.read_i32(vip + pos * 4)
                if vidx is not None and vidx >= 0:
                    out[int(key)] = self._value_at(vals, vidx)
        return out

    def _attr_object_at(self, vals: int, vidx: int) -> int:
        """Return the raw ZAttr object pointer for a value index (undecoded), or 0."""
        if vidx is None or vidx < 0:
            return 0
        arrp = self.pm.read_u64(vals + ARRAY_ELEMS_OFF
                                + (vidx >> 5) * VALUES_TUPLE_STRIDE + VALUES_TUPLE_ARR_OFF)
        if not _plaus(arrp):
            return 0
        return self.pm.read_u64(arrp + ARRAY_ELEMS_OFF + (vidx & 31) * 8)

    def _read_il2cpp_string(self, sp: int) -> str:
        """Decode an Il2CppString object -> str. Layout (x64): length @+0x10 (int32),
        UTF-16LE chars @+0x14. Sanity-capped so a non-string pointer just yields ''."""
        if not _plaus(sp):
            return ""
        n = self.pm.read_u32(sp + STR_LEN_OFF) or 0
        if n <= 0 or n > STR_MAX_CHARS:
            return ""
        raw = self.pm.read_bytes(sp + STR_CHARS_OFF, n * 2)
        if not raw or len(raw) < n * 2:
            return ""
        try:
            s = raw.decode("utf-16-le", "replace").strip()
        except Exception:
            return ""
        # reject control/garbage (a real name is printable; allow CJK + ascii)
        if not s or any(ord(c) < 0x20 for c in s):
            return ""
        return s

    def read_name_attr(self, ent_addr: int) -> str:
        """Read the entity's NAME attr (id=1) as the game's resolved display-name string,
        independent of our JSON tables. '' when the entity has no NAME attr (many monsters
        carry only a template id). The authoritative source for the JSON self-heal."""
        attrs = self.pm.read_u64(ent_addr + self.off_ent_attrs)
        if not _plaus(attrs):
            return ""
        ip = self.pm.read_u64(attrs + self.off_coll_indexpart)
        vals = self.pm.read_u64(attrs + self.off_coll_values)
        if not (_plaus(ip) and _plaus(vals)):
            return ""
        for k in range(INDEX_SEG_COUNT):
            segp = self.pm.read_u64(ip + INDEX_KEYSEG_OFF + k * 8)
            if not _plaus(segp):
                continue
            cnt = self.pm.read_u32(segp + KEYSEG_COUNT_OFF) or 0
            if cnt <= 0 or cnt > INDEX_SEG_SIZE:
                continue
            vip = self.pm.read_u64(ip + INDEX_VALIDX_OFF + k * 8)
            if not _plaus(vip):
                continue
            for pos in range(cnt):
                if int(self.pm.read_u32(segp + pos * 4)) != A_NAME:
                    continue
                o = self._attr_object_at(vals, self.pm.read_i32(vip + pos * 4))
                if not _plaus(o):
                    return ""
                # ZAttr<string>.value_ (reference type) is the string ptr at obj+0x18
                return self._read_il2cpp_string(self.pm.read_u64(o + ATTR_VAL8_OFF))
        return ""

    def is_combat_entity(self, ent_addr: int) -> bool:
        """True if the entity's _indexPart carries the HP attr ids (has an HP bar)."""
        attrs = self.pm.read_u64(ent_addr + self.off_ent_attrs)
        if not _plaus(attrs):
            return False
        ip = self.pm.read_u64(attrs + self.off_coll_indexpart)
        if not _plaus(ip):
            return False
        blob = self.pm.read_bytes(ip, 0x4000)
        if not blob:
            return False
        ids = {int.from_bytes(blob[o:o + 4], "little") for o in range(0, len(blob) - 3, 4)}
        return A_HP in ids and A_MAX_HP in ids

    def read_hp(self, ent_addr: int) -> Optional[Tuple[int, int]]:
        """Return (cur_hp, max_hp) for a combat entity, else None.

        HP invariant: the two HP-range LongAttr where 0 <= cur <= max. MaxHp = the
        larger, CurHp = the smaller (equal at full HP). Server-time longs are excluded
        by the HP-plausibility cap.
        """
        longs = [(s, v) for (s, t, v) in self.read_typed_attrs(ent_addr)
                 if t == "LongAttr" and isinstance(v, int) and 0 <= v <= MAX_HP_PLAUSIBLE]
        if not longs:
            return None
        # MaxHp = largest HP-range long; CurHp = largest long that is <= MaxHp and not
        # the MaxHp slot itself (adjacent lower slot for monsters), else == MaxHp.
        longs.sort(key=lambda sv: sv[1])
        max_slot, maxhp = longs[-1]
        if maxhp <= 0:
            return None
        cur = maxhp
        for s, v in reversed(longs[:-1]):
            if 0 <= v <= maxhp:
                cur = v
                break
        return int(cur), int(maxhp)

    def _build_layout(self, ip: int, vals: int, wanted) -> Dict[int, tuple]:
        """Decode the Burst index ONCE -> {attr_id: (obj_ptr, val_off, type_char)}.

        Caches the resolved ZAttr OBJECT pointer (the object persists; only its
        value_ mutates), so each per-tick read is 1 RPM/attr -- no pointer chasing.
        """
        want = set(wanted)
        lay: Dict[int, tuple] = {}
        for k in range(INDEX_SEG_COUNT):
            if len(lay) >= len(want):
                break
            segp = self.pm.read_u64(ip + INDEX_KEYSEG_OFF + k * 8)
            if not _plaus(segp):
                continue
            cnt = self.pm.read_u32(segp + KEYSEG_COUNT_OFF) or 0
            if cnt <= 0 or cnt > INDEX_SEG_SIZE:
                continue
            vip = self.pm.read_u64(ip + INDEX_VALIDX_OFF + k * 8)
            if not _plaus(vip):
                continue
            for pos in range(cnt):
                key = self.pm.read_u32(segp + pos * 4)
                if key not in want:
                    continue
                vidx = self.pm.read_i32(vip + pos * 4)
                if vidx is None or vidx < 0:
                    continue
                arrp = self.pm.read_u64(vals + ARRAY_ELEMS_OFF
                                        + (vidx >> 5) * VALUES_TUPLE_STRIDE + VALUES_TUPLE_ARR_OFF)
                obj = self.pm.read_u64(arrp + ARRAY_ELEMS_OFF + (vidx & 31) * 8) if _plaus(arrp) else 0
                if not _plaus(obj):
                    continue
                tc = _TYPE_CHAR.get(self._kname(self.pm.read_u64(obj)))
                if tc:
                    off = ATTR_VAL8_OFF if tc == "L" else ATTR_VAL4_OFF
                    lay[int(key)] = (int(obj), off, tc)
        return lay

    @staticmethod
    def _interp(raw, tc: str):
        """Interpret an 8-byte batch read as the typed attr value."""
        if raw is None:
            return None
        if tc == "L":
            return raw if raw < (1 << 63) else raw - (1 << 64)
        if tc == "I":
            v = raw & 0xFFFFFFFF
            return v - (1 << 32) if v >= (1 << 31) else v
        if tc == "F":
            return struct.unpack("<f", (raw & 0xFFFFFFFF).to_bytes(4, "little"))[0]
        if tc == "B":
            return bool(raw & 0xFF)
        return None

    def _layout_for(self, ent_addr: int):
        """Return (ip, layout) for an entity (layout cached by _indexPart ptr)."""
        attrs = self.pm.read_u64(ent_addr + self.off_ent_attrs)
        if not _plaus(attrs):
            return 0, None
        ip = self.pm.read_u64(attrs + self.off_coll_indexpart)
        vals = self.pm.read_u64(attrs + self.off_coll_values)
        if not (_plaus(ip) and _plaus(vals)):
            return 0, None
        lay = self._layout_cache.get(ip)
        if lay is None:
            if len(self._layout_cache) > 512:
                self._layout_cache.clear()
            lay = self._build_layout(ip, vals, COMBAT_ATTR_IDS)
            self._layout_cache[ip] = lay
        return ip, lay

    def _combat_from_amap(self, amap: Dict[int, object]) -> Optional[dict]:
        cur = amap.get(A_HP)
        mx = amap.get(A_MAX_HP)
        # same acceptance predicate as the Cython fast path (read_entity_combat_many):
        # 0 <= cur <= mx <= plausible cap -- keeps fallback and fast path consistent
        # (a server-time long surfacing as MaxHp must be rejected here too)
        if not (isinstance(cur, int) and isinstance(mx, int)
                and 0 < mx <= MAX_HP_PLAUSIBLE and 0 <= cur <= mx):
            return None

        def _n(aid):
            v = amap.get(aid)
            return int(v) if isinstance(v, (int, float)) else None

        sk = amap.get(A_SKILL_ID)
        st = amap.get(A_STOP_TICKING)
        return {
            "cur_hp": int(cur), "max_hp": int(mx),
            "hp_pct": (cur / mx) if mx else 0.0,
            "breaking_stage": _n(A_BREAK_STAGE),
            "overdrive": _n(A_OVERDRIVE),
            "stun": _n(A_STUN),
            "extinction": _n(A_EXT),
            "max_extinction": _n(A_MAX_EXT),
            "stop_breaking_ticking": bool(st) if isinstance(st, (int, float)) else False,
            "cast_skill_id": (int(sk) if isinstance(sk, (int, float)) and sk else None),
        }

    def read_combat(self, ent_addr: int) -> Optional[dict]:
        """Single-entity combat snapshot (HP + state), via the cached obj-ptr layout."""
        ip, lay = self._layout_for(ent_addr)
        # non-combat entity (no HP attr in the index) -> cheap reject, no value reads
        if not lay or A_HP not in lay or A_MAX_HP not in lay:
            return None
        amap = {aid: self._interp(self.pm.read_u64(t[0] + t[1]), t[2]) for aid, t in lay.items()}
        c = self._combat_from_amap(amap)
        if c is not None:
            return c
        # HP attr present but value implausible -> stale obj-ptr (pooled reuse) -> rebuild once
        self._layout_cache.pop(ip, None)
        ip, lay = self._layout_for(ent_addr)
        if lay and A_HP in lay and A_MAX_HP in lay:
            amap = {aid: self._interp(self.pm.read_u64(t[0] + t[1]), t[2]) for aid, t in lay.items()}
            return self._combat_from_amap(amap)
        return None

    def read_combat_batch(self, ent_addrs) -> Dict[int, Optional[dict]]:
        """Combat (HP + state) for many entities. Returns {ent_addr: dict} (combat only).

        Fast path: the whole decode (reads + index walk + value reads) runs in one
        Cython nogil pass (read_entity_combat_many) -> no per-entity Python/GIL cost.
        Fallback: the single obj-ptr loop (cached layout, pymem reads) -- this beats a
        Python-orchestrated batch, whose per-entity dict building dominates the cost.
        """
        ent_addrs = list(ent_addrs)
        if not ent_addrs:
            return {}
        if _HAS_FULLDECODE and _cymem is not None:
            try:
                # pass the auto-resolved offsets so the nogil kernel self-heals too
                # (wrapper falls back to the baked-offset call on a pre-rebuild kernel)
                flat = _cymem.read_entity_combat_many(
                    self.pm._handle, ent_addrs,
                    self.off_ent_attrs, self.off_coll_indexpart, self.off_coll_values)
            except Exception:
                flat = None
            if flat is not None and len(flat) == len(ent_addrs) * 8:
                out: Dict[int, dict] = {}
                _v = lambda x: (None if x < 0 else int(x))
                for i, e in enumerate(ent_addrs):
                    b = i * 8
                    cur = flat[b]
                    mx = flat[b + 1]
                    if cur < 0 or mx <= 0:
                        continue               # non-combat
                    sk = flat[b + 6]
                    out[e] = {
                        "cur_hp": int(cur), "max_hp": int(mx),
                        "hp_pct": (cur / mx) if mx else 0.0,
                        "breaking_stage": _v(flat[b + 2]),
                        "overdrive": _v(flat[b + 3]),
                        "stun": _v(flat[b + 4]),
                        "extinction": _v(flat[b + 5]),
                        "max_extinction": _v(flat[b + 7]),
                        "cast_skill_id": (int(sk) if sk > 0 else None),
                    }
                return out
        # fallback: single obj-ptr loop
        out2: Dict[int, dict] = {}
        for e in ent_addrs:
            c = self.read_combat(e)
            if c is not None:
                out2[e] = c
        return out2


__all__ = ["EntityCombatReader"]
