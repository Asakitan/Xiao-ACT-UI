"""Phase 4 — Panda.ZGame.ZEntityMgr reader.

通过堆扫描定位 ZEntityMgr 单例实例 (用已知 player_uuid 作锚点),
然后读 bossDict_/monsterDict_/npcDict_ (.NET Dictionary<long, ZEntity>) 列出实体.

ZEntityMgr 字段 (来自 dump @ ef9ef95a):
  +0x10  long                       playerUuid_
  +0x18  PlayerEnt                  playerEnt_
  +0x28  Dictionary<long, ZEntity>  entityDict_
  +0x68  Dictionary<long, ZEntity>  bossDict_         ← BOSS HP 主源
  +0x70  Dictionary<long, ZEntity>  monsterDict_
  +0x78  Dictionary<long, ZEntity>  npcDict_

ZEntity 字段:
  +0x30  ZAttrCollection                              attrs_
  +0xC0  long                                         Uuid
  +0xD8  long                                         CharId    (ConfigUuid @ +0xC8)

ZAttrCollection (HP 数据走 attrs):
  +0x18  ZAttrCacheSlim   cacheSlim_   (含 _values: ValueTuple<uint, object[]>[])
  +0x28  Dictionary<uint, IMixAttr>  mixItemDict_

⚠ 当前未做 ZAttrCacheSlim 解码 (索引部分 IntPtr → 需要本地结构推断),
故本模块 v1 只输出: uuid + entType + 是否激活 (entityState_).
HP 字段后续追加.
"""
from __future__ import annotations

import os
import sys
import time
from dataclasses import dataclass, field
from typing import Iterable, List, Optional, Tuple

from mem_probe.il2cpp.static_dps_source import StaticDpsSource
from mem_probe.il2cpp.script_parser import ScriptIndex


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

# ZEntity 字段
ENT_UUID_OFF = 0xC0
ENT_CHARID_OFF = 0xD8
ENT_ATTRS_OFF = 0x48   # attrs_ 字段在 dump 里看到 +0x48 ZAttrCollection
ENT_STATE_OFF = 0x28


