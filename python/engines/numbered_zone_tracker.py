# -*- coding: utf-8 -*-
"""numbered_zone_tracker - 编号圈(1/2/3 按顺序出的 AOE 圈)顺序判定 + 命中归属。

纯逻辑(不碰内存, 可单测): 喂入 ZoneReader.snapshot() 的时序快照, 按**出现顺序**给圈编号
(同 group_id 的圈算一批, 组内 1/2/3), 并用圈自带的成员列表(entitiesIdInZone_=服务端真实
命中判定)做"哪个编号圈命中了谁"的精确归属——不靠 DamagePos 最近邻猜。战斗中若有伤害事件
(skill_id + 命中世界坐标 DamagePos)再补充技能名/坐标。

用法:
    t = NumberedZoneTracker()
    t.observe(zone_snapshot_list, now=ts)       # 每次区域快照(节流调用)
    t.observe_damage(skill_id, target_uuid, pos, now=ts)  # 可选: TCP 伤害事件
    seq = t.active_sequence()    # 当前存活编号圈按序 → UI/"下一个该去的圈"
    stats = t.summary()          # 每个编号圈: 序号/组/命中名单/伤害/存活时长
"""
from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

Vec3 = Tuple[float, float, float]


@dataclass
class TrackedZone:
    zone_uuid: int
    base_id: int
    group_id: int
    seq: int                 # 全局出现序号(从1)
    seq_in_group: int        # 组内出现序号(从1)=编号圈的"1/2/3"
    first_seen: float
    last_seen: float
    alive: bool = True
    members_now: Set[int] = field(default_factory=set)
    members_ever: Set[int] = field(default_factory=set)
    pos: Optional[Vec3] = None                 # 由伤害事件 DamagePos 补充
    hits: List[Tuple[int, int, float]] = field(default_factory=list)  # (skill_id,target,ts)

    def lifetime(self) -> float:
        return max(0.0, self.last_seen - self.first_seen)


class NumberedZoneTracker:
    # 持久区域(开放世界区/安全区)不算编号圈: zone_type<0 或在忽略 base_id 集
    IGNORE_BASE_IDS = frozenset({1001, 1002})

    def __init__(self, *, ignore_persistent: bool = True):
        self._zones: Dict[int, TrackedZone] = {}     # zone_uuid → TrackedZone
        self._seq_counter = 0
        self._group_counter: Dict[int, int] = {}      # group_id → 已出现数
        self._ignore_persistent = ignore_persistent
        # 采样线程写 observe/prune, 主线程读 summary/active_sequence, 包线程 observe_damage
        # → 同一 _zones dict 跨线程访问必须加锁 (审查 #3: 防"dict changed size during iter")
        self._lock = threading.RLock()

    def _is_circle(self, z: Dict) -> bool:
        if z.get("base_id", 0) in self.IGNORE_BASE_IDS:
            return False
        if self._ignore_persistent and int(z.get("zone_type", 0)) < 0:
            return False
        return True

    def observe(self, snapshot: List[Dict], now: float) -> List[TrackedZone]:
        """吃一帧区域快照。返回本帧**新出现**的编号圈(已分配序号)。"""
        live_uuids = set()
        new_zones: List[TrackedZone] = []
        with self._lock:
            for z in snapshot or []:
                if not self._is_circle(z):
                    continue
                uuid = int(z.get("zone_uuid", 0))
                if not uuid:
                    continue
                live_uuids.add(uuid)
                members = set(z.get("members") or ())
                gid = int(z.get("group_id", 0))
                tz = self._zones.get(uuid)
                if tz is None:
                    self._seq_counter += 1
                    self._group_counter[gid] = self._group_counter.get(gid, 0) + 1
                    tz = TrackedZone(
                        zone_uuid=uuid, base_id=int(z.get("base_id", 0)), group_id=gid,
                        seq=self._seq_counter, seq_in_group=self._group_counter[gid],
                        first_seen=now, last_seen=now)
                    self._zones[uuid] = tz
                    new_zones.append(tz)
                tz.alive = True
                tz.last_seen = now
                tz.members_now = members
                tz.members_ever |= members
            # 不在本帧快照里的 → 标记消失(保留做统计)
            for uuid, tz in self._zones.items():
                if uuid not in live_uuids:
                    tz.alive = False
                    tz.members_now = set()
        return new_zones

    def observe_damage(self, skill_id: int, target_uuid: int,
                       pos: Optional[Vec3], now: float) -> Optional[TrackedZone]:
        """把一次伤害归到目标当前所在的存活编号圈(成员判定优先, 退而求其次按 pos)。"""
        with self._lock:
            best: Optional[TrackedZone] = None
            for tz in self._zones.values():
                if tz.alive and target_uuid and target_uuid in tz.members_now:
                    # 多个圈都含该目标时, 取最近出现的(编号最大)
                    if best is None or tz.seq > best.seq:
                        best = tz
            if best is None and pos is not None:
                best = self._nearest_alive_zone(pos)
            if best is not None:
                best.hits.append((int(skill_id or 0), int(target_uuid or 0), now))
                if pos is not None and best.pos is None:
                    best.pos = pos
            return best

    def _nearest_alive_zone(self, pos: Vec3) -> Optional[TrackedZone]:
        import math
        best, best_d = None, 1e18
        for tz in self._zones.values():
            if tz.alive and tz.pos is not None:
                d = math.hypot(pos[0] - tz.pos[0], pos[2] - tz.pos[2])
                if d < best_d:
                    best_d, best = d, tz
        return best

    def active_sequence(self) -> List[TrackedZone]:
        """当前存活编号圈按出现顺序 → "1/2/3" 该走/该躲的顺序。"""
        with self._lock:
            return sorted((z for z in self._zones.values() if z.alive),
                          key=lambda z: z.seq)

    def active_groups(self) -> Dict[int, List[TrackedZone]]:
        groups: Dict[int, List[TrackedZone]] = {}
        for z in self.active_sequence():        # active_sequence 自带锁
            groups.setdefault(z.group_id, []).append(z)
        for g in groups.values():
            g.sort(key=lambda z: z.seq_in_group)
        return groups

    def summary(self) -> List[Dict]:
        """每个追踪过的编号圈一条统计(含已消失的), 按出现序。"""
        out = []
        with self._lock:
            for z in sorted(self._zones.values(), key=lambda z: z.seq):
                out.append({
                    "seq": z.seq, "no": z.seq_in_group, "group_id": z.group_id,
                    "zone_uuid": z.zone_uuid, "base_id": z.base_id, "alive": z.alive,
                    "members_now": sorted(z.members_now),
                    "members_ever": sorted(z.members_ever),
                    "hit_count": len(z.hits), "pos": z.pos,
                    "lifetime_s": round(z.lifetime(), 2),
                })
        return out

    def reset(self) -> None:
        with self._lock:
            self._zones.clear()
            self._seq_counter = 0
            self._group_counter.clear()

    def prune(self, now: float, keep_s: float = 30.0) -> None:
        """清掉消失超过 keep_s 的圈, 防内存涨。战斗中低频调用。"""
        with self._lock:
            dead = [u for u, z in self._zones.items()
                    if not z.alive and (now - z.last_seen) > keep_s]
            for u in dead:
                self._zones.pop(u, None)


__all__ = ["NumberedZoneTracker", "TrackedZone"]
