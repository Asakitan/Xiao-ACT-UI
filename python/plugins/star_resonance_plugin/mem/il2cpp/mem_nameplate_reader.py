# -*- coding: utf-8 -*-
# mem_nameplate_reader - read the game's *rendered* entity name from the
# world-space nameplate widgets (read-only).
#
# The authoritative display name an entity shows over its head is NOT stored on the
# ZEntity, its combat table row, or the localization pool in a template-keyed way --
# it lives in the nameplate UI widget. Each rendered nameplate widget has a fixed
# field layout:
#
# widget + 0x10 : long  Uuid          (== ZEntity.Uuid, the live anchor)
# widget + 0x60 : Il2CppString  Text   ('NN 级 <name>' for mobs, bare for NPCs)
#
# We string-anchor on the live entity uuids (already enumerated by the entity
# provider) instead of a version-fragile widget klass: scan the heap for any u64
# equal to a live uuid (Cython find_aligned_u64_in_set), and where the slot 0x50
# ahead decodes to a CJK/printable Il2CppString, that's the entity's nameplate.
# Strip the 'NN 级 ' level prefix to get the bare name.
#
# This makes the read auto-offset (the uuid is the anchor, the two field offsets are
# structural il2cpp offsets, stable across ASLR) and needs no disassembly. The heavy
# heap sweep is the Cython multi-needle scan; the per-hit work is a couple of reads.
#
# Verified live (2026-06-08, training hall): baseid 114->敌方木桩, 115->精英敌方木桩,
# 121->友方木桩, 122->精英守护木桩 (corrects the offline JSON's generic '木桩').
from __future__ import annotations

import re
from collections import Counter
from typing import Dict, List, Optional, Tuple

try:
    from mem_probe import cy_memscan as _cy
except Exception:  # pragma: no cover
    _cy = None

UUID_OFF = 0x10
NAME_OFF = 0x60
TYPE_OFF = 0x50                            # plate-type enum (int32)
NAME_FROM_UUID = NAME_OFF - UUID_OFF      # name ptr sits 0x50 ahead of the uuid slot
TYPE_FROM_UUID = TYPE_OFF - UUID_OFF      # 0x40 ahead of the uuid slot
STR_LEN_OFF = 0x10
STR_CHARS_OFF = 0x14
STR_MAX_CHARS = 64
# plate type (widget+0x50): which kind of label this widget renders. Verified live
# across 12 monster + 2 NPC plates (each NPC has a name plate AND a title plate).
PLATE_NPC_NAME = 1        # NPC proper name (雷沃兰德 / 露西小姐)
PLATE_MONSTER = 3         # monster/dummy 'NN 级 <name>'
PLATE_NPC_TITLE = 100     # NPC occupation/title (练习室管理员 / 工作人员) -- NOT the name
# 'NN 级 ' monster nameplate prefix (full/half-width digits + optional spaces)
_LEVEL_RE = re.compile(r"^[\s　]*[0-9０-９]+[\s　]*级[\s　]*")


def _plaus(p: Optional[int]) -> bool:
    return bool(p) and 0x10000 <= p <= 0x7FFFFFFFFFFF and (p & 7) == 0


