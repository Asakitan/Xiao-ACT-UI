# -*- coding: utf-8 -*-
"""mem_config_table_reader - route-2: read the game's own config tables (id -> name).

The offline JSON name tables have gaps. The game itself holds the authoritative id->name
in its config tables (MonsterTable / NpcTable / DummyTable / SkillTable / BuffTable). This
reads them straight from memory so we can overlay-fill the JSON.

Structure (RE'd from dump fdc7111b + il2cpp.h disasm, see route2 research):

  SceneConfigMgr (ZSingleton, dump.cs:65596) holds the scene tables as fields:
    monsterTable_ @0x50  npcTable_ @0x58  dummyTable_ @0x80  ...  : ZTable<int, *TableBase>
  ZTable<K,V> (dump.cs:920090): _datas @0x28 is a std .NET Dictionary<long, object>
    (the row store; reuse the standard dict walk).
  Each dict value is a *TableBase row object that is FIELDLESS ({klass@0, monitor@8}).
  The row's data lives in a serialized ZLoader blob; a ReadProxy id is cached in the row's
  MONITOR slot (row+0x8). Resolve it through TableProxyManager static fields:
    id   = read_i32(row+0x8)                         # >0 (0 == never loaded -> skip)
    page = (id-1) >> 10 ; slot = (id-1) & 0x3FF
    sf      = read_u64(proxymgr_klass + STATIC_FIELDS_OFF)
    proxies = read_u64(sf + 0x8)                      # object[][]
    inner   = read_u64(proxies + 0x20 + page*8)      # object[]
    proxy   = inner + 0x20 + slot*12                  # ReadProxy{loaderIdx@0, baseOff@4, len@8}
    loaders = read_u64(sf + 0x18)                     # object[] of ZLoader
    zloader = read_u64(loaders + 0x20 + loaderIdx*8)
    mem     = read_u64(zloader + 0x108)               # ZLoader.Memory._object (byte[])
    midx    = read_i32(zloader + 0x110)               # ZLoader.Memory._index
    blob    = mem + 0x20 + midx + baseOff             # row's serialized columns
    Id   = read_i32(blob + ID_COL)                    # col 0 for all tables
    NameMlId = read_i32(blob + NAME_COL[kind])        # MLString id (NOT a string ptr)
  NameMlId -> CN string is resolved via MLStringPoolBridge (the extra hop, TODO/verify).

THIS FILE IS A SELFTEST HARNESS FIRST: it prints the chain for the first rows so the
structure can be verified live before any JSON overlay / Cython / auto-offset work. The
constants below are the researched offsets; once the chain is confirmed live they get
replaced by dci.field_offset(by name) for metadata fields + an empirical column gate.

Run:  python -m mem_probe.il2cpp.mem_config_table_reader
"""
from __future__ import annotations

import time
from typing import Dict, Iterator, Optional, Tuple

from mem_probe.il2cpp.auto_registration_locator import build_live_class_index

try:
    from mem_probe import cy_memscan as _cy
except Exception:  # pragma: no cover
    _cy = None

CLASS_NAME_OFF = 0x10

# --- researched offsets (to be auto-offset-ized after the chain is verified live) ---
SCENECFG_CANDIDATES = ("SceneConfigMgr", "Panda.ZGame.SceneConfigMgr")
PROXYMGR_CANDIDATES = ("TableProxyManager", "Table.Utility.TableProxyManager",
                       "Bokura.Table.TableProxyManager")
SCENECFG_CURSCENE_OFF = 0xE0          # curSceneId_ (sentinel: 0..9999)
SCENE_TABLE_OFF = {"monster": 0x50, "npc": 0x58, "dummy": 0x80}
ZTABLE_DATAS_OFF = 0x28               # ZTable._datas -> Dictionary<long, object>

# std .NET Dictionary<K,V>
DICT_ENTRIES_OFF = 0x18
DICT_COUNT_OFF = 0x20
ENTRY_KEY_OFF = 0x8
ENTRY_VAL_OFF = 0x10
ENTRY_STRIDE = 0x18
ARRAY_LEN_OFF = 0x18
ARRAY_ELEMS_OFF = 0x20

