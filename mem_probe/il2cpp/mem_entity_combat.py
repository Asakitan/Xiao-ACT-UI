# -*- coding: utf-8 -*-
"""mem_entity_combat - read live combat HP/state from a ZEntity (read-only).

The displayed monster/boss HP lives in the entity's attribute cache:
  ZEntity.attrs_ (@+0x48) -> ZAttrCollection.cacheSlim_._values (@+0x20)
    -> ValueTuple<uint, object[]>[]  (element0.Item2 @ array+0x28 is the object[])
  each object[] slot is a typed ZAttr<T>:
    ZAttr<T> = { bool isDefault_; T value_; object bindWatchers_ } after the 0x10
    il2cpp header -> value_ @ obj+0x14 (Int/Float/Bool) or obj+0x18 (Long/String/ref).

attr_id -> slot is encoded in the native _indexPart (@attrs+0x18) Burst index, which
is a hash structure (not slot-ordered); rather than reverse the SIMD index we use the
HP invariant: CurHp/MaxHp are the LongAttr pair with 0 <= CurHp <= MaxHp in HP range.
Presence of attr ids 11310(HP)/11320(MAX_HP) in _indexPart gates "is a combat entity".

All offsets are field offsets within IL2CPP objects (stable across base/ASLR); the
only version-sensitive inputs are the class layout, taken from the live dump.
"""
from __future__ import annotations
import struct
from typing import Dict, List, Optional, Tuple

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

# attr ids (== packet_parser.enums.AttrType)
A_HP, A_MAX_HP = 11310, 11320
A_MAX_EXT, A_EXT, A_MAX_STUN, A_STUN = 440, 441, 442, 443
A_OVERDRIVE, A_BREAK_STAGE = 444, 455

MAX_HP_PLAUSIBLE = 5_000_000_000   # exclude server-time longs (~1.7e12)


def _plaus(p: Optional[int]) -> bool:
    return bool(p and 0x10000 <= p <= 0x7FFFFFFFFFFF)


class EntityCombatReader:
    """Reads HP/state from a ZEntity's attribute cache. ``pm`` needs read_u64/i64/
    u32/i32/bytes."""

    def __init__(self, pm):
        self.pm = pm
        self._klass_name: Dict[int, str] = {}     # klass ptr -> name (session cache)

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
        attrs = self.pm.read_u64(ent_addr + ENT_ATTRS_OFF)
        if not _plaus(attrs):
            return 0, 0, 0
        vals = self.pm.read_u64(attrs + COLL_VALUES_OFF)
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

    def is_combat_entity(self, ent_addr: int) -> bool:
        """True if the entity's _indexPart carries the HP attr ids (has an HP bar)."""
        attrs = self.pm.read_u64(ent_addr + ENT_ATTRS_OFF)
        if not _plaus(attrs):
            return False
        ip = self.pm.read_u64(attrs + COLL_INDEXPART_OFF)
        if not _plaus(ip):
            return False
        blob = self.pm.read_bytes(ip, 0x4000)
        if not blob:
            return False
        ids = {int.from_bytes(blob[o:o + 4], "little") for o in range(0, len(blob) - 4, 4)}
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

    def read_combat(self, ent_addr: int) -> Optional[dict]:
        """Full combat snapshot: hp + breaking/overdrive/stun where identifiable."""
        if not self.is_combat_entity(ent_addr):
            return None
        hp = self.read_hp(ent_addr)
        if hp is None:
            return None
        cur, mx = hp
        return {
            "cur_hp": cur, "max_hp": mx,
            "hp_pct": (cur / mx) if mx else 0.0,
        }


__all__ = ["EntityCombatReader"]