class NameplateReader:
    # Harvest {uuid -> rendered name strings} from nameplate widgets (read-only).

    def __init__(self, pm):
        self.pm = pm
        self.last_raw: Dict[int, List[str]] = {}   # uuid -> all nameplate strings seen
        # nameplate widgets cluster in one heap region; remember which region bases
        # yielded a confirmed nameplate so warm harvests scan only those (~0.1s) and
        # skip the full private-heap sweep (~9s). Rebuilt only when the hint goes stale.
        self._hint_bases: List[int] = []

    # ---- Il2CppString decode (CJK/printable only; rejects non-string ptrs) ----
    def _read_str(self, sp: int) -> str:
        if not _plaus(sp):
            return ""
        n = self.pm.read_u32(sp + STR_LEN_OFF) or 0
        if n <= 0 or n > STR_MAX_CHARS:
            return ""
        raw = self.pm.read_bytes(sp + STR_CHARS_OFF, n * 2)
        if not raw or len(raw) < n * 2:
            return ""
        try:
            s = raw.decode("utf-16-le")
        except Exception:
            return ""
        if not s or any(ord(c) < 0x20 for c in s):
            return ""
        return s

    @staticmethod
    def strip_level(text: str) -> Tuple[str, bool]:
        # Return (bare_name, had_level_prefix). 'NN 级 敌方木桩' -> ('敌方木桩', True).
        m = _LEVEL_RE.match(text or "")
        if m:
            return text[m.end():].strip(), True
        return (text or "").strip(), False

    # ---- the heap sweep ----
    _CHUNK = 64 * 1024 * 1024

    def _scan_region(self, r, want, by_uuid: Dict[int, List[Tuple[int, str]]],
                     scratch=None) -> bool:
        # Scan one region for uuid slots and decode the plate (type@+0x40, name@+0x50
        # ahead of the uuid). Appends (plate_type, name) into ``by_uuid``. Returns True
        # if this region held a confirmed nameplate (so it's remembered as a hint).
        #
        # ``scratch`` is an optional reusable bytearray (allocated once per harvest by
        # _collect): the chunk is RPM'd straight into it and scanned in place, so the
        # sweep does zero per-chunk allocations/copies.
        had = False
        off = 0
        read_into = getattr(self.pm, "read_bytes_into", None)
        use_scratch = (scratch is not None and read_into is not None
                       and _cy is not None and hasattr(_cy, "find_aligned_u64_in_set"))
        while off < r.size:
            n = min(self._CHUNK, r.size - off)
            if use_scratch:
                got = read_into(r.base + off, scratch, n)
                if got <= 0:
                    break
                hits = _cy.find_aligned_u64_in_set(memoryview(scratch)[:got], want, 8192)
            else:
                blob = self.pm.read_bytes(r.base + off, n)
                if not blob:
                    break
                if _cy is not None and hasattr(_cy, "find_aligned_u64_in_set"):
                    hits = _cy.find_aligned_u64_in_set(blob, want, 8192)
                else:  # pragma: no cover - pure-Python fallback
                    mv = memoryview(blob)
                    hits = [(p, int.from_bytes(mv[p:p + 8], "little"))
                            for p in range(0, (len(blob) // 8) * 8, 8)
                            if int.from_bytes(mv[p:p + 8], "little") in want]
            for hit_off, val in hits:
                a = r.base + off + hit_off
                s = self._read_str(self.pm.read_u64(a + NAME_FROM_UUID))
                if s:
                    ptype = self.pm.read_u32(a + TYPE_FROM_UUID) or 0
                    lst = by_uuid.setdefault(int(val), [])
                    if (ptype, s) not in lst:
                        lst.append((ptype, s))
                    had = True
            off += n
        return had

    def _collect(self, uuids) -> Dict[int, List[Tuple[int, str]]]:
        # Return {uuid -> [(plate_type, nameplate string), ...]}.
        #
        # Warm path: scan only the remembered hint region(s) (the nameplate region is
        # stable within a session). Cold/stale path: full private-heap sweep, which
        # rebuilds the hint. Keeps the heavy scan in Cython and warm reads cheap.
        want = {int(u) & 0xFFFFFFFFFFFFFFFF for u in uuids if int(u) > 0}
        by_uuid: Dict[int, List[Tuple[int, str]]] = {}
        if not want:
            return by_uuid
        regions = list(self.pm.iter_regions(only_readable=True, only_private=True))
        # One scratch buffer per harvest, sized to the largest chunk we'll read;
        # freed when _collect returns (not kept resident on the reader).
        max_chunk = 0
        for r in regions:
            max_chunk = max(max_chunk, min(self._CHUNK, r.size))
        scratch = bytearray(max_chunk) if max_chunk > 0 else None
        hint = set(self._hint_bases)
        new_hint: List[int] = []
        if hint:
            for r in regions:
                if r.base in hint and self._scan_region(r, want, by_uuid, scratch):
                    new_hint.append(r.base)
        if not by_uuid:                      # cold start or hint went stale -> full sweep
            new_hint = []
            for r in regions:
                if self._scan_region(r, want, by_uuid, scratch):
                    new_hint.append(r.base)
        if new_hint:
            self._hint_bases = new_hint
        return by_uuid

    def harvest(self, uuid_to_base: Dict[int, int]) -> Dict[str, object]:
        # Scan nameplates for the given live entities.
        #
        # ``uuid_to_base`` maps live ZEntity.Uuid -> base_id (template id). Returns:
        # {
        # 'names':     {base_id: bare_name},   # confident (level-prefixed mob, or
        # #  a uuid with a single bare string)
        # 'ambiguous': {base_id: [candidates]}, # uuid with >1 bare string (NPC name+title)
        # 'by_uuid':   {uuid: [raw strings]},
        # }
        uuid_to_base = {int(u): int(b) for u, b in (uuid_to_base or {}).items() if int(u) > 0}
        by_uuid = self._collect(uuid_to_base.keys())
        self.last_raw = {u: [s for _, s in plates] for u, plates in by_uuid.items()}

        names: Dict[int, str] = {}
        ambiguous: Dict[int, List[str]] = {}
        # aggregate per base_id (several entities can share one base)
        per_base: Dict[int, List[Tuple[int, str]]] = {}
        for uuid, plates in by_uuid.items():
            per_base.setdefault(uuid_to_base.get(uuid, 0), []).extend(plates)
        for base, plates in per_base.items():
            if base <= 0:
                continue
            # monster: the level-prefixed plate is the unambiguous name (robust even
            # if the type enum ever shifts)
            stripped = [self.strip_level(s) for _, s in plates]
            leveled = [name for name, had_level in stripped if had_level]
            if leveled:
                names[base] = Counter(leveled).most_common(1)[0][0]
                continue
            # NPC: the plate whose type == PLATE_NPC_NAME is the proper name (not the
            # title plate PLATE_NPC_TITLE)
            npc_named = [s for t, s in plates if t == PLATE_NPC_NAME and s]
            if npc_named:
                names[base] = Counter(npc_named).most_common(1)[0][0]
                continue
            bare = sorted(set(s for _, s in plates if s))
            if len(bare) == 1:
                names[base] = bare[0]
            elif len(bare) > 1:
                ambiguous[base] = bare
        return {"names": names, "ambiguous": ambiguous, "by_uuid": by_uuid}


# ───────── selftest (live) ─────────

def _selftest():
    import sys
    from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
    from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_mgr import EntityMgrReader

    src = StaticDpsSource()
    snap = src.get_self_snapshot(force_rescan=False)
    if not snap:
        print("[nameplate] no self snapshot"); return
    pm = src.sr.pm
    emr = EntityMgrReader(src)
    if not emr.locate(int(snap.uid)):
        print("[nameplate] locate failed"); return
    full = emr.read(int(snap.uid), include_monsters=True, include_npcs=True)
    uuid_to_base = {}
    for e in (full.monsters + full.npcs):
        uuid_to_base[e.uuid] = pm.read_i64(e.obj_addr + 0xE0) or 0

    rd = NameplateReader(pm)
    res = rd.harvest(uuid_to_base)
    print("[nameplate] confident names (base_id -> name):")
    for b in sorted(res["names"]):
        print("   %-9d -> %s" % (b, res["names"][b]))
    if res["ambiguous"]:
        print("[nameplate] ambiguous (NPC name+title?):")
        for b in sorted(res["ambiguous"]):
            print("   %-9d -> %s" % (b, res["ambiguous"][b]))
    sys.stdout.flush()


if __name__ == "__main__":
    _selftest()
