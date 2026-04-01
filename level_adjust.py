# -*- coding: utf-8 -*-
"""
level_adjust.py — 深眠心相仪等级/经验覆盖模块

覆盖在 PacketParser 解析 CharSerialize field 102 后立即生效。

用法示例
--------
from level_adjust import LevelAdjust, get_global_adjuster

# 设置覆盖（uid=0 表示覆盖所有玩家）
adj = get_global_adjuster()
adj.set_level(uid=36668136, level=30)          # 只改等级
adj.set_exp(uid=36668136, exp=500000)          # 只改经验
adj.set(uid=36668136, level=30, exp=500000)   # 同时改
adj.clear(uid=36668136)                        # 清除覆盖
adj.clear_all()                                # 清除所有覆盖

# 查询
info = adj.get(uid=36668136)
# => {'level': 30, 'exp': 500000} 或 {}

# 启用/禁用
adj.enabled = False   # 暂时关闭所有覆盖
adj.enabled = True
"""

import threading
import logging
from typing import Dict, Optional, Tuple

logger = logging.getLogger('sao_auto.level_adjust')

_WILDCARD_UID = 0  # uid=0 → 覆盖所有玩家


class LevelAdjust:
    """线程安全的深眠心相仪等级/经验覆盖管理器。"""

    def __init__(self):
        self._lock = threading.Lock()
        # { uid: {'level': int|None, 'exp': int|None} }
        self._overrides: Dict[int, Dict[str, Optional[int]]] = {}
        self.enabled: bool = True

    # ── 写操作 ──────────────────────────────────────────

    def set(self, uid: int = 0, *,
            level: Optional[int] = None,
            exp: Optional[int] = None) -> None:
        """设置指定 uid 的等级/经验覆盖。uid=0 代表所有玩家。"""
        if level is None and exp is None:
            return
        uid = int(uid or 0)
        entry: Dict[str, Optional[int]] = {}
        if level is not None:
            entry['level'] = max(1, int(level))
        if exp is not None:
            entry['exp'] = max(0, int(exp))
        with self._lock:
            existing = self._overrides.get(uid, {})
            existing.update(entry)
            self._overrides[uid] = existing
        logger.info(f'[LevelAdjust] set uid={uid} override={entry}')

    def set_level(self, uid: int = 0, level: int = 1) -> None:
        self.set(uid=uid, level=level)

    def set_exp(self, uid: int = 0, exp: int = 0) -> None:
        self.set(uid=uid, exp=exp)

    def clear(self, uid: int = 0) -> None:
        """清除指定 uid 的覆盖。"""
        uid = int(uid or 0)
        with self._lock:
            self._overrides.pop(uid, None)
        logger.info(f'[LevelAdjust] cleared override for uid={uid}')

    def clear_all(self) -> None:
        """清除所有覆盖。"""
        with self._lock:
            self._overrides.clear()
        logger.info('[LevelAdjust] all overrides cleared')

    # ── 读操作 ──────────────────────────────────────────

    def get(self, uid: int = 0) -> Dict[str, Optional[int]]:
        """
        返回 uid 对应的覆盖字典 {'level': int|None, 'exp': int|None}。
        先查 uid，若无则查 wildcard(0)，若都没有返回空 {}。
        """
        if not self.enabled:
            return {}
        uid = int(uid or 0)
        with self._lock:
            specific = self._overrides.get(uid)
            wildcard = self._overrides.get(_WILDCARD_UID)

        result: Dict[str, Optional[int]] = {}
        # Merge: uid-specific takes priority over wildcard
        if wildcard:
            result.update(wildcard)
        if specific and uid != _WILDCARD_UID:
            result.update(specific)
        return result

    def apply(self, uid: int, level: int, exp: int) -> Tuple[int, int]:
        """
        将覆盖应用到 (level, exp) 并返回 (new_level, new_exp)。
        这是 PacketParser 在解析 field 102 后调用的核心函数。

        Parameters
        ----------
        uid   : 玩家 UID
        level : 从包中解析到的原始等级
        exp   : 从包中解析到的原始经验

        Returns
        -------
        (adjusted_level, adjusted_exp)
        """
        override = self.get(uid)
        if not override:
            return level, exp

        new_level = int(override.get('level') or level)
        new_exp = int(override.get('exp') or exp)

        if new_level != level or new_exp != exp:
            logger.info(
                f'[LevelAdjust] uid={uid}: level {level}->{new_level}, '
                f'exp {exp}->{new_exp}'
            )
        return new_level, new_exp

    def list_all(self) -> Dict[int, Dict[str, Optional[int]]]:
        """返回所有当前覆盖设置的副本，用于 UI 展示。"""
        with self._lock:
            return {k: dict(v) for k, v in self._overrides.items()}

    def __bool__(self) -> bool:
        with self._lock:
            return self.enabled and bool(self._overrides)

    def __repr__(self) -> str:
        with self._lock:
            return (
                f'LevelAdjust(enabled={self.enabled}, '
                f'overrides={dict(self._overrides)})'
            )


# ── 全局单例 ────────────────────────────────────────────

_global_adjuster: Optional[LevelAdjust] = None
_global_lock = threading.Lock()


def get_global_adjuster() -> LevelAdjust:
    """获取全局单例 LevelAdjust 实例（懒初始化）。"""
    global _global_adjuster
    if _global_adjuster is None:
        with _global_lock:
            if _global_adjuster is None:
                _global_adjuster = LevelAdjust()
                logger.info('[LevelAdjust] global adjuster initialized')
    return _global_adjuster


# ── 便捷顶层函数 ────────────────────────────────────────

def set_level_override(uid: int = 0, level: int = 1, exp: Optional[int] = None) -> None:
    """快速设置等级覆盖（可选同时设经验）。"""
    get_global_adjuster().set(uid=uid, level=level, exp=exp)


def set_exp_override(uid: int = 0, exp: int = 0) -> None:
    """快速设置经验覆盖。"""
    get_global_adjuster().set_exp(uid=uid, exp=exp)


def clear_override(uid: int = 0) -> None:
    """快速清除指定 uid 的覆盖。"""
    get_global_adjuster().clear(uid=uid)


def clear_all_overrides() -> None:
    """快速清除所有覆盖。"""
    get_global_adjuster().clear_all()


def apply_override(uid: int, level: int, exp: int) -> Tuple[int, int]:
    """应用覆盖（供 PacketParser 调用）。"""
    return get_global_adjuster().apply(uid, level, exp)
