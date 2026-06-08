# -*- coding: utf-8 -*-
"""mem_map_name_reader - read the current map/scene DISPLAY name from memory.

The offline scene table has no entry for many scenes (training/open-world ids), so
the editor showed "场景#<id>". The localized name shown in the game's top-left UI
(e.g. "协会活动中心") lives in the client's own config table + localization pool. This
reads it straight from memory, resolving every IL2CPP class + field offset BY NAME
(version-robust, no hard-coded RVAs) — only the IL2CPP/.NET structural layouts
(string / array / NativeArray / Dictionary) are stable constants.

Chain (RE'd + verified live, dump 7068f8aa):
  scene id  (= CharSerialize.SceneData.MapId, read by the bridge)
    -> Bokura.SceneTableBase rows: find_instances(klass) -> per-row ReadProxy blob
       (route2 MemConfigTableReader._row_blob); blob col0 = Id, name_mlid in a column
       auto-detected by "which column's mlid resolves to a CJK string".
    -> Panda.Module.StringPoolRuntimeImpl: allLocalizationString_ (string[]) indexed
       by array slot; indexes_ (NativeArray<KV<int,int>>) maps name_mlid -> slot.
    -> the localized CN name.

Heavy one-shot build (two heap scans + blob reads); run it on a background thread and
cache. Read-only; never raises.
"""
from __future__ import annotations

import struct
from typing import Dict, Optional

from mem_probe.il2cpp.auto_registration_locator import build_live_class_index
from mem_probe.il2cpp.mem_config_table_reader import MemConfigTableReader

# IL2CPP / .NET structural layout (runtime-defined, layout-stable across game patches)
CLASS_FIELDS_OFF = 0x80          # Il2CppClass.fields -> Il2CppFieldInfo[]
FI_NAME_OFF, FI_PARENT_OFF, FI_OFF_OFF, FI_STRIDE = 0x0, 0x10, 0x18, 0x20
STR_LEN_OFF, STR_CHARS_OFF = 0x10, 0x14          # Il2CppString
ARR_LEN_OFF, ARR_ELEMS_OFF = 0x18, 0x20          # IL2CPP array
NATIVEARRAY_LEN_OFF = 0x8                         # NativeArray<T>: m_Buffer@0, m_Length@8
PROXYMGR_STATIC_FIELDS_OFF = 0xB8                 # Il2CppClass.static_fields (route2)

SCENE_CLS = "Bokura.SceneTableBase"
POOL_CLS = "Panda.Module.StringPoolRuntimeImpl"
PROXY_CLS = "Table.Utility.TableProxyManager"

# layout fallbacks if the live field-table walk can't resolve a name (kept tiny)
_FALLBACK = {("StringPoolRuntimeImpl", "allLocalizationString_"): 0x10,
             ("StringPoolRuntimeImpl", "indexes_"): 0x18}


def _plaus(p: Optional[int]) -> bool:
    return bool(p and 0x10000 <= p <= 0x7FFFFFFFFFFF)


def _is_cjk(s: str) -> bool:
    return any("一" <= c <= "鿿" for c in (s or ""))


