"""Phase 4 — Panda.ZGame.ZEntityMgr reader.

通过堆扫描定位 ZEntityMgr 单例实例 (用已知 player_uuid 作锚点),
然后读 bossDict_/monsterDict_/npcDict_ (.NET Dictionary<long, ZEntity>) 列出实体.

ZEntityMgr 字段 (verified against dump fdc7111b):
  +0x10  long                       playerUuid_
  +0x18  PlayerEnt                  playerEnt_
  +0x28  Dictionary<long, ZEntity>  entityDict_
  +0x60  Dictionary<long, ZEntity>  bossDict_         ← BOSS HP 主源
  +0x68  Dictionary<long, ZEntity>  monsterDict_
  +0x70  Dictionary<long, ZEntity>  npcDict_

ZEntity 字段 (verified against dump fdc7111b):
  +0x28  EEntityState                                 entityState_
  +0x48  ZAttrCollection                              attrs_
  +0xC0  long                                         Uuid
  +0xC8  long                                         ConfigUuid  (template id → 名字表)
  +0xD8  long                                         CharId

ZAttrCollection (HP/状态/仇恨等数值走 attrs):
  +0x18  ZAttrCacheSlim              cacheSlim_   (Burst/SIMD cache — 不解码)
  +0x28  Dictionary<uint, IMixAttr>  mixItemDict_  ← 由 mem_attr_reader.ZAttrReader 解码

v2: 接入 ZAttrReader 后可输出 HP/MaxHp/breaking_stage/extinction/stunned/overdrive/
hated_char 等数值 (attr id = packet_parser.enums.AttrType)。ZAttr 的 Value 偏移随
ZMixAttr<T> 的 T 变化, 故 ZAttrReader 在运行时用 TCP 已知值按 klass 标定一次。
未提供/未标定 reader 时退回 v1 行为 (uuid + config_uuid + state)。
"""
from __future__ import annotations

import glob
import os
import sys
import time
from dataclasses import dataclass, field
from typing import Iterable, List, Optional, Tuple

from mem_probe.il2cpp.static_dps_source import StaticDpsSource
from mem_probe.il2cpp.script_parser import ScriptIndex
from mem_probe.il2cpp import auto_offsets as _ao
from mem_probe.il2cpp import root_pointer_cache as _rpc

try:
    from mem_probe import cy_memscan as _cy
except Exception:  # pragma: no cover
    _cy = None


# RootPointerCache named entry for the ZEntityMgr singleton. Per the O(1) steady-state
# contract: bootstrap writes both ``addr`` and ``klass_addr`` here so Stage B can
# validate via a single read_u64 + compare, never a scan.
_RPC_NAME = "ZEntityMgr"


# ────────── 常量 ──────────
ENTITY_MGR_CLASS = "Panda.ZGame.ZEntityMgr"
ENTITY_CLASS = "Panda.ZGame.ZEntity"
PLAYERUUID_OFF = 0x10
PLAYERENT_OFF = 0x18
ENTITY_DICT_OFF = 0x28
BOSS_DICT_OFF = 0x60
MONSTER_DICT_OFF = 0x68
NPC_DICT_OFF = 0x70

# .NET Dictionary<long, ZEntity*> 布局 (Mono/IL2CPP runtime):
#   +0x10  buckets   (int[])
#   +0x18  entries   (Entry[])
#   +0x20  count     (int)
# Entry { int hashCode; int next; long key; ref value; } = 24 bytes
DICT_BUCKETS_OFF = 0x10
DICT_ENTRIES_OFF = 0x18
DICT_COUNT_OFF = 0x20

ARRAY_LENGTH_OFF = 0x18
ARRAY_ELEMS_OFF = 0x20

ENTRY_HASH_OFF = 0
ENTRY_NEXT_OFF = 4
ENTRY_KEY_OFF = 8
ENTRY_VAL_OFF = 16
ENTRY_SIZE = 24

# ZEntity 字段 (fdc7111b)
ENT_UUID_OFF = 0xC0
ENT_CONFIG_OFF = 0xC8   # ConfigUuid (runtime instance id; == uuid for pooled mobs)
ENT_ENTID_OFF = 0xD0
ENT_CHARID_OFF = 0xD8
ENT_BASEID_OFF = 0xE0   # BaseId — the monster/template id -> name table (e.g. 122=精英守护木桩)
ENT_ATTRS_OFF = 0x48    # attrs_ → ZAttrCollection
ENT_STATE_OFF = 0x28

