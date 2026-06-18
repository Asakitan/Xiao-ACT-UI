# -*- coding: utf-8 -*-
"""Star Resonance ACT text labels for Tk panels."""

from __future__ import annotations

import tkinter as tk
from typing import Any, Callable, Iterable, Mapping, Optional

from gui_modules import sao_panel_ui as ui
from gui_modules.sao_panel_components import SP_SM, _finite_int, _pc, fmt_clock, status_badge

_TOPIC_CN = {
    "damage": "伤害", "heal": "治疗", "skill": "技能", "actor_skill": "技能",
    "monster": "怪物", "monster_skill": "怪物技能", "boss": "首领", "boss_state": "首领状态",
    "boss_mechanic": "首领机制", "boss_mechanic_skill": "首领机制", "dungeon": "地牢",
    "scene": "场景", "death": "死亡", "buff": "增益", "player_buff": "玩家增益",
    "factor_buff": "因子增益", "trigger": "触发", "timer": "计时", "target": "目标",
    "log": "日志", "event": "事件", "shield": "护盾", "mitigation": "减伤",
    "incoming_damage": "承受伤害", "healing": "治疗", "ultimate_skill": "终极技",
    "environment_skill": "环境技能", "field_marker": "场地标记",
    "system": "系统", "plugin_ui_invalidate": "插件重绘", "encounter_finalized": "战斗结束",
    "encounter_started": "战斗开始", "encounter_reset": "战斗重置",
}
_SOURCE_CN = {
    "tcp": "封包", "packet": "封包", "entity": "实体", "mem": "内存", "memory": "内存",
    "live": "实时", "hybrid": "混合", "history": "历史",
    "replay": "回放", "ui": "界面", "offline_import": "离线导入", "plugin": "插件",
    "unknown": "未知",
}


def topic_cn(value: Any, *, default: str = "事件") -> str:
    text = str(value or "").strip()
    if not text:
        return default
    return _TOPIC_CN.get(text.lower(), text)


def source_cn(value: Any, *, default: str = "未知") -> str:
    text = str(value or "").strip()
    if not text:
        return default
    return _SOURCE_CN.get(text.lower(), text)


def readable_event_line(row: Mapping[str, Any], *, value_fmt: Optional[Callable[[Any], Any]] = None) -> str:
    if not isinstance(row, Mapping):
        return ""
    fmt_value = value_fmt or (lambda v: "" if v in (None, "") else str(v))
    clock = fmt_clock(row.get("time_ms"))
    topic = topic_cn(row.get("topic"))
    actor = str(row.get("actor") or "").strip()
    target = str(row.get("target") or "").strip()
    label = str(row.get("label") or "").strip()
    value = str(fmt_value(row.get("value")) or "")
    parts = [clock, topic]
    actor_disp = source_cn(actor) if actor else ""
    if actor_disp and target:
        parts.append(f"{actor_disp} → {target}")
    elif target or actor_disp:
        parts.append(target or actor_disp)
    raw_topic = str(row.get("topic") or "").strip().lower()
    if label and label.lower() != raw_topic and label not in (target, topic):
        parts.append(label)
    if value and value not in ("0", "0.00"):
        parts.append(value)
    return " · ".join(part for part in parts if part)


def source_badges(parent: tk.Misc, sources: Iterable[Mapping[str, Any]]) -> tk.Frame:
    frame = tk.Frame(parent, bg=_pc('body_bg', ui._SAO_PANEL_BODY_BG))
    for item in sources or []:
        if not isinstance(item, Mapping):
            continue
        text = f"{source_cn(item.get('source'), default='-')} · {_finite_int(item.get('count'), 0, lo=0)}"
        status_badge(frame, text, kind='cyan').pack(side='left', padx=(0, SP_SM), pady=2)
    return frame
