# -*- coding: utf-8 -*-
"""
skill_cd_monitor — 自定义技能 CD 监控引擎.

在标准 9 个装备槽之外, 允许用户额外监控最多 5 个技能的 CD 状态.
支持:
  - 从 skill_cd_map (TCP/MEM) 实时读取任意技能 CD
  - 用户手动指定天赋→父槽位归属
  - 独立/分组两种提醒模式
  - CD 进度条 + 就绪亮灯 + 可选 TTS

数据契约 — custom_skill_monitors 配置项:
  [
    {
      "slot": 10,                   # display index 10-14
      "skill_level_id": 240601,     # 监控的 skill_level_id
      "skill_id": 2406,             # base skill_id (= skill_level_id // 100)
      "name": "先锋追击",           # 缓存的显示名
      "parent_slot": 2,             # 归属的父槽位 (0=独立)
      "tts_enabled": True,
      "tts_text": "",               # 自定义 TTS 文本 (空=用 name)
      "visual_enabled": True,
    },
    ...
  ]

输出 — custom_skill_slots (与 GameState.skill_slots 同格式):
  [
    {
      "index": 10,
      "skill_level_id": 240601,
      "skill_id": 2406,
      "name": "先锋追击",
      "state": "ready" | "cooldown",
      "cooldown_pct": 0.0,
      "remaining_ms": 0,
      "total_cd_ms": 12000,
      "charge_count": 0,
      "active": False,
      "parent_slot": 2,
      "tts_enabled": True,
      "visual_enabled": True,
      "custom": True,
      "ready_edge": False,
    },
    ...
  ]
"""
from __future__ import annotations

import time
from typing import Any, Dict, List, Optional, Tuple

try:
    import _sao_cy_packet as _CY_PACKET  # type: ignore[import-not-found]
except Exception:
    _CY_PACKET = None

CUSTOM_SLOT_MIN = 10
CUSTOM_SLOT_MAX = 14
MAX_CUSTOM_SLOTS = CUSTOM_SLOT_MAX - CUSTOM_SLOT_MIN + 1


def _safe_int(v, default: int = 0) -> int:
    try:
        return int(v or default)
    except Exception:
        return default


def _resolve_skill_name(skill_id: int, skill_level_id: int) -> str:
    try:
        from tools.tablekit.name_tables import names
        n = names.skill(skill_id)
        if n and not n.startswith('#') and '未知' not in n:
            return n
        n = names.skill(skill_level_id)
        if n and not n.startswith('#') and '未知' not in n:
            return n
    except Exception:
        pass
    return f'技能#{skill_id}'


