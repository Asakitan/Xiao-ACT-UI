# -*- coding: utf-8 -*-
"""mem_damage_reader - read the game's own per-player damage table from memory.

The game keeps its full DPS table in DamageDataMgr (ZSingleton). So damage is NOT
only a TCP event stream -- the aggregated per-player / per-skill table the game itself
renders is live in memory:

  DamageDataMgr (Panda.ZGame.DamageDataMgr), dump.cs TypeDefIndex 5257:
    damageValue_      @0xB0 : ZDictionary<long uuid, ZDictionary<int skillId, DamageData>>
    takeDamageValue_  @0xB8 : (same shape, damage taken)
    healValue_        @0xC0 : (same shape, healing)
    totalPlayerValue_ @0xD0 : ZDictionary<DamageShowTotalType, ZDictionary<long uuid, long>>
    playerUuidList_   @0xF0 : ZList<long>
    IsActive          @0xA0

  ZDictionary<K,V> (TypeDefIndex 27652): buckets_@0x18, entries_@0x20, count_@0x38.
  Entry { uint hashCode@0x0; int next@0x4; K key@0x8; V value@0x10 } stride 0x18
  (holds for <int,ptr> and <long,long> -- 8-aligned key/value).

  DamageData struct (TypeDefIndex 5254): playerUuid@0x0, skillId@0x8, damageId@0x10,
  actualValue@0x18.

  DamageShowTotalType: Damage=1, Cure=2, TakeDamage=3, DamageSecond=4, CureSecond=5.

Offsets are IL2CPP field offsets (stable across base/ASLR); the DamageDataMgr klass is
resolved by name via auto_registration_locator (version-robust). The singleton instance
is found by a klass-sentinel heap scan and cached (revalidated each call).
"""
from __future__ import annotations

import time
from typing import Dict, Optional

try:
    from mem_probe import cy_memscan as _cy
except Exception:  # pragma: no cover
    _cy = None

DDM_CLASS = "Panda.ZGame.DamageDataMgr"
DDM_ISACTIVE_OFF = 0xA0
DDM_DAMAGEVALUE_OFF = 0xB0
DDM_TAKEDAMAGE_OFF = 0xB8
DDM_HEAL_OFF = 0xC0
DDM_TOTALPLAYER_OFF = 0xD0
DDM_PLAYERLIST_OFF = 0xF0

ZDICT_ENTRIES_OFF = 0x20
ZDICT_COUNT_OFF = 0x38
ENTRY_KEY_OFF = 0x8
ENTRY_VAL_OFF = 0x10
ENTRY_STRIDE = 0x18
ARRAY_LEN_OFF = 0x18
ARRAY_ELEMS_OFF = 0x20

DMG_SKILLID_OFF = 0x8
DMG_ACTUALVALUE_OFF = 0x18
# inner damageValue_[uuid] is ZDictionary<int skillId, DamageData(struct, 0x20)>.
# Entry { hashCode@0; next@4; int key@0x8; DamageData value@0x10 } -> stride 0x30,
# value.actualValue @ 0x10 + 0x18 = 0x28.
SKILL_ENTRY_STRIDE = 0x30
SKILL_ENTRY_ACTUAL_OFF = 0x10 + DMG_ACTUALVALUE_OFF

TOTAL_DAMAGE = 1
TOTAL_CURE = 2
TOTAL_TAKEDAMAGE = 3

CLASS_NAME_OFF = 0x10


def _plaus(p: Optional[int]) -> bool:
    return bool(p and 0x10000 <= p <= 0x7FFFFFFFFFFF)


