# -*- coding: utf-8 -*-
"""
SAO Auto — 统一游戏状态模型

所有识别结果汇总到 GameState，UI 只订阅此对象。
"""

import time
import threading
from dataclasses import dataclass, field
from typing import Optional, List, Callable

# 缓存字段名列表
_CACHE_FIELDS = (
    'player_name', 'level_base', 'level_extra', 'player_id',
    'hp_current', 'hp_max', 'hp_pct',
    'stamina_current', 'stamina_max', 'stamina_pct',
    'profession_id', 'profession_name',
)


@dataclass
class GameState:
    """游戏状态快照 — 所有字段由识别层写入，UI 层只读。"""

    # ── 身份信息 ──
    player_name: str = ''
    level_base: int = 0                # 基础等级 (0=未识别)
    level_extra: int = 0               # 括号内加成等级 (+XX)
    player_id: str = ''                # 玩家编号

    # ── 生命值 ──
    hp_current: int = 0
    hp_max: int = 0
    hp_pct: float = 1.0                # 0.0 ~ 1.0

    # ── 体力值 ──
    stamina_current: int = 0
    stamina_max: int = 0
    stamina_pct: float = 1.0

    # ── 技能栏 ──
    skill_slots: List = field(default_factory=list)
    # 每个元素: {'index': int, 'cooldown_pct': float (0.0=就绪, 1.0=完全冷却),
    #            'active': bool (是否正在释放), 'name': str}
    profession_id: int = 0             # 职业 ID (来自抓包)
    profession_name: str = ''          # 职业名称

    # ── 窗口信息 ──
    window_rect: Optional[tuple] = None    # (left, top, right, bottom)
    window_width: int = 0
    window_height: int = 0

    # ── 采集元信息 ──
    capture_timestamp: float = 0.0
    recognition_ok: bool = False
    error_msg: str = ''

    @property
    def level_text(self) -> str:
        """格式化等级文本: 60(+12)"""
        if self.level_extra > 0:
            return f'{self.level_base}(+{self.level_extra})'
        return str(self.level_base)

    @property
    def hp_text(self) -> str:
        return f'{self.hp_current}/{self.hp_max}'

    @property
    def stamina_text(self) -> str:
        return f'{self.stamina_current}/{self.stamina_max}'

    def to_dict(self) -> dict:
        return {
            'player_name': self.player_name,
            'level_base': self.level_base,
            'level_extra': self.level_extra,
            'level_text': self.level_text,
            'player_id': self.player_id,
            'hp_current': self.hp_current,
            'hp_max': self.hp_max,
            'hp_pct': round(self.hp_pct, 4),
            'stamina_current': self.stamina_current,
            'stamina_max': self.stamina_max,
            'stamina_pct': round(self.stamina_pct, 4),
            'hp_text': self.hp_text,
            'stamina_text': self.stamina_text,
            'recognition_ok': self.recognition_ok,
            'capture_ts': self.capture_timestamp,
        }


