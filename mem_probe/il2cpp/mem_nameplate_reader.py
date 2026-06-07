# -*- coding: utf-8 -*-
"""mem_nameplate_reader - read the game's *rendered* entity name from the
world-space nameplate widgets (read-only).

The authoritative display name an entity shows over its head is NOT stored on the
ZEntity, its combat table row, or the localization pool in a template-keyed way --
it lives in the nameplate UI widget. Each rendered nameplate widget has a fixed
field layout:

    widget + 0x10 : long  Uuid          (== ZEntity.Uuid, the live anchor)
    widget + 0x60 : Il2CppString  Text   ('NN 级 <name>' for mobs, bare for NPCs)

We string-anchor on the live entity uuids (already enumerated by the entity
provider) instead of a version-fragile widget klass: scan the heap for any u64
equal to a live uuid (Cython find_aligned_u64_in_set), and where the slot 0x50
ahead decodes to a CJK/printable Il2CppString, that's the entity's nameplate.
Strip the 'NN 级 ' level prefix to get the bare name.

This makes the read auto-offset (the uuid is the anchor, the two field offsets are
structural il2cpp offsets, stable across ASLR) and needs no disassembly. The heavy
heap sweep is the Cython multi-needle scan; the per-hit work is a couple of reads.

Verified live (2026-06-08, training hall): baseid 114->敌方木桩, 115->精英敌方木桩,
121->友方木桩, 122->精英守护木桩 (corrects the offline JSON's generic '木桩').
"""
from __future__ import annotations

import re
from typing import Dict, List, Optional, Tuple

try:
    from mem_probe import cy_memscan as _cy
except Exception:  # pragma: no cover
    _cy = None

UUID_OFF = 0x10
NAME_OFF = 0x60
NAME_FROM_UUID = NAME_OFF - UUID_OFF      # name ptr sits 0x50 ahead of the uuid slot
STR_LEN_OFF = 0x10
STR_CHARS_OFF = 0x14
STR_MAX_CHARS = 64
# 'NN 级 ' monster nameplate prefix (full/half-width digits + optional spaces)
_LEVEL_RE = re.compile(r"^[\s　]*[0-9０-９]+[\s　]*级[\s　]*")


def _plaus(p: Optional[int]) -> bool:
    return bool(p) and 0x10000 <= p <= 0x7FFFFFFFFFFF and (p & 7) == 0


class NameplateReader:
    """Harvest {uuid -> rendered name strings} from nameplate widgets (read-only)."""

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
        """Return (bare_name, had_level_prefix). 'NN 级 敌方木桩' -> ('敌方木桩', True)."""
        m = _LEVEL_RE.match(text or "")
        if m:
            return text[m.end():].strip(), True
        return (text or "").strip(), False

    # ---- the heap sweep ----
    def _scan_region(self, r, want, by_uuid: Dict[int, List[str]]) -> bool:
        """Scan one region for uuid slots and decode the nameplate name 0x50 ahead.

        Appends decoded names into ``by_uuid``. Returns True if this region held at
        least one confirmed nameplate (so it can be remembered as a hint region).
        """
        had = False
        off = 0
        while off < r.size:
            n = min(64 * 1024 * 1024, r.size - off)
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
                s = self._read_str(self.pm.read_u64(r.base + off + hit_off + NAME_FROM_UUID))
                if s:
                    lst = by_uuid.setdefault(int(val), [])
                    if s not in lst:
                        lst.append(s)
                    had = True
            off += n
        return had

    def _collect(self, uuids) -> Dict[int, List[str]]:
        """Return {uuid -> [nameplate strings]}.

        Warm path: scan only the remembered hint region(s) (the nameplate region is
        stable within a session). Cold/stale path: full private-heap sweep, which
        rebuilds the hint. Keeps the heavy scan in Cython and warm reads cheap.
        """
        want = {int(u) & 0xFFFFFFFFFFFFFFFF for u in uuids if int(u) > 0}
        by_uuid: Dict[int, List[str]] = {}
        if not want:
            return by_uuid
        regions = list(self.pm.iter_regions(only_readable=True, only_private=True))
        hint = set(self._hint_bases)
        new_hint: List[int] = []
        if hint:
            for r in regions:
                if r.base in hint and self._scan_region(r, want, by_uuid):
                    new_hint.append(r.base)
        if not by_uuid:                      # cold start or hint went stale -> full sweep
            new_hint = []
            for r in regions:
                if self._scan_region(r, want, by_uuid):
                    new_hint.append(r.base)
        if new_hint:
            self._hint_bases = new_hint
        return by_uuid

    def harvest(self, uuid_to_base: Dict[int, int]) -> Dict[str, object]:
        """Scan nameplates for the given live entities.

        ``uuid_to_base`` maps live ZEntity.Uuid -> base_id (template id). Returns:
          {
            'names':     {base_id: bare_name},   # confident (level-prefixed mob, or
                                                  #  a uuid with a single bare string)
            'ambiguous': {base_id: [candidates]}, # uuid with >1 bare string (NPC name+title)
            'by_uuid':   {uuid: [raw strings]},
          }
        """
        uuid_to_base = {int(u): int(b) for u, b in (uuid_to_base or {}).items() if int(u) > 0}
        by_uuid = self._collect(uuid_to_base.keys())
        self.last_raw = by_uuid

        names: Dict[int, str] = {}
        ambiguous: Dict[int, List[str]] = {}
        # aggregate per base_id (several entities can share one base)
        per_base: Dict[int, List[str]] = {}
        for uuid, strs in by_uuid.items():
            per_base.setdefault(uuid_to_base.get(uuid, 0), []).extend(strs)
        for base, strs in per_base.items():
            if base <= 0:
                continue
            leveled = [self.strip_level(s)[0] for s in strs if self.strip_level(s)[1]]
            if leveled:
                # monster: the level-prefixed plate is the unambiguous name
                names[base] = max(set(leveled), key=leveled.count)
                continue
            bare = sorted(set(s for s in strs if s))
            if len(bare) == 1:
                names[base] = bare[0]
            elif len(bare) > 1:
                ambiguous[base] = bare
        return {"names": names, "ambiguous": ambiguous, "by_uuid": by_uuid}


# ───────── selftest (live) ─────────

def _selftest():
    import sys
    from mem_probe.il2cpp.static_dps_source import StaticDpsSource
    from mem_probe.il2cpp.mem_entity_mgr import EntityMgrReader

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