class MemDamageReader:
    """Reads the in-memory per-player damage table (the game's own DPS aggregation)."""

    def __init__(self, dps_source):
        self._src = dps_source
        self.pm = dps_source.sr.pm
        self._klass = 0
        self._inst = 0
        # Locate backoff: the damage table only exists once combat starts. Without
        # a cooldown, every entity-loop tick out of combat re-ran a full private-heap
        # sweep (the dominant idle-time CPU waste). Failed scans back off
        # exponentially; a confirmed hit's region is remembered as a warm hint.
        self._miss_until = 0.0
        self._miss_backoff = 1.0
        self._hint_base = 0
        self._scratch: Optional[bytearray] = None
        # auto-offset: DamageDataMgr / DamageData fields by name from the dump,
        # literal fallback (the curated bundle may omit DamageDataMgr -> falls back;
        # add it to the bundle class list to activate self-heal). The ZDictionary
        # entry offsets stay literal — it's an open generic (no fields in the dump).
        from plugins.star_resonance_plugin.mem.il2cpp import auto_offsets as _ao
        ddm = _ao.resolve(self._src, DDM_CLASS, {
            "off_isactive": ("IsActive", DDM_ISACTIVE_OFF),
            "off_damagevalue": ("damageValue_", DDM_DAMAGEVALUE_OFF),
            "off_totalplayer": ("totalPlayerValue_", DDM_TOTALPLAYER_OFF),
        })
        for _k, _v in ddm.items():
            setattr(self, _k, _v)
        self.off_dmg_actual = _ao.resolve(self._src, "Panda.ZGame.DamageData", {
            "a": ("actualValue", DMG_ACTUALVALUE_OFF)})["a"]
        # inner damageValue_[uuid] is ZDictionary<int, DamageData(struct)>; the value
        # sits at Entry+0x10, so actualValue is Entry-relative ENTRY_VAL_OFF + its offset.
        self.off_skill_entry_actual = ENTRY_VAL_OFF + self.off_dmg_actual

    def _kname(self, kp: int) -> str:
        if not _plaus(kp):
            return ""
        np = self.pm.read_u64(kp + CLASS_NAME_OFF)
        b = self.pm.read_bytes(np, 32) if _plaus(np) else None
        return b.split(b"\x00", 1)[0].decode("utf-8", "replace") if b else ""

    def _resolve_klass(self) -> int:
        if self._klass and self._kname(self._klass) == "DamageDataMgr":
            return self._klass
        from plugins.star_resonance_plugin.mem.il2cpp.klass_index import resolve_klasses
        idx = resolve_klasses(self.pm, {DDM_CLASS}, time_budget_s=30)
        self._klass = int(idx.get(DDM_CLASS, 0) or 0)
        return self._klass

    def _scan_region_for_inst(self, r, kp: int) -> int:
        """Cython klass-sentinel scan of one region (chunked, zero-copy). Returns
        the validated DamageDataMgr instance address or 0."""
        read_into = getattr(self.pm, "read_bytes_into", None)
        chunk = 16 * 1024 * 1024
        if self._scratch is None or len(self._scratch) < min(chunk, r.size):
            self._scratch = bytearray(min(chunk, r.size))
        off = 0
        while off < r.size:
            n = min(chunk, r.size - off)
            if read_into is not None:
                got = read_into(r.base + off, self._scratch, n)
                if got <= 0:
                    break
                mv = memoryview(self._scratch)[:got]
            else:
                blob = self.pm.read_bytes(r.base + off, n)
                if not blob:
                    break
                mv = blob
                got = n
            for h in _cy.find_aligned_u64(mv, kp, 64):
                a = r.base + off + h
                ab = self.pm.read_bytes(a + self.off_isactive, 1)
                if ab and ab[0] == 1 and _plaus(self.pm.read_u64(a + self.off_totalplayer)):
                    return a
            off += n
        return 0

    def locate(self, *, force: bool = False) -> int:
        """Return the live DamageDataMgr instance (cached, klass-sentinel revalidated).

        Backs off after a miss so out-of-combat ticks (table not yet allocated) do
        not re-sweep the heap every tick. A confirmed hit's region is the warm hint
        for the next cold acquisition.
        """
        if self._inst and not force:
            if self.pm.read_u64(self._inst) == self._klass and self._klass:
                return self._inst
            self._inst = 0
        now = time.time()
        if not force and now < self._miss_until:
            return 0
        kp = self._resolve_klass()
        if not kp or _cy is None:
            return 0
        # Warm hint first: the table is large and committed; its region is stable
        # within a session, so re-checking it before the full sweep is near-free.
        if self._hint_base:
            for r in self.pm.iter_regions(only_readable=True, only_private=True):
                if r.base == self._hint_base:
                    a = self._scan_region_for_inst(r, kp)
                    if a:
                        self._inst = a
                        self._miss_backoff = 1.0
                        return a
                    break
        t0 = time.time()
        for r in self.pm.iter_regions(only_readable=True, only_private=True):
            if (time.time() - t0) > 30:
                break
            a = self._scan_region_for_inst(r, kp)
            if a:
                self._inst = a
                self._hint_base = r.base
                self._miss_backoff = 1.0
                return a
        # miss: back off (cap 15s) so idle ticks stop hammering the heap
        self._miss_until = now + self._miss_backoff
        self._miss_backoff = min(self._miss_backoff * 2.0, 15.0)
        return 0

    def _entry_addrs(self, zdict: int, stride: int = ENTRY_STRIDE):
        """Yield each Entry address of a ZDictionary (key@+0x8, value@+0x10).

        ``stride`` is the Entry size: 0x18 for primitive/ref value (long/ptr), larger
        when the value is an inline struct (e.g. DamageData -> 0x30).
        """
        if not _plaus(zdict):
            return
        cnt = self.pm.read_i32(zdict + ZDICT_COUNT_OFF) or 0
        if cnt <= 0 or cnt > 100000:
            return
        entries = self.pm.read_u64(zdict + ZDICT_ENTRIES_OFF)
        if not _plaus(entries):
            return
        alen = self.pm.read_u32(entries + ARRAY_LEN_OFF) or 0
        base = entries + ARRAY_ELEMS_OFF
        for i in range(min(cnt, alen)):
            yield base + i * stride

    def _entries_block(self, zdict: int, stride: int = ENTRY_STRIDE) -> Optional[bytes]:
        """Read a ZDictionary's whole entries[] slab in ONE read_bytes.

        Returns the raw entry bytes (n*stride) for local decode, or None. Collapses
        the per-entry single-RPM walk (O(entries) syscalls/tick) into one block read.
        """
        if not _plaus(zdict):
            return None
        import struct as _st
        hdr = self.pm.read_bytes(zdict, max(ZDICT_COUNT_OFF, ZDICT_ENTRIES_OFF) + 8)
        if not hdr or len(hdr) < ZDICT_ENTRIES_OFF + 8:
            return None
        cnt = _st.unpack_from("<i", hdr, ZDICT_COUNT_OFF)[0]
        if cnt <= 0 or cnt > 100000:
            return None
        entries = _st.unpack_from("<Q", hdr, ZDICT_ENTRIES_OFF)[0]
        if not _plaus(entries):
            return None
        alen = self.pm.read_u32(entries + ARRAY_LEN_OFF) or 0
        n = min(cnt, alen)
        if n <= 0:
            return None
        blob = self.pm.read_bytes(entries + ARRAY_ELEMS_OFF, n * stride)
        if blob and len(blob) >= stride:
            return blob[:(len(blob) // stride) * stride]
        return None

    def _inner_for_total_type(self, inst: int, total_type: int) -> int:
        tpv = self.pm.read_u64(inst + self.off_totalplayer)
        blob = self._entries_block(tpv)
        if blob is not None:
            import struct
            # Entry { u32 hash; i32 next; i64 key; u64 value } stride 0x18
            for _h, _n, key, val in struct.iter_unpack("<IiqQ", blob):
                if key == total_type:
                    return int(val)
            return 0
        for e in self._entry_addrs(tpv):
            if self.pm.read_i32(e + ENTRY_KEY_OFF) == total_type:
                return int(self.pm.read_u64(e + ENTRY_VAL_OFF) or 0)
        return 0

    def read_player_totals(self, total_type: int = TOTAL_DAMAGE) -> Dict[int, int]:
        """{playerUuid: total} from totalPlayerValue_[total_type] (Damage by default)."""
        inst = self.locate()
        if not inst:
            return {}
        inner = self._inner_for_total_type(inst, total_type)
        out: Dict[int, int] = {}
        blob = self._entries_block(inner)
        if blob is not None:
            import struct
            for _h, _n, uuid, total in struct.iter_unpack("<Iiqq", blob):
                if uuid and 0 < total <= (1 << 60):
                    out[int(uuid)] = int(total)
            return out
        for e in self._entry_addrs(inner):
            uuid = self.pm.read_i64(e + ENTRY_KEY_OFF)
            total = self.pm.read_i64(e + ENTRY_VAL_OFF)
            if uuid and 0 < total <= (1 << 60):
                out[int(uuid)] = int(total)
        return out

    def read_player_skill_damage(self, uuid: int) -> Dict[int, int]:
        """{skillId: actualValue} for one player from damageValue_[uuid]."""
        inst = self.locate()
        if not inst:
            return {}
        dmgv = self.pm.read_u64(inst + self.off_damagevalue)
        inner = 0
        blob = self._entries_block(dmgv)
        if blob is not None:
            import struct
            for _h, _n, key, val in struct.iter_unpack("<IiqQ", blob):
                if key == int(uuid):
                    inner = int(val)
                    break
        else:
            for e in self._entry_addrs(dmgv):
                if self.pm.read_i64(e + ENTRY_KEY_OFF) == int(uuid):
                    inner = int(self.pm.read_u64(e + ENTRY_VAL_OFF) or 0)
                    break
        out: Dict[int, int] = {}
        # inner is ZDictionary<int skillId, DamageData(struct)>, stride 0x30; the
        # actualValue sits at self.off_skill_entry_actual from the entry start.
        sblob = self._entries_block(inner, stride=SKILL_ENTRY_STRIDE)
        if sblob is not None:
            import struct
            stride = SKILL_ENTRY_STRIDE
            ko = ENTRY_KEY_OFF
            ao = self.off_skill_entry_actual
            for i in range(len(sblob) // stride):
                o = i * stride
                skill = struct.unpack_from("<i", sblob, o + ko)[0]
                actual = struct.unpack_from("<q", sblob, o + ao)[0]
                if skill and 0 < actual <= (1 << 60):
                    out[int(skill)] = int(actual)
            return out
        for e in self._entry_addrs(inner, stride=SKILL_ENTRY_STRIDE):
            skill = self.pm.read_i32(e + ENTRY_KEY_OFF)
            actual = int(self.pm.read_i64(e + self.off_skill_entry_actual) or 0)
            if skill and 0 < actual <= (1 << 60):
                out[int(skill)] = actual
        return out


__all__ = ["MemDamageReader"]