ROW_PROXYID_OFF = 0x8                 # ReadProxy id cached in the row's monitor slot
STATIC_FIELDS_OFF = 0xB8              # Il2CppClass.static_fields (version-sensitive)
PROXIES_OFF = 0x8                     # TableProxyManager._proxies (object[][]) in static_fields
LOADERS_OFF = 0x18                    # TableProxyManager._loaders (object[]) in static_fields
PROXY_PAGE = 1024
PROXY_STRIDE = 12
ZLOADER_MEM_OBJ_OFF = 0x108          # ZLoader.Memory._object (byte[])
ZLOADER_MEM_IDX_OFF = 0x110          # ZLoader.Memory._index
ID_COL = 0
NAME_COL = {"monster": 4, "npc": 4, "dummy": 4, "skill": 12, "buff": 16}


def _plaus(p: Optional[int]) -> bool:
    return bool(p and 0x10000 <= p <= 0x7FFFFFFFFFFF)


class MemConfigTableReader:
    """Read-only walker over the game's config tables (selftest/diagnostic stage)."""

    def __init__(self, dps_source):
        self._src = dps_source
        self.pm = dps_source.sr.pm
        self._klass: Dict[str, int] = {}
        self._scenecfg = 0
        self._proxymgr_sf = 0

    # ---- klass resolution (by name, version-robust) ----
    def _kname(self, kp: int) -> str:
        if not _plaus(kp):
            return ""
        np = self.pm.read_u64(kp + CLASS_NAME_OFF)
        b = self.pm.read_bytes(np, 48) if _plaus(np) else None
        return b.split(b"\x00", 1)[0].decode("utf-8", "replace") if b else ""

    def _resolve(self, candidates) -> Tuple[str, int]:
        want = set(candidates)
        idx = build_live_class_index(self.pm, want, time_budget_s=30)
        for c in candidates:
            kp = int(idx.get(c, 0) or 0)
            if kp:
                return c, kp
        return "", 0

    # ---- locate SceneConfigMgr singleton (klass-sentinel heap scan) ----
    def locate_scenecfg(self, *, force: bool = False) -> int:
        if self._scenecfg and not force:
            kp = self._klass.get("scenecfg", 0)
            if kp and self.pm.read_u64(self._scenecfg) == kp:
                return self._scenecfg
            self._scenecfg = 0
        name, kp = self._resolve(SCENECFG_CANDIDATES)
        if not kp or _cy is None:
            return 0
        self._klass["scenecfg"] = kp
        print(f"[cfgtable] SceneConfigMgr klass={name!r} @0x{kp:x}")
        t0 = time.time()
        for r in self.pm.iter_regions(only_readable=True, only_private=True):
            if r.size > 64 * 1024 * 1024 or (time.time() - t0) > 30:
                continue
            blob = self.pm.read_bytes(r.base, r.size)
            if not blob:
                continue
            for h in _cy.find_aligned_u64(blob, kp, 64):
                a = r.base + h
                scene = self.pm.read_i32(a + SCENECFG_CURSCENE_OFF)
                mt = self.pm.read_u64(a + SCENE_TABLE_OFF["monster"])
                if scene is not None and 0 <= scene <= 9999 and _plaus(mt):
                    self._scenecfg = a
                    print(f"[cfgtable] SceneConfigMgr inst @0x{a:x} curScene={scene} "
                          f"monsterTable@0x{mt:x}")
                    return a
        return 0

    def _proxymgr_static_fields(self) -> int:
        if self._proxymgr_sf:
            return self._proxymgr_sf
        name, kp = self._resolve(PROXYMGR_CANDIDATES)
        if not kp:
            print("[cfgtable] TableProxyManager klass NOT resolved")
            return 0
        sf = self.pm.read_u64(kp + STATIC_FIELDS_OFF)
        print(f"[cfgtable] TableProxyManager klass={name!r} @0x{kp:x} static_fields@0x{sf:x}")
        self._proxymgr_sf = sf if _plaus(sf) else 0
        return self._proxymgr_sf

    # ---- std .NET Dictionary<long, object> walk ----
    def _dict_entries(self, d: int) -> Iterator[Tuple[int, int]]:
        if not _plaus(d):
            return
        cnt = self.pm.read_i32(d + DICT_COUNT_OFF) or 0
        if cnt <= 0 or cnt > 300000:
            return
        entries = self.pm.read_u64(d + DICT_ENTRIES_OFF)
        if not _plaus(entries):
            return
        alen = self.pm.read_u32(entries + ARRAY_LEN_OFF) or 0
        base = entries + ARRAY_ELEMS_OFF
        for i in range(min(cnt, alen)):
            ea = base + i * ENTRY_STRIDE
            if (self.pm.read_i32(ea) or -1) < 0:          # hashCode < 0 -> free slot
                continue
            yield (self.pm.read_i64(ea + ENTRY_KEY_OFF),
                   self.pm.read_u64(ea + ENTRY_VAL_OFF))

    # ---- row blob resolution via ReadProxy + ZLoader ----
    def _row_blob(self, row: int) -> int:
        sf = self._proxymgr_static_fields()
        if not sf:
            return 0
        pid = self.pm.read_i32(row + ROW_PROXYID_OFF)
        if not pid or pid <= 0:
            return 0
        page, slot = (pid - 1) >> 10, (pid - 1) & (PROXY_PAGE - 1)
        proxies = self.pm.read_u64(sf + PROXIES_OFF)
        if not _plaus(proxies):
            return 0
        inner = self.pm.read_u64(proxies + ARRAY_ELEMS_OFF + page * 8)
        if not _plaus(inner):
            return 0
        proxy = inner + ARRAY_ELEMS_OFF + slot * PROXY_STRIDE
        loader_idx = self.pm.read_i32(proxy + 0)
        base_off = self.pm.read_i32(proxy + 4)
        if loader_idx is None or loader_idx < 0 or base_off is None or base_off < 0:
            return 0
        loaders = self.pm.read_u64(sf + LOADERS_OFF)
        if not _plaus(loaders):
            return 0
        zloader = self.pm.read_u64(loaders + ARRAY_ELEMS_OFF + loader_idx * 8)
        if not _plaus(zloader):
            return 0
        mem = self.pm.read_u64(zloader + ZLOADER_MEM_OBJ_OFF)
        midx = self.pm.read_i32(zloader + ZLOADER_MEM_IDX_OFF) or 0
        if not _plaus(mem):
            return 0
        return mem + ARRAY_ELEMS_OFF + midx + base_off

    def walk_raw(self, kind: str, limit: int = 20):
        """Yield (dict_key, row_ptr, blob, Id, name_mlid) for the first rows (diagnostic)."""
        mgr = self.locate_scenecfg()
        if not mgr:
            print("[cfgtable] SceneConfigMgr not located")
            return
        off = SCENE_TABLE_OFF.get(kind)
        if off is None:
            print(f"[cfgtable] no scene-table offset for kind={kind}")
            return
        ztable = self.pm.read_u64(mgr + off)
        datas = self.pm.read_u64(ztable + ZTABLE_DATAS_OFF) if _plaus(ztable) else 0
        cnt = self.pm.read_i32(datas + DICT_COUNT_OFF) if _plaus(datas) else 0
        print(f"[cfgtable] {kind}: ztable@0x{ztable:x} _datas@0x{datas:x} count={cnt}")
        name_col = NAME_COL.get(kind, 4)
        n = 0
        for key, row in self._dict_entries(datas):
            if n >= limit:
                break
            n += 1
            blob = self._row_blob(row)
            rid = self.pm.read_i32(blob + ID_COL) if blob else None
            mlid = self.pm.read_i32(blob + name_col) if blob else None
            yield (key, row, blob, rid, mlid)


def _selftest():
    from mem_probe.il2cpp.static_dps_source import StaticDpsSource
    src = StaticDpsSource()
    _ = src.sr  # trigger lazy open
    rd = MemConfigTableReader(src)
    for kind in ("monster", "dummy", "npc"):
        print(f"\n===== {kind} table =====")
        rows = list(rd.walk_raw(kind, limit=15))
        for key, row, blob, rid, mlid in rows:
            print(f"  dict_key={key:<10} row@0x{row:x} blob@0x{(blob or 0):x} "
                  f"Id={rid} name_mlid={mlid}")
        if rows:
            ids = [r[3] for r in rows if r[3] is not None]
            print(f"  -> {len(rows)} rows, Id sample={ids[:8]} "
                  f"(sane if Ids look like template ids, e.g. 114/121/122)")


if __name__ == "__main__":
    _selftest()
