# -*- coding: utf-8 -*-
"""
SAO Auto — 网络抓包数据桥接

PacketBridge 与 RecognitionEngine 接口一致，可直接替换。
通过 Npcap 抓包获取游戏数据，更新到 GameStateManager。

用法:
    from game_state import GameStateManager
    from packet_bridge import PacketBridge

    mgr = GameStateManager()
    bridge = PacketBridge(mgr)
    bridge.start()   # 后台抓包 + 解析 + 推送
    bridge.stop()
"""

import threading
import time
import logging
import math
import json
import os
import sys

from game_state import GameStateManager, compute_burst_ready
from packet_parser import (PacketParser, PlayerData,
                           PROFESSION_NORMAL_ATTACK, PROFESSION_SKILL,
                           PROFESSION_ULTIMATE)
from packet_capture import PacketCapture, list_devices, auto_select_device

logger = logging.getLogger('sao_auto.bridge')

# ── Skill name table (from SRDC skill_names_new.json) ──
_SKILL_NAMES: dict = {}


def _load_skill_names() -> dict:
    """Load skill name mapping {int_id: str_name} from assets/skill_names.json."""
    global _SKILL_NAMES
    if _SKILL_NAMES:
        return _SKILL_NAMES
    try:
        if getattr(sys, 'frozen', False):
            base = sys._MEIPASS if hasattr(sys, '_MEIPASS') else os.path.dirname(sys.executable)
        else:
            base = os.path.dirname(os.path.abspath(__file__))
        path = os.path.join(base, 'assets', 'skill_names.json')
        if not os.path.exists(path):
            logger.warning(f'[Bridge] skill_names.json not found at {path}')
            return {}
        with open(path, 'r', encoding='utf-8') as f:
            raw = json.load(f)
        _SKILL_NAMES = {int(k): str(v) for k, v in raw.items() if str(k).isdigit()}
        logger.info(f'[Bridge] loaded {len(_SKILL_NAMES)} skill names')
        return _SKILL_NAMES
    except Exception as e:
        logger.warning(f'[Bridge] failed to load skill names: {e}')
        return {}


def _get_skill_name(skill_id: int) -> str:
    """Resolve a numeric skill_id to its display name."""
    if not _SKILL_NAMES:
        _load_skill_names()
    skill_id = int(skill_id or 0)
    if skill_id <= 0:
        return ''
    # Try exact match first
    name = _SKILL_NAMES.get(skill_id)
    if name:
        return name
    # Try base skill id (strip level suffix — skill_level_id = skill_id * 100 + level)
    if skill_id >= 100:
        base_id = skill_id // 100
        name = _SKILL_NAMES.get(base_id)
        if name:
            return name
    return ''
_energy_samples = []          # 最近 N 个 energy 值
_energy_domain = 'unknown'    # 'pct' | 'absolute' | 'unknown'
_stamina_max_cached = 0       # 从 OCR 或观测推断的最大体力


def _sanitize_packet_stamina_max(candidate: int, previous: int) -> int:
    """Reject implausible STA max spikes before they reach the HUD."""
    if candidate <= 0:
        return previous if previous > 0 else 0

    if candidate > 1300:
        if 0 < previous <= 1300:
            return previous
        return 0

    # Allow the first sane self STA cap (for example 1200) to replace
    # placeholder values like 100 coming from the early full-sync path.
    if 0 < previous <= 200 and 500 <= candidate <= 1300:
        return candidate

    if 0 < previous <= 1300:
        if candidate > previous + max(80, int(previous * 0.10)):
            return previous
        if candidate < previous - max(220, int(previous * 0.35)):
            return previous

    return candidate


def _resolve_packet_stamina(energy_value: float, stamina_max: int):
    """Convert OriginEnergy into (current, pct) when it looks sane."""
    if stamina_max <= 0:
        return None
    if not isinstance(energy_value, (int, float)):
        return None

    value = float(energy_value)
    if not math.isfinite(value) or value < 0:
        return None

    if value <= 1.05 and stamina_max > 1:
        current = int(round(stamina_max * value))
    else:
        current = int(round(value))

    if current < 0:
        return None
    if current > stamina_max:
        if current <= int(stamina_max * 1.2):
            current = stamina_max
        else:
            return None

    pct = (current / stamina_max) if stamina_max > 0 else 0.0
    return current, pct


