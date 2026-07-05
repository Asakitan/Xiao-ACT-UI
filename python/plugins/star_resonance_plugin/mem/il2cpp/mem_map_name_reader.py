# -*- coding: utf-8 -*-
# mem_map_name_reader - read the current map/scene DISPLAY name from memory.
#
# The offline scene table has no entry for many scenes (training/open-world ids), so
# the editor showed "场景#<id>". The localized name shown in the game's top-left UI
# (e.g. "协会活动中心") lives in the client's own config table + localization pool. This
# reads it straight from memory, resolving every IL2CPP class + field offset BY NAME
# (version-robust, no hard-coded RVAs) — only the IL2CPP/.NET structural layouts
# (string / array / NativeArray / Dictionary) are stable constants.
#
# Chain (RE'd + verified live, dump 7068f8aa):
# scene id  (= CharSerialize.SceneData.MapId, read by the bridge)
# -> Bokura.SceneTableBase rows: find_instances(klass) -> per-row ReadProxy blob
# (route2 MemConfigTableReader._row_blob); blob col0 = Id, name_mlid in a column
# auto-detected by "which column's mlid resolves to a CJK string".
# -> Panda.Module.StringPoolRuntimeImpl: allLocalizationString_ (string[]) indexed
# by array slot; indexes_ (NativeArray<KV<int,int>>) maps name_mlid -> slot.
# -> the localized CN name.
#
# Heavy one-shot build (two heap scans + blob reads); run it on a background thread and
# cache. Read-only; never raises.
from __future__ import annotations

from typing import Dict, Optional

from plugins.star_resonance_plugin.mem.il2cpp.mem_config_table_reader import MemConfigTableReader
from plugins.star_resonance_plugin.mem.il2cpp.mem_string_pool import (
    StringPoolBridge, POOL_CLS, _FALLBACK, _plaus, _is_cjk,
    CLASS_FIELDS_OFF, FI_NAME_OFF, FI_PARENT_OFF, FI_OFF_OFF, FI_STRIDE,
    STR_LEN_OFF, STR_CHARS_OFF, ARR_LEN_OFF, ARR_ELEMS_OFF, NATIVEARRAY_LEN_OFF,
)
from plugins.star_resonance_plugin.mem.il2cpp import root_pointer_cache as _rpc

try:
    from mem_probe import cy_memscan as _cy
except Exception:  # pragma: no cover
    _cy = None

PROXYMGR_STATIC_FIELDS_OFF = 0xB8                 # Il2CppClass.static_fields (route2)

SCENE_CLS = "Bokura.SceneTableBase"
PROXY_CLS = "Table.Utility.TableProxyManager"
_RPC_SCENECFG = "SceneConfigMgr"