def validate_monitors(monitors: Any) -> List[dict]:
    """校验并规范化 custom_skill_monitors 配置."""
    if not isinstance(monitors, list):
        return []
    result = []
    used_slots = set()
    for entry in monitors:
        if not isinstance(entry, dict):
            continue
        slot = _safe_int(entry.get('slot'), 0)
        if slot < CUSTOM_SLOT_MIN or slot > CUSTOM_SLOT_MAX:
            continue
        if slot in used_slots:
            continue
        slid = _safe_int(entry.get('skill_level_id'), 0)
        if slid <= 0:
            continue
        sid = _safe_int(entry.get('skill_id'), slid // 100)
        parent = _safe_int(entry.get('parent_slot'), 0)
        if parent < 0 or parent > 9:
            parent = 0
        name = str(entry.get('name') or '') or _resolve_skill_name(sid, slid)
        result.append({
            'slot': slot,
            'skill_level_id': slid,
            'skill_id': sid,
            'name': name,
            'parent_slot': parent,
            'tts_enabled': bool(entry.get('tts_enabled', True)),
            'tts_text': str(entry.get('tts_text') or ''),
            'visual_enabled': bool(entry.get('visual_enabled', True)),
        })
        used_slots.add(slot)
    result.sort(key=lambda x: x['slot'])
    return result[:MAX_CUSTOM_SLOTS]


def next_available_slot(monitors: List[dict]) -> int:
    """返回下一个可用的自定义槽位号, 无可用返回 0."""
    used = {_safe_int(m.get('slot'), 0) for m in monitors}
    for s in range(CUSTOM_SLOT_MIN, CUSTOM_SLOT_MAX + 1):
        if s not in used:
            return s
    return 0


def compute_custom_skill_slots(
    monitors: List[dict],
    skill_cd_map: Dict[int, dict],
    server_time_offset_ms: Optional[float] = None,
    player_attrs: Optional[dict] = None,
    prev_slots: Optional[List[dict]] = None,
) -> List[dict]:
    """从 skill_cd_map 计算自定义槽位的 CD 状态.

    Parameters
    ----------
    monitors : list
        validated custom_skill_monitors config
    skill_cd_map : dict
        skill_level_id -> CD state dict (from TCP or MEM)
    server_time_offset_ms : float | None
        server clock offset
    player_attrs : dict | None
        {attr_skill_cd, attr_skill_cd_pct, ...} for CD computation
    prev_slots : list | None
        previous frame's custom_skill_slots (for edge detection)

    Returns
    -------
    list[dict]
        custom skill slot entries in GameState.skill_slots format
    """
    if not monitors:
        return []

    now_local_ms = int(time.time() * 1000)
    now_server_ms = int(now_local_ms + (server_time_offset_ms or 0))
    now_t = time.time()
    attrs = player_attrs or {}

    prev_map: Dict[int, dict] = {}
    if prev_slots:
        for ps in prev_slots:
            idx = _safe_int(ps.get('index'), 0)
            if idx >= CUSTOM_SLOT_MIN:
                prev_map[idx] = ps

    result = []
    for mon in monitors:
        slot_idx = mon['slot']
        slid = mon['skill_level_id']
        cd_info = skill_cd_map.get(slid)

        state = 'ready'
        cooldown_pct = 0.0
        remaining_ms = 0
        total_cd_ms = 0
        charge_count = 0
        active = False
        confidence = 1.0

        if cd_info and _safe_int(cd_info.get('duration'), 0) > 0:
            total_ms = _safe_int(cd_info.get('duration'), 0)
            elapsed_ms = _safe_int(cd_info.get('valid_cd_time'), 0)
            cc = max(0, _safe_int(cd_info.get('charge_count'), 0))

            if _CY_PACKET is not None:
                try:
                    (state, cooldown_pct, remaining_ms, effective_ms,
                     charge_count, active, confidence) = (
                        _CY_PACKET.compute_skill_cd_ui_state(
                            total_ms, elapsed_ms, cc,
                            _safe_int(cd_info.get('sub_cd_ratio'), 0),
                            _safe_int(cd_info.get('sub_cd_fixed'), 0),
                            _safe_int(cd_info.get('accelerate_cd_ratio'), 0),
                            _safe_int(attrs.get('attr_skill_cd'), 0),
                            _safe_int(attrs.get('attr_skill_cd_pct'), 0),
                            _safe_int(attrs.get('attr_cd_accelerate_pct'), 0),
                            _safe_int(attrs.get('temp_attr_cd_pct'), 0),
                            _safe_int(attrs.get('temp_attr_cd_fixed'), 0),
                            _safe_int(attrs.get('temp_attr_cd_accel'), 0),
                            now_server_ms, now_local_ms,
                            _safe_int(cd_info.get('begin_time'), 0),
                            _safe_int(
                                cd_info.get('last_vcd_update_ms')
                                or cd_info.get('observed_at_ms'), now_local_ms),
                            float(cd_info.get('vcd_speed_ratio') or 0),
                            0.0, now_t,
                        ))
                    total_cd_ms = max(0, int(effective_ms or total_ms))
                except Exception:
                    state, cooldown_pct, remaining_ms = _fallback_cd_calc(
                        cd_info, now_server_ms, now_local_ms)
                    total_cd_ms = total_ms
                    charge_count = cc
            else:
                state, cooldown_pct, remaining_ms = _fallback_cd_calc(
                    cd_info, now_server_ms, now_local_ms)
                total_cd_ms = total_ms
                charge_count = cc

        prev_slot = prev_map.get(slot_idx)
        prev_ready = False
        if prev_slot:
            ps = str(prev_slot.get('state') or '').lower()
            prev_ready = ps in ('ready', 'active')
        now_ready = state in ('ready', 'active')
        ready_edge = now_ready and not prev_ready

        result.append({
            'index': slot_idx,
            'skill_level_id': slid,
            'skill_id': mon['skill_id'],
            'name': mon['name'],
            'state': state,
            'cooldown_pct': round(float(cooldown_pct), 4),
            'remaining_ms': max(0, int(remaining_ms)),
            'total_cd_ms': max(0, int(total_cd_ms)),
            'charge_count': max(0, int(charge_count)),
            'active': bool(active),
            'parent_slot': mon['parent_slot'],
            'tts_enabled': mon['tts_enabled'],
            'tts_text': mon.get('tts_text') or '',
            'visual_enabled': mon['visual_enabled'],
            'custom': True,
            'ready_edge': ready_edge,
        })

    return result


def _fallback_cd_calc(
    cd_info: dict,
    now_server_ms: int,
    now_local_ms: int,
) -> Tuple[str, float, int]:
    """Pure-Python fallback when Cython unavailable."""
    total = _safe_int(cd_info.get('duration'), 0)
    if total <= 0:
        return ('ready', 0.0, 0)
    begin = _safe_int(cd_info.get('begin_time'), 0)
    vcd = _safe_int(cd_info.get('valid_cd_time'), 0)
    if vcd > 0:
        remaining = max(0, total - vcd)
    elif begin > 0 and now_server_ms > 0:
        remaining = max(0, (begin + total) - now_server_ms)
    else:
        observed = _safe_int(cd_info.get('observed_at_ms'), 0)
        if observed > 0:
            elapsed_local = now_local_ms - observed
            remaining = max(0, total - int(elapsed_local))
        else:
            remaining = total
    pct = remaining / total if total > 0 else 0.0
    if remaining <= 120 or pct <= 0.02:
        return ('ready', 0.0, 0)
    return ('cooldown', round(pct, 4), remaining)


def enumerate_available_skills(
    skill_cd_map: Dict[int, dict],
    skill_seen_ids: Optional[list] = None,
    equipped_slids: Optional[set] = None,
) -> List[dict]:
    """枚举所有可用于自定义监控的技能.

    Returns list of {skill_level_id, skill_id, name, has_cd, remaining_ms}
    sorted by name.
    """
    all_slids: set = set()
    if skill_cd_map:
        all_slids.update(skill_cd_map.keys())
    if skill_seen_ids:
        all_slids.update(int(s) for s in skill_seen_ids if _safe_int(s, 0) > 0)

    equipped = equipped_slids or set()
    now_local_ms = int(time.time() * 1000)
    items = []
    for slid in sorted(all_slids):
        if slid <= 0:
            continue
        if slid in equipped:
            continue
        sid = slid // 100
        name = _resolve_skill_name(sid, slid)
        cd_info = skill_cd_map.get(slid) if skill_cd_map else None
        has_cd = False
        remaining = 0
        total = 0
        if cd_info:
            total = _safe_int(cd_info.get('duration'), 0)
            if total > 0:
                has_cd = True
                _, _, remaining = _fallback_cd_calc(cd_info, 0, now_local_ms)
        items.append({
            'skill_level_id': slid,
            'skill_id': sid,
            'name': name,
            'has_cd': has_cd,
            'remaining_ms': remaining,
            'total_cd_ms': total,
        })
    items.sort(key=lambda x: (not x['has_cd'], x['name']))
    return items