def _resolve_resource_stamina(player: PlayerData, stamina_max: int):
    resource_id = int(getattr(player, 'stamina_resource_id', 0) or 0)
    resource_values = getattr(player, 'resource_values', {}) or {}
    if resource_id <= 0 or resource_id not in resource_values:
        return None

    current = int(resource_values.get(resource_id, 0) or 0)
    if current < 0:
        return None

    energy_info = (getattr(player, 'energy_info_map', {}) or {}).get(resource_id) or {}
    resource_max = int(energy_info.get('energy_value', 0) or 0)
    if resource_max > 0:
        stamina_max = resource_max if stamina_max <= 0 else min(max(stamina_max, current), resource_max)

    if stamina_max <= 0:
        return None
    if current > stamina_max:
        if current <= int(stamina_max * 1.15):
            current = stamina_max
        else:
            return None
    pct = (current / stamina_max) if stamina_max > 0 else 0.0
    return current, pct


def _resolve_ratio_stamina(ratio_value: float, stamina_max: int):
    """Convert a 0..1 packet ratio candidate into (current, pct)."""
    if stamina_max <= 0:
        return None
    if not isinstance(ratio_value, (int, float)):
        return None
    ratio = float(ratio_value)
    if not math.isfinite(ratio):
        return None
    ratio = max(0.0, min(1.0, ratio))
    current = int(round(stamina_max * ratio))
    return current, ratio