class MapNameReader:
    # Read-only resolver: scene id -> localized map name. Build once, cache.

    def __init__(self, dps_source):
        self._src = dps_source
        self._sr = dps_source.sr
        self.pm = dps_source.sr.pm
        self._cfg = MemConfigTableReader(dps_source)
        self._pool = StringPoolBridge(dps_source)
        self._scene_names: Dict[int, str] = {}
        self._scene_cfg = 0
        self._scene_cfg_hint = 0          # region base that last held SceneConfigMgr
        self._scene_cfg_klass = 0
        self._cfg_scratch = None          # reused scan buffer
        self._ready = False
        self._scene_cfg_offsets_ready = False
        self._scene_cur_off = self.SCENECFG_CUR_OFF
        self._scene_table_off = self.SCENECFG_SCENE_TABLE_OFF
        self._scene_first_table_off = self.SCENECFG_FIRST_TABLE_OFF
        self._game_key = ""
        try:
            self._game_key = getattr(dps_source, "game_key", "") or \
                (dps_source.sr.bundle_meta.get("ga_sha256_first_1mb") if
                 getattr(dps_source, "sr", None) else "") or ""
        except Exception:
            self._game_key = ""
        if self._game_key:
            cached = _rpc.get_entry(self._game_key, _RPC_SCENECFG) or {}
            addr = cached.get("addr") if isinstance(cached.get("addr"), int) else 0
            klass = cached.get("klass_addr") if isinstance(cached.get("klass_addr"), int) else 0
            if addr and klass:
                self._scene_cfg = addr
                self._scene_cfg_klass = klass

    # ── klass/field/string resolution: delegated to the shared bridge ─────────
    def _resolve_klass(self, full: str) -> int:
        return self._pool.resolve_klass(full)

    def _field_off(self, klass: int, field: str) -> Optional[int]:
        return self._pool.field_off(klass, field)

    def _read_str(self, sp: int) -> str:
        return self._pool.read_str(sp)

    # ── localization pool: name_mlid -> CN string (StringPoolBridge) ──────────
    def _ensure_pool(self) -> bool:
        return self._pool.build()

    def _resolve_mlid(self, mlid: Optional[int]) -> str:
        return self._pool.resolve(mlid)

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
    # Verified-live literal fallbacks for the SceneConfigMgr singleton's three
    # fields. Resolved by name (via StringPoolBridge's live field-table walk) on
    # first use; the class attribute remains as the self-healing fallback.
    SCENECFG_CUR_OFF = 0xE0          # curSceneId_
    SCENECFG_SCENE_TABLE_OFF = 0xD8  # sceneTable_
    SCENECFG_FIRST_TABLE_OFF = 0x50  # monsterTable_ (first of ~18 consecutive ZTable ptrs)

    def _scene_cfg_off(self, field: str, fallback: int) -> int:
        # Auto-offset a SceneConfigMgr field by name (live field table -> literal).
        try:
            k = self._resolve_klass(self.SCENECFG_CLS)
            if k:
                off = self._field_off(k, field)
                if off is not None and off > 0:
                    return int(off)
        except Exception:
            pass
        return fallback

    def _resolve_scene_cfg_offsets(self) -> None:
        if self._scene_cfg_offsets_ready:
            return
        self._scene_cur_off = self._scene_cfg_off("curSceneId_", self.SCENECFG_CUR_OFF)
        self._scene_table_off = self._scene_cfg_off("sceneTable_", self.SCENECFG_SCENE_TABLE_OFF)
        self._scene_first_table_off = self._scene_cfg_off("monsterTable_", self.SCENECFG_FIRST_TABLE_OFF)
        self._scene_cfg_offsets_ready = True

    def _validate_scene_cfg(self, obj: int, sset: set) -> bool:
        # One block read of 0x50..0xE4 + local checks (was 14 single RPMs).
        if obj < 0:
            return False
        self._resolve_scene_cfg_offsets()
        if min(self._scene_cur_off, self._scene_table_off, self._scene_first_table_off) < 0x50:
            return False
        end_off = max(self._scene_cur_off + 4, self._scene_table_off + 8,
                      self._scene_first_table_off + 8, self.SCENECFG_CUR_OFF + 4)
        blob = self.pm.read_bytes(obj + 0x50, end_off - 0x50)
        if not blob or len(blob) < end_off - 0x50:
            return False
        import struct
        st = struct.unpack_from("<Q", blob, self._scene_table_off - 0x50)[0]
        mt = struct.unpack_from("<Q", blob, self._scene_first_table_off - 0x50)[0]
        if not (_plaus(st) and _plaus(mt) and st != mt):
            return False
        run = 0
        for o in range(0x50, 0xE0, 8):
            p = struct.unpack_from("<Q", blob, o - 0x50)[0]
            if _plaus(p):
                run += 1
        if run < 12:
            return False
        cur = struct.unpack_from("<i", blob, self._scene_cur_off - 0x50)[0]
        return cur in sset

    def _locate_scene_cfg(self, hint_scene_id: int = 0) -> int:
        # SceneConfigMgr singleton (klass not pointer-scannable in the GA image, so
        # located structurally): an object carrying a long run of heap-pointer table
        # fields (0x50..0xD8) AND a known scene id at curSceneId_(0xE0).
        #
        # ``hint_scene_id`` (the TCP-known current scene id, when available) makes the
        # heap value-scan a single-needle search instead of an N-way set scan, and is
        # the most specific anchor; the cython kernel + a reused scratch buffer replace
        # the per-region numpy copy. A confirmed object's region is remembered so a
        # re-locate after invalidation re-scans it first.
        self._resolve_scene_cfg_offsets()
        sset = set(self._scene_names.keys())
        if not sset:
            return 0
        if self._scene_cfg and self._scene_cfg_klass:
            trusted = False
            if self._game_key:
                trusted = _rpc.validate(self._game_key, _RPC_SCENECFG, self.pm)
            else:
                try:
                    trusted = self.pm.read_u64(self._scene_cfg) == self._scene_cfg_klass
                except Exception:
                    trusted = False
            if trusted:
                return self._scene_cfg
            if self._game_key:
                _rpc.invalidate(self._game_key, _RPC_SCENECFG)
            self._scene_cfg = 0
            self._scene_cfg_klass = 0
        cur_off = self._scene_cur_off
        # Prefer the single TCP-known scene id as the needle; fall back to the full
        # known-id set. The id is a u32 at curSceneId_; subtract the offset for obj.
        needle_one = int(hint_scene_id) if (hint_scene_id and hint_scene_id in sset) else 0

        def _scan_region(r) -> int:
            read_into = getattr(self.pm, "read_bytes_into", None)
            if self._cfg_scratch is None or len(self._cfg_scratch) < r.size:
                self._cfg_scratch = bytearray(min(r.size, 64 * 1024 * 1024))
            n = min(r.size, len(self._cfg_scratch))
            if read_into is not None:
                got = read_into(r.base, self._cfg_scratch, n)
                if got <= 0:
                    return 0
                mv = memoryview(self._cfg_scratch)[:got]
            else:
                blob = self.pm.read_bytes(r.base, n)
                if not blob:
                    return 0
                mv = blob
                got = n
            if _cy is not None and needle_one and hasattr(_cy, "find_aligned_u32"):
                # 4-aligned only; curSceneId_ is at obj+0xE0 (8-aligned obj -> 0xE0
                # is 4-aligned), so an aligned scan can't miss it.
                hit_offs = [o for o in _cy.find_aligned_u32(mv, needle_one & 0xFFFFFFFF, 4096)]
                cands = ((o, needle_one) for o in hit_offs)
            elif _cy is not None and hasattr(_cy, "find_aligned_u32_in_set"):
                cands = _cy.find_aligned_u32_in_set(mv, sset, 1 << 20)
            else:
                return 0
            for item in cands:
                i = item[0] if isinstance(item, tuple) else item
                obj = r.base + i - cur_off
                if self._validate_scene_cfg(obj, sset):
                    return obj
            return 0

        # warm hint first
        if self._scene_cfg_hint:
            for r in self.pm.iter_regions(only_readable=True, only_private=True):
                if r.base == self._scene_cfg_hint and r.size <= 64 * 1024 * 1024:
                    obj = _scan_region(r)
                    if obj:
                        self._scene_cfg = obj
                        self._scene_cfg_klass = int(self.pm.read_u64(obj) or 0)
                        if self._game_key and self._scene_cfg_klass:
                            _rpc.set(self._game_key, _RPC_SCENECFG,
                                     addr=obj, klass_addr=self._scene_cfg_klass,
                                     klass_name=_RPC_SCENECFG,
                                     extras={"hint_region_base": r.base})
                            _rpc.persist(self._game_key)
                        return obj
                    break
        for r in self.pm.iter_regions(only_readable=True, only_private=True):
            if r.size > 64 * 1024 * 1024:
                continue
            obj = _scan_region(r)
            if obj:
                self._scene_cfg = obj
                self._scene_cfg_hint = r.base
                self._scene_cfg_klass = int(self.pm.read_u64(obj) or 0)
                if self._game_key and self._scene_cfg_klass:
                    _rpc.set(self._game_key, _RPC_SCENECFG,
                             addr=obj, klass_addr=self._scene_cfg_klass,
                             klass_name=_RPC_SCENECFG,
                             extras={"hint_region_base": r.base})
                    _rpc.persist(self._game_key)
                return obj
        return 0

    def current_scene_id(self, hint_scene_id: int = 0) -> int:
        cfg = self._locate_scene_cfg(hint_scene_id)
        if not cfg:
            return 0
        return int(self.pm.read_i32(cfg + self._scene_cur_off) or 0)

    def current_map_name(self, hint_scene_id: int = 0) -> str:
        return self.name_for_scene(self.current_scene_id(hint_scene_id))

    # ── public ────────────────────────────────────────────────────────────────
    def build(self) -> bool:
        # One-shot heavy resolve (two heap scans). Returns True if names are ready.
        try:
            # Resolve every class this reader needs in ONE shared-index pass (the
            # union), so first launch scans the GA image once instead of 3x; warm
            # launches hit the persisted per-version RVAs. The individual
            # resolve_klass calls below then just read the cache.
            try:
                from plugins.star_resonance_plugin.mem.il2cpp.klass_index import resolve_klasses
                resolve_klasses(self.pm, {POOL_CLS, SCENE_CLS, PROXY_CLS}, time_budget_s=90)
            except Exception:
                pass
            if not self._ensure_pool():
                return False
            self._resolve_scene_cfg_offsets()
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
    from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
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