# Combat attribute ids (== packet_parser.enums.AttrType) read via ZAttrReader.
A_HP = 11310
A_MAX_HP = 11320
A_MAX_EXTINCTION = 440
A_EXTINCTION = 441
A_MAX_STUNNED = 442
A_STUNNED = 443
A_IN_OVERDRIVE = 444
A_BREAKING_STAGE = 455
A_HATED_CHAR_ID = 471
A_SKILL_ID = 0x64       # current/triggered skill id
_ENTITY_NUMERIC_ATTRS = (
    A_HP, A_MAX_HP, A_MAX_EXTINCTION, A_EXTINCTION, A_MAX_STUNNED, A_STUNNED,
    A_IN_OVERDRIVE, A_BREAKING_STAGE, A_HATED_CHAR_ID, A_SKILL_ID,
)


@dataclass
class EntitySnap:
    uuid: int = 0
    char_id: int = 0
    state: int = 0
    obj_addr: int = 0
    config_uuid: int = 0          # template id → 名字表
    # 数值字段 (经 ZAttrReader 解码; reader 未标定时保持 0/None)
    cur_hp: int = 0
    max_hp: int = 0
    breaking_stage: int = 0
    extinction: int = 0
    max_extinction: int = 0
    stunned: int = 0
    max_stunned: int = 0
    in_overdrive: int = 0
    hated_char_id: int = 0
    cast_skill_id: int = 0
    attrs_read: bool = False      # True if ZAttr numeric fields were filled

    @property
    def hp_pct(self) -> float:
        return (float(self.cur_hp) / float(self.max_hp)) if self.max_hp > 0 else 0.0


@dataclass
class EntityMgrSnap:
    mgr_addr: int = 0
    player_uuid: int = 0
    bosses: List[EntitySnap] = field(default_factory=list)
    monsters: List[EntitySnap] = field(default_factory=list)
    npcs: List[EntitySnap] = field(default_factory=list)
    fetched_at: float = 0.0


