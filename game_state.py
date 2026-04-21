# -*- coding: utf-8 -*-
"""
SAO Auto — 统一游戏状态模型

所有识别结果汇总到 GameState，UI 只订阅此对象。

"""

import time
import threading
from dataclasses import dataclass, field
from typing import Optional, List, Callable

from perf_probe import probe as _probe

# 缓存字段名列表
_CACHE_FIELDS = (
    'player_name', 'level_base', 'level_extra', 'player_id',
    'season_exp', 'fight_point',
    'hp_current', 'hp_max', 'hp_pct',
    'stamina_current', 'stamina_max', 'stamina_pct',
    'profession_id', 'profession_name',
)

# 身份类字段: 仅当新值有效 (非零/非空) 时才覆写缓存
_CACHE_IDENTITY_FIELDS = frozenset((
    'player_name', 'level_base', 'level_extra', 'player_id',
    'profession_id', 'profession_name', 'fight_point',
))


def compute_burst_ready(skill_slots, watched_slots) -> bool:
    """Return True when every watched slot is ready and at least one matched."""
    if not skill_slots or not watched_slots:
        return False
    try:
        watched = {int(x) for x in watched_slots if int(x) > 0}
    except Exception:
        watched = set()
    if not watched:
        return False

    matched = []
    for slot in skill_slots:
        if not isinstance(slot, dict):
            continue
        try:
            idx = int(slot.get('index', 0) or 0)
        except Exception:
            idx = 0
        if idx in watched:
            matched.append(slot)
    if not matched:
        return False

    def _slot_ready(slot):
        state = str(slot.get('state', '') or '').strip().lower()
        if state:
            if state in ('ready', 'active'):
                return True
        try:
            if bool(slot.get('active')):
                return True
        except Exception:
            pass
        try:
            charge_count = int(slot.get('charge_count', 0) or 0)
        except Exception:
            charge_count = 0
        if charge_count > 0:
            return True
        try:
            remaining_ms = int(slot.get('remaining_ms', 0) or 0)
            if remaining_ms <= 120:
                return True
        except Exception:
            pass
        value = slot.get('cooldown_pct', 1.0)
        try:
            return float(value) <= 0.02
        except Exception:
            return False

    return all(_slot_ready(slot) for slot in matched)


