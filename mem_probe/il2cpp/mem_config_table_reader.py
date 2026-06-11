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

Class-FIELD offsets (SceneConfigMgr tables, TableProxyManager statics, ZLoader
members) resolve through auto_offsets (live field table -> dump bundle -> the
literals below). Row blob COLUMN offsets are not class fields; they come from
table_columns (getter-thunk extraction, versioned cache, curated fallback).

Beyond the original selftest walk this module provides:
  - col_i32 / col_mlid / col_i32_array: typed column readers over a row blob
    (Int32Array columns decode through ZLoader.IntArrayPool).
  - iter_rows(class_full_name): heap-scan row instances -> (row, zloader, blob).
  - iter_rows_via_loader(class_full_name): walk ZLoader._offsets (the full
    serialized row set, including rows the game never instantiated).
  - TABLE_CLASS: kind -> row-class registry for the global config tables.

Run:  python -m mem_probe.il2cpp.mem_config_table_reader
"""
from __future__ import annotations

import struct
import time
from typing import Dict, Iterator, List, Optional, Tuple

from mem_probe.il2cpp import auto_offsets
from mem_probe.il2cpp.klass_index import resolve_klasses

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
ZLOADER_CLS = "Bokura.Table.ZLoader"
ZLOADER_MEM_OBJ_OFF = 0x108          # ZLoader.Memory._object (byte[])
ZLOADER_MEM_IDX_OFF = 0x110          # ZLoader.Memory._index
ZLOADER_INTARRAYPOOL_OFF = 0x18      # ZLoader.IntArrayPool (inline Pool struct)
ZLOADER_STRINGPOOL_OFF = 0xF0        # ZLoader.StringPool (i16 len + utf8 bytes payload)
ZLOADER_DATASIZE_OFF = 0x118         # ZLoader.DataSize (fixed row record size in bytes)
ZLOADER_BUFRANGE_OFF = 0x128         # ZLoader._bufferRange (offset@0, len@4) over Memory
# ZLoader._offsets: Dictionary<long,int> row key -> RECORD index. GetRowData:
#   byte_off = _offsets[key] * DataSize; slice = Buffer[byte_off : byte_off+DataSize]
#   Buffer   = Memory[_bufferRange.offset : +_bufferRange.len]
ZLOADER_OFFSETS_OFF = 0x130
# Bokura.Table.Pool (inline struct): _memory{obj@0, idx@8, len@0xC}, _dataSize@0x10.
# len bit31 is a flag -> mask with 0x7FFFFFFF (verified in Pool.GetMemory: btr eax,0x1F).
POOL_MEM_OBJ_OFF = 0x0
POOL_MEM_IDX_OFF = 0x8
POOL_MEM_LEN_OFF = 0xC
POOL_DATASIZE_OFF = 0x10
ID_COL = 0
NAME_COL = {"monster": 4, "npc": 4, "dummy": 4, "skill": 12, "buff": 16}

# kind -> full row-class name for the global (non-scene) config tables. Their
# row objects are heap-scanned by klass; the per-table ZLoader walk is the
# fallback for rows the game never touched.
TABLE_CLASS = {
    "raid_dungeon": "Bokura.RaidDungeonTableBase",
    "skill": "Bokura.SkillTableBase",
    "buff": "Bokura.BuffTableBase",
    "monster": "Bokura.MonsterTableBase",
    "scene": "Bokura.SceneTableBase",
}

_ARRAY_MAX_COUNT = 4096               # hard sanity cap for Int32Array column decode


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
        self._off: Optional[Dict[str, int]] = None      # auto-resolved field offsets
        self._row_klass: Dict[str, int] = {}            # row class full name -> klass
        self._loader_cache: Dict[str, int] = {}         # row class full name -> ZLoader

    # ---- class-field offsets via auto_offsets (live -> dump -> literal) ----
    def _offsets(self) -> Dict[str, int]:
        if self._off is not None:
            return self._off
        out: Dict[str, int] = {}
        try:
            out.update(auto_offsets.resolve(self._src, "SceneConfigMgr", {
                "scenecfg_cur_scene": ("curSceneId_", SCENECFG_CURSCENE_OFF),
                "scene_table_monster": ("monsterTable_", SCENE_TABLE_OFF["monster"]),
                "scene_table_npc": ("npcTable_", SCENE_TABLE_OFF["npc"]),
                "scene_table_dummy": ("dummyTable_", SCENE_TABLE_OFF["dummy"]),
            }, log=print))
            out.update(auto_offsets.resolve(self._src, "Table.Utility.TableProxyManager", {
                "proxies": ("_proxies", PROXIES_OFF),     # static: off in static_fields
                "loaders": ("_loaders", LOADERS_OFF),
            }, log=print))
            out.update(auto_offsets.resolve(self._src, ZLOADER_CLS, {
                "zloader_mem": ("Memory", ZLOADER_MEM_OBJ_OFF),
                "zloader_int_pool": ("IntArrayPool", ZLOADER_INTARRAYPOOL_OFF),
                "zloader_str_pool": ("StringPool", ZLOADER_STRINGPOOL_OFF),
                "zloader_data_size": ("DataSize", ZLOADER_DATASIZE_OFF),
                "zloader_buf_range": ("_bufferRange", ZLOADER_BUFRANGE_OFF),
                "zloader_offsets": ("_offsets", ZLOADER_OFFSETS_OFF),
            }, log=print))
        except Exception:
            out = {
                "scenecfg_cur_scene": SCENECFG_CURSCENE_OFF,
                "scene_table_monster": SCENE_TABLE_OFF["monster"],
                "scene_table_npc": SCENE_TABLE_OFF["npc"],
                "scene_table_dummy": SCENE_TABLE_OFF["dummy"],
                "proxies": PROXIES_OFF, "loaders": LOADERS_OFF,
                "zloader_mem": ZLOADER_MEM_OBJ_OFF,
                "zloader_int_pool": ZLOADER_INTARRAYPOOL_OFF,
                "zloader_str_pool": ZLOADER_STRINGPOOL_OFF,
                "zloader_data_size": ZLOADER_DATASIZE_OFF,
                "zloader_buf_range": ZLOADER_BUFRANGE_OFF,
                "zloader_offsets": ZLOADER_OFFSETS_OFF,
            }
        self._off = out
        return out

    def _scene_table_off(self, kind: str) -> Optional[int]:
        return self._offsets().get(f"scene_table_{kind}")

    # ---- klass resolution (by name, version-robust) ----
    def _kname(self, kp: int) -> str:
        if not _plaus(kp):
            return ""
        np = self.pm.read_u64(kp + CLASS_NAME_OFF)
        b = self.pm.read_bytes(np, 48) if _plaus(np) else None
        return b.split(b"\x00", 1)[0].decode("utf-8", "replace") if b else ""

    def _resolve(self, candidates) -> Tuple[str, int]:
        want = set(candidates)
        idx = resolve_klasses(self.pm, want, time_budget_s=30)
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
        off = self._offsets()
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
                scene = self.pm.read_i32(a + off["scenecfg_cur_scene"])
                mt = self.pm.read_u64(a + off["scene_table_monster"])
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
    def _row_proxy(self, row: int) -> Tuple[int, int]:
        """row object -> (zloader, blob). (0, 0) when the row was never loaded."""
        sf = self._proxymgr_static_fields()
        if not sf:
            return 0, 0
        off = self._offsets()
        pid = self.pm.read_i32(row + ROW_PROXYID_OFF)
        if not pid or pid <= 0:
            return 0, 0
        page, slot = (pid - 1) >> 10, (pid - 1) & (PROXY_PAGE - 1)
        proxies = self.pm.read_u64(sf + off["proxies"])
        if not _plaus(proxies):
            return 0, 0
        inner = self.pm.read_u64(proxies + ARRAY_ELEMS_OFF + page * 8)
        if not _plaus(inner):
            return 0, 0
        proxy = inner + ARRAY_ELEMS_OFF + slot * PROXY_STRIDE
        loader_idx = self.pm.read_i32(proxy + 0)
        base_off = self.pm.read_i32(proxy + 4)
        if loader_idx is None or loader_idx < 0 or base_off is None or base_off < 0:
            return 0, 0
        loaders = self.pm.read_u64(sf + off["loaders"])
        if not _plaus(loaders):
            return 0, 0
        zloader = self.pm.read_u64(loaders + ARRAY_ELEMS_OFF + loader_idx * 8)
        if not _plaus(zloader):
            return 0, 0
        blob = self._loader_blob(zloader, base_off)
        return (zloader, blob) if blob else (0, 0)

    def _row_blob(self, row: int) -> int:
        return self._row_proxy(row)[1]

    def _loader_blob(self, zloader: int, base_off: int) -> int:
        """ZLoader + blob offset -> absolute blob address (0 on failure)."""
        off = self._offsets()
        mem = self.pm.read_u64(zloader + off["zloader_mem"] + POOL_MEM_OBJ_OFF)
        midx = self.pm.read_i32(zloader + off["zloader_mem"] + POOL_MEM_IDX_OFF) or 0
        if not _plaus(mem) or base_off is None or base_off < 0:
            return 0
        return mem + ARRAY_ELEMS_OFF + midx + base_off

    # ---- typed column readers over a row blob ----
    def col_i32(self, blob: int, col: Optional[int]) -> Optional[int]:
        if not blob or col is None or col < 0:
            return None
        return self.pm.read_i32(blob + col)

    def col_u8(self, blob: int, col: Optional[int]) -> Optional[int]:
        """单字节列 (bool / 小枚举如 IsAoe / SkillRangeType)。

        ★bool/枚举列必须按 1 字节读: getter-thunk 的列偏移(table_columns auto-offset)
        是对的, 但用 col_i32 读 4 字节会把相邻字段读进来(IsAoe 实测会读成 14592=
        byte0+byte1<<8)。这是类型读法问题不是 offset 错。"""
        if not blob or col is None or col < 0:
            return None
        b = self.pm.read_bytes(blob + col, 1)
        return b[0] if b else None

    def col_bool(self, blob: int, col: Optional[int]) -> Optional[bool]:
        v = self.col_u8(blob, col)
        return None if v is None else bool(v)

    def col_mlid(self, blob: int, col: Optional[int]) -> Optional[int]:
        """MLString columns store an mlid (resolve via StringPoolBridge)."""
        return self.col_i32(blob, col)

    def col_i32_array(self, zloader: int, blob: int, col: Optional[int]) -> List[int]:
        """Int32Array column: the i32 at blob+col is an offset into the row's
        ZLoader.IntArrayPool; the pooled payload is i16 count + count*4 bytes.
        Returns [] on any bound/plausibility failure."""
        pool_off = self.col_i32(blob, col)
        if pool_off is None or pool_off < 0 or not _plaus(zloader):
            return []
        off = self._offsets()
        pool = zloader + off["zloader_int_pool"]
        pobj = self.pm.read_u64(pool + POOL_MEM_OBJ_OFF)
        pidx = self.pm.read_i32(pool + POOL_MEM_IDX_OFF) or 0
        plen = (self.pm.read_i32(pool + POOL_MEM_LEN_OFF) or 0) & 0x7FFFFFFF
        dsize = self.pm.read_i32(pool + POOL_DATASIZE_OFF) or 0
        if not _plaus(pobj) or dsize != 4 or pool_off + 2 > plen:
            return []
        base = pobj + ARRAY_ELEMS_OFF + pidx + pool_off
        raw = self.pm.read_bytes(base, 2)
        if not raw or len(raw) < 2:
            return []
        count = struct.unpack("<h", raw)[0]
        if count <= 0 or count > _ARRAY_MAX_COUNT or pool_off + 2 + count * dsize > plen:
            return []
        data = self.pm.read_bytes(base + 2, count * dsize)
        if not data or len(data) < count * dsize:
            return []
        return list(struct.unpack(f"<{count}i", data))

    def col_string(self, zloader: int, blob: int, col: Optional[int]) -> str:
        """String column: the i32 at blob+col is an offset into the row's
        ZLoader.StringPool; the pooled payload is i16 length + utf8 bytes.
        Returns "" on any bound/plausibility failure."""
        pool_off = self.col_i32(blob, col)
        if pool_off is None or pool_off < 0 or not _plaus(zloader):
            return ""
        off = self._offsets()
        pool = zloader + off["zloader_str_pool"]
        pobj = self.pm.read_u64(pool + POOL_MEM_OBJ_OFF)
        pidx = self.pm.read_i32(pool + POOL_MEM_IDX_OFF) or 0
        plen = (self.pm.read_i32(pool + POOL_MEM_LEN_OFF) or 0) & 0x7FFFFFFF
        if not _plaus(pobj) or pool_off + 2 > plen:
            return ""
        base = pobj + ARRAY_ELEMS_OFF + pidx + pool_off
        raw = self.pm.read_bytes(base, 2)
        if not raw or len(raw) < 2:
            return ""
        n = struct.unpack("<h", raw)[0]
        if n <= 0 or n > 4096 or pool_off + 2 + n > plen:
            return ""
        data = self.pm.read_bytes(base + 2, n)
        return data.decode("utf-8", "replace") if data else ""

    # ---- row enumeration (global tables: heap scan + loader-walk fallback) ----
    def _resolve_row_klass(self, class_full_name: str) -> int:
        kp = self._row_klass.get(class_full_name)
        if kp is not None:
            return kp
        idx = resolve_klasses(self.pm, {class_full_name}, time_budget_s=90)
        kp = int(idx.get(class_full_name, 0) or 0)
        self._row_klass[class_full_name] = kp
        return kp

    def iter_rows(self, class_full_name: str, limit: Optional[int] = None,
                  max_hits: int = 100000) -> Iterator[Tuple[int, int, int]]:
        """Heap-scan instantiated rows of a table class.

        Yields (row_ptr, zloader, blob); rows whose ReadProxy was never loaded
        (or klass-metadata false hits) are skipped."""
        kp = self._resolve_row_klass(class_full_name)
        if not kp or not self._proxymgr_static_fields():
            return
        n = 0
        for row in self._src.sr.find_instances(kp, max_hits=max_hits):
            zloader, blob = self._row_proxy(row)
            if not blob:
                continue
            yield (row, zloader, blob)
            n += 1
            if limit is not None and n >= limit:
                return

    def _loader_key_check(self, zloader: int, sample: int = 8,
                          min_ratio: float = 0.75) -> bool:
        """A loader serves the table we think it does when its _offsets walk is
        self-consistent: blob col0 (the row id) equals the dictionary key."""
        n = ok = 0
        for key, _zl, blob in self.iter_rows_via_loader("", zloader=zloader):
            v = self.col_i32(blob, 0)
            n += 1
            ok += int(v is not None and v > 0 and int(v) == int(key))
            if n >= sample:
                break
        return n > 0 and (ok / n) >= min_ratio

    def find_loader(self, class_full_name: str) -> int:
        """The ZLoader serving a table class.

        Resolved from instantiated rows' ReadProxy entries. Stale heap objects
        can carry RECYCLED proxy ids that point into another table's loader, so
        candidates are ranked by frequency and validated with _loader_key_check
        before being trusted."""
        zl = self._loader_cache.get(class_full_name)
        if zl:
            return zl
        cands: List[int] = []
        for _row, zloader, _blob in self.iter_rows(class_full_name, limit=96):
            cands.append(zloader)
        if not cands:
            return 0
        from collections import Counter
        ranked = [z for z, _c in Counter(cands).most_common()]
        for z in ranked:
            if self._loader_key_check(z):
                self._loader_cache[class_full_name] = z
                return z
        print(f"[cfgtable] {class_full_name}: no candidate ZLoader passed the "
              f"key/col0 self-consistency check; using the most common one")
        self._loader_cache[class_full_name] = ranked[0]
        return ranked[0]

    def _dict_entries_i32(self, d: int) -> Iterator[Tuple[int, int]]:
        """std .NET Dictionary<long, int> walk -> (key, value)."""
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
            key = self.pm.read_i64(ea + ENTRY_KEY_OFF)
            val = self.pm.read_i32(ea + ENTRY_VAL_OFF)
            if key is None or val is None:
                continue
            yield (key, val)

    def iter_rows_via_loader(self, class_full_name: str,
                             zloader: int = 0) -> Iterator[Tuple[int, int, int]]:
        """Walk the table's ZLoader._offsets (row key -> record index).

        Covers EVERY serialized row, including ones the game never instantiated
        (no heap object, ReadProxy id 0). Mirrors ZLoader.GetRowData:
        ``blob = Memory + _bufferRange.offset + _offsets[key] * DataSize``.
        Yields (row_key, zloader, blob)."""
        zl = zloader or self.find_loader(class_full_name)
        if not _plaus(zl):
            return
        off = self._offsets()
        dsize = self.pm.read_i32(zl + off["zloader_data_size"]) or 0
        if dsize <= 0 or dsize > 0x10000:
            return
        rng_off = self.pm.read_i32(zl + off["zloader_buf_range"]) or 0
        rng_len = (self.pm.read_i32(zl + off["zloader_buf_range"] + 4) or 0) & 0x7FFFFFFF
        offsets_dict = self.pm.read_u64(zl + off["zloader_offsets"])
        for key, rec_idx in self._dict_entries_i32(offsets_dict):
            if rec_idx < 0:
                continue
            byte_off = rec_idx * dsize
            if rng_len and byte_off + dsize > rng_len:
                continue
            blob = self._loader_blob(zl, rng_off + byte_off)
            if blob:
                yield (key, zl, blob)

    def walk_raw(self, kind: str, limit: int = 20):
        """Yield (dict_key, row_ptr, blob, Id, name_mlid) for the first rows (diagnostic)."""
        mgr = self.locate_scenecfg()
        if not mgr:
            print("[cfgtable] SceneConfigMgr not located")
            return
        off = self._scene_table_off(kind) if kind in SCENE_TABLE_OFF else None
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