class GameStateManager:
    """线程安全的状态管理器，支持订阅通知。"""

    def __init__(self):
        self._state = GameState()
        self._lock = threading.Lock()
        self._listeners: List[Callable[[GameState], None]] = []

    @property
    def state(self) -> GameState:
        with self._lock:
            return self._state

    def update(self, **kwargs):
        """部分更新状态字段并通知所有监听器 (含范围校验)"""
        with self._lock:
            # ── 预过滤: 拦截 HP/LV/STA 的 0 值, 在 setattr 之前保护 ──
            if 'hp_current' in kwargs and kwargs['hp_current'] == 0:
                if self._state.hp_max > 0:
                    prev = getattr(self, '_prev_hp_current', self._state.hp_current)
                    if prev > 0:
                        kwargs['hp_current'] = prev
                    else:
                        kwargs['hp_current'] = self._state.hp_max
            if 'level_base' in kwargs and kwargs['level_base'] == 0:
                prev_lv = getattr(self, '_prev_level_base', self._state.level_base)
                if prev_lv > 0:
                    kwargs['level_base'] = prev_lv
            if 'level_extra' in kwargs and kwargs['level_extra'] == 0:
                prev_lv_extra = getattr(self, '_prev_level_extra', self._state.level_extra)
                if prev_lv_extra > 0:
                    kwargs['level_extra'] = prev_lv_extra
            if 'stamina_current' in kwargs and kwargs['stamina_current'] == 0:
                prev_sta = getattr(self, '_prev_stamina_current', self._state.stamina_current)
                if prev_sta > 0:
                    kwargs['stamina_current'] = prev_sta

            for k, v in kwargs.items():
                if not hasattr(self._state, k):
                    continue
                # ── 范围校验 ──
                if k in ('hp_pct', 'stamina_pct'):
                    if not isinstance(v, (int, float)):
                        continue
                    v = max(0.0, min(1.0, float(v)))
                elif k == 'level_base':
                    if not isinstance(v, int) or v < 0 or v > 999:
                        continue
                elif k == 'level_extra':
                    if not isinstance(v, int) or v < 0 or v > 999:
                        continue
                elif k in ('hp_current', 'hp_max', 'stamina_current', 'stamina_max'):
                    if not isinstance(v, int) or v < 0:
                        continue
                elif k == 'player_name':
                    if not isinstance(v, str) or len(v) > 20:
                        continue
                elif k == 'player_id':
                    if not isinstance(v, str) or len(v) > 30:
                        continue
                setattr(self._state, k, v)

            # ── 更新 prev 追踪值 (仅当值有效时) ──
            if self._state.hp_current > 0:
                self._prev_hp_current = self._state.hp_current
            if self._state.level_base > 0:
                self._prev_level_base = self._state.level_base
            if self._state.level_extra > 0:
                self._prev_level_extra = self._state.level_extra
            if self._state.stamina_current > 0:
                self._prev_stamina_current = self._state.stamina_current

            # hp_current > hp_max: 回滚 current
            if self._state.hp_max > 0 and self._state.hp_current > self._state.hp_max:
                self._state.hp_current = self._state.hp_max
            if self._state.stamina_max > 0 and self._state.stamina_current > self._state.stamina_max:
                self._state.stamina_current = self._state.stamina_max
            self._state.capture_timestamp = time.time()
            snapshot = GameState(**{
                f.name: getattr(self._state, f.name)
                for f in self._state.__dataclass_fields__.values()
            })
        # 通知在锁外执行，避免死锁
        for cb in self._listeners:
            try:
                cb(snapshot)
            except Exception as e:
                print(f'[GameState] listener error: {e}')

    def subscribe(self, callback: Callable[[GameState], None]):
        self._listeners.append(callback)

    def unsubscribe(self, callback: Callable[[GameState], None]):
        try:
            self._listeners.remove(callback)
        except ValueError:
            pass

    # ── 缓存持久化 ──

    def load_cache(self, settings):
        """从 settings 加载上次缓存的游戏状态 (启动时调用)"""
        cache = settings.get('game_cache', {})
        if not cache or not isinstance(cache, dict):
            return
        with self._lock:
            for k in _CACHE_FIELDS:
                v = cache.get(k)
                if v is not None and hasattr(self._state, k):
                    try:
                        expected_type = type(getattr(self._state, k))
                        setattr(self._state, k, expected_type(v))
                    except (ValueError, TypeError):
                        pass
            # 初始化 prev 追踪值, 防止缓存数据被后续 0 值覆盖
            if self._state.hp_current > 0:
                self._prev_hp_current = self._state.hp_current
            if self._state.level_base > 0:
                self._prev_level_base = self._state.level_base
            if self._state.level_extra > 0:
                self._prev_level_extra = self._state.level_extra
            if self._state.stamina_current > 0:
                self._prev_stamina_current = self._state.stamina_current
            print(f'[GameState] 从缓存加载: HP={self._state.hp_current}/{self._state.hp_max}, '
                  f'LV={self._state.level_base}')

    def save_cache(self, settings):
        """将当前状态持久化到 settings (定期调用)"""
        with self._lock:
            cache = {}
            for k in _CACHE_FIELDS:
                v = getattr(self._state, k, None)
                if v is not None:
                    cache[k] = v
        settings.set('game_cache', cache)
        settings.save()