class EntityMgrReader:
    """寻找并读取 ZEntityMgr 单例.

    使用方式:
        emr = EntityMgrReader(dps_source)
        snap = emr.read(player_uuid=36668136)
    """

    def __init__(self, dps_source: StaticDpsSource):
        self._src = dps_source
        self._mgr_addr: int = 0
        self._mgr_klass: int = 0
        self._last_uuid: int = 0
        self._last_check: float = 0.0
        self._full_si: Optional[ScriptIndex] = None
        # Cold-locate state: a confirmed mgr's region is the warm hint for the
        # next acquisition; a failed scan backs off so loading screens / entity-less
        # scenes don't re-sweep the whole heap every 1s tick.
        self._hint_base: int = 0
        self._scratch: Optional[bytearray] = None
        self._miss_until: float = 0.0
        self._miss_backoff: float = 1.0
        # auto-offset: resolve ZEntityMgr/ZEntity field offsets BY NAME from the dump
        # once, with the verified literals as fallback. The inline heap scan and the
        # batched id reads then use these ints exactly like the old constants, so the
        # layout self-heals on a game patch with zero per-tick cost.
        self._resolve_offsets()

    def _resolve_offsets(self) -> None:
        mgr = _ao.resolve(self._src, ENTITY_MGR_CLASS, {
            "off_player_uuid": ("playerUuid_", PLAYERUUID_OFF),
            "off_player_ent": ("playerEnt_", PLAYERENT_OFF),
            "off_entity_dict": ("entityDict_", ENTITY_DICT_OFF),
            "off_boss_dict": ("bossDict_", BOSS_DICT_OFF),
            "off_monster_dict": ("monsterDict_", MONSTER_DICT_OFF),
            "off_npc_dict": ("npcDict_", NPC_DICT_OFF),
        })
        ent = _ao.resolve(self._src, ENTITY_CLASS, {
            "off_ent_attrs": ("attrs_", ENT_ATTRS_OFF),
            "off_ent_state": ("entityState_", ENT_STATE_OFF),
            "off_ent_uuid": ("Uuid", ENT_UUID_OFF),
            "off_ent_config": ("ConfigUuid", ENT_CONFIG_OFF),
            "off_ent_entid": ("EntId", ENT_ENTID_OFF),
            "off_ent_charid": ("CharId", ENT_CHARID_OFF),
            "off_ent_baseid": ("BaseId", ENT_BASEID_OFF),
        })
        for k, v in {**mgr, **ent}.items():
            setattr(self, k, int(v))
        # bytes that must be in-bounds for the inline mgr-field reads in _scan_for_mgr
        self._scan_field_span = max(
            self.off_player_uuid, self.off_entity_dict, self.off_boss_dict,
            self.off_monster_dict, self.off_npc_dict) + 8
        # Phase 4: hydrate from RootPointerCache so Stage A (full heap scan + klass
        # resolve) is skipped entirely on warm starts. The cached klass_addr is the
        # Stage-B trust needle — a single read_u64 + compare in locate().
        self._game_key = ""
        try:
            self._game_key = getattr(self._src, "game_key", "") or \
                (self._src.sr.bundle_meta.get("ga_sha256_first_1mb") if
                 getattr(self._src, "sr", None) else "") or ""
        except Exception:
            self._game_key = ""
        if self._game_key:
            cached = _rpc.get_entry(self._game_key, _RPC_NAME) or {}
            addr = cached.get("addr") if isinstance(cached.get("addr"), int) else 0
            klass = cached.get("klass_addr") if isinstance(cached.get("klass_addr"), int) else 0
            if addr and klass:
                self._mgr_addr = addr
                self._mgr_klass = klass

    def _klass_name(self, kp: int) -> str:
        """Read an Il2CppClass name (klass+0x10 -> char*) for validation."""
        try:
            pm = self._src.sr.pm
            np = pm.read_u64(kp + 0x10)
            if not (0x10000 <= (np or 0) <= 0x7FFFFFFFFFFF):
                return ""
            b = pm.read_bytes(np, 40)
            return b.split(b"\x00", 1)[0].decode("utf-8", "replace") if b else ""
        except Exception:
            return ""

    def _resolve_mgr_klass(self) -> int:
        """Resolve ZEntityMgr's Il2CppClass* robustly, version-independently.

        Tries the active bundle, then EVERY on-disk dump's script.json (newest
        first), validating each candidate by reading the live klass name. No
        hard-coded dump_id (which drifts on every game update); the result is
        cached and re-validated so a relaunch/patch can't silently use a stale
        klass.
        """
        if self._mgr_klass and self._klass_name(self._mgr_klass) == "ZEntityMgr":
            return self._mgr_klass
        self._mgr_klass = 0
        sr = self._src.sr
        ga = int(sr.ga)
        # 1) active bundle (fast path when the curated bundle includes it)
        try:
            kp = sr.resolve_klass(ENTITY_MGR_CLASS) or 0
            if kp and self._klass_name(kp) == "ZEntityMgr":
                self._mgr_klass = kp
                return kp
        except Exception:
            pass
        # 2) in-memory live locator: derive the klass pointer by NAME from process
        #    memory (no dump, no script.json, version-robust, onedir-safe). This is
        #    the production path for frozen clients and for new game versions.
        try:
            from mem_probe.il2cpp.klass_index import resolve_klasses
            idx = resolve_klasses(sr.pm, {ENTITY_MGR_CLASS}, time_budget_s=30)
            kp = int(idx.get(ENTITY_MGR_CLASS, 0) or 0)
            if kp and self._klass_name(kp) == "ZEntityMgr":
                self._mgr_klass = kp
                return kp
        except Exception as e:
            print(f"[entity-mgr] live locator failed: {e}", file=sys.stderr)
        # 3) any on-disk dump's script.json (dev fallback), newest first, name-validated
        here = os.path.dirname(os.path.abspath(__file__))
        out_dir = os.path.join(here, "out")
        try:
            sjs = glob.glob(os.path.join(out_dir, "*", "script.json")) + \
                glob.glob(os.path.join(out_dir, "*", "dumper_out", "script.json"))
            sjs.sort(key=lambda p: os.path.getmtime(p), reverse=True)
        except Exception:
            sjs = []
        for sj in sjs:
            try:
                si = ScriptIndex.load(sj)
                rva = si.find_klass(ENTITY_MGR_CLASS)
                if not rva:
                    continue
                kp = sr.pm.read_u64(ga + rva) or 0
                if kp and self._klass_name(kp) == "ZEntityMgr":
                    self._mgr_klass = kp
                    return kp
            except Exception as e:
                print(f"[entity-mgr] resolve via {os.path.basename(os.path.dirname(sj))} "
                      f"failed: {e}", file=sys.stderr)
        return 0

    # ---------- 锚点扫描 ----------

    def _scan_one_region_for_mgr(self, r, klass_ptr: int, player_uuid: int):
        """Scan one region for the ZEntityMgr instance.

        Returns ``(obj, score)`` for the best klass-sentinel hit in this region
        whose 4 dict fields are distinct heap pointers and whose entityDict_.count
        is sane, or ``(0, -1)``. An exact playerUuid_ match returns immediately
        with score = 1<<62 so the caller stops the whole sweep.

        The klass-pointer hunt runs in the Cython AVX2 kernel over a reused scratch
        buffer (zero-copy); only the handful of aligned hits pay the inline field
        decode. (Was a pure-Python ``bytes.find`` walk + per-hit Python decode — the
        single heaviest Python full-heap scan in the probe.)
        """
        pm = self._src.sr.pm
        MIN_HEAP = 0x0000_0000_0010_0000
        MAX_HEAP = 0x0000_7FFF_FFFF_FFFF

        def _looks_heap(p: int) -> bool:
            return MIN_HEAP <= p <= MAX_HEAP and (p & 0x7) == 0

        read_into = getattr(pm, "read_bytes_into", None)
        chunk = 16 * 1024 * 1024
        span = self._scan_field_span
        if self._scratch is None or len(self._scratch) < min(chunk, r.size):
            self._scratch = bytearray(min(chunk, r.size))
        best_obj = 0
        best_score = -1
        off = 0
        while off < r.size:
            n = min(chunk, r.size - off)
            if read_into is not None:
                got = read_into(r.base + off, self._scratch, n)
                if got <= 0:
                    break
                buf = self._scratch
            else:
                blob = pm.read_bytes(r.base + off, n)
                if blob is None:
                    break
                buf = blob
                got = len(blob)
            mv = memoryview(buf)[:got]
            if _cy is not None and hasattr(_cy, "find_aligned_u64"):
                hits = _cy.find_aligned_u64(mv, klass_ptr, 4096)
            else:  # pragma: no cover - pure-Python last resort
                kpb = klass_ptr.to_bytes(8, "little")
                bb = bytes(mv)
                hits, s = [], 0
                while True:
                    j = bb.find(kpb, s)
                    if j < 0:
                        break
                    if (j & 7) == 0:
                        hits.append(j)
                    s = j + 8
            for idx in hits:
                if idx + span > got:
                    continue
                bd = int.from_bytes(mv[idx + self.off_boss_dict:idx + self.off_boss_dict + 8], "little")
                md = int.from_bytes(mv[idx + self.off_monster_dict:idx + self.off_monster_dict + 8], "little")
                nd = int.from_bytes(mv[idx + self.off_npc_dict:idx + self.off_npc_dict + 8], "little")
                ed = int.from_bytes(mv[idx + self.off_entity_dict:idx + self.off_entity_dict + 8], "little")
                if not (_looks_heap(bd) and _looks_heap(md) and _looks_heap(nd)
                        and _looks_heap(ed) and len({bd, md, nd, ed}) == 4):
                    continue
                obj = r.base + off + idx
                puid = int.from_bytes(mv[idx + self.off_player_uuid:idx + self.off_player_uuid + 8],
                                      "little", signed=True)
                try:
                    ed_cnt = pm.read_i32(ed + DICT_COUNT_OFF)
                except Exception:
                    ed_cnt = None
                if ed_cnt is not None and 0 <= ed_cnt <= 8192:
                    if puid == player_uuid and player_uuid:
                        return obj, (1 << 62)   # absolute self, stop everything
                    if ed_cnt > best_score:
                        best_score = ed_cnt
                        best_obj = obj
            off += n
        return best_obj, best_score

    def _scan_for_mgr(self, player_uuid: int) -> Optional[int]:
        """单遍扫描堆: 找 *(addr)==klass_ptr 且 bd/md/nd 都是有效堆指针 + entityDict_.count 合理.

        早退条件: playerUuid_ 字段匹配 (绝对真身); 否则收集 best-by-count.
        命中 region 记为 warm hint, 下次冷定位先扫它。
        """
        sr = self._src.sr
        pm = sr.pm
        klass_ptr = self._resolve_mgr_klass()
        self._mgr_klass = klass_ptr
        if not klass_ptr:
            return None

        best_obj = 0
        best_score = -1
        # Warm hint: re-scan the region that last held the mgr before the full sweep.
        if self._hint_base:
            for r in pm.iter_regions(only_readable=True, only_private=True):
                if r.base == self._hint_base:
                    obj, score = self._scan_one_region_for_mgr(r, klass_ptr, player_uuid)
                    if score >= (1 << 62):
                        self._hint_base = r.base
                        return obj
                    if score > best_score:
                        best_score, best_obj = score, obj
                    break
            if best_obj:
                return best_obj

        for r in pm.iter_regions(only_readable=True, only_private=True):
            obj, score = self._scan_one_region_for_mgr(r, klass_ptr, player_uuid)
            if score >= (1 << 62):
                self._hint_base = r.base
                return obj
            if score > best_score:
                best_score, best_obj = score, obj
                self._hint_base = r.base
        return best_obj or None

    def locate(self, player_uuid: int, force_rescan: bool = False) -> Optional[int]:
        """返回 ZEntityMgr 实例地址. 缓存已找到的 mgr_addr, 用 player_uuid 校验依然有效.

        Phase 4 O(1) steady-state: when the in-memory addr is set, validation is a
        SINGLE read_u64 + klass compare (no scan, no _resolve_mgr_klass metadata
        walk). On failure the addr is dropped to the slow Stage-A path. The hot
        tick therefore pays at most one syscall per call when warm.
        """
        if not force_rescan and self._mgr_addr and self._mgr_klass:
            # Stage-B trust check: one read_u64(addr) == cached klass pointer.
            # RootPointerCache.validate() contracts exactly this and is the only
            # Stage-B read that can fail (cross-version / singleton respawn).
            if self._game_key:
                if _rpc.validate(self._game_key, _RPC_NAME, self._src.sr.pm):
                    return self._mgr_addr
            else:
                # No version key available (dev/diagnostic path). Keep the O(1)
                # in-session fast path by validating the in-memory klass sentinel
                # directly instead of discarding the addr and re-scanning.
                try:
                    if self._src.sr.pm.read_u64(self._mgr_addr) == self._mgr_klass:
                        return self._mgr_addr
                except Exception:
                    pass
            # Cache invalidation (Stage C): singleton respawn / scene reload / patch
            # → drop both the in-memory addr and the persisted entry, then bootstrap.
            if self._game_key:
                _rpc.invalidate(self._game_key, _RPC_NAME)
            self._mgr_addr = 0
            self._mgr_klass = 0
        # Negative-result backoff: while the mgr can't be located (loading screen /
        # entity-less scene) a 1s entity tick was re-sweeping the whole heap every
        # tick. Skip the scan until the cooldown elapses (cleared on success).
        now = time.time()
        if not force_rescan and now < self._miss_until:
            return None
        addr = self._scan_for_mgr(player_uuid)
        if addr:
            self._mgr_addr = addr
            self._mgr_klass = self._mgr_klass or 0
            self._last_uuid = player_uuid
            self._miss_backoff = 1.0
            # Stage A → write through RootPointerCache so the next session's
            # bootstrap is one pointer-chase (no full heap scan). klass_addr is
            # required for the Stage-B trust check above.
            if self._game_key and self._mgr_klass:
                try:
                    _rpc.set(self._game_key, _RPC_NAME,
                             addr=addr, klass_addr=self._mgr_klass,
                             klass_name="ZEntityMgr",
                             extras={"hint_region_base": self._hint_base})
                    _rpc.persist(self._game_key)
                except Exception:
                    pass
            return addr
        self._miss_until = now + self._miss_backoff
        self._miss_backoff = min(self._miss_backoff * 2.0, 8.0)
        return None

    # ---------- 字典读 ----------

    def _read_dict_entries(self, dict_addr: int, max_entries: int = 256) -> List[Tuple[int, int]]:
        """读 .NET Dictionary<long, ref> 的 (key, value_ptr) 列表.

        entries[] 整块一次 read_bytes 进来本地解码 (struct), 取代每槽 3 笔单读 RPM —
        每 tick 把 ~1.5k 次系统调用收成 1 次大读 + 本地循环。
        """
        if not dict_addr:
            return []
        pm = self._src.sr.pm
        try:
            count = pm.read_i32(dict_addr + DICT_COUNT_OFF) or 0
        except Exception:
            return []
        if count <= 0 or count > 4096:
            return []
        entries_arr = pm.read_u64(dict_addr + DICT_ENTRIES_OFF)
        if not entries_arr:
            return []
        max_len = pm.read_u32(entries_arr + ARRAY_LENGTH_OFF) or 0
        # 注: .NET Dict 中 entries[] 包含已用和空闲槽 (hashCode<0 = 空); 遍历整个数组直到收集 count 条
        scan_n = min(max_len, max_entries * 4)
        if scan_n <= 0:
            return []
        base = entries_arr + ARRAY_ELEMS_OFF
        blob = pm.read_bytes(base, scan_n * ENTRY_SIZE)
        out: List[Tuple[int, int]] = []
        if blob and len(blob) >= ENTRY_SIZE:
            import struct
            usable = (len(blob) // ENTRY_SIZE) * ENTRY_SIZE
            # Entry { i32 hashCode; i32 next; i64 key; u64 value } stride 24
            for hash_code, _next, key, val in struct.iter_unpack("<iiqQ", blob[:usable]):
                if len(out) >= count or len(out) >= max_entries:
                    break
                if hash_code < 0:           # 空槽
                    continue
                if val:
                    out.append((key, val))
            return out
        # fallback: per-slot reads (read_bytes failed — page straddle on a huge dict)
        for i in range(scan_n):
            if len(out) >= count or len(out) >= max_entries:
                break
            ep = base + i * ENTRY_SIZE
            try:
                hash_code = pm.read_i32(ep + ENTRY_HASH_OFF)
            except Exception:
                continue
            if hash_code is None or hash_code < 0:
                continue
            try:
                key = pm.read_i64(ep + ENTRY_KEY_OFF)
                val = pm.read_u64(ep + ENTRY_VAL_OFF)
            except Exception:
                continue
            if val:
                out.append((key, val))
        return out

    def _read_entity(self, ent_addr: int) -> Optional[EntitySnap]:
        if not ent_addr:
            return None
        pm = self._src.sr.pm
        try:
            uuid = pm.read_i64(ent_addr + self.off_ent_uuid) or 0
            cfg = pm.read_i64(ent_addr + self.off_ent_config) or 0
            cid = pm.read_i64(ent_addr + self.off_ent_charid) or 0
            st = pm.read_i32(ent_addr + self.off_ent_state) or 0
        except Exception:
            return None
        if uuid <= 0:
            return None
        return EntitySnap(uuid=uuid, char_id=cid, state=st, obj_addr=ent_addr,
                          config_uuid=cfg)

    # ---------- v2: ZAttr 数值 ----------

    def _entity_attrs_obj(self, ent_addr: int) -> int:
        """Return the entity's ``ZAttrCollection`` pointer (attrs_ @ +0x48)."""
        if not ent_addr:
            return 0
        try:
            return int(self._src.sr.pm.read_u64(ent_addr + self.off_ent_attrs) or 0)
        except Exception:
            return 0

    def calibrate_attrs(self, ent_addr: int, attr_reader, known: dict,
                        ga_base: int = 0) -> int:
        """Calibrate ``attr_reader`` against this entity using TCP-known values.

        ``known`` = ``{attr_id: expected_value}`` (e.g. ``{A_MAX_HP: boss_max_hp}``
        from the TCP parser). Returns the number of klasses calibrated. Done once
        per session; the reader then decodes every same-typed attr.
        """
        attrs = self._entity_attrs_obj(ent_addr)
        if not attrs or attr_reader is None:
            return 0
        try:
            return int(attr_reader.calibrate(attrs, known, ga_base=ga_base) or 0)
        except Exception:
            return 0

    def fill_entity_numeric(self, snap: EntitySnap, attr_reader) -> EntitySnap:
        """Fill ``snap``'s numeric fields from memory via a calibrated reader.

        No-op (leaves zeros, ``attrs_read=False``) when the reader is missing or
        not yet calibrated — callers then fall back to TCP for those fields.
        """
        if snap is None or attr_reader is None or not snap.obj_addr:
            return snap
        attrs = self._entity_attrs_obj(snap.obj_addr)
        if not attrs:
            return snap
        try:
            vals = attr_reader.read_attrs(attrs, _ENTITY_NUMERIC_ATTRS)
        except Exception:
            return snap
        if not vals:
            return snap
        snap.cur_hp = int(vals.get(A_HP, 0) or 0)
        snap.max_hp = int(vals.get(A_MAX_HP, 0) or 0)
        snap.breaking_stage = int(vals.get(A_BREAKING_STAGE, 0) or 0)
        snap.extinction = int(vals.get(A_EXTINCTION, 0) or 0)
        snap.max_extinction = int(vals.get(A_MAX_EXTINCTION, 0) or 0)
        snap.stunned = int(vals.get(A_STUNNED, 0) or 0)
        snap.max_stunned = int(vals.get(A_MAX_STUNNED, 0) or 0)
        snap.in_overdrive = int(vals.get(A_IN_OVERDRIVE, 0) or 0)
        snap.hated_char_id = int(vals.get(A_HATED_CHAR_ID, 0) or 0)
        snap.cast_skill_id = int(vals.get(A_SKILL_ID, 0) or 0)
        snap.attrs_read = True
        return snap

    def read_entity_numeric(self, ent_addr: int, attr_reader=None) -> Optional[EntitySnap]:
        """Read one entity with v1 identity + (if reader calibrated) numeric attrs."""
        snap = self._read_entity(ent_addr)
        if snap is None:
            return None
        if attr_reader is not None:
            self.fill_entity_numeric(snap, attr_reader)
        return snap

    def read(self, player_uuid: int, force_rescan: bool = False,
             include_monsters: bool = False, include_npcs: bool = False) -> Optional[EntityMgrSnap]:
        addr = self.locate(player_uuid, force_rescan=force_rescan)
        if not addr:
            return None
        pm = self._src.sr.pm
        snap = EntityMgrSnap(mgr_addr=addr, player_uuid=player_uuid, fetched_at=time.time())
        # bossDict_
        bd = pm.read_u64(addr + self.off_boss_dict)
        for key, val in self._read_dict_entries(bd, max_entries=32):
            es = self._read_entity(val)
            if es:
                snap.bosses.append(es)
        if include_monsters:
            md = pm.read_u64(addr + self.off_monster_dict)
            for key, val in self._read_dict_entries(md, max_entries=128):
                es = self._read_entity(val)
                if es:
                    snap.monsters.append(es)
        if include_npcs:
            nd = pm.read_u64(addr + self.off_npc_dict)
            for key, val in self._read_dict_entries(nd, max_entries=128):
                es = self._read_entity(val)
                if es:
                    snap.npcs.append(es)
        return snap


# ───────── selftest ─────────

def _selftest():
    src = StaticDpsSource()
    self_snap = src.get_self_snapshot(force_rescan=False)
    if self_snap is None:
        print("[entity-mgr] no self snapshot — aborting", file=sys.stderr)
        return
    uid = int(self_snap.uid)
    print(f"[entity-mgr] self uuid={uid}", file=sys.stderr)
    emr = EntityMgrReader(src)
    t0 = time.time()
    addr = emr.locate(uid)
    print(f"[entity-mgr] locate took {time.time()-t0:.2f}s, mgr_addr={addr:#x}" if addr
          else "[entity-mgr] locate FAILED", file=sys.stderr)
    if not addr:
        return
    snap = emr.read(uid, include_monsters=True, include_npcs=False)
    if snap is None:
        print("[entity-mgr] read FAILED")
        return
    print(f"[entity-mgr] bosses={len(snap.bosses)}, monsters={len(snap.monsters)}")
    for b in snap.bosses[:10]:
        print(f"  BOSS uuid={b.uuid} char_id={b.char_id} state={b.state} addr={b.obj_addr:#x}")
    for m in snap.monsters[:5]:
        print(f"  MOB  uuid={m.uuid} char_id={m.char_id} state={m.state}")


if __name__ == "__main__":
    _selftest()