def _get_skill_id_for_level(player: PlayerData, skill_level_id: int) -> int:
    skill_level_id = int(skill_level_id or 0)
    if skill_level_id <= 0:
        return 0
    skill_info = (getattr(player, 'skill_level_info_map', {}) or {}).get(skill_level_id) or {}
    skill_id = int(skill_info.get('skill_id', 0) or 0)
    if skill_id > 0:
        return skill_id
    if skill_level_id >= 100:
        return int(skill_level_id // 100)
    return skill_level_id


def _resolve_skill_level_id_from_cd(base_skill_id: int, cd_map: dict,
                                    last_use_map: dict = None,
                                    seen_ids: list = None) -> int:
    """Find the skill_level_id that matches a base skill_id.

    skill_level_id = skill_id * 100 + level,  so skill_level_id // 100 == skill_id.
    Searches cd_map first, then last_use_map and seen_ids as fallbacks.
    Returns the matching skill_level_id, or 0 if not found.
    """
    if base_skill_id <= 0:
        return 0
    # Search in cd_map (active CDs)
    for slid in (cd_map or {}):
        if int(slid or 0) > 0 and int(slid) // 100 == base_skill_id:
            return int(slid)
    # Search in last_use_at (skills that were used but CD expired)
    for slid in (last_use_map or {}):
        if int(slid or 0) > 0 and int(slid) // 100 == base_skill_id:
            return int(slid)
    # Search in seen_ids history
    for slid in (seen_ids or []):
        if int(slid or 0) > 0 and int(slid) // 100 == base_skill_id:
            return int(slid)
    return 0


def _infer_slot_map_from_cds(player: PlayerData) -> dict:
    """When ProfessionList is missed, infer a synthetic slot map from observed skill CDs.

    Uses profession-specific anchor tables to pin normal attack to slot 1
    and ultimate to slot 7 when the profession is known.
    """
    cd_map = getattr(player, 'skill_cd_map', {}) or {}
    seen_ids = list(getattr(player, 'skill_seen_ids', []) or [])
    last_use = getattr(player, 'skill_last_use_at', {}) or {}

    # Collect all known skill_level_ids from CDs and seen history
    all_skill_ids = set()
    for slid in cd_map:
        if int(slid or 0) > 0:
            all_skill_ids.add(int(slid))
    for slid in seen_ids:
        if int(slid or 0) > 0:
            all_skill_ids.add(int(slid))
    for slid in last_use:
        if int(slid or 0) > 0:
            all_skill_ids.add(int(slid))

    if not all_skill_ids:
        return {}

    # Filter out very short/normal-attack pings (duration=0 and never had a real CD)
    meaningful = []
    for slid in sorted(all_skill_ids):
        cd_info = cd_map.get(slid)
        if cd_info and int(cd_info.get('duration', 0) or 0) > 0:
            meaningful.append(slid)
        elif slid in last_use:
            meaningful.append(slid)

    if not meaningful:
        # If we only have zero-duration CDs (normal attacks), nothing to show
        return {}

    # Deduplicate by base skill_id (different levels map to same skill)
    seen_base = {}
    deduped = []
    for slid in meaningful:
        base = slid // 100 if slid >= 100 else slid
        if base not in seen_base:
            seen_base[base] = slid
            deduped.append(slid)

    # ── Profession-based anchoring ──
    # 固定三个槽位: 普攻→1, 职业技能→2, 大招→7
    profession_id = int(getattr(player, 'profession_id', 0) or 0)
    normal_attack_base = PROFESSION_NORMAL_ATTACK.get(profession_id, 0)
    prof_skill_base = PROFESSION_SKILL.get(profession_id, 0)
    ultimate_base = PROFESSION_ULTIMATE.get(profession_id, 0)

    pinned_normal = None
    pinned_skill = None
    pinned_ultimate = None
    rest = []
    for slid in deduped:
        base = slid // 100 if slid >= 100 else slid
        if normal_attack_base > 0 and base == normal_attack_base and pinned_normal is None:
            pinned_normal = slid
        elif prof_skill_base > 0 and base == prof_skill_base and pinned_skill is None:
            pinned_skill = slid
        elif ultimate_base > 0 and base == ultimate_base and pinned_ultimate is None:
            pinned_ultimate = slid
        else:
            rest.append(slid)

    # Sort remaining by most recently used, then by id
    rest.sort(key=lambda slid: (-last_use.get(slid, 0.0), slid))

    # Build slot map: pin normal→1, profession skill→2, ultimate→7
    # Remaining fill into 3-6 (选配) and 8-9 (共鸣)
    slot_map = {}
    if pinned_normal:
        slot_map[1] = pinned_normal
    if pinned_skill:
        slot_map[2] = pinned_skill
    if pinned_ultimate:
        slot_map[7] = pinned_ultimate

    fill_positions = [i for i in [3, 4, 5, 6, 8, 9] if i not in slot_map]
    for slid in rest:
        if not fill_positions:
            break
        slot_map[fill_positions.pop(0)] = slid

    # If no profession anchoring at all, fall back to simple sequential assignment
    if not pinned_normal and not pinned_skill and not pinned_ultimate:
        slot_map = {}
        deduped.sort(key=lambda slid: (-last_use.get(slid, 0.0), slid))
        for idx, slid in enumerate(deduped[:9], start=1):
            slot_map[idx] = slid
    return slot_map


def _slot_is_ready(slot) -> bool:
    if not isinstance(slot, dict):
        return False
    state = str(slot.get('state', '') or '').strip().lower()
    if state in ('ready', 'active'):
        return True
    try:
        if bool(slot.get('active')):
            return True
    except Exception:
        pass
    try:
        if int(slot.get('charge_count', 0) or 0) > 0:
            return True
    except Exception:
        pass
    try:
        if int(slot.get('remaining_ms', 0) or 0) <= 120:
            return True
    except Exception:
        pass
    try:
        return float(slot.get('cooldown_pct', 1.0) or 1.0) <= 0.02
    except Exception:
        return False


# ── 槽位重映射 ──
# ProfessionList 内部槽位编号 {1,2,3,4,5,6,9} + slot_bar {7,8}
# 游戏技能栏实际显示顺序:
#   位置 1: 普攻 (内部槽 1, 固定)
#   位置 2: 职业技能 (内部槽 2, 固定)
#   位置 3-5: 选配技能 (内部槽 3-5, 可变)
#   位置 6: 选配技能 (内部槽 9, 可变)
#   位置 7: 大招/终技 (内部槽 6, 固定)
#   位置 8-9: 共鸣技能 (内部槽 7-8, 可变)
_SLOT_DISPLAY_ORDER: dict = {
    1: 1,   # 普攻 (固定)
    2: 2,   # 职业技能 (固定)
    3: 3,   # 选配技能
    4: 4,   # 选配技能
    5: 5,   # 选配技能
    9: 6,   # 选配技能 → 显示位 6
    6: 7,   # 大招/终技 → 显示位 7 (固定)
    7: 8,   # 共鸣技能 1 → 显示位 8
    8: 9,   # 共鸣技能 2 → 显示位 9
}


def _remap_slot_index(internal_slot: int) -> int:
    """Map internal ProfessionList slot number to HUD display position."""
    return _SLOT_DISPLAY_ORDER.get(internal_slot, internal_slot)


def _build_packet_skill_slots(player: PlayerData):
    """Convert packet skill mappings + cooldown state into the HUD slot format.

    Proto semantics (SkillCDInfo / SkillCD):
      - duration   : total cooldown length in ms
      - begin_time : server timestamp (ms) when the CD started
      - valid_cd_time : elapsed cooldown time in ms (increases over time)
      - charge_count : charges remaining (for charge-type skills)

    When ProfessionList is not available (full sync missed), falls back to
    inferring a slot map from observed skill CDs and usage history.
    """
    slot_map = dict(getattr(player, 'skill_slot_map', {}) or {})
    inferred = False
    if not slot_map:
        # Try to infer from observed skill CDs
        slot_map = _infer_slot_map_from_cds(player)
        if slot_map:
            inferred = True
            logger.info(f'[Bridge] inferred {len(slot_map)} skill slots from observed CDs')
    if not slot_map:
        return []

    # Merge missing slots from CharSerialize.Slots (field 55).
    # ProfessionList typically provides slots {1..6, 9} but NOT 7, 8 which
    # are resonance/environment skills defined in the Slots bar.
    slot_bar_map = dict(getattr(player, 'slot_bar_map', {}) or {})
    cd_map = getattr(player, 'skill_cd_map', {}) or {}
    last_use_map = getattr(player, 'skill_last_use_at', {}) or {}
    seen_ids = list(getattr(player, 'skill_seen_ids', []) or [])
    if slot_bar_map:
        for bar_slot_id, bar_skill_id in slot_bar_map.items():
            if bar_slot_id in slot_map:
                continue  # ProfessionList already provides this slot
            if bar_skill_id <= 0:
                continue
            # Resolve skill_level_id: find a matching CD entry for this base skill_id
            skill_level_id = _resolve_skill_level_id_from_cd(
                bar_skill_id, cd_map, last_use_map, seen_ids)
            if skill_level_id <= 0:
                # Fallback: try compose with level 1
                skill_level_id = bar_skill_id * 100 + 1
            slot_map[bar_slot_id] = skill_level_id
    server_offset_ms = float(getattr(player, 'server_time_offset_ms', 0.0) or 0.0)
    now_local_ms = int(time.time() * 1000)
    now_server_ms = int(now_local_ms + server_offset_ms) if server_offset_ms else 0
    now_t = time.time()

    slots = []
    for slot_idx, skill_level_id in sorted(slot_map.items(), key=lambda item: item[0]):
        # Remap internal slot index to display position (only for non-inferred ProfessionList data)
        display_idx = _remap_slot_index(slot_idx) if not inferred else slot_idx
        cooldown_pct = 0.0
        active = False
        remaining_ms = 0
        charge_count = 0
        skill_cd_type = 0
        source_confidence = 0.0
        cd_info = cd_map.get(skill_level_id)
        if cd_info:
            # 'duration' is the total CD length; 'valid_cd_time' is elapsed time
            total_ms = int(cd_info.get('duration') or 0)
            elapsed_ms = int(cd_info.get('valid_cd_time') or 0)
            charge_count = max(0, int(cd_info.get('charge_count') or 0))
            skill_cd_type = max(0, int(cd_info.get('skill_cd_type') or 0))

            # Filter out impossible values (wrapped int64 from charge entries)
            if total_ms > 600_000 or total_ms < 0:
                total_ms = 0
            if elapsed_ms > 600_000 or elapsed_ms < 0:
                elapsed_ms = 0

            if total_ms > 0:
                begin_ms = int(cd_info.get('begin_time') or 0)
                if now_server_ms > 0 and begin_ms > 0:
                    # Best path: compute remaining from server clock
                    remaining_ms = max(0, begin_ms + total_ms - now_server_ms)
                    source_confidence = 1.0
                elif 0 < elapsed_ms <= total_ms:
                    # valid_cd_time is elapsed: remaining = total - elapsed
                    remaining_ms = max(0, total_ms - elapsed_ms)
                    source_confidence = 0.8
                else:
                    # Fallback: use local observation timestamp
                    observed_at_ms = int(cd_info.get('observed_at_ms') or now_local_ms)
                    local_elapsed = max(0, now_local_ms - observed_at_ms)
                    remaining_ms = max(0, total_ms - local_elapsed)
                    source_confidence = 0.55
                if charge_count > 0:
                    cooldown_pct = 0.0
                else:
                    cooldown_pct = max(0.0, min(1.0, remaining_ms / total_ms))
                active = remaining_ms > 0 and (now_t - float(last_use_map.get(skill_level_id, 0.0))) <= 0.45
        skill_id = _get_skill_id_for_level(player, skill_level_id)
        skill_name = _get_skill_name(skill_id) or _get_skill_name(skill_level_id)
        # Compute display state for the HUD
        if charge_count > 0 or (remaining_ms <= 120 and cooldown_pct <= 0.02):
            state = 'ready'
        elif active:
            state = 'active'
        elif remaining_ms > 0 and cooldown_pct > 0.02:
            state = 'cooldown'
        else:
            state = 'ready'
        slots.append({
            'index': int(display_idx),
            'skill_level_id': int(skill_level_id or 0),
            'skill_id': int(skill_id or 0),
            'state': state,
            'cooldown_pct': round(cooldown_pct, 3),
            'remaining_ms': max(0, int(remaining_ms or 0)),
            'charge_count': max(0, int(charge_count or 0)),
            'skill_cd_type': max(0, int(skill_cd_type or 0)),
            'active': bool(active),
            'source_confidence': round(float(source_confidence or 0.0), 2),
            'ready_edge': False,
            'name': skill_name,
            'inferred': inferred,
        })

    # Sort by display index for consistent HUD ordering
    slots.sort(key=lambda s: s['index'])
    return slots


class PacketBridge:
    """
    与 RecognitionEngine 同接口的抓包数据桥。

    Interface:
      - __init__(state_mgr, settings=None)
      - start()
      - stop()
      - single_capture() -> Optional[dict]
    """

    # ── 节流参数 ──
    _PUBLISH_MIN_INTERVAL = 0.08     # 非 tick 推送最小间隔 (秒), 约 12fps
    _SAVE_CACHE_INTERVAL = 5.0       # settings 写盘最小间隔 (秒)

    def __init__(self, state_mgr: GameStateManager, settings=None):
        self._state_mgr = state_mgr
        self._settings = settings
        self._running = False

        # 抓包层
        self._capture = None
        self._parser = None
        self._thread = None
        self._last_update_t: float = 0
        self._last_publish_t: float = 0      # 上次非 tick 推送时间
        self._last_save_t: float = 0          # 上次 settings 写盘时间
        self._lock = threading.Lock()
        self._stable_sta_current: int = 0
        self._stable_sta_max: int = 0
        self._pending_sta_current = None
        self._pending_sta_hits: int = 0
        self._last_player = None

        # 检查 Npcap 可用性
        self._npcap_ok = False
        self._error_msg = ''

    def _get_watched_slots(self):
        """Return set of watched skill slot indices from settings."""
        if self._settings:
            watched = self._settings.get('watched_skill_slots', None)
            if isinstance(watched, list) and watched:
                return set(int(x) for x in watched)
        # Default: watch all slots (1-based)
        return set(range(1, 20))

    def _get_component_source(self, component: str, default: str = 'packet') -> str:
        if self._settings and hasattr(self._settings, 'get_component_source'):
            return self._settings.get_component_source(component, default)
        if self._settings and hasattr(self._settings, 'get'):
            legacy = self._settings.get('data_source', default)
            return 'packet' if str(legacy).strip().lower() == 'packet' else 'vision'
        return default

    def _use_packet_source(self, component: str) -> bool:
        return self._get_component_source(component, 'packet') == 'packet'

    def start(self):
        """启动抓包，后台线程运行"""
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True,
                                        name='sao_bridge')
        self._thread.start()

    def stop(self):
        """停止抓包"""
        self._running = False
        if self._capture:
            try:
                self._capture.stop()
            except Exception:
                pass
        if self._thread:
            self._thread.join(timeout=3)
            self._thread = None

    def single_capture(self):
        """返回当前快照 (兼容 RecognitionEngine 接口)"""
        return self._state_mgr.state.to_dict()

    # ─── 内部 ───

    def _run(self):
        """主运行流程"""
        logger.info('[Bridge] 启动网络抓包数据桥...')

        # ── Npcap 自动安装 ──
        try:
            from install_npcap import ensure_npcap, is_npcap_installed
            if not is_npcap_installed():
                self._state_mgr.update(recognition_ok=False,
                                       error_msg='正在自动安装 Npcap...')
                ok, msg = ensure_npcap(silent=True)
                if not ok:
                    self._error(f'Npcap 安装失败: {msg}')
                    return
        except Exception as e:
            logger.warning(f'[Bridge] Npcap 自动安装检查失败: {e}')

        # 检查设备
        try:
            forced_dev = (self._settings or {}).get('capture_device')
            if forced_dev:
                # 从设备列表里按名称或描述子串匹配
                devs = list_devices()
                dev = next(
                    (d for d in devs
                     if forced_dev in d['name'] or forced_dev in d['description']),
                    None
                )
                if not dev:
                    logger.warning(f'[Bridge] 未找到 capture_device={forced_dev!r}，回退到自动选择')
                    dev = auto_select_device()
            else:
                dev = auto_select_device()
            if not dev:
                self._error('未找到网络设备，请安装 Npcap')
                return
            logger.info(f'[Bridge] 选择网络设备: {dev["description"]}')
        except RuntimeError as e:
            self._error(str(e))
            return

        # 创建解析器
        preferred_uid = 0
        cached_uid = getattr(self._state_mgr.state, 'player_id', '')
        try:
            if isinstance(cached_uid, str) and cached_uid.isdigit():
                preferred_uid = int(cached_uid)
        except Exception:
            preferred_uid = 0
        self._parser = PacketParser(
            on_self_update=self._on_player_update,
            preferred_uid=preferred_uid,
        )

        # ── 从 settings 恢复缓存的职业技能映射 ──
        if self._settings:
            cached_prof = self._settings.get('profession_skill_cache')
            if isinstance(cached_prof, dict):
                for k, v in cached_prof.items():
                    try:
                        pid = int(k)
                        slot_map = {int(sk): int(sv) for sk, sv in v.items()}
                        self._parser._profession_skill_cache[pid] = slot_map
                        logger.info(f'[Bridge] restored profession_skill_cache pid={pid} slots={slot_map}')
                    except Exception:
                        pass

        # 创建抓包器
        self._capture = PacketCapture(
            on_game_packet=self._parser.process_packet,
            device=dev
        )

        self._npcap_ok = True
        self._state_mgr.update(recognition_ok=False,
                               error_msg='等待游戏服务器连接...')

        # 启动抓包
        self._capture.start()

        # 状态监控循环
        next_status_check = 0.0
        next_tick = 0.0
        while self._running:
            now = time.time()
            if now >= next_tick:
                try:
                    with self._lock:
                        player = self._last_player
                        if player is not None and (
                            self._use_packet_source('skills') or self._use_packet_source('stamina')
                        ):
                            self._publish_player_update(player, from_tick=True)
                except Exception:
                    pass
                next_tick = now + 0.20

            if now < next_status_check:
                time.sleep(0.05)
                continue
            next_status_check = now + 1.0
            if self._capture.server_identified:
                # 检查数据超时
                with self._lock:
                    if self._last_update_t > 0:
                        idle = time.time() - self._last_update_t
                        if idle > 10:
                            self._state_mgr.update(
                                recognition_ok=False,
                                error_msg=f'数据超时 ({idle:.0f}s)')
                    else:
                        self._state_mgr.update(
                            recognition_ok=False,
                            error_msg='已连接服务器，等待角色数据...')
            else:
                self._state_mgr.update(recognition_ok=False,
                                       error_msg='搜索游戏服务器中...')

        logger.info('[Bridge] 数据桥已停止')

    def _on_player_update(self, player: PlayerData):
        """解析器回调: 当前玩家数据变更 (节流: 跳过高频重复推送)"""
        now = time.time()
        with self._lock:
            self._last_update_t = now
            self._last_player = player
            # 节流: 距离上次非 tick 推送不足 _PUBLISH_MIN_INTERVAL 则跳过,
            # 由 200ms tick 循环补推.
            if now - self._last_publish_t < self._PUBLISH_MIN_INTERVAL:
                return
            self._last_publish_t = now
            self._publish_player_update(player, from_tick=False)

    def _publish_player_update(self, player: PlayerData, from_tick: bool = False):
        updates = {
            'recognition_ok': True,
            'error_msg': '',
        }

        if player.name and self._use_packet_source('identity'):
            updates['player_name'] = player.name
        if player.uid and self._use_packet_source('identity'):
            updates['player_id'] = str(player.uid)
        if player.level > 0 and self._use_packet_source('level'):
            updates['level_base'] = player.level
        # Do not treat rank_level as level_extra.
        # Keep the larger recognized extra level to avoid stale packet values
        # overwriting OCR's newer result.
        _level_extra = max(0, int(getattr(player, 'level_extra', 0) or 0))
        if self._use_packet_source('level') and _level_extra > 0:
            updates['level_extra'] = _level_extra
            logger.info(
                f'[Bridge] level_extra={_level_extra} '
                f'(source={getattr(player, "level_extra_source", "")}, '
                f'medal={player.season_medal_level}, hunt={player.monster_hunt_level}, '
                f'bp={player.battlepass_level}, bp_data={player.battlepass_data_level})'
            )
        if player.profession_id > 0 and self._use_packet_source('identity'):
            updates['profession_id'] = player.profession_id
            if player.profession:
                updates['profession_name'] = player.profession
        if self._use_packet_source('hp') and player.max_hp > 0 and player.hp > 0:
            updates['hp_current'] = int(player.hp)
            updates['hp_max'] = int(player.max_hp)
            updates['hp_pct'] = player.hp / player.max_hp
        elif self._use_packet_source('hp') and player.max_hp > 0 and player.hp == 0:
            # 只有来自完整同步 (SyncContainerData) 的 HP=0 才接受
            if getattr(player, 'hp_from_full_sync', False):
                updates['hp_current'] = 0
                updates['hp_max'] = int(player.max_hp)
                updates['hp_pct'] = 0.0
                player.hp_from_full_sync = False  # 重置标记
            else:
                logger.debug(f'[Bridge] 忽略增量 HP=0 更新 (max_hp={player.max_hp})')
        elif self._use_packet_source('hp') and player.hp == 0 and player.max_hp == 0:
            pass  # 未知，不更新
        elif self._use_packet_source('hp'):
            updates['hp_current'] = int(player.hp)

        global _stamina_max_cached
        stamina_resource_id = int(getattr(player, 'stamina_resource_id', 0) or 0)
        resource_energy_info = (getattr(player, 'energy_info_map', {}) or {}).get(stamina_resource_id) or {}
        packet_sta_max = int(resource_energy_info.get('energy_value', 0) or 0)
        if packet_sta_max <= 0:
            packet_sta_max = int(
                max(0, getattr(player, 'energy_limit', 0)) +
                max(0, getattr(player, 'extra_energy_limit', 0))
            )
        packet_sta_max = _sanitize_packet_stamina_max(
            packet_sta_max,
            int(self._state_mgr.state.stamina_max or _stamina_max_cached or 0),
        )
        if packet_sta_max > 0:
            _stamina_max_cached = packet_sta_max
        else:
            packet_sta_max = _stamina_max_cached or self._state_mgr.state.stamina_max or 0

        if self._use_packet_source('stamina'):
            packet_sta = None
            energy_priority = 0
            ratio_value = getattr(player, 'stamina_ratio', -1.0)
            ratio_observed_at = float(getattr(player, 'stamina_ratio_observed_at', 0.0) or 0.0)
            packet_sta = _resolve_resource_stamina(player, packet_sta_max)
            if packet_sta is not None:
                energy_priority = 3

            ratio_sta = None
            if packet_sta_max > 0 and ratio_observed_at > 0 and (time.time() - ratio_observed_at) <= 2.5:
                ratio_sta = _resolve_ratio_stamina(ratio_value, packet_sta_max)
            if ratio_sta is not None:
                if packet_sta is None:
                    packet_sta = ratio_sta
                    energy_priority = max(energy_priority, 2)
                else:
                    ratio_cur, _ = ratio_sta
                    packet_cur, _ = packet_sta
                    threshold = max(90, int(packet_sta_max * 0.10))
                    if abs(ratio_cur - packet_cur) >= threshold:
                        packet_sta = ratio_sta
                        energy_priority = max(energy_priority, 2)

            if (
                packet_sta is None and
                getattr(player, 'energy_valid', False) and
                isinstance(getattr(player, 'energy', 0.0), (int, float))
            ):
                packet_sta = _resolve_packet_stamina(player.energy, packet_sta_max)
                if packet_sta is not None:
                    energy_priority = max(energy_priority, 1)

            if packet_sta is not None:
                sta_cur, sta_pct = packet_sta
                sta_cur = self._stabilize_packet_stamina(sta_cur, packet_sta_max, energy_priority)
                sta_pct = (sta_cur / packet_sta_max) if packet_sta_max > 0 else 0.0
                updates['stamina_current'] = sta_cur
                updates['stamina_max'] = packet_sta_max
                updates['stamina_pct'] = sta_pct
            elif packet_sta_max > 0:
                self._stable_sta_max = packet_sta_max
                updates['stamina_max'] = packet_sta_max

            # ── 比率兜底: 当 max 未知但 ratio 可用时, 直接推送百分比 ──
            if packet_sta is None and packet_sta_max <= 0:
                ratio_value = getattr(player, 'stamina_ratio', -1.0)
                ratio_observed_at = float(getattr(player, 'stamina_ratio_observed_at', 0.0) or 0.0)
                if 0.0 <= ratio_value <= 1.0 and ratio_observed_at > 0 and (time.time() - ratio_observed_at) <= 3.0:
                    updates['stamina_pct'] = ratio_value
                    # 没有绝对数值, 不更新 stamina_current/stamina_max
                    logger.debug(f'[Bridge] ratio-only STA fallback: pct={ratio_value:.3f}')

        if self._use_packet_source('skills'):
            skill_slots = _build_packet_skill_slots(player)
            previous_slots = {}
            for slot in getattr(self._state_mgr.state, 'skill_slots', []) or []:
                if not isinstance(slot, dict):
                    continue
                try:
                    previous_slots[int(slot.get('index', 0) or 0)] = slot
                except Exception:
                    continue
            for slot in skill_slots:
                prev_slot = previous_slots.get(int(slot.get('index', 0) or 0))
                slot['ready_edge'] = bool(prev_slot and _slot_is_ready(slot) and not _slot_is_ready(prev_slot))
            updates['skill_slots'] = skill_slots
            watched = self._get_watched_slots()
            updates['burst_ready'] = compute_burst_ready(skill_slots, watched)

        self._state_mgr.update(**updates)
        # ── 积极缓存: 有意义的数据就保存 (节流写盘) ──
        _should_save = False
        if not from_tick and self._settings is not None:
            now_save = time.time()
            if now_save - self._last_save_t >= self._SAVE_CACHE_INTERVAL:
                if updates.get('level_extra', 0) > 0:
                    _should_save = True
                elif updates.get('stamina_max', 0) > 0 and int(self._state_mgr.state.stamina_max or 0) > 0:
                    _should_save = True
                elif updates.get('skill_slots') and len(updates['skill_slots']) > 0:
                    _should_save = True
        if _should_save:
            self._last_save_t = time.time()
            try:
                self._state_mgr.save_cache(self._settings)
                # 也持久化职业技能缓存
                if self._parser and self._parser._profession_skill_cache:
                    serializable = {}
                    for pid, smap in self._parser._profession_skill_cache.items():
                        serializable[str(pid)] = {str(k): int(v) for k, v in smap.items()}
                    self._settings.set('profession_skill_cache', serializable)
                    self._settings.save()
            except Exception:
                pass

    def _error(self, msg: str):
        logger.error(f'[Bridge] {msg}')
        self._state_mgr.update(recognition_ok=False, error_msg=msg)

    def _stabilize_packet_stamina(self, current: int, stamina_max: int, energy_priority: int) -> int:
        """Hold packet-only STA spikes until they repeat, instead of showing instant jumps."""
        if stamina_max <= 0:
            self._stable_sta_current = 0
            self._stable_sta_max = 0
            self._pending_sta_current = None
            self._pending_sta_hits = 0
            return max(0, int(current))

        current = max(0, min(int(current), int(stamina_max)))
        if self._stable_sta_max != stamina_max:
            self._stable_sta_current = min(max(0, int(self._stable_sta_current or 0)), int(stamina_max))
            self._stable_sta_max = int(stamina_max)
            self._pending_sta_current = None
            self._pending_sta_hits = 0

        if energy_priority != 2:
            self._stable_sta_current = current
            self._pending_sta_current = None
            self._pending_sta_hits = 0
            return current

        previous = max(0, min(int(self._stable_sta_current or 0), int(stamina_max)))
        if previous <= 0:
            self._stable_sta_current = current
            return current

        threshold = max(36, int(stamina_max * 0.08))
        if abs(current - previous) <= threshold:
            self._stable_sta_current = current
            self._pending_sta_current = None
            self._pending_sta_hits = 0
            return current

        if self._pending_sta_current == current:
            self._pending_sta_hits += 1
        else:
            self._pending_sta_current = current
            self._pending_sta_hits = 1

        if self._pending_sta_hits >= 2:
            logger.info(
                f'[Bridge] accept repeated STA candidate {current}/{stamina_max} '
                f'after filtering spike from {previous}/{stamina_max}'
            )
            self._stable_sta_current = current
            self._pending_sta_current = None
            self._pending_sta_hits = 0
            return current

        logger.debug(
            f'[Bridge] hold STA spike candidate {current}/{stamina_max} '
            f'(prev={previous}/{stamina_max}, priority={energy_priority})'
        )
        return previous