class MapNameReader:
    """Read-only resolver: scene id -> localized map name. Build once, cache."""

    def __init__(self, dps_source):
        self._src = dps_source
        self._sr = dps_source.sr
        self.pm = dps_source.sr.pm
        self._cfg = MemConfigTableReader(dps_source)
        self._klass: Dict[str, int] = {}
        self._field: Dict[tuple, Optional[int]] = {}
        self._pool_arr = 0
        self._pool_len = 0
        self._pool_kv: Dict[int, int] = {}
        self._scene_names: Dict[int, str] = {}
        self._scene_cfg = 0
        self._ready = False

    # ── auto offset: klass by name + field offset by live field table ──────────
    def _resolve_klass(self, full: str) -> int:
        if full in self._klass:
            return self._klass[full]
        idx = build_live_class_index(self.pm, {full}, time_budget_s=90)
        kp = int(idx.get(full, 0) or 0)
        self._klass[full] = kp
        return kp

    def _field_off(self, klass: int, field: str) -> Optional[int]:
        key = (klass, field)
        if key in self._field:
            return self._field[key]
        off = None
        fields = self.pm.read_u64(klass + CLASS_FIELDS_OFF) if _plaus(klass) else 0
        if _plaus(fields):
            want = field.encode("utf-8")
            backing = ("<%s>k__BackingField" % field).encode("utf-8")
            for i in range(512):
                fi = fields + i * FI_STRIDE
                if self.pm.read_u64(fi + FI_PARENT_OFF) != klass:
                    break
                namep = self.pm.read_u64(fi + FI_NAME_OFF)
                nm = self.pm.read_bytes(namep, 64) if _plaus(namep) else None
                if nm:
                    nm = nm.split(b"\x00", 1)[0]
                    if nm == want or nm == backing:
                        off = self.pm.read_i32(fi + FI_OFF_OFF)
                        break
        self._field[key] = off
        return off

    def _read_str(self, sp: int) -> str:
        if not _plaus(sp):
            return ""
        ln = self.pm.read_u32(sp + STR_LEN_OFF) or 0
        if ln <= 0 or ln > 128:
            return ""
        raw = self.pm.read_bytes(sp + STR_CHARS_OFF, ln * 2)
        return raw.decode("utf-16-le", "replace") if raw else ""

    # ── localization pool: name_mlid -> CN string ─────────────────────────────
    def _ensure_pool(self) -> bool:
        if self._pool_kv:
            return True
        pk = self._resolve_klass(POOL_CLS)
        if not pk:
            return False
        arr_off = self._field_off(pk, "allLocalizationString_") \
            or _FALLBACK[("StringPoolRuntimeImpl", "allLocalizationString_")]
        idx_off = self._field_off(pk, "indexes_") \
            or _FALLBACK[("StringPoolRuntimeImpl", "indexes_")]
        # find_instances yields many klass-metadata false hits; validate the real
        # impl by a sane string[] pool + a sane indexes_ NativeArray + a real string.
        for p in self._sr.find_instances(pk, max_hits=4000):
            arr = self.pm.read_u64(p + arr_off)
            if not _plaus(arr):
                continue
            alen = self.pm.read_u32(arr + ARR_LEN_OFF) or 0
            if not (10000 <= alen <= 500000):
                continue
            if not self._read_str(self.pm.read_u64(arr + ARR_ELEMS_OFF)):
                continue
            ibuf = self.pm.read_u64(p + idx_off)
            ilen = self.pm.read_i32(p + idx_off + NATIVEARRAY_LEN_OFF) or 0
            if not (_plaus(ibuf) and 1000 <= ilen <= 2_000_000):
                continue
            raw = self.pm.read_bytes(ibuf, ilen * 8) or b""
            kv: Dict[int, int] = {}
            for j in range(len(raw) // 8):
                k, v = struct.unpack_from("<ii", raw, j * 8)
                if 0 <= v < alen:
                    kv[k] = v
            if len(kv) < 1000:
                continue
            self._pool_arr, self._pool_len, self._pool_kv = arr, alen, kv
            return True
        return False

    def _resolve_mlid(self, mlid: Optional[int]) -> str:
        if not mlid:
            return ""
        v = self._pool_kv.get(int(mlid))
        if v is None or not (0 <= v < self._pool_len):
            return ""
        return self._read_str(self.pm.read_u64(self._pool_arr + ARR_ELEMS_OFF + v * 8))

    # ── scene table: scene id -> name (auto-detected name column) ─────────────
    def _ensure_scene_index(self) -> bool:
        if self._scene_names:
            return True
        sk = self._resolve_klass(SCENE_CLS)
        pxk = self._resolve_klass(PROXY_CLS)
        if not sk or not pxk:
            return False
        self._cfg._proxymgr_sf = self.pm.read_u64(pxk + PROXYMGR_STATIC_FIELDS_OFF)
        if not _plaus(self._cfg._proxymgr_sf):
            return False
        out: Dict[int, str] = {}
        # name_mlid is the row's 2nd serialized column (col4) for scene rows; probe a
        # few following columns per-row as a fallback (rows are not all the same shape).
        for row in self._sr.find_instances(sk, max_hits=20000):
            blob = self._cfg._row_blob(row)
            if not blob:
                continue
            rid = self.pm.read_i32(blob + 0)
            if rid is None or rid <= 0:
                continue
            for col in (4, 8, 12, 16, 20, 24):
                s = self._resolve_mlid(self.pm.read_i32(blob + col))
                if s and _is_cjk(s):
                    out[int(rid)] = s
                    break
        self._scene_names = out
        return bool(out)

    # ── current scene id: SceneConfigMgr.curSceneId_ (CharSerialize.SceneData is
    #    null in many scenes, so it's not a reliable source) ─────────────────────
    SCENECFG_CLS = "SceneConfigMgr"
    SCENECFG_CUR_OFF = 0xE0          # curSceneId_
    SCENECFG_SCENE_TABLE_OFF = 0xD8  # sceneTable_
    SCENECFG_FIRST_TABLE_OFF = 0x50  # monsterTable_ (first of ~18 consecutive ZTable ptrs)

    def _locate_scene_cfg(self) -> int:
        """SceneConfigMgr singleton (klass not pointer-scannable in the GA image, so
        located structurally): an object carrying a long run of heap-pointer table
        fields (0x50..0xD8) AND a known scene id at curSceneId_(0xE0)."""
        if self._scene_cfg and self.pm.read_i32(self._scene_cfg + self.SCENECFG_CUR_OFF) in self._scene_names:
            return self._scene_cfg
        try:
            import numpy as np
        except Exception:
            np = None
        sset = set(self._scene_names.keys())
        if np is None or not sset:
            return 0
        sarr = np.fromiter(sset, dtype="<u4")
        cur_off = self.SCENECFG_CUR_OFF
        for r in self.pm.iter_regions(only_readable=True, only_private=True):
            if r.size > 64 * 1024 * 1024:
                continue
            blob = self.pm.read_bytes(r.base, r.size)
            if not blob:
                continue
            u = np.frombuffer(blob[:(len(blob) // 4) * 4], dtype="<u4")
            hits = np.nonzero(np.isin(u, sarr))[0]
            for i in hits.tolist():
                obj = r.base + i * 4 - cur_off
                st = self.pm.read_u64(obj + self.SCENECFG_SCENE_TABLE_OFF)
                mt = self.pm.read_u64(obj + self.SCENECFG_FIRST_TABLE_OFF)
                if not (_plaus(st) and _plaus(mt) and st != mt):
                    continue
                # confirm the long table-pointer run (most of 0x50..0xD8 are heap ptrs)
                run = sum(1 for o in range(0x50, 0xE0, 8)
                          if _plaus(self.pm.read_u64(obj + o)))
                if run >= 12 and self.pm.read_i32(obj + cur_off) in sset:
                    self._scene_cfg = obj
                    return obj
        return 0

    def current_scene_id(self) -> int:
        cfg = self._locate_scene_cfg()
        if not cfg:
            return 0
        return int(self.pm.read_i32(cfg + self.SCENECFG_CUR_OFF) or 0)

    def current_map_name(self) -> str:
        return self.name_for_scene(self.current_scene_id())

    # ── public ────────────────────────────────────────────────────────────────
    def build(self) -> bool:
        """One-shot heavy resolve (two heap scans). Returns True if names are ready."""
        try:
            if not self._ensure_pool():
                return False
            if not self._ensure_scene_index():
                return False
            self._ready = True
            return True
        except Exception:
            return False

    @property
    def ready(self) -> bool:
        return self._ready

    def name_for_scene(self, scene_id) -> str:
        try:
            return self._scene_names.get(int(scene_id or 0), "")
        except Exception:
            return ""


def _selftest():
    from mem_probe.il2cpp.static_dps_source import StaticDpsSource
    import time
    src = StaticDpsSource()
    _ = src.sr
    r = MapNameReader(src)
    t0 = time.time()
    ok = r.build()
    print(f"build ok={ok} scenes={len(r._scene_names)} ({time.time()-t0:.1f}s)")
    # show a few + the live scene if the provider exposes it
    for sid, nm in list(r._scene_names.items())[:10]:
        print(f"  {sid} -> {nm!r}")


if __name__ == "__main__":
    _selftest()