@dataclass
class GameState:
    """游戏状态快照 — 所有字段由识别层写入，UI 层只读。"""

    # ── 身份信息 ──
    player_name: str = ''
    level_base: int = 0                # 基础等级 (0=未识别)
    level_extra: int = 0               # 括号内加成等级 (+XX)
    season_exp: int = 0                # 当前赛季/extraLevel EXP
    player_id: str = ''                # 玩家编号
    fight_point: int = 0               # 战斗力

    # ── 生命值 ──
    hp_current: int = 0
    hp_max: int = 0
    hp_pct: float = 1.0                # 0.0 ~ 1.0

    # ── 体力值 ──
    stamina_current: int = 0
    stamina_max: int = 0
    stamina_pct: float = 1.0
    stamina_offline: bool = False          # True when STA bar not visible

    # ── 技能栏 ──
    skill_slots: List = field(default_factory=list)
    # 每个元素:
    # {
    #   'index': int,
    #   'rect': {'x': int, 'y': int, 'w': int, 'h': int},
    #   'state': 'ready' | 'cooldown' | 'insufficient_energy' | 'unknown',
    #   'cooldown_pct': float,
    #   'insufficient_energy': bool,
    #   'active': bool,
    #   'ready_edge': bool,
    # }

    # ── Burst Mode Ready (CD 提醒) ──
    burst_ready: bool = False          # 所有监视技能 CD 就绪时为 True

    profession_id: int = 0             # 职业 ID (来自抓包)
    profession_name: str = ''          # 职业名称

    # ── 战斗状态 ──
    in_combat: bool = False            # AttrCombatState (104) — 是否在战斗中

    # ── 窗口信息 ──
    window_rect: Optional[tuple] = None    # (left, top, right, bottom)
    window_width: int = 0
    window_height: int = 0

    # ── Boss Raid ──
    boss_raid_active: bool = False
    boss_raid_phase: int = 0
    boss_raid_phase_name: str = ''
    boss_enrage_remaining: float = 0.0     # seconds till enrage (0=inactive)
    boss_timer_text: str = ''              # formatted text for ID plate
    boss_total_damage: int = 0
    boss_dps: int = 0
    boss_hp_est_pct: float = 1.0           # estimated boss HP% (1.0=full)
    boss_current_hp: int = 0               # real HP from packets
    boss_total_hp: int = 0                 # real MaxHP from packets (or profile)
    boss_hp_source: str = 'none'           # 'packet' | 'estimate' | 'none'
    boss_shield_active: bool = False
    boss_shield_pct: float = 0.0
    boss_breaking_stage: int = -1          # -1=not received; 0=Breaking; 1=BreakEnd
    boss_extinction_pct: float = 0.0
    boss_in_overdrive: bool = False
    boss_invincible: bool = False

    # ── 采集元信息 ──
    capture_timestamp: float = 0.0
    recognition_ok: bool = False
    packet_active: bool = False
    error_msg: str = ''
    identity_alert_serial: int = 0
    identity_alert_title: str = ''
    identity_alert_message: str = ''

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
        return f'{int(round(max(0.0, min(1.0, self.stamina_pct)) * 100.0))}%'

    def to_dict(self) -> dict:
        return {
            'player_name': self.player_name,
            'level_base': self.level_base,
            'level_extra': self.level_extra,
            'season_exp': self.season_exp,
            'level_text': self.level_text,
            'player_id': self.player_id,
            'hp_current': self.hp_current,
            'hp_max': self.hp_max,
            'hp_pct': round(self.hp_pct, 4),
            'stamina_current': self.stamina_current,
            'stamina_max': self.stamina_max,
            'stamina_pct': round(self.stamina_pct, 4),
            'skill_slots': list(self.skill_slots),
            'burst_ready': bool(self.burst_ready),
            'profession_id': self.profession_id,
            'profession_name': self.profession_name,
            'hp_text': self.hp_text,
            'stamina_text': self.stamina_text,
            'recognition_ok': self.recognition_ok,
            'packet_active': self.packet_active,
            'capture_ts': self.capture_timestamp,
            'boss_raid_active': self.boss_raid_active,
            'boss_raid_phase': self.boss_raid_phase,
            'boss_raid_phase_name': self.boss_raid_phase_name,
            'boss_enrage_remaining': round(self.boss_enrage_remaining, 1),
            'boss_timer_text': self.boss_timer_text,
            'boss_total_damage': self.boss_total_damage,
            'boss_dps': self.boss_dps,
            'boss_hp_est_pct': round(self.boss_hp_est_pct, 4),
            'boss_current_hp': self.boss_current_hp,
            'boss_total_hp': self.boss_total_hp,
            'boss_hp_source': self.boss_hp_source,
            'boss_shield_active': self.boss_shield_active,
            'boss_shield_pct': round(self.boss_shield_pct, 4),
            'boss_breaking_stage': self.boss_breaking_stage,
            'boss_extinction_pct': round(self.boss_extinction_pct, 4),
            'boss_in_overdrive': self.boss_in_overdrive,
            'boss_invincible': self.boss_invincible,
            'identity_alert_serial': self.identity_alert_serial,
            'identity_alert_title': self.identity_alert_title,
            'identity_alert_message': self.identity_alert_message,
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

    @_probe.decorate('state.update')
    def update(self, **kwargs):
        """部分更新状态字段并通知所有监听器 (含范围校验)"""
        with self._lock:
            # ── 预过滤: 拦截 HP/LV/STA 的 0 值, 在 setattr 之前保护 ──
            allow_zero_hp = False
            if 'hp_pct' in kwargs:
                try:
                    allow_zero_hp = float(kwargs['hp_pct']) <= 0.001
                except Exception:
                    allow_zero_hp = False
            if 'hp_current' in kwargs and kwargs['hp_current'] == 0:
                if self._state.hp_max > 0 and not allow_zero_hp:
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
                next_sta_max = kwargs.get('stamina_max', self._state.stamina_max)
                if 'stamina_pct' not in kwargs and int(next_sta_max or 0) <= 0:
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
                elif k == 'season_exp':
                    if not isinstance(v, int) or v < 0:
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
                elif k == 'identity_alert_serial':
                    if not isinstance(v, int) or v < 0:
                        continue
                elif k == 'identity_alert_title':
                    if not isinstance(v, str) or len(v) > 80:
                        continue
                elif k == 'identity_alert_message':
                    if not isinstance(v, str) or len(v) > 600:
                        continue
                elif k == 'boss_raid_phase_name':
                    if not isinstance(v, str) or len(v) > 60:
                        continue
                elif k == 'boss_enrage_remaining':
                    if not isinstance(v, (int, float)) or v < 0:
                        continue
                    v = float(v)
                elif k == 'boss_timer_text':
                    if not isinstance(v, str) or len(v) > 40:
                        continue
                elif k in ('boss_total_damage', 'boss_dps'):
                    if not isinstance(v, int) or v < 0:
                        continue
                elif k == 'boss_hp_est_pct':
                    if not isinstance(v, (int, float)):
                        continue
                    v = max(0.0, min(1.0, float(v)))
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
            print(f'[GameState] 从缓存加载: HP={self._state.hp_current}/{self._state.hp_max}, '
                  f'LV={self._state.level_base}'
                  f'{"(+" + str(self._state.level_extra) + ")" if self._state.level_extra > 0 else ""}')
        # 通知订阅者立即渲染缓存数据 (避免等待首个数据包)
        try:
            snapshot = GameState(**{
                f.name: getattr(self._state, f.name)
                for f in self._state.__dataclass_fields__.values()
            })
            for cb in self._listeners:
                try:
                    cb(snapshot)
                except Exception:
                    pass
        except Exception:
            pass

    def save_cache(self, settings):
        """将当前状态持久化到 settings (定期调用)

        身份类字段 (name/level/profession) 仅在收到有效值时才覆写缓存,
        避免工具中途启动时用默认 0/空字符串覆盖上次缓存的值。
        """
        with self._lock:
            # 以现有缓存为基底, 避免丢失上次保存的身份数据
            cache = dict(settings.get('game_cache', {}) or {})
            for k in _CACHE_FIELDS:
                v = getattr(self._state, k, None)
                if v is None:
                    continue
                # 身份类字段: 仅当值非零/非空时覆写
                if k in _CACHE_IDENTITY_FIELDS:
                    if isinstance(v, str) and not v.strip():
                        continue
                    if isinstance(v, (int, float)) and v <= 0:
                        continue
                cache[k] = v
        settings.set('game_cache', cache)
        settings.save()