@dataclass
class EntitySnap:
    uuid: int = 0
    char_id: int = 0
    state: int = 0
    obj_addr: int = 0
    # HP 占位 — 待 ZAttrCacheSlim 解码后填
    cur_hp: int = 0
    max_hp: int = 0


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

    def _resolve_mgr_klass(self) -> int:
        """优先 bundle; 不命中则 lazy-load script.json (一次性加载 ~250MB)."""
        sr = self._src.sr
        kp = sr.resolve_klass(ENTITY_MGR_CLASS) or 0
        if kp:
            return kp
        # bundle 不含 ZEntityMgr (只有 130 个 curated klass), 回落到完整 script.json
        if self._full_si is None:
            try:
                # 推断 dump_id下的 script.json 路径
                here = os.path.dirname(os.path.abspath(__file__))
                sj_path = os.path.join(here, "out", self._src.dump_id, "dumper_out", "script.json")
                if not os.path.isfile(sj_path):
                    return 0
                self._full_si = ScriptIndex.load(sj_path)
            except Exception as e:
                print(f"[entity-mgr] script.json load failed: {e}", file=sys.stderr)
                return 0
        rva = self._full_si.find_klass(ENTITY_MGR_CLASS)
        if not rva:
            return 0
        try:
            return sr.pm.read_u64(sr.ga + rva) or 0
        except Exception:
            return 0

    # ---------- 锚点扫描 ----------

    def _scan_for_mgr(self, player_uuid: int) -> Optional[int]:
        """单遍扫描堆: 找 *(addr)==klass_ptr 且 bd/md/nd 都是有效堆指针 + entityDict_.count 合理.

        早退条件: playerUuid_ 字段匹配 (绝对真身); 否则收集 best-by-count.
        """
        sr = self._src.sr
        pm = sr.pm
        klass_ptr = self._resolve_mgr_klass()
        self._mgr_klass = klass_ptr
        if not klass_ptr:
            return None

        kp_bytes = klass_ptr.to_bytes(8, "little")
        MIN_HEAP = 0x0000_0001_0000_0000
        MAX_HEAP = 0x0000_7FFF_FFFF_FFFF

        def _looks_heap(p: int) -> bool:
            return MIN_HEAP <= p <= MAX_HEAP and (p & 0x7) == 0

        best_obj = 0
        best_score = -1

        for r in pm.iter_regions(only_readable=True, only_private=True):
            chunk = 16 * 1024 * 1024
            off = 0
            while off < r.size:
                n = min(chunk, r.size - off)
                blob = pm.read_bytes(r.base + off, n)
                if blob is None:
                    break
                view = memoryview(blob)
                # 找所有 klass_ptr 出现位置
                start = 0
                while True:
                    idx = blob.find(kp_bytes, start)
                    if idx < 0 or idx + NPC_DICT_OFF + 8 > n:
                        break
                    if (idx & 0x7) == 0:
                        # 内联读 6 字段
                        bd = int.from_bytes(view[idx + BOSS_DICT_OFF:idx + BOSS_DICT_OFF + 8], "little")
                        md = int.from_bytes(view[idx + MONSTER_DICT_OFF:idx + MONSTER_DICT_OFF + 8], "little")
                        nd = int.from_bytes(view[idx + NPC_DICT_OFF:idx + NPC_DICT_OFF + 8], "little")
                        ed = int.from_bytes(view[idx + ENTITY_DICT_OFF:idx + ENTITY_DICT_OFF + 8], "little")
                        if (_looks_heap(bd) and _looks_heap(md) and _looks_heap(nd)
                                and _looks_heap(ed) and len({bd, md, nd, ed}) == 4):
                            obj = r.base + off + idx
                            puid = int.from_bytes(view[idx + PLAYERUUID_OFF:idx + PLAYERUUID_OFF + 8], "little", signed=True)
                            # entityDict_.count 校验 (跨内存读, 因为 ed 在另一块)
                            try:
                                ed_cnt = pm.read_i32(ed + DICT_COUNT_OFF)
                            except Exception:
                                ed_cnt = None
                            if ed_cnt is not None and 0 <= ed_cnt <= 8192:
                                if puid == player_uuid:
                                    return obj  # 绝对真身, 早退
                                # 否则记录最高 count
                                if ed_cnt > best_score:
                                    best_score = ed_cnt
                                    best_obj = obj
                    start = idx + 1
                off += n
        return best_obj or None

    def locate(self, player_uuid: int, force_rescan: bool = False) -> Optional[int]:
        """返回 ZEntityMgr 实例地址. 缓存已找到的 mgr_addr, 用 player_uuid 校验依然有效."""
        if not force_rescan and self._mgr_addr and self._last_uuid == player_uuid:
            # 校验缓存仍指向有效对象
            try:
                cur_uuid = self._src.sr.pm.read_u64(self._mgr_addr + PLAYERUUID_OFF)
                if cur_uuid == player_uuid:
                    return self._mgr_addr
            except Exception:
                pass
            self._mgr_addr = 0
        addr = self._scan_for_mgr(player_uuid)
        if addr:
            self._mgr_addr = addr
            self._last_uuid = player_uuid
        return addr or None

    # ---------- 字典读 ----------

    def _read_dict_entries(self, dict_addr: int, max_entries: int = 256) -> List[Tuple[int, int]]:
        """读 .NET Dictionary<long, ref> 的 (key, value_ptr) 列表."""
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
        out: List[Tuple[int, int]] = []
        for i in range(scan_n):
            if len(out) >= count or len(out) >= max_entries:
                break
            ep = base + i * ENTRY_SIZE
            try:
                hash_code = pm.read_i32(ep + ENTRY_HASH_OFF)
            except Exception:
                continue
            # hashCode < 0 表示空槽 (.NET 标准)
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
            uuid = pm.read_i64(ent_addr + ENT_UUID_OFF) or 0
            cid = pm.read_i64(ent_addr + ENT_CHARID_OFF) or 0
            st = pm.read_i32(ent_addr + ENT_STATE_OFF) or 0
        except Exception:
            return None
        if uuid <= 0:
            return None
        return EntitySnap(uuid=uuid, char_id=cid, state=st, obj_addr=ent_addr)

    def read(self, player_uuid: int, force_rescan: bool = False,
             include_monsters: bool = False, include_npcs: bool = False) -> Optional[EntityMgrSnap]:
        addr = self.locate(player_uuid, force_rescan=force_rescan)
        if not addr:
            return None
        pm = self._src.sr.pm
        snap = EntityMgrSnap(mgr_addr=addr, player_uuid=player_uuid, fetched_at=time.time())
        # bossDict_
        bd = pm.read_u64(addr + BOSS_DICT_OFF)
        for key, val in self._read_dict_entries(bd, max_entries=32):
            es = self._read_entity(val)
            if es:
                snap.bosses.append(es)
        if include_monsters:
            md = pm.read_u64(addr + MONSTER_DICT_OFF)
            for key, val in self._read_dict_entries(md, max_entries=128):
                es = self._read_entity(val)
                if es:
                    snap.monsters.append(es)
        if include_npcs:
            nd = pm.read_u64(addr + NPC_DICT_OFF)
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

