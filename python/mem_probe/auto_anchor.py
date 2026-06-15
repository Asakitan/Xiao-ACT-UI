"""自动重定位 self HP/MaxHP/UID anchor.

作为 reader.py 的"安全网": 当 anchors.json 中所有 fingerprint/chain 都失效时
(典型场景: 跨重启 player 对象重新分配, fingerprint 不再唯一/匹配),
本模块复用 auto_locate 的内部函数 (locate_uid / locate_hp / locate_max_hp)
直接基于 PacketBridge TCP 真值在当前进程上重新扫描, 写回 anchors.json.

主要入口:
    relocate_now(pm, src) -> dict | None
        在已 attach 的 StarProcess + 已 ready 的 _TcpSource 上跑一次完整定位,
        返回 {hp_addr, max_hp_addr, uid_addr} 或 None (失败).
        副作用: 更新 anchors.json (合并写, 不覆盖 fingerprint 字段).
"""

from __future__ import annotations

import json
import os
import time
from typing import Any, Dict, List, Optional

from .process import StarProcess
from .auto_locate import _TcpSource, locate_uid, locate_hp, locate_max_hp

ANCHORS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "anchors.json")


def _load_anchors() -> Dict[str, Any]:
    if not os.path.isfile(ANCHORS_PATH):
        return {}
    try:
        with open(ANCHORS_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}


def _save_anchors(d: Dict[str, Any]) -> None:
    tmp = ANCHORS_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(d, f, indent=2, ensure_ascii=False)
    os.replace(tmp, ANCHORS_PATH)


def relocate_now(
    pm: StarProcess,
    src: _TcpSource,
    *,
    hp_timeout: float = 60.0,
    require_tcp_ready: bool = True,
) -> Optional[Dict[str, int]]:
    """重新定位 self HP/MaxHP/UID. 必须在 PacketBridge 已 ready 时调用.

    返回 {"hp_addr": int, "max_hp_addr": int, "uid_addr": int|None} 或 None.
    """
    if require_tcp_ready:
        snap = src.snapshot()
        if not snap.get("packet_active") or snap.get("uid", 0) <= 0 or snap.get("hp", 0) <= 0:
            print(f"[relocate] TCP 未就绪, 当前快照: {snap}")
            return None

    snap = src.snapshot()
    uid = int(snap["uid"])
    cur_hp = int(snap["hp"])
    cur_max = int(snap["max_hp"])
    print(f"[relocate] 起点 TCP: uid={uid} hp={cur_hp}/{cur_max}")

    # 阶段 A: UID
    t0 = time.time()
    uid_hits: List[int] = locate_uid(pm, uid)
    print(f"[relocate] UID candidates = {len(uid_hits)} ({time.time()-t0:.1f}s)")

    # 阶段 B: HP (依赖 TCP 动态变化收敛)
    t0 = time.time()
    hp_hits: List[int] = locate_hp(pm, src, timeout=hp_timeout)
    print(f"[relocate] HP candidates = {len(hp_hits)} ({time.time()-t0:.1f}s)")
    if not hp_hits:
        print("[relocate] HP 0 匹配, 终止")
        return None

    # 阶段 C: MaxHP (与 HP 邻近过滤)
    t0 = time.time()
    cur = src.snapshot()
    mh_hits: List[int] = []
    if cur["max_hp"] > 0:
        mh_hits = locate_max_hp(pm, cur["max_hp"], near_hits=hp_hits)
    print(f"[relocate] MaxHP candidates = {len(mh_hits)} ({time.time()-t0:.1f}s)")

    # 选择最终地址: 优先 1 个候选; 否则取第一个
    hp_addr = hp_hits[0]
    max_hp_addr = mh_hits[0] if mh_hits else (hp_addr + 0x4)
    uid_addr = uid_hits[0] if uid_hits else None

    # 写回 anchors.json (合并)
    anchors = _load_anchors()
    anchors.update({
        "saved_at": time.time(),
        "pid": pm.pid,
        "ground_truth": cur,
        "uid_candidates": [hex(a) for a in uid_hits],
        "hp_candidates": [hex(a) for a in hp_hits],
        "max_hp_candidates": [hex(a) for a in mh_hits],
        "self_hp_addr": hex(hp_addr),
        "self_max_hp_addr": hex(max_hp_addr),
        "self_uid_addr": hex(uid_addr) if uid_addr else None,
        "relocated_at": time.time(),
    })
    _save_anchors(anchors)
    print(f"[relocate] saved -> {ANCHORS_PATH}")
    print(f"[relocate] hp=0x{hp_addr:X} max=0x{max_hp_addr:X} "
          f"uid={'0x%X' % uid_addr if uid_addr else 'none'}")

    return {
        "hp_addr": hp_addr,
        "max_hp_addr": max_hp_addr,
        "uid_addr": uid_addr,
        "hp_candidates": hp_hits,
        "max_hp_candidates": mh_hits,
    }


def is_addr_sane(pm: StarProcess, hp_addr: int, max_hp_addr: int) -> bool:
    """快速校验当前 anchor 是否还合理 (HP 在 [0, max_hp] 范围, max_hp > 0)."""
    hp = pm.read_i32(hp_addr)
    mh = pm.read_i32(max_hp_addr)
    if hp is None or mh is None:
        return False
    if mh <= 0 or mh > 100_000_000:
        return False
    if hp < 0 or hp > mh:
        return False
    return True
