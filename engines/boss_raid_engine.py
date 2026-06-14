# -*- coding: utf-8 -*-
"""Boss Raid timeline engine — profile model, state machine, DPS tracking."""

import copy
import json
import math
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from typing import Any, Callable, Dict, List, Optional, Tuple

from config import BASE_DIR
from tools.tablekit.combat_preparse import enrich_boss_event
from engines.boss_skill_store import (
    BossSkillStore, KIND_SKILL, KIND_MECHANIC, KIND_STATE,
)

# Synthetic observation ids for tracked boss states (kept disjoint from real
# skill/event ids by their KIND_STATE namespace).
_STATE_ENRAGE = 1
_STATE_INVINCIBLE = 2

# BuffEventType → semantic tag for mechanic observations.
_EVENT_TAG = {
    12: "death", 15: "body_part", 17: "body_part", 47: "shield",
    51: "super_armor", 58: "breaking", 88: "fracture",
}

# Boss hard-enrage timer buffs observed in the offline name tables.  These are
# timer carriers, not mechanics/casts, so they should drive the enrage clock and
# be ignored by the pure-TCP boss-skill diff path.
_ENRAGE_TIMER_BUFF_IDS = {501706, 501710, 501712, 501714}
_ENRAGE_TIMER_NAME_TOKENS = ("硬狂暴计时器",)

from utils.perf_probe import probe as _probe
import _sao_cy_combat as _CY_COMBAT  # type: ignore[import-not-found]

BOSS_RAID_SCHEMA_VERSION = 2
DEFAULT_BOSS_RAID_SERVER_URL = "http://doi.sakisense.top:15538"
BOSS_RAID_EXPORT_DIR = os.path.join(BASE_DIR, "exports", "boss_raids")

# ═══════════════════════════════════════════════
#  Helpers
# ═══════════════════════════════════════════════

def _utc_now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _new_id(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex[:12]}"


def _coerce_bool(value: Any, default: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        text = value.strip().lower()
        if text in ("1", "true", "yes", "on"):
            return True
        if text in ("0", "false", "no", "off"):
            return False
    if isinstance(value, (int, float)):
        return bool(value)
    return default


def _coerce_int(value: Any, default: int = 0, minimum: Optional[int] = None,
                maximum: Optional[int] = None) -> int:
    try:
        result = int(value)
    except Exception:
        result = int(default)
    if minimum is not None:
        result = max(minimum, result)
    if maximum is not None:
        result = min(maximum, result)
    return result


def _coerce_float(value: Any, default: float = 0.0, minimum: Optional[float] = None,
                  maximum: Optional[float] = None) -> float:
    try:
        result = float(value)
    except Exception:
        result = float(default)
    if not math.isfinite(result):
        result = float(default)
    if minimum is not None:
        result = max(minimum, result)
    if maximum is not None:
        result = min(maximum, result)
    return result


def _string(value: Any) -> str:
    return str(value or "").strip()


def _is_enrage_timer_buff(buff: Any) -> bool:
    if not isinstance(buff, dict):
        return False
    bid = _coerce_int(buff.get("buff_id") or buff.get("id"), 0)
    if bid in _ENRAGE_TIMER_BUFF_IDS:
        return True
    name = _string(buff.get("name") or buff.get("buff_name"))
    return any(token in name for token in _ENRAGE_TIMER_NAME_TOKENS)


def _slugify_filename(text: str) -> str:
    value = (text or "boss_raid_profile").strip().replace(" ", "_")
    out = []
    for ch in value:
        if ch.isalnum() or ch in ("-", "_"):
            out.append(ch)
        elif "\u4e00" <= ch <= "\u9fff":
            out.append(ch)
        else:
            out.append("_")
    cleaned = "".join(out).strip("_")
    return cleaned or "boss_raid_profile"


def _mask_token(token: str) -> str:
    token = _string(token)
    if not token:
        return ""
    if len(token) <= 8:
        return "*" * len(token)
    return f"{token[:4]}...{token[-4:]}"


# ═══════════════════════════════════════════════
#  Data Model
# ═══════════════════════════════════════════════

def make_default_timeline() -> Dict[str, Any]:
    return {
        "id": _new_id("tl"),
        "time_s": 30.0,
        "label": "Alert",
        "alert_type": "both",       # sound | visual | both
        "repeat_interval_s": 0.0,   # 0 = once
        "pre_warn_s": 0.0,          # seconds before main alert to fire a softer pre-warning
        "duration_s": 0.0,          # countdown overlay duration (0 = no countdown)
        "condition": None,           # optional condition dict: {type, comparator, value}
    }


def normalize_timeline(raw: Any) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    cond = src.get("condition")
    if isinstance(cond, dict):
        cond_type = _string(cond.get("type"))
        if cond_type not in ("hp_pct", "shield_active", "breaking", "always"):
            cond_type = "always"
        cond = {
            "type": cond_type,
            "comparator": _string(cond.get("comparator")) or ">=",
            "value": _coerce_float(cond.get("value"), 0.0),
        }
    else:
        cond = None
    return {
        "id": _string(src.get("id")) or _new_id("tl"),
        "time_s": _coerce_float(src.get("time_s"), 30.0, 0.0, 86400.0),
        "label": _string(src.get("label")) or "Alert",
        "alert_type": _string(src.get("alert_type")) or "both",
        "repeat_interval_s": _coerce_float(src.get("repeat_interval_s"), 0.0, 0.0, 86400.0),
        "pre_warn_s": _coerce_float(src.get("pre_warn_s"), 0.0, 0.0, 600.0),
        "duration_s": _coerce_float(src.get("duration_s"), 0.0, 0.0, 600.0),
        "condition": cond,
    }


def _coerce_id_list(raw: Any, cap: int = 64) -> List[int]:
    """Sorted unique positive-int id list (skill/buff bindings)."""
    out: List[int] = []
    if isinstance(raw, (list, tuple, set)):
        for v in raw:
            iv = _coerce_int(v, 0)
            if iv > 0 and iv not in out:
                out.append(iv)
    return sorted(out)[:cap]


def _coerce_str_list(raw: Any, cap: int = 64) -> List[str]:
    out: List[str] = []
    if isinstance(raw, (list, tuple)):
        for v in raw:
            sv = _string(v)
            if sv and sv not in out:
                out.append(sv)
    return out[:cap]


def normalize_dodge_step(raw: Any) -> Optional[Dict[str, Any]]:
    """One auto-dodge sequence step: key + pre-delay + optional hold duration."""
    if not isinstance(raw, dict):
        return None
    key = _string(raw.get("key")).upper()
    if not key:
        return None
    return {
        "key": key,
        "delay_ms": _coerce_int(raw.get("delay_ms"), 0, 0, 10000),
        "hold_ms": _coerce_int(raw.get("hold_ms"), 0, 0, 10000),
    }


_CAM_DIRECTIONS = ("forward", "back", "left", "right",
                   "forward_left", "forward_right", "back_left", "back_right")


def _normalize_cam_direction(raw: Any, default: str = "") -> str:
    v = _string(raw).lower()
    return v if v in _CAM_DIRECTIONS else default


def _normalize_dodge_direction(raw: Any) -> str:
    """方向值: 空 / 相机相对8向 / world:dx,dz / away_point:x,z / away_boss|away_nearest /
    自动走位 goto_teammate|goto_circle|walk_sequence / goto_point:x,z。"""
    v = _string(raw).strip()
    if not v:
        return ""
    low = v.lower()
    if low in _CAM_DIRECTIONS or low in (
            "away_boss", "away_nearest",
            "goto_teammate", "goto_circle", "walk_sequence"):
        return low
    for pfx in ("world:", "away_point:", "goto_point:"):
        if low.startswith(pfx):
            try:
                a, b = [float(x) for x in v[len(pfx):].split(",")[:2]]
                return "%s%g,%g" % (pfx, a, b)
            except Exception:
                return ""
    return ""


def make_default_dodge_inline() -> Dict[str, Any]:
    return {
        "action_key": "",
        "action_label": "",
        "press_mode": "tap",     # tap | hold
        "hold_ms": 600,
        "press_count": 1,
        "sequence": [],          # [{key, delay_ms, hold_ms}] multi-key dodge
        "delay_ms": 0,           # fire N ms after the mechanic event
        "lead_ms": 300,          # fire N ms before countdown/cast end
        "cooldown_s": 3.0,
        # 定向躲避 (auto_dodge_director): 按住 WASD 把人物挪开。空=不定向。
        "direction": "",         # ''|forward|..|away_boss|away_nearest|goto_teammate|goto_point:x,z|goto_circle|walk_sequence
        "move_ms": 600,          # WASD 按住时长 (相机相对) / 闭环出圈/走位的超时上限
        "fallback_direction": "back",  # 世界/away 模式无相机或无目标时的后备相机相对方向
        "exit_margin_m": 6.0,    # away_* 闭环精准出圈: 水平距≥此值即停 ('跑出去一点就行')
        "arrive_m": 2.0,         # goto_*/walk_* 自动走位: 走到离目标≤此值即停
        # ★机制壳子的"实际范围"占位字段: 智能分析判定需自动躲避的机制留此占位,
        # 几何到手(运行时实体/离线逆向/手填)后填入 → 自动躲避按精确范围出圈;
        # source='' 表示未填(用招式生命周期兜底), 非空表示已填可精准判定。
        "geometry": make_default_geometry(),
    }


def make_default_geometry() -> Dict[str, Any]:
    """机制实际范围占位 (待填)。shape 空 / source 空 = 未填。"""
    return {
        "shape": "",       # circle/ring/cone/line/cross/'' (空=未定)
        "radius": 0.0,     # 主半径(圆/环外径/锥半径/线长) 米
        "inner": 0.0,      # 内径(环形) 米
        "angle": 0.0,      # 锥角 度
        "width": 0.0,      # 线宽 米
        "center": "boss",  # 范围中心: boss(以Boss为心) / self(以自己为心,如点名) / hit(命中点)
        "source": "",      # ''=未填占位 / manual / runtime / reverse
    }


def normalize_geometry(raw: Any) -> Dict[str, Any]:
    g = raw if isinstance(raw, dict) else {}
    shape = _string(g.get("shape")).lower()
    if shape not in ("circle", "ring", "cone", "line", "cross", ""):
        shape = ""
    center = _string(g.get("center")).lower()
    if center not in ("boss", "self", "hit"):
        center = "boss"
    src = _string(g.get("source")).lower()
    if src not in ("manual", "runtime", "reverse", ""):
        src = ""
    return {
        "shape": shape,
        "radius": _coerce_float(g.get("radius"), 0.0, 0.0, 200.0),
        "inner": _coerce_float(g.get("inner"), 0.0, 0.0, 200.0),
        "angle": _coerce_float(g.get("angle"), 0.0, 0.0, 360.0),
        "width": _coerce_float(g.get("width"), 0.0, 0.0, 200.0),
        "center": center,
        "source": src,
    }


def normalize_dodge(raw: Any) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    inline_src = src.get("inline") if isinstance(src.get("inline"), dict) else {}
    press_mode = _string(inline_src.get("press_mode")).lower()
    if press_mode not in ("tap", "hold"):
        press_mode = "tap"
    seq = []
    for step in (inline_src.get("sequence") or []) if isinstance(inline_src.get("sequence"), list) else []:
        ns = normalize_dodge_step(step)
        if ns:
            seq.append(ns)
    default = make_default_dodge_inline()
    inline = {
        "action_key": _string(inline_src.get("action_key")).upper(),
        "action_label": _string(inline_src.get("action_label")),
        "press_mode": press_mode,
        "hold_ms": _coerce_int(inline_src.get("hold_ms"), default["hold_ms"], 0, 10000),
        "press_count": _coerce_int(inline_src.get("press_count"), 1, 1, 20),
        "sequence": seq,
        "delay_ms": _coerce_int(inline_src.get("delay_ms"), 0, 0, 60000),
        "lead_ms": _coerce_int(inline_src.get("lead_ms"), default["lead_ms"], 0, 60000),
        "cooldown_s": _coerce_float(inline_src.get("cooldown_s"), 3.0, 0.0, 600.0),
        "direction": _normalize_dodge_direction(inline_src.get("direction")),
        "move_ms": _coerce_int(inline_src.get("move_ms"), default["move_ms"], 80, 6000),
        "fallback_direction": _normalize_cam_direction(
            inline_src.get("fallback_direction"), "back"),
        "exit_margin_m": _coerce_float(inline_src.get("exit_margin_m"), 6.0, 1.0, 40.0),
        "arrive_m": _coerce_float(inline_src.get("arrive_m"), 2.0, 0.5, 30.0),
        "geometry": normalize_geometry(inline_src.get("geometry")),
    }
    return {
        "enabled": _coerce_bool(src.get("enabled"), False),
        "linkage_id": _string(src.get("linkage_id")),
        "inline": inline,
    }


def make_default_mechanic() -> Dict[str, Any]:
    return {
        "id": _new_id("mech"),
        "name": "新机制",
        "kind": "",              # free UI token (red_stack / purple_delay / ...)
        "enabled": True,
        "notes": "",
        "color": "",
        "phase_ids": [],         # [] = active in all phases
        "detect": {
            "skill_ids": [],          # OR-matched vs boss cast skill_id (SkillTable id)
            "buff_ids": [],           # OR-matched vs new buff base_ids (BuffComp/self)
            "source": "any",          # any | boss | self — which buff list carries it
            "boss_base_id": 0,        # 0 = any boss
            "hp_pct": 0.0,            # >0: fire when boss HP% crosses below
            "time_into_phase_s": 0.0, # >0: phase-relative timer anchor
            "time_into_fight_s": 0.0, # >0: fight-relative timer anchor
            "repeat_interval_s": 0.0, # >0 with a time anchor: re-fire interval
            "event": "",              # "" | breaking | shield_broken | overdrive | mechanic key text
        },
        "alert": {
            "enabled": True,
            "banner_text": "",        # "" -> mechanic name
            "tts_text": "",           # "" -> mechanic name
            "alert_type": "both",     # sound | visual | both
            "countdown_s": 8.0,
            "pre_warn_s": 3.0,
            "cooldown_s": 5.0,        # dedup window per mechanic
            "sound": "boss_alert",
        },
        "dodge": {"enabled": False, "linkage_id": "", "inline": make_default_dodge_inline()},
    }


def normalize_mechanic(raw: Any) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    default = make_default_mechanic()
    d = src.get("detect") if isinstance(src.get("detect"), dict) else {}
    a = src.get("alert") if isinstance(src.get("alert"), dict) else {}
    alert_type = _string(a.get("alert_type")).lower()
    if alert_type not in ("sound", "visual", "both"):
        alert_type = "both"
    source = _string(d.get("source")).lower()
    if source not in ("any", "boss", "self"):
        source = "any"
    return {
        "id": _string(src.get("id")) or default["id"],
        "name": _string(src.get("name")) or default["name"],
        "kind": _string(src.get("kind")),
        "enabled": _coerce_bool(src.get("enabled"), True),
        "notes": _string(src.get("notes")),
        "color": _string(src.get("color")),
        "phase_ids": _coerce_str_list(src.get("phase_ids")),
        "detect": {
            "skill_ids": _coerce_id_list(d.get("skill_ids")),
            "buff_ids": _coerce_id_list(d.get("buff_ids")),
            "source": source,
            "boss_base_id": _coerce_int(d.get("boss_base_id"), 0, 0),
            "hp_pct": _coerce_float(d.get("hp_pct"), 0.0, 0.0, 100.0),
            "time_into_phase_s": _coerce_float(d.get("time_into_phase_s"), 0.0, 0.0, 86400.0),
            "time_into_fight_s": _coerce_float(d.get("time_into_fight_s"), 0.0, 0.0, 86400.0),
            "repeat_interval_s": _coerce_float(d.get("repeat_interval_s"), 0.0, 0.0, 86400.0),
            "event": _string(d.get("event")),
        },
        "alert": {
            "enabled": _coerce_bool(a.get("enabled"), True),
            "banner_text": _string(a.get("banner_text")),
            "tts_text": _string(a.get("tts_text")),
            "alert_type": alert_type,
            "countdown_s": _coerce_float(a.get("countdown_s"), 8.0, 0.0, 600.0),
            "pre_warn_s": _coerce_float(a.get("pre_warn_s"), 3.0, 0.0, 600.0),
            "cooldown_s": _coerce_float(a.get("cooldown_s"), 5.0, 0.0, 600.0),
            "sound": _string(a.get("sound")) or "boss_alert",
        },
        "dodge": normalize_dodge(src.get("dodge")),
    }


def make_default_enrage(time_s: int = 0) -> Dict[str, Any]:
    return {
        "time_s": _coerce_int(time_s, 0, 0, 86400),
        "anchor": "fight",            # fight | phase
        "phase_id": "",               # anchor=phase: countdown starts on phase enter
        "warn_threshold_s": 60,       # ID-plate amber tier
        "urgent_threshold_s": 30,     # ID-plate red-pulse tier
        "tts_milestones": [60, 30, 10],
    }


def normalize_enrage(raw: Any, legacy_time_s: int = 0) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    anchor = _string(src.get("anchor")).lower()
    if anchor not in ("fight", "phase"):
        anchor = "fight"
    milestones: List[int] = []
    raw_ms = src.get("tts_milestones")
    if isinstance(raw_ms, (list, tuple)):
        for v in raw_ms:
            iv = _coerce_int(v, 0)
            if 0 < iv <= 3600 and iv not in milestones:
                milestones.append(iv)
    if not milestones and raw_ms is None:
        milestones = [60, 30, 10]
    time_s = _coerce_int(src.get("time_s"), 0, 0, 86400)
    if time_s <= 0:
        time_s = _coerce_int(legacy_time_s, 0, 0, 86400)
    return {
        "time_s": time_s,
        "anchor": anchor,
        "phase_id": _string(src.get("phase_id")),
        "warn_threshold_s": _coerce_int(src.get("warn_threshold_s"), 60, 0, 3600),
        "urgent_threshold_s": _coerce_int(src.get("urgent_threshold_s"), 30, 0, 3600),
        "tts_milestones": sorted(milestones, reverse=True)[:8],
    }


def make_default_phase_trigger() -> Dict[str, Any]:
    return {
        "type": "manual",   # manual | time | dps_total | hp_pct | breaking | buff_event | shield_broken | overdrive | extinction_pct | breaking_stage | boss_mechanic | boss_mechanic_family | boss_skill | boss_mechanic_skill | ultimate_skill
        "value": 0,
    }


def normalize_phase_trigger(raw: Any) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    trigger_type = _string(src.get("type")).lower()
    if trigger_type not in ("manual", "time", "dps_total", "hp_pct", "breaking", "buff_event",
                            "shield_broken", "overdrive", "extinction_pct", "breaking_stage",
                            "boss_mechanic", "boss_mechanic_family", "boss_skill",
                            "boss_mechanic_skill", "ultimate_skill"):
        trigger_type = "manual"
    return {
        "type": trigger_type,
        "value": _string(src.get("value")) if trigger_type in ("boss_mechanic", "boss_mechanic_family", "boss_skill", "boss_mechanic_skill", "ultimate_skill") else _coerce_float(src.get("value"), 0.0, 0.0),
    }


def make_default_phase(index: int = 1) -> Dict[str, Any]:
    return {
        "id": _new_id("phase"),
        "name": f"P{index}",
        "trigger": make_default_phase_trigger(),
        "timelines": [],
    }


def normalize_phase(raw: Any, fallback_index: int = 1) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    timelines = []
    for item in (src.get("timelines") or []) if isinstance(src.get("timelines"), list) else []:
        timelines.append(normalize_timeline(item))
    return {
        "id": _string(src.get("id")) or _new_id("phase"),
        "name": _string(src.get("name")) or f"P{fallback_index}",
        "trigger": normalize_phase_trigger(src.get("trigger")),
        "timelines": timelines,
    }


def make_default_profile(author_snapshot: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    author = _normalize_author(author_snapshot)
    return {
        "id": _new_id("boss"),
        "schema_version": BOSS_RAID_SCHEMA_VERSION,
        "profile_name": "New Boss Raid",
        "description": "",
        "boss_total_hp": 0,
        "enrage_time_s": 600,
        "enrage": make_default_enrage(600),
        "simple_mode": True,
        "target_name_pattern": "",
        "dungeon_id": 0,
        "difficulty": "",
        "phases": [make_default_phase(1)],
        "mechanics": [],
        "source": "local",
        "remote_id": None,
        "created_at": _utc_now_iso(),
        "updated_at": _utc_now_iso(),
        "author_snapshot": author,
    }


def _normalize_author(author: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    a = author if isinstance(author, dict) else {}
    return {
        "player_uid": _string(a.get("player_uid") or a.get("uid")),
        "player_name": _string(a.get("player_name") or a.get("name")),
        "profession_id": _coerce_int(a.get("profession_id"), 0, 0),
        "profession_name": _string(a.get("profession_name") or a.get("profession")),
    }


def normalize_profile(raw: Any, author_snapshot: Optional[Dict[str, Any]] = None,
                       source: Optional[str] = None) -> Dict[str, Any]:
    base = raw if isinstance(raw, dict) else {}
    default = make_default_profile(author_snapshot)
    profile_source = _string(source or base.get("source") or "local").lower()
    if profile_source not in ("local", "downloaded", "uploaded"):
        profile_source = "local"
    author = _normalize_author(base.get("author_snapshot") or author_snapshot)
    phases = []
    for idx, item in enumerate(base.get("phases", []) or [], start=1):
        phases.append(normalize_phase(item, idx))
    if not phases:
        phases = [make_default_phase(1)]
    enrage_time_s = _coerce_int(base.get("enrage_time_s"), 600, 0, 86400)
    mechanics = []
    raw_mechs = base.get("mechanics")
    if isinstance(raw_mechs, list):
        for item in raw_mechs:
            if isinstance(item, dict):
                mechanics.append(normalize_mechanic(item))
    return {
        "id": _string(base.get("id")) or default["id"],
        "schema_version": BOSS_RAID_SCHEMA_VERSION,
        "profile_name": _string(base.get("profile_name")) or default["profile_name"],
        "description": _string(base.get("description")),
        "boss_total_hp": _coerce_int(base.get("boss_total_hp"), 0, 0),
        "enrage_time_s": enrage_time_s,
        "enrage": normalize_enrage(base.get("enrage"), enrage_time_s),
        "simple_mode": _coerce_bool(base.get("simple_mode"), True),
        "target_name_pattern": _string(base.get("target_name_pattern")),
        "dungeon_id": _coerce_int(base.get("dungeon_id"), 0, 0),
        "difficulty": _string(base.get("difficulty")),
        "phases": phases,
        "mechanics": mechanics,
        "source": profile_source,
        "remote_id": _string(base.get("remote_id")) or None,
        "created_at": _string(base.get("created_at")) or default["created_at"],
        # 保留原 updated_at — 全量 normalize 不算"修改"; 真改动点(upsert/机制增删绑)负责盖章
        "updated_at": _string(base.get("updated_at")) or _utc_now_iso(),
        "author_snapshot": author,
    }


# ═══════════════════════════════════════════════
#  Config load / save helpers
# ═══════════════════════════════════════════════

def default_boss_raid_config() -> Dict[str, Any]:
    return {
        "enabled": False,
        "active_profile_id": "",
        "server_url": DEFAULT_BOSS_RAID_SERVER_URL,
        "profiles": [],
        "last_remote_search": {
            "query": {"q": "", "page": 1, "page_size": 20},
            "results": [],
            "error": "",
            "fetched_at": "",
        },
    }


def normalize_boss_raid_config(raw: Any,
                                state_snapshot: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    author = _normalize_author(state_snapshot)
    result = default_boss_raid_config()
    result["enabled"] = _coerce_bool(src.get("enabled"), False)
    result["active_profile_id"] = _string(src.get("active_profile_id"))
    result["server_url"] = _string(src.get("server_url")) or DEFAULT_BOSS_RAID_SERVER_URL
    profiles = []
    for item in (src.get("profiles", []) if isinstance(src.get("profiles"), list) else []):
        profiles.append(normalize_profile(item, author_snapshot=author))
    result["profiles"] = profiles
    if result["active_profile_id"] and not any(
        p["id"] == result["active_profile_id"] for p in profiles
    ):
        result["active_profile_id"] = ""
    if not result["active_profile_id"] and profiles:
        result["active_profile_id"] = profiles[0]["id"]
    search = src.get("last_remote_search") if isinstance(src.get("last_remote_search"), dict) else {}
    query = search.get("query") if isinstance(search.get("query"), dict) else {}
    result["last_remote_search"] = {
        "query": {
            "q": _string(query.get("q")),
            "page": _coerce_int(query.get("page"), 1, 1),
            "page_size": _coerce_int(query.get("page_size"), 20, 1, 100),
        },
        "results": list(search.get("results") or []),
        "error": _string(search.get("error")),
        "fetched_at": _string(search.get("fetched_at")),
    }
    return result


def _seed_from_assets(config: Dict[str, Any]) -> bool:
    """首次启动: 把 assets/boss_raids/ 下的 *_机制示例.json 自动导入 (仅在0个档案时)。"""
    if config.get("profiles"):
        return False
    try:
        assets_dir = os.path.join(BASE_DIR, "assets", "boss_raids")
        if not os.path.isdir(assets_dir):
            return False
        added = False
        for fn in sorted(os.listdir(assets_dir)):
            if not fn.endswith(".json") or not fn[0].isdigit():
                continue
            path = os.path.join(assets_dir, fn)
            try:
                profile = import_profile_from_path(path)
                config.setdefault("profiles", []).append(profile)
                added = True
            except Exception:
                continue
        if added and not config.get("active_profile_id"):
            config["active_profile_id"] = config["profiles"][0]["id"]
        return added
    except Exception:
        return False


def load_boss_raid_config(settings, state_snapshot=None) -> Dict[str, Any]:
    config = normalize_boss_raid_config(settings.get("boss_raid", {}),
                                         state_snapshot=state_snapshot)
    if _seed_from_assets(config):
        settings.set("boss_raid", config)
        try:
            settings.save()
        except Exception:
            pass
    return config


def save_boss_raid_config(settings, config: Dict[str, Any]) -> Dict[str, Any]:
    normalized = normalize_boss_raid_config(config)
    settings.set("boss_raid", normalized)
    settings.save()
    return normalized


def find_profile(config: Dict[str, Any], profile_id: str) -> Optional[Dict[str, Any]]:
    pid = _string(profile_id)
    if not pid:
        return None
    for p in config.get("profiles", []) or []:
        if _string(p.get("id")) == pid:
            return p
    return None


def active_profile(config: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    return find_profile(config, config.get("active_profile_id", ""))


def upsert_profile(config: Dict[str, Any], profile: Dict[str, Any],
                    activate: bool = False) -> Dict[str, Any]:
    profile["updated_at"] = _utc_now_iso()
    profiles = list(config.get("profiles", []) or [])
    replaced = False
    for idx, existing in enumerate(profiles):
        if _string(existing.get("id")) == _string(profile.get("id")):
            profiles[idx] = profile
            replaced = True
            break
    if not replaced:
        profiles.append(profile)
    config["profiles"] = profiles
    if activate or not _string(config.get("active_profile_id")):
        config["active_profile_id"] = profile["id"]
    return config


def delete_profile(config: Dict[str, Any], profile_id: str) -> Dict[str, Any]:
    pid = _string(profile_id)
    profiles = [p for p in list(config.get("profiles", []) or []) if _string(p.get("id")) != pid]
    config["profiles"] = profiles
    if _string(config.get("active_profile_id")) == pid:
        config["active_profile_id"] = profiles[0]["id"] if profiles else ""
    return config


def _reissue_profile_ids(profile: Dict[str, Any]) -> None:
    """Re-id phases/timelines/mechanics in place, remapping the phase-id
    references carried by mechanics[].phase_ids and enrage.phase_id."""
    id_map: Dict[str, str] = {}
    for phase in profile.get("phases", []) or []:
        old = _string(phase.get("id"))
        phase["id"] = _new_id("phase")
        if old:
            id_map[old] = phase["id"]
        for tl in phase.get("timelines", []) or []:
            tl["id"] = _new_id("tl")
    for mech in profile.get("mechanics", []) or []:
        if not isinstance(mech, dict):
            continue
        mech["id"] = _new_id("mech")
        mech["phase_ids"] = [id_map.get(_string(p), _string(p))
                             for p in (mech.get("phase_ids") or []) if _string(p)]
    enrage = profile.get("enrage")
    if isinstance(enrage, dict):
        pid = _string(enrage.get("phase_id"))
        if pid:
            enrage["phase_id"] = id_map.get(pid, pid)


def clone_profile(config: Dict[str, Any], profile_id: str,
                  author_snapshot=None) -> Optional[Dict[str, Any]]:
    profile = find_profile(config, profile_id)
    if not profile:
        return None
    cloned = copy.deepcopy(profile)
    cloned["id"] = _new_id("boss")
    cloned["profile_name"] = f'{cloned.get("profile_name") or "Profile"} Copy'
    cloned["source"] = "local"
    cloned["remote_id"] = None
    cloned["created_at"] = _utc_now_iso()
    cloned["updated_at"] = _utc_now_iso()
    if author_snapshot:
        cloned["author_snapshot"] = _normalize_author(author_snapshot)
    _reissue_profile_ids(cloned)
    upsert_profile(config, normalize_profile(cloned, author_snapshot=author_snapshot),
                   activate=False)
    return cloned


def summarize_profile(profile: Dict[str, Any]) -> Dict[str, Any]:
    phases = list(profile.get("phases", []) or [])
    tl_count = sum(len(p.get("timelines") or []) for p in phases)
    return {
        "id": profile.get("id"),
        "profile_name": profile.get("profile_name"),
        "description": profile.get("description"),
        "boss_total_hp": profile.get("boss_total_hp", 0),
        "enrage_time_s": profile.get("enrage_time_s", 0),
        "simple_mode": profile.get("simple_mode", True),
        "phase_count": len(phases),
        "timeline_count": tl_count,
        "mechanic_count": len(profile.get("mechanics") or []),
        "dungeon_id": profile.get("dungeon_id", 0),
        "difficulty": profile.get("difficulty", ""),
        "source": profile.get("source", "local"),
        "remote_id": profile.get("remote_id"),
        "updated_at": profile.get("updated_at", ""),
    }


def find_mechanic(profile: Dict[str, Any], mechanic_id: str) -> Optional[Dict[str, Any]]:
    mid = _string(mechanic_id)
    if not mid:
        return None
    for mech in profile.get("mechanics", []) or []:
        if isinstance(mech, dict) and _string(mech.get("id")) == mid:
            return mech
    return None


def bind_mechanic_skill(profile: Dict[str, Any], mechanic_id: str, skill_id: Any) -> bool:
    """Append a skill id to a mechanic's detect.skill_ids (in place)."""
    mech = find_mechanic(profile, mechanic_id)
    sid = _coerce_int(skill_id, 0)
    if not mech or sid <= 0:
        return False
    detect = mech.setdefault("detect", {})
    ids = list(detect.get("skill_ids") or [])
    if sid in ids:
        return False
    ids.append(sid)
    detect["skill_ids"] = sorted(ids)
    return True


def unbind_mechanic_skill(profile: Dict[str, Any], mechanic_id: str, skill_id: Any) -> bool:
    mech = find_mechanic(profile, mechanic_id)
    sid = _coerce_int(skill_id, 0)
    if not mech or sid <= 0:
        return False
    detect = mech.setdefault("detect", {})
    ids = [i for i in (detect.get("skill_ids") or []) if _coerce_int(i, 0) != sid]
    buff_ids = [i for i in (detect.get("buff_ids") or []) if _coerce_int(i, 0) != sid]
    changed = (len(ids) != len(detect.get("skill_ids") or [])
               or len(buff_ids) != len(detect.get("buff_ids") or []))
    detect["skill_ids"] = ids
    detect["buff_ids"] = buff_ids
    return changed


def build_boss_raid_state(config: Dict[str, Any],
                          engine_status: Optional[Dict[str, Any]] = None,
                          upload_auth: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    ap = active_profile(config)
    return {
        "enabled": _coerce_bool(config.get("enabled"), False),
        "active_profile_id": _string(config.get("active_profile_id")),
        "active_profile_name": _string((ap or {}).get("profile_name")),
        "profiles": [summarize_profile(p) for p in list(config.get("profiles", []) or [])],
        "profiles_full": copy.deepcopy(list(config.get("profiles", []) or [])),
        "active_profile": copy.deepcopy(ap) if ap else None,
        "local_profile_count": len(list(config.get("profiles", []) or [])),
        "server_url": _string(config.get("server_url")) or DEFAULT_BOSS_RAID_SERVER_URL,
        "upload_auth": upload_auth or {},
        "last_remote_search": copy.deepcopy(config.get("last_remote_search") or {}),
        "runtime": copy.deepcopy(engine_status or {}),
    }


# ═══════════════════════════════════════════════
#  Export / Import
# ═══════════════════════════════════════════════

def export_profile_json(profile: Dict[str, Any]) -> str:
    payload = {"schema_version": BOSS_RAID_SCHEMA_VERSION, "profile": copy.deepcopy(profile)}
    return json.dumps(payload, ensure_ascii=False, indent=2)


def ensure_export_dir() -> str:
    os.makedirs(BOSS_RAID_EXPORT_DIR, exist_ok=True)
    return BOSS_RAID_EXPORT_DIR


def export_profile_to_default_path(profile: Dict[str, Any]) -> str:
    export_dir = ensure_export_dir()
    stamp = time.strftime("%Y%m%d_%H%M%S", time.localtime())
    filename = f'{_slugify_filename(profile.get("profile_name"))}_{stamp}.json'
    path = os.path.join(export_dir, filename)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(export_profile_json(profile))
    return path


def import_profile_from_path(path: str, author_snapshot=None) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    profile_data = data.get("profile") if isinstance(data, dict) else {}
    if not profile_data and isinstance(data, dict):
        profile_data = data
    profile = normalize_profile(profile_data, author_snapshot=author_snapshot, source="local")
    profile["id"] = _new_id("boss")
    profile["remote_id"] = None
    profile["source"] = "local"
    profile["created_at"] = _utc_now_iso()
    profile["updated_at"] = _utc_now_iso()
    _reissue_profile_ids(profile)
    return profile


# ═══════════════════════════════════════════════
#  Cloud Client
# ═══════════════════════════════════════════════

class BossRaidCloudClient:
    def __init__(self, base_url: str, timeout: float = 5.0):
        self._base_url = base_url.rstrip("/")
        self._timeout = timeout

    def _request(self, method: str, path: str, body=None, headers=None) -> Dict[str, Any]:
        url = f"{self._base_url}{path}"
        data = json.dumps(body).encode("utf-8") if body else None
        hdrs = {"Content-Type": "application/json", "Accept": "application/json"}
        if headers:
            hdrs.update(headers)
        req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self._timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            try:
                body_text = e.read().decode("utf-8")
                return json.loads(body_text)
            except Exception:
                return {"error": f"HTTP {e.code}: {e.reason}"}
        except Exception as e:
            return {"error": str(e)}

    def search(self, query: Dict[str, Any]) -> Dict[str, Any]:
        qs = urllib.parse.urlencode({k: v for k, v in query.items() if v})
        return self._request("GET", f"/api/boss-raids?{qs}")

    def get(self, remote_id: str) -> Dict[str, Any]:
        return self._request("GET", f"/api/boss-raids/{remote_id}")

    def upload(self, payload: Dict[str, Any], upload_token: str) -> Dict[str, Any]:
        headers = {"X-SAO-Upload-Token": upload_token or ""}
        return self._request("POST", "/api/boss-raids", body=payload, headers=headers)

    def issue_upload_token(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        return self._request("POST", "/api/upload-token/issue", body=payload)


# ═══════════════════════════════════════════════
#  Runtime Engine
# ═══════════════════════════════════════════════

class BossRaidEngine:
    """State-machine engine for Boss Raid timeline tracking.

    States: IDLE → RUNNING → COMPLETED / manually RESET
    Phases advance by manual hotkey, elapsed time, accumulated DPS, or boss HP%.
    """

    STATE_IDLE = "idle"
    STATE_RUNNING = "running"
    STATE_PAUSED = "paused"
    STATE_COMPLETED = "completed"

    def __init__(self, state_mgr, settings,
                 on_alert: Optional[Callable[[str, str], None]] = None,
                 on_sound: Optional[Callable[[str], None]] = None,
                 on_entity_update: Optional[Callable[[List[Dict[str, Any]]], None]] = None,
                 on_boss_action: Optional[Callable[[Dict[str, Any]], None]] = None,
                 on_mechanic: Optional[Callable[[Dict[str, Any]], None]] = None):
        """
        Args:
            state_mgr: GameStateManager instance
            settings: SettingsManager instance
            on_alert: callback(title, message) for visual alert
            on_sound: callback(sound_name) for playing sound
            on_entity_update: callback([entity_dict, ...]) for visual editor entity list
            on_boss_action: callback(action_record) forwarding the mem boss-action
                feed to the auto-key linkage (skill_id-accurate, gated by the host)
            on_mechanic: callback(mechanic_event) for the TTS/banner notification
                stack; when absent, mechanic fires degrade to on_alert/on_sound
        """
        self._state_mgr = state_mgr
        self._settings = settings
        self._on_alert = on_alert
        self._on_sound = on_sound
        self._on_entity_update_cb = on_entity_update
        self._on_boss_action_cb = on_boss_action
        self._on_mechanic = on_mechanic

        # ── profile mechanics runtime (indexes prebuilt at start/reset) ──
        self._mech_index_by_skill: Dict[int, List[Dict[str, Any]]] = {}
        self._mech_event_anchored: List[Dict[str, Any]] = []
        self._mech_time_anchored: List[Dict[str, Any]] = []
        self._mech_hp_anchored: List[Dict[str, Any]] = []
        self._mech_last_fire: Dict[str, float] = {}        # cooldown per mechanic id
        self._mech_fired_once: set = set()                 # one-shot hp anchors
        self._mech_time_fire_counts: Dict[Tuple, int] = {}  # time anchors (repeat)
        self._mech_prev_hp_pct: float = 100.0
        self._mech_countdowns: List[Dict[str, Any]] = []   # {id, name, color, ends_at, ...}
        self._mech_countdowns_push: List[Dict[str, Any]] = []
        self._mech_countdown_sig: Tuple = ()
        self._last_mechanic_event: Dict[str, Any] = {}
        self._unbound_skills: Dict[int, Dict[str, Any]] = {}   # binding inbox (≤64)
        self._enrage_anchor_ts: float = 0.0
        self._enrage_milestones_fired: set = set()
        self._runtime_enrage_total_s: float = 0.0
        self._runtime_enrage_end_ts: float = 0.0
        self._runtime_enrage_source: str = ""
        self._pending_mech_forwards: List[Dict[str, Any]] = []
        # last self-buff id snapshot for edge detect; None = no snapshot yet
        # (an empty set is a VALID snapshot — a player with zero buffs must
        # still trigger on the first new point-name buff)
        self._self_buff_prev: Optional[set] = None

        # ── memory-driven boss action state (cast_skill_id edge feed) ──
        # observed skills per boss: base_id -> {skill_id -> {name, count, last_cast_duration_ms, last_ts}}
        self._observed_boss_skills: Dict[int, Dict[int, Dict[str, Any]]] = {}
        self._observed_boss_names: Dict[int, str] = {}   # base_id -> boss display name
        # persisted scene→boss→observation aggregate (survives restarts)
        self._skill_store = BossSkillStore()
        self._boss_base_id: int = 0          # base/template id of the tracked boss
        self._obs_prev_overdrive: bool = False   # state-onset memos (record-once)
        self._obs_prev_invincible: bool = False
        self._boss_cast_skill_id: int = 0
        self._boss_cast_skill_name: str = ""
        self._boss_cast_active: bool = False
        self._boss_cast_start_ts: float = 0.0
        self._boss_cast_duration_ms: int = 0
        self._boss_stun: bool = False
        self._self_dead: bool = False
        # rising-edge memo for offensive windows (avoid level-triggered spam)
        self._mem_prev_overdrive: bool = False
        self._mem_prev_stun: bool = False
        self._mem_prev_breaking: int = -1
        # TCP-mode boss-skill detection (diff monster.buff_list for new base_ids).
        # Memory is authoritative in hybrid; TCP only drives detection in pure-TCP
        # mode, gated by "no memory action seen in MEM_PRIORITY_WINDOW seconds".
        self._tcp_prev_buff_ids: Dict[int, set] = {}   # uuid -> set(buff base_id)
        self._last_mem_boss_action_ts: float = 0.0

        self._lock = threading.Lock()
        self._state = self.STATE_IDLE
        self._profile: Optional[Dict[str, Any]] = None

        # Timing
        self._start_time: float = 0.0
        self._phase_start_time: float = 0.0
        self._current_phase_idx: int = 0

        # DPS tracking
        self._total_damage: int = 0
        self._damage_start_time: float = 0.0

        # Timeline trigger tracking — set of (phase_idx, timeline_id, fire_count)
        self._fired_timelines: Dict[Tuple[int, str], int] = {}

        # ── Real-time boss monster tracking ──
        self._boss_uuid: int = 0              # UUID of the tracked boss monster
        self._boss_hp: int = 0                # Real HP from packets
        self._boss_max_hp: int = 0            # Real MaxHP from packets
        self._boss_shield_pct: float = 0.0    # Shield remaining %
        self._boss_shield_active: bool = False
        self._boss_breaking_stage: int = 0
        self._boss_extinction_pct: float = 0.0  # Breaking bar %
        self._boss_in_overdrive: bool = False
        self._boss_invincible: bool = False
        self._last_boss_event: Dict[str, Any] = {}
        self._last_boss_mechanic_key: str = ""
        self._last_boss_mechanic_label: str = ""
        self._last_boss_trigger_family: str = ""
        self._immune_streak: int = 0          # Consecutive immune hits for invincibility detection
        self._immune_window_start: float = 0.0
        self._last_monster_data: Optional[Dict[str, Any]] = None

        # ── Multi-entity tracking (boss + enemies) ──
        # entities: uuid → {uuid, name, role, hp, max_hp, damage_dealt, first_seen, last_seen, ...}
        # role: 'boss' | 'enemy' | 'unknown'
        self._entities: Dict[int, Dict[str, Any]] = {}  # all tracked monsters
        self._entity_order: List[int] = []               # UUIDs in order of first attack
        self._boss_manually_set: bool = False             # user manually pinned the boss
        self._on_entity_update: Optional[Callable] = None # callback for visual editor

        # v3.1.7 round 16: entity-update callback is throttled to 10 Hz max.
        # `on_damage_event` previously rebuilt + fired the full entity list
        # on every damage event (500-1000/sec under heavy combat). Per-event
        # firing now just flips this dirty bit; `_run_loop` drains it at
        # ≤10 Hz (matches typical UI cadence) so the heavy combat path
        # avoids 14-field dict construction per event.
        self._entity_update_dirty: bool = False
        self._last_entity_update_emit_at: float = 0.0
        self.ENTITY_UPDATE_MIN_INTERVAL_S: float = 0.10

        # Thread
        self._running = False
        self._thread: Optional[threading.Thread] = None

    # ── Public API ──

    def start(self, profile: Optional[Dict[str, Any]] = None):
        """Start (or restart) a boss raid with the given profile."""
        with self._lock:
            if profile:
                self._profile = normalize_profile(copy.deepcopy(profile))
            if not self._profile:
                return
            self._state = self.STATE_RUNNING
            now = time.time()
            self._start_time = now
            self._phase_start_time = now
            self._current_phase_idx = 0
            self._total_damage = 0
            self._damage_start_time = now
            self._fired_timelines.clear()
            # Reset boss tracking
            self._boss_uuid = 0
            self._boss_hp = 0
            self._boss_max_hp = 0
            self._boss_shield_pct = 0.0
            self._boss_shield_active = False
            self._boss_breaking_stage = 0
            self._boss_extinction_pct = 0.0
            self._boss_in_overdrive = False
            self._boss_invincible = False
            self._immune_streak = 0
            self._clear_last_boss_mechanic_locked()
            self._last_monster_data = None
            # Reset multi-entity tracking
            self._entities.clear()
            self._entity_order.clear()
            self._boss_manually_set = False
            self._rebuild_mechanic_index_locked()
            self._maybe_anchor_enrage_locked()

        if not self._running:
            self._running = True
            self._thread = threading.Thread(target=self._run_loop, daemon=True,
                                            name="boss_raid_engine")
            self._thread.start()

        self._fire_alert("Boss Raid", f"▶ {self._profile['profile_name']} — START")
        self._fire_sound("boss_phase")

    def stop(self):
        """Stop the engine and reset state."""
        self._running = False
        thread = self._thread
        if thread and thread.is_alive():
            thread.join(timeout=1.0)
        self._thread = None
        with self._lock:
            self._state = self.STATE_IDLE
            self._total_damage = 0
            self._boss_uuid = 0
            self._boss_hp = 0
            self._boss_max_hp = 0
            self._boss_invincible = False
            self._immune_streak = 0
            self._runtime_enrage_total_s = 0.0
            self._runtime_enrage_end_ts = 0.0
            self._runtime_enrage_source = ""
            self._clear_last_boss_mechanic_locked()
            self._entities.clear()
            self._entity_order.clear()
            self._boss_manually_set = False
        self._skill_store.save()   # flush observed skills/mechanics to disk
        self._push_game_state_clear()

    def next_phase(self):
        """Manually advance to next phase."""
        with self._lock:
            if self._state != self.STATE_RUNNING:
                return
            self._advance_phase()

    def reset(self):
        """Reset to idle without stopping the engine thread."""
        with self._lock:
            self._state = self.STATE_IDLE
            self._total_damage = 0
            self._current_phase_idx = 0
            self._fired_timelines.clear()
            self._boss_uuid = 0
            self._boss_hp = 0
            self._boss_max_hp = 0
            self._boss_shield_active = False
            self._boss_shield_pct = 0.0
            self._boss_breaking_stage = 0
            self._boss_extinction_pct = 0.0
            self._boss_in_overdrive = False
            self._boss_invincible = False
            self._immune_streak = 0
            self._clear_mem_cast_state_locked()
            self._clear_last_boss_mechanic_locked()
            self._last_monster_data = None
            self._entities.clear()
            self._entity_order.clear()
            self._boss_manually_set = False
            self._rebuild_mechanic_index_locked()
        self._skill_store.save()   # flush observed skills/mechanics to disk
        self._push_game_state_clear()

    # ── Entity role management (for visual editor) ──

    def _clear_last_boss_mechanic_locked(self):
        self._last_boss_event = {}
        self._last_boss_mechanic_key = ""
        self._last_boss_mechanic_label = ""
        self._last_boss_trigger_family = ""

    def _clear_mem_cast_state_locked(self):
        self._boss_cast_skill_id = 0
        self._boss_cast_skill_name = ""
        self._boss_cast_active = False
        self._boss_cast_start_ts = 0.0
        self._boss_cast_duration_ms = 0
        self._boss_stun = False
        self._mem_prev_overdrive = False
        self._mem_prev_stun = False
        self._mem_prev_breaking = -1
        self._tcp_prev_buff_ids.clear()
        self._obs_prev_overdrive = False
        self._obs_prev_invincible = False

    def set_entity_role(self, uuid: int, role: str):
        """Set entity role: 'boss' or 'enemy'. Called from visual editor overlay.

        When a user explicitly marks a UUID as 'boss', the old boss (if any)
        is demoted to 'enemy', and all future boss-tracking fields update
        to the newly-designated boss.
        """
        uuid = int(uuid or 0)
        if not uuid or role not in ('boss', 'enemy'):
            return
        with self._lock:
            if uuid not in self._entities:
                return
            if role == 'boss':
                # Demote current boss to enemy
                for u, ent in self._entities.items():
                    if ent['role'] == 'boss' and u != uuid:
                        ent['role'] = 'enemy'
                self._entities[uuid]['role'] = 'boss'
                self._boss_uuid = uuid
                self._boss_manually_set = True
                # Sync HP fields from entity
                ent = self._entities[uuid]
                if ent.get('max_hp', 0) > 0:
                    self._boss_hp = ent['hp']
                    self._boss_max_hp = ent['max_hp']
                self._boss_shield_active = ent.get('shield_active', False)
                self._boss_shield_pct = ent.get('shield_pct', 0.0)
                self._boss_breaking_stage = ent.get('breaking_stage', 0)
                self._boss_extinction_pct = ent.get('extinction_pct', 0.0)
                self._boss_in_overdrive = ent.get('in_overdrive', False)
            else:
                self._entities[uuid]['role'] = 'enemy'
                if self._boss_uuid == uuid:
                    self._boss_uuid = 0
                    self._boss_manually_set = False
            self._fire_entity_update_locked()

    def get_entities(self) -> List[Dict[str, Any]]:
        """Return ordered list of tracked entities with their roles + stats."""
        with self._lock:
            return self._get_entities_locked()

    def _get_entities_locked(self) -> List[Dict[str, Any]]:
        """Build entity list (called under lock)."""
        result = []
        for uuid in self._entity_order:
            ent = self._entities.get(uuid)
            if not ent:
                continue
            result.append({
                'uuid': ent['uuid'],
                'name': ent.get('name', ''),
                'role': ent.get('role', 'unknown'),
                'hp': ent.get('hp', 0),
                'max_hp': ent.get('max_hp', 0),
                'hp_pct': round(ent['hp'] / max(1, ent['max_hp']), 4) if ent.get('max_hp', 0) > 0 else 0.0,
                'damage_dealt': ent.get('damage_dealt', 0),
                'hit_count': ent.get('hit_count', 0),
                'shield_active': ent.get('shield_active', False),
                'shield_pct': round(ent.get('shield_pct', 0.0), 4),
                'breaking_stage': ent.get('breaking_stage', 0),
                'extinction_pct': round(ent.get('extinction_pct', 0.0), 4),
                'in_overdrive': ent.get('in_overdrive', False),
                'is_boss': ent.get('role') == 'boss',
            })
        return result

    def _fire_entity_update_locked(self):
        """Notify visual editor of entity list change (call under lock)."""
        if self._on_entity_update_cb:
            entities = self._get_entities_locked()
            try:
                self._on_entity_update_cb(entities)
            except Exception:
                pass

    @_probe.decorate('boss.on_damage_event')
    def on_damage_event(self, event: Dict[str, Any]):
        """Called from packet_parser damage callback. Accumulates boss damage and detects invincibility.

        Multi-entity auto-detect logic:
        - First attacked monster UUID becomes the boss
        - Subsequent unique UUIDs become enemies (mechanic adds)
        - Users can override roles via the visual editor
        """
        if not event:
            return
        with self._lock:
            if self._state != self.STATE_RUNNING:
                return
            # D3: typed cython coercion replaces 5+ int()/bool()/.get() boxings.
            (target_is_monster, attacker_is_self,
             is_immune, is_absorbed, _ev_is_heal,
             damage_ll, target_uuid_ll, target_name) = (
                _CY_COMBAT.boss_raid_event_coerce(event)
            )
            damage = int(damage_ll)
            target_uuid = int(target_uuid_ll)

            if attacker_is_self and target_is_monster:
                now = time.time()

                # ── Track entity ──
                if target_uuid and target_uuid not in self._entities:
                    role = 'unknown'
                    if not self._boss_manually_set:
                        if self._boss_uuid == 0:
                            role = 'boss'  # First attacked = boss
                        else:
                            role = 'enemy'  # Subsequent = enemy (mechanic add)
                    self._entities[target_uuid] = {
                        'uuid': target_uuid,
                        'name': target_name,
                        'role': role,
                        'hp': 0, 'max_hp': 0,
                        'damage_dealt': 0,
                        'hit_count': 0,
                        'first_seen': now, 'last_seen': now,
                        'shield_active': False, 'shield_pct': 0.0,
                        'breaking_stage': 0, 'extinction_pct': 0.0,
                        'in_overdrive': False,
                    }
                    self._entity_order.append(target_uuid)

                # Auto-detect boss: first monster we hit
                if self._boss_uuid == 0 and target_uuid:
                    self._boss_uuid = target_uuid
                    if target_uuid in self._entities:
                        self._entities[target_uuid]['role'] = 'boss'

                # Update entity damage tracking
                if target_uuid in self._entities:
                    ent = self._entities[target_uuid]
                    ent['last_seen'] = now
                    if not (is_immune or is_absorbed) and not _ev_is_heal:
                        ent['damage_dealt'] += damage
                        ent['hit_count'] += 1
                    if not ent.get('name'):
                        ent['name'] = target_name

                if is_immune or is_absorbed:
                    # Invincibility detection: consecutive immune/absorbed hits
                    if self._immune_streak == 0:
                        self._immune_window_start = now
                    self._immune_streak += 1
                    if self._immune_streak >= 3 and (now - self._immune_window_start) < 5.0:
                        if not self._boss_invincible:
                            self._boss_invincible = True
                else:
                    # Normal damage — clear invincibility and accumulate
                    if self._boss_invincible:
                        self._boss_invincible = False
                    self._immune_streak = 0
                    if not _ev_is_heal:
                        self._total_damage += damage

                # Notify visual editor of entity list change — throttled to
                # ≤10 Hz via `_entity_update_dirty` + `_run_loop` flush.
                self._entity_update_dirty = True

    @_probe.decorate('boss.on_monster_update')
    def on_monster_update(self, monster_data: Dict[str, Any]):
        """Called from packet_parser monster update callback. Updates real boss HP/shield/breaking.

        Also updates multi-entity tracking for all known monsters.
        """
        if not monster_data:
            return
        tcp_forwards: List[Dict[str, Any]] = []
        with self._lock:
            if self._state != self.STATE_RUNNING:
                # Still track even when idle so we can show info
                pass
            uuid = _coerce_int(monster_data.get("uuid"), 0, minimum=0)
            hp = _coerce_int(monster_data.get("hp"), 0, minimum=0)
            max_hp = _coerce_int(monster_data.get("max_hp"), 0, minimum=0)

            # ── Update multi-entity tracking ──
            if uuid and uuid in self._entities:
                ent = self._entities[uuid]
                if max_hp > 0:
                    ent['hp'] = hp
                    ent['max_hp'] = max_hp
                ent['shield_active'] = bool(monster_data.get('shield_active'))
                ent['shield_pct'] = _coerce_float(monster_data.get('shield_pct'), 0.0, minimum=0.0)
                ent['breaking_stage'] = _coerce_int(monster_data.get('breaking_stage'), 0)
                ent['extinction_pct'] = _coerce_float(monster_data.get('extinction_pct'), 0.0, minimum=0.0)
                ent['in_overdrive'] = bool(monster_data.get('in_overdrive'))
                name = _string(monster_data.get('name', ''))
                if name and not ent.get('name'):
                    ent['name'] = name

            # Auto-detect boss: biggest max_hp monster (if no boss yet)
            if self._boss_uuid == 0:
                if max_hp > 0:
                    self._boss_uuid = uuid
                    if uuid in self._entities:
                        self._entities[uuid]['role'] = 'boss'

            # ── Update boss-specific fields (for main tracking) ──
            if uuid != self._boss_uuid:
                return

            self._last_monster_data = monster_data
            now = time.time()
            self._update_runtime_enrage_from_buffs_locked(monster_data, now)
            self._boss_base_id = _coerce_int(monster_data.get("template_id"), 0, minimum=0) or self._boss_base_id
            if max_hp > 0:
                self._boss_hp = hp
                self._boss_max_hp = max_hp
            self._boss_breaking_stage = _coerce_int(monster_data.get("breaking_stage"), 0)
            self._boss_extinction_pct = _coerce_float(monster_data.get("extinction_pct"), 0.0, minimum=0.0)
            self._boss_in_overdrive = bool(monster_data.get("in_overdrive"))
            self._boss_shield_active = bool(monster_data.get("shield_active"))
            self._boss_shield_pct = _coerce_float(monster_data.get("shield_pct"), 0.0, minimum=0.0)
            self._observe_state_transitions_locked()

            # pure-TCP boss-skill detection (no-op while a memory feed is live)
            tcp_forwards = self._tcp_detect_boss_skills_locked(uuid, monster_data)

        for fwd in tcp_forwards:
            try:
                self._on_boss_action_cb(fwd)
            except Exception:
                pass
        self._dispatch_mech_forwards()

    def on_boss_event(self, event: Dict[str, Any]):
        """Called from packet_parser boss event callback. Handles buff-based phase triggers."""
        if not event:
            return
        with self._lock:
            event_type = event.get("event_type", 0)
            fact = event.get("combat_fact") if isinstance(event.get("combat_fact"), dict) else enrich_boss_event(event)
            boss_mechanic_key = _string(event.get("boss_mechanic_key") or fact.get("boss_mechanic_key"))
            boss_mechanic_label = _string(event.get("boss_mechanic_label") or fact.get("boss_mechanic_label"))
            trigger_family = _string(event.get("trigger_family") or fact.get("trigger_family"))
            skill_id = _coerce_int(event.get("skill_id") or fact.get("skill_id"), 0)
            skill_name = _string(event.get("skill_name") or fact.get("skill_name"))
            skill_category = _string(event.get("skill_category") or event.get("skill_kind") or fact.get("skill_category") or fact.get("skill_kind"))
            self._last_boss_event = dict(event or {})
            self._last_boss_event.setdefault("combat_fact", fact)
            self._last_boss_mechanic_key = boss_mechanic_key
            self._last_boss_mechanic_label = boss_mechanic_label
            self._last_boss_trigger_family = trigger_family
            # record the mechanic into the persisted aggregate (free combat too)
            self._record_boss_event_locked(event_type, boss_mechanic_label or boss_mechanic_key, trigger_family)

            if self._state != self.STATE_RUNNING:
                return

            # Check for breaking/buff_event/shield_broken phase triggers
            profile = self._profile or {}
            phases = list(profile.get("phases") or [])
            if self._current_phase_idx < len(phases) - 1:
                next_idx = self._current_phase_idx + 1
                next_phase = phases[next_idx]
                trigger = next_phase.get("trigger") or {}
                trigger_type = _string(trigger.get("type"))
                trigger_value = _coerce_int(trigger.get("value"), 0)

                if trigger_type == "breaking" and event_type == 58:  # EnterBreaking
                    self._advance_phase()
                elif trigger_type == "buff_event" and trigger_value == event_type:
                    self._advance_phase()
                elif trigger_type == "shield_broken" and event_type == 47:  # ShieldBroken
                    self._advance_phase()
                elif trigger_type == "overdrive" and event_type == 58:
                    # Overdrive is also signalled via entering breaking or special events
                    if self._boss_in_overdrive:
                        self._advance_phase()
                elif trigger_type == "boss_mechanic":
                    trigger_text = _string(trigger.get("value"))
                    if trigger_text and trigger_text in (str(event_type), boss_mechanic_key, boss_mechanic_label):
                        self._advance_phase()
                elif trigger_type == "boss_mechanic_family":
                    trigger_text = _string(trigger.get("value"))
                    if trigger_text and trigger_text == trigger_family:
                        self._advance_phase()
                elif trigger_type in ("boss_skill", "boss_mechanic_skill", "ultimate_skill"):
                    trigger_text = _string(trigger.get("value"))
                    expected_category = {
                        "boss_skill": "boss_skill",
                        "boss_mechanic_skill": "boss_mechanic_skill",
                        "ultimate_skill": "ultimate_skill",
                    }.get(trigger_type, trigger_type)
                    if skill_category == expected_category and (not trigger_text or trigger_text in (str(skill_id), skill_name, skill_category)):
                        self._advance_phase()
            self._match_mechanic_on_event_locked(event_type, boss_mechanic_key,
                                                 boss_mechanic_label, trigger_family)
            self._push_game_state_locked(time.time())
        self._dispatch_mech_forwards()

    # ── Memory-driven boss action feed (cast_skill_id edge) ──

    # Pure-TCP boss-skill detection is suppressed while a memory boss-action feed
    # has been seen within this many seconds (hybrid → memory is authoritative).
    MEM_PRIORITY_WINDOW = 3.0

    def on_mem_boss_action(self, action: Dict[str, Any]):
        """Consume a memory boss-action record (cast edge + state). Updates cast
        state, records observed skills, fires boss_skill phase triggers, pushes
        GameState, and forwards to the auto-key linkage with rising-edge flags.
        Runs even when not RUNNING so free-combat observation + offensive windows
        work (mirrors on_monster_update tracking while idle)."""
        if not action:
            return
        forward = None
        with self._lock:
            self._last_mem_boss_action_ts = time.time()   # memory feed is live
            forward = self._apply_boss_action_locked(action)
        if forward is not None:
            try:
                self._on_boss_action_cb(forward)
            except Exception:
                pass
        self._dispatch_mech_forwards()

    def _apply_boss_action_locked(self, action: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        """Shared body for memory + TCP boss actions (caller holds the lock).
        Returns the linkage-forward dict (or None). skill_id is the buff/skill
        base_id read from the data source (memory or TCP BuffInfoSync)."""
        base_id = _coerce_int(action.get("boss_base_id"), 0)
        if base_id:
            self._boss_base_id = int(base_id)
        skill_id = _coerce_int(action.get("skill_id"), 0)
        name = _string(action.get("skill_name"))
        edge = _string(action.get("cast_edge")).lower()
        dur = action.get("cast_duration_ms")

        # apply the boss's concurrent state FIRST (offensive-window rising edges,
        # memoized) so a skill recorded below gets the correct coincident tags.
        overdrive = bool(action.get("overdrive"))
        overdrive_edge = overdrive and not self._mem_prev_overdrive
        self._mem_prev_overdrive = overdrive
        self._boss_in_overdrive = overdrive

        stun = bool(action.get("stun"))
        stun_edge = stun and not self._mem_prev_stun
        self._mem_prev_stun = stun
        self._boss_stun = stun

        bs = action.get("breaking_stage")
        breaking_edge = False
        if isinstance(bs, int) and bs >= 0:
            breaking_edge = (bs != self._mem_prev_breaking)
            self._mem_prev_breaking = bs
            self._boss_breaking_stage = bs

        ext = action.get("extinction")
        if isinstance(ext, (int, float)):
            self._boss_extinction_pct = float(ext)

        mech_fwd = None
        if edge == "start":
            self._boss_cast_skill_id = skill_id
            # instant skills (counterattacks) have no skill id — label them so the
            # editor can still bind a reaction to "this boss's action".
            self._boss_cast_skill_name = name or ('动作/反击' if not skill_id else '')
            self._boss_cast_active = True
            self._boss_cast_start_ts = time.time()
            self._boss_cast_duration_ms = _coerce_int(dur, 0)
            self._record_observed_skill_locked(base_id, skill_id, self._boss_cast_skill_name, dur)
            boss_name = _string(action.get("boss_name"))
            if base_id and boss_name:
                self._observed_boss_names[int(base_id)] = boss_name
            mech_fwd = self._match_mechanic_on_cast_locked(
                int(base_id or self._boss_base_id), skill_id,
                _coerce_int(dur, 0), self._boss_cast_skill_name)
        elif edge == "end":
            self._boss_cast_active = False
        else:
            self._boss_cast_active = bool(action.get("cast_active"))
            if isinstance(dur, (int, float)) and dur:
                self._boss_cast_duration_ms = int(dur)

        # state-edge anchored mechanics (detect.event = breaking/overdrive/stun)
        for ev_name, ev_edge in (("breaking", breaking_edge),
                                 ("overdrive", overdrive_edge),
                                 ("stun", stun_edge)):
            if ev_edge:
                self._match_mechanic_on_state_edge_locked(ev_name)

        if self._state == self.STATE_RUNNING and edge == "start" and skill_id:
            self._maybe_advance_on_mem_skill_locked(skill_id, name)

        self._observe_state_transitions_locked()
        self._push_game_state_locked(time.time())

        forward = None
        if self._on_boss_action_cb:
            forward = dict(action)
            forward["breaking_edge"] = breaking_edge
            forward["overdrive_edge"] = overdrive_edge
            forward["stun_edge"] = stun_edge
            if mech_fwd:
                forward.update(mech_fwd)
        return forward

    def _tcp_detect_boss_skills_locked(self, uuid: int,
                                       monster_data: Dict[str, Any]) -> List[Dict[str, Any]]:
        """Pure-TCP equivalent of the memory buff-skill feed: a NEW base_id in the
        boss's BuffInfoSync-fed buff_list = a skill/mechanic cast. Suppressed while
        a memory feed is live (hybrid → memory is authoritative). Returns the
        linkage-forward dicts to dispatch outside the lock."""
        if time.time() - self._last_mem_boss_action_ts < self.MEM_PRIORITY_WINDOW:
            return []
        buff_list = monster_data.get("buff_list")
        if not isinstance(buff_list, list):
            return []
        cur: Dict[int, int] = {}
        for b in buff_list:
            if isinstance(b, dict):
                if _is_enrage_timer_buff(b):
                    continue
                bid = _coerce_int(b.get("buff_id"), 0)
                if bid > 0:
                    cur[bid] = _coerce_int(b.get("duration"), 0)
        prev = self._tcp_prev_buff_ids.get(uuid)
        self._tcp_prev_buff_ids[uuid] = set(cur)
        if prev is None:
            return []   # first sight → seed baseline, don't fire on persistent buffs
        new_ids = [bid for bid in cur if bid not in prev]
        if not new_ids:
            return []
        # longest-duration new buff is the primary cast (symmetric with memory)
        best = max(new_ids, key=lambda bid: cur[bid])
        base_id = _coerce_int(monster_data.get("template_id"), 0)
        action = {
            "boss_base_id": base_id,
            "boss_name": _string(monster_data.get("name")),
            "boss_uuid": uuid,
            "skill_id": best,
            "skill_name": "",   # name is an offline-table hint only; id is authoritative
            "cast_edge": "start",
            "cast_active": True,
            "cast_duration_ms": cur[best],
            "cast_duration_src": "tcp_buff",
            "overdrive": bool(monster_data.get("in_overdrive")),
            "stun": bool(monster_data.get("stunned")),
            "breaking_stage": _coerce_int(monster_data.get("breaking_stage"), -1),
            "extinction": float(monster_data.get("extinction_pct") or 0.0),
            "source": "tcp",
        }
        fwds: List[Dict[str, Any]] = []
        fwd = self._apply_boss_action_locked(action)
        if fwd is not None:
            fwds.append(fwd)
        # secondary simultaneous buffs: record + mechanic-match them too, so a
        # mechanic bound to a non-primary buff id is never missed on pure TCP.
        for bid in new_ids:
            if bid == best:
                continue
            self._record_observed_skill_locked(base_id, bid, "", cur[bid])
            mech_fwd = self._match_mechanic_on_cast_locked(base_id, bid, cur[bid], "")
            if mech_fwd and self._on_boss_action_cb:
                extra = {"boss_base_id": base_id, "boss_uuid": uuid,
                         "skill_id": bid, "cast_duration_ms": cur[bid],
                         "source": "tcp_mechanic"}
                extra.update(mech_fwd)
                fwds.append(extra)
        return fwds

    def _maybe_advance_on_mem_skill_locked(self, skill_id: int, skill_name: str):
        phases = list((self._profile or {}).get("phases") or [])
        if self._current_phase_idx >= len(phases) - 1:
            return
        trig = (phases[self._current_phase_idx + 1].get("trigger") or {})
        ttype = _string(trig.get("type"))
        if ttype in ("boss_skill", "boss_mechanic_skill", "ultimate_skill"):
            tval = _string(trig.get("value"))
            if not tval or tval in (str(skill_id), skill_name):
                self._advance_phase()

    def _record_observed_skill_locked(self, base_id: int, skill_id: int,
                                      name: str, dur: Any):
        if skill_id < 0:
            return  # skill_id 0 == an instant action (counterattack) with no id
        by_skill = self._observed_boss_skills.setdefault(int(base_id), {})
        rec = by_skill.get(skill_id)
        if rec is None:
            rec = {"skill_id": skill_id, "name": name or "", "count": 0,
                   "last_cast_duration_ms": None, "last_ts": 0.0}
            by_skill[skill_id] = rec
        rec["count"] += 1
        rec["last_ts"] = time.time()
        if name and not rec["name"]:
            rec["name"] = name
        if isinstance(dur, (int, float)) and dur > 0:
            rec["last_cast_duration_ms"] = int(dur)
        # persisted aggregate: scene → boss → skill, tagged with concurrent state
        if base_id:
            self._boss_base_id = int(base_id)
        self._observe_to_store_locked(
            int(skill_id or 0), name, KIND_SKILL,
            tags=self._derive_state_tags_locked(),
            duration_ms=int(dur) if isinstance(dur, (int, float)) and dur > 0 else None)

    # ── profile mechanics runtime ───────────────────────────────────────────────

    def _rebuild_mechanic_index_locked(self, preserve_runtime: bool = False):
        """Prebuild O(1) lookup structures from the active profile's mechanics
        (called from start/reset; never per tick). `preserve_runtime` keeps
        cooldowns / fire counters / countdown rows / binding inbox alive so an
        editor save can hot-apply mid-fight without resetting state."""
        self._mech_index_by_skill = {}
        self._mech_event_anchored = []
        self._mech_time_anchored = []
        self._mech_hp_anchored = []
        if not preserve_runtime:
            self._mech_last_fire.clear()
            self._mech_fired_once.clear()
            self._mech_time_fire_counts.clear()
            self._mech_prev_hp_pct = 100.0
            self._mech_countdowns = []
            self._mech_countdowns_push = []
            self._mech_countdown_sig = ()
            self._last_mechanic_event = {}
            self._unbound_skills.clear()
            self._enrage_anchor_ts = 0.0
            self._enrage_milestones_fired.clear()
            self._runtime_enrage_total_s = 0.0
            self._runtime_enrage_end_ts = 0.0
            self._runtime_enrage_source = ""
            self._pending_mech_forwards = []
            self._self_buff_prev = None
        profile = self._profile or {}
        for mech in profile.get("mechanics") or []:
            if not isinstance(mech, dict) or not mech.get("enabled", True):
                continue
            det = mech.get("detect") or {}
            for sid in list(det.get("skill_ids") or []) + list(det.get("buff_ids") or []):
                sid = _coerce_int(sid, 0)
                if sid > 0:
                    self._mech_index_by_skill.setdefault(sid, []).append(mech)
            if _string(det.get("event")):
                self._mech_event_anchored.append(mech)
            if _coerce_float(det.get("hp_pct"), 0.0) > 0:
                self._mech_hp_anchored.append(mech)
            if (_coerce_float(det.get("time_into_phase_s"), 0.0) > 0
                    or _coerce_float(det.get("time_into_fight_s"), 0.0) > 0):
                self._mech_time_anchored.append(mech)

    def _mechanic_phase_ok_locked(self, mech: Dict[str, Any]) -> bool:
        pids = mech.get("phase_ids") or []
        if not pids:
            return True
        phases = list((self._profile or {}).get("phases") or [])
        if self._current_phase_idx >= len(phases):
            return False
        cur_id = _string(phases[self._current_phase_idx].get("id"))
        return cur_id in {_string(p) for p in pids}

    def _maybe_anchor_enrage_locked(self):
        """Arm the enrage countdown when its anchor point is reached."""
        profile = self._profile or {}
        enrage = profile.get("enrage") if isinstance(profile.get("enrage"), dict) else {}
        anchor = _string(enrage.get("anchor")) or "fight"
        if anchor != "phase":
            self._enrage_anchor_ts = self._start_time
            return
        if self._enrage_anchor_ts > 0:
            return
        pid = _string(enrage.get("phase_id"))
        phases = list(profile.get("phases") or [])
        cur = phases[self._current_phase_idx] if self._current_phase_idx < len(phases) else {}
        if pid and _string(cur.get("id")) == pid:
            self._enrage_anchor_ts = time.time()
            self._enrage_milestones_fired.clear()

    def _server_now_ms_locked(self) -> float:
        try:
            state = getattr(self._state_mgr, "state", None)
            offset = float(getattr(state, "server_time_offset_ms", 0.0) or 0.0)
        except Exception:
            offset = 0.0
        return time.time() * 1000.0 + offset

    def _update_runtime_enrage_from_buffs_locked(self, monster_data: Dict[str, Any],
                                                 now: float) -> None:
        """Use live Boss hard-enrage timer buff when present.

        BuffInfoSync carries BeginTime + Duration for the hard-enrage timer; this
        is more accurate than profile fallback seconds and survives difficulty HP
        differences.  Absence in a later partial sync does not clear the clock.
        """
        buff_list = monster_data.get("buff_list")
        if not isinstance(buff_list, list):
            return
        server_now_ms = self._server_now_ms_locked()
        best: Optional[Tuple[float, float, int]] = None
        for buff in buff_list:
            if not _is_enrage_timer_buff(buff):
                continue
            duration_ms = _coerce_int(
                buff.get("duration") or buff.get("duration_ms"), 0, 0)
            if duration_ms <= 0:
                continue
            begin_ms = _coerce_int(
                buff.get("begin_time") or buff.get("begin_ms"), 0, 0)
            if begin_ms > 0:
                remaining_ms = begin_ms + duration_ms - server_now_ms
                # If server_time_offset is not ready yet, BeginTime can be in a
                # different clock domain.  Keep the timer useful by falling back
                # to the advertised duration instead of dropping the runtime
                # enrage signal entirely.
                if remaining_ms <= 0 or remaining_ms > duration_ms + 60000:
                    remaining_ms = float(duration_ms)
            else:
                remaining_ms = float(duration_ms)
            if remaining_ms <= 0:
                continue
            total_s = duration_ms / 1000.0
            remaining_s = remaining_ms / 1000.0
            bid = _coerce_int(buff.get("buff_id") or buff.get("id"), 0, 0)
            if best is None or remaining_s > best[1]:
                best = (total_s, remaining_s, bid)
        if best is None:
            return
        total_s, remaining_s, bid = best
        self._runtime_enrage_total_s = max(total_s, remaining_s)
        self._runtime_enrage_end_ts = now + remaining_s
        self._runtime_enrage_source = f"buff:{bid}" if bid else "buff"

    def _enrage_state_locked(self, now: float) -> Dict[str, Any]:
        """Unified enrage countdown state for tick + status + HUD tiers."""
        profile = self._profile or {}
        enrage = profile.get("enrage") if isinstance(profile.get("enrage"), dict) else {}
        time_s = _coerce_int(enrage.get("time_s"), 0, 0) \
            or _coerce_int(profile.get("enrage_time_s"), 0, 0)
        warn_s = _coerce_int(enrage.get("warn_threshold_s"), 60, 0)
        urgent_s = _coerce_int(enrage.get("urgent_threshold_s"), 30, 0)
        milestones = [m for m in (enrage.get("tts_milestones") or [])
                      if isinstance(m, int) and m > 0]
        live_remaining = max(0.0, self._runtime_enrage_end_ts - now) \
            if self._state == self.STATE_RUNNING else 0.0
        armed = False
        remaining = float(time_s)
        source = "profile"
        if live_remaining > 0:
            armed = True
            remaining = live_remaining
            time_s = int(round(max(self._runtime_enrage_total_s, live_remaining)))
            source = self._runtime_enrage_source or "buff"
        elif time_s > 0 and self._state == self.STATE_RUNNING:
            anchor = _string(enrage.get("anchor")) or "fight"
            anchor_ts = self._start_time if anchor != "phase" else self._enrage_anchor_ts
            if anchor_ts > 0:
                armed = True
                remaining = max(0.0, time_s - (now - anchor_ts))
        urgency = ""
        if armed and remaining > 0:
            if urgent_s > 0 and remaining <= urgent_s:
                urgency = "urgent"
            elif warn_s > 0 and remaining <= warn_s:
                urgency = "warn"
        return {
            "time_s": time_s,
            "armed": armed,
            "remaining_s": remaining,
            "urgency": urgency,
            "milestones": milestones,
            "source": source,
        }

    def _note_unbound_skill_locked(self, skill_id: int, name: str = "",
                                   dur: Any = None):
        """Binding inbox: a detected cast no mechanic is bound to (bounded 64)."""
        sid = int(skill_id or 0)
        if sid <= 0:
            return
        now = time.time()
        rec = self._unbound_skills.get(sid)
        if rec is None:
            if len(self._unbound_skills) >= 64:
                oldest = min(self._unbound_skills.values(), key=lambda r: r["last_ts"])
                self._unbound_skills.pop(int(oldest["skill_id"]), None)
            rec = {"skill_id": sid, "name": "", "count": 0, "first_ts": now,
                   "last_ts": 0.0, "boss_base_id": int(self._boss_base_id or 0),
                   "last_cast_duration_ms": 0}
            self._unbound_skills[sid] = rec
        rec["count"] += 1
        rec["last_ts"] = now
        if name and not rec["name"]:
            rec["name"] = _string(name)
        if isinstance(dur, (int, float)) and dur > 0:
            rec["last_cast_duration_ms"] = int(dur)

    def _match_mechanic_on_cast_locked(self, base_id: int, skill_id: int,
                                       cast_duration_ms: int,
                                       name: str = "",
                                       feed: str = "boss") -> Optional[Dict[str, Any]]:
        """Resolve a cast skill id / buff base_id to a profile mechanic; returns
        the linkage-forward extras dict (mechanic_id/dodge) or None. `feed` is the
        carrier the id came from ('boss' = boss buff/cast list, 'self' = the local
        player's buff list); a mechanic only matches when its detect.source is
        'any' or equals `feed`."""
        if skill_id <= 0 or self._profile is None:
            return None
        mechs = self._mech_index_by_skill.get(int(skill_id))
        if not mechs:
            if feed == "boss":
                self._note_unbound_skill_locked(skill_id, name, cast_duration_ms)
            return None
        if self._state != self.STATE_RUNNING:
            return None
        for mech in mechs:
            det = mech.get("detect") or {}
            src = _string(det.get("source")) or "any"
            if src != "any" and src != feed:
                continue
            m_base = _coerce_int(det.get("boss_base_id"), 0)
            # boss_base_id scopes the boss feed; the self feed carries no boss id
            if feed == "boss" and m_base and base_id and m_base != base_id:
                continue
            if not self._mechanic_phase_ok_locked(mech):
                continue
            return self._fire_mechanic_locked(mech, feed, cast_duration_ms, skill_id)
        return None

    def on_self_buff_change(self, buff_ids):
        """Feed the local player's current buff base_ids. New ids (since the last
        snapshot) are matched against self/any-source mechanics so point-named
        mechanics (你被点了分摊/分散/死刑) fire off YOUR buff list, which the boss
        feed never sees. Reuses the existing self_buffs snapshot — no new reads."""
        try:
            cur = set(int(b) for b in (buff_ids or []) if int(b) > 0)
        except Exception:
            return
        forwards: List[Dict[str, Any]] = []
        with self._lock:
            if self._state != self.STATE_RUNNING or self._profile is None:
                self._self_buff_prev = cur
                return
            prev = self._self_buff_prev
            new_ids = cur - prev if prev is not None else set()
            self._self_buff_prev = cur
            for bid in new_ids:
                if bid not in self._mech_index_by_skill:
                    continue
                fwd = self._match_mechanic_on_cast_locked(0, bid, 0, "", feed="self")
                if fwd:
                    self._queue_mech_forward_locked(fwd)
            if self._pending_mech_forwards:
                forwards = self._pending_mech_forwards
                self._pending_mech_forwards = []
        for fwd in forwards:
            try:
                self._on_boss_action_cb(fwd)
            except Exception:
                pass

    def _match_mechanic_on_state_edge_locked(self, edge_name: str):
        """Event-anchored mechanics on breaking/overdrive/stun rising edges."""
        if self._state != self.STATE_RUNNING or not self._mech_event_anchored:
            return
        for mech in self._mech_event_anchored:
            if _string((mech.get("detect") or {}).get("event")) != edge_name:
                continue
            if not self._mechanic_phase_ok_locked(mech):
                continue
            fwd = self._fire_mechanic_locked(mech, "event", None, 0)
            if fwd:
                self._queue_mech_forward_locked(fwd)

    def _match_mechanic_on_event_locked(self, event_type: int, mechanic_key: str,
                                        mechanic_label: str, trigger_family: str):
        """Event-anchored mechanics on packet boss events (text/key matched)."""
        if self._state != self.STATE_RUNNING or not self._mech_event_anchored:
            return
        for mech in self._mech_event_anchored:
            ev = _string((mech.get("detect") or {}).get("event"))
            if not ev:
                continue
            if ev == "breaking":
                hit = (int(event_type or 0) == 58)
            elif ev == "shield_broken":
                hit = (int(event_type or 0) == 47)
            elif ev in ("overdrive", "stun"):
                hit = False   # handled by the state-edge path
            else:
                hit = ev in (str(event_type), mechanic_key, mechanic_label, trigger_family)
            if hit and self._mechanic_phase_ok_locked(mech):
                fwd = self._fire_mechanic_locked(mech, "event", None, 0)
                if fwd:
                    self._queue_mech_forward_locked(fwd)

    def _fire_mechanic_locked(self, mech: Dict[str, Any], source: str,
                              cast_duration_ms: Any = None,
                              skill_id: int = 0) -> Optional[Dict[str, Any]]:
        """Fire one mechanic: cooldown dedup, countdown row, notification event,
        and the linkage-forward extras for auto-dodge. Caller holds the lock."""
        now = time.time()
        mid = _string(mech.get("id"))
        alert = mech.get("alert") or {}
        cd = _coerce_float(alert.get("cooldown_s"), 5.0, 0.0)
        last = self._mech_last_fire.get(mid, 0.0)
        # cooldown gate with a 0.1s floor: even cooldown_s=0 dedups two bound ids
        # of one mechanic appearing in the same snapshot (one fire, not two).
        if (now - last) < max(cd, 0.1):
            return None
        self._mech_last_fire[mid] = now
        name = _string(mech.get("name")) or "机制"
        configured_countdown_s = _coerce_float(alert.get("countdown_s"), 0.0, 0.0)
        countdown_s = configured_countdown_s
        if isinstance(cast_duration_ms, (int, float)) and cast_duration_ms > 0:
            cast_s = max(0.0, float(cast_duration_ms) / 1000.0)
            if cast_s > 0:
                countdown_s = cast_s if configured_countdown_s <= 0 else min(
                    configured_countdown_s, cast_s)
        pre_warn_s = _coerce_float(alert.get("pre_warn_s"), 0.0, 0.0)
        if countdown_s > 0:
            pre_warn_s = min(pre_warn_s, countdown_s)
        dodge = mech.get("dodge") if isinstance(mech.get("dodge"), dict) else {}
        phases = list((self._profile or {}).get("phases") or [])
        phase_name = _string(phases[self._current_phase_idx].get("name")) \
            if self._current_phase_idx < len(phases) else ""
        evt = {
            "mechanic_id": mid,
            "name": name,
            "kind": _string(mech.get("kind")),
            "color": _string(mech.get("color")),
            "banner_text": _string(alert.get("banner_text")) or name,
            "tts_text": _string(alert.get("tts_text")) or name,
            "alert_type": _string(alert.get("alert_type")) or "both",
            "alert_enabled": _coerce_bool(alert.get("enabled"), True),
            "countdown_s": countdown_s,
            "configured_countdown_s": configured_countdown_s,
            "pre_warn_s": pre_warn_s,
            "sound": _string(alert.get("sound")) or "boss_alert",
            "source": _string(source),
            "skill_id": int(skill_id or 0),
            "cast_duration_ms": int(cast_duration_ms)
                if isinstance(cast_duration_ms, (int, float)) and cast_duration_ms else 0,
            "phase_idx": self._current_phase_idx,
            "phase_name": phase_name,
            "dodge_enabled": _coerce_bool(dodge.get("enabled"), False),
            "ts": now,
        }
        self._last_mechanic_event = evt
        if countdown_s > 0:
            self._mech_countdowns.append({
                "id": mid, "name": name, "color": evt["color"],
                "ends_at": now + countdown_s, "countdown_s": countdown_s,
                "pre_warn_s": pre_warn_s,
            })
            if len(self._mech_countdowns) > 6:
                self._mech_countdowns.pop(0)
            self._refresh_countdown_push_locked(now, force=True)
        self._fire_mechanic_event_unlocked(evt)

        fwd: Dict[str, Any] = {"mechanic_id": mid, "mechanic_name": name}
        if evt["dodge_enabled"]:
            inline = dodge.get("inline") if isinstance(dodge.get("inline"), dict) else None
            if inline:
                fwd["mechanic_dodge"] = inline
                lead_ms = _coerce_int(inline.get("lead_ms"), 0, 0)
                if countdown_s > 0:
                    fwd["dodge_wait_s"] = max(0.0, countdown_s - lead_ms / 1000.0)
        return fwd

    def _fire_mechanic_event_unlocked(self, evt: Dict[str, Any]):
        """Dispatch the mechanic notification off-lock (the engine lock is
        non-reentrant). Falls back to the legacy alert/sound pipeline when no
        notification stack is wired."""
        if self._on_mechanic is not None:
            cb = self._on_mechanic
            payload = dict(evt)

            def _dispatch():
                try:
                    cb(payload)
                except Exception:
                    pass

            threading.Thread(target=_dispatch, daemon=True).start()
            return
        if not evt.get("alert_enabled", True):
            return
        alert_type = evt.get("alert_type") or "both"
        if alert_type in ("visual", "both"):
            self._fire_alert_unlocked("Boss Mechanic",
                                      _string(evt.get("banner_text")) or _string(evt.get("name")))
        if alert_type in ("sound", "both"):
            self._fire_sound_unlocked(_string(evt.get("sound")) or "boss_alert")

    def _queue_mech_forward_locked(self, mech_fwd: Dict[str, Any]):
        if not self._on_boss_action_cb:
            return
        fwd = {"boss_base_id": int(self._boss_base_id or 0),
               "boss_uuid": int(self._boss_uuid or 0),
               "source": "mechanic"}
        fwd.update(mech_fwd)
        self._pending_mech_forwards.append(fwd)

    def _drain_mech_forwards(self) -> List[Dict[str, Any]]:
        with self._lock:
            if not self._pending_mech_forwards:
                return []
            pending = self._pending_mech_forwards
            self._pending_mech_forwards = []
        return pending

    def _dispatch_mech_forwards(self):
        for fwd in self._drain_mech_forwards():
            try:
                self._on_boss_action_cb(fwd)
            except Exception:
                pass

    def _refresh_countdown_push_locked(self, now: float, force: bool = False):
        """Maintain the JSON-safe countdown list pushed to GameState; rebuilt
        only when the int-second signature changes."""
        if self._mech_countdowns:
            self._mech_countdowns = [c for c in self._mech_countdowns
                                     if c["ends_at"] > now]
        sig = tuple((c["id"], int(c["ends_at"] - now)) for c in self._mech_countdowns)
        if not force and sig == self._mech_countdown_sig:
            return
        self._mech_countdown_sig = sig
        self._mech_countdowns_push = [
            {"id": c["id"], "name": c["name"], "color": c.get("color", ""),
             "remaining_s": round(max(0.0, c["ends_at"] - now), 1),
             "countdown_s": c["countdown_s"],
             "pre_warn_s": c.get("pre_warn_s", 0.0)}
            for c in self._mech_countdowns]

    def _tick_mechanics_locked(self, now: float, elapsed: float, phase_elapsed: float):
        """4Hz upkeep: HP-crossing anchors, time anchors (early by countdown_s,
        with repeats), countdown rows, enrage TTS milestones."""
        # HP-crossing anchors (one-shot per mechanic)
        if self._mech_hp_anchored and self._boss_max_hp > 0:
            hp_pct = self._boss_hp / self._boss_max_hp * 100.0
            prev = self._mech_prev_hp_pct
            for mech in self._mech_hp_anchored:
                thr = _coerce_float((mech.get("detect") or {}).get("hp_pct"), 0.0)
                if thr <= 0:
                    continue
                key = (_string(mech.get("id")), "hp")
                if key in self._mech_fired_once:
                    continue
                if hp_pct <= thr < prev:
                    if self._mechanic_phase_ok_locked(mech):
                        self._mech_fired_once.add(key)
                        fwd = self._fire_mechanic_locked(mech, "hp", None, 0)
                        if fwd:
                            self._queue_mech_forward_locked(fwd)
            self._mech_prev_hp_pct = hp_pct

        # time anchors — alert fires countdown_s early so the bar hits 0 at impact
        for mech in self._mech_time_anchored:
            det = mech.get("detect") or {}
            countdown_s = _coerce_float((mech.get("alert") or {}).get("countdown_s"), 0.0)
            rep = _coerce_float(det.get("repeat_interval_s"), 0.0)
            for anchor_key, base_elapsed, scope in (
                    ("time_into_phase_s", phase_elapsed, self._current_phase_idx),
                    ("time_into_fight_s", elapsed, -1)):
                t0 = _coerce_float(det.get(anchor_key), 0.0)
                if t0 <= 0:
                    continue
                if not self._mechanic_phase_ok_locked(mech):
                    continue
                eff = max(0.0, t0 - countdown_s)
                key = (_string(mech.get("id")), anchor_key, scope)
                cnt = self._mech_time_fire_counts.get(key, 0)
                if cnt > 0 and rep <= 0:
                    continue
                target = eff + rep * cnt
                if base_elapsed >= target:
                    fwd = self._fire_mechanic_locked(mech, "time", None, 0)
                    # only consume the repeat slot when the fire actually landed,
                    # so a cooldown-swallowed fire isn't lost forever
                    if fwd is not None:
                        self._mech_time_fire_counts[key] = cnt + 1
                        self._queue_mech_forward_locked(fwd)

        self._refresh_countdown_push_locked(now)

        # enrage TTS milestones
        est = self._enrage_state_locked(now)
        if est["armed"] and est["remaining_s"] > 0:
            for m in est["milestones"]:
                if m in self._enrage_milestones_fired or est["remaining_s"] > m:
                    continue
                self._enrage_milestones_fired.add(m)
                evt = {
                    "mechanic_id": "enrage_milestone",
                    "name": "狂暴倒计时",
                    "kind": "enrage",
                    "color": "#ef684e",
                    "banner_text": f"狂暴 T-{m}s",
                    "tts_text": f"距离狂暴还有{m}秒",
                    "alert_type": "both",
                    "alert_enabled": True,
                    "countdown_s": 0.0,
                    "pre_warn_s": 0.0,
                    "sound": "boss_alert",
                    "source": "enrage",
                    "skill_id": 0,
                    "cast_duration_ms": 0,
                    "phase_idx": self._current_phase_idx,
                    "phase_name": "",
                    "dodge_enabled": False,
                    "ts": now,
                }
                self._last_mechanic_event = evt
                self._fire_mechanic_event_unlocked(evt)
                break

    def reload_active_profile(self, profile: Dict[str, Any]) -> bool:
        """Hot-apply edited mechanics/enrage to the loaded profile without
        restarting the raid: editor saves and inbox bindings take effect
        immediately, while runtime counters/cooldowns/inbox survive."""
        if not isinstance(profile, dict):
            return False
        with self._lock:
            cur = self._profile
            if not cur or _string(cur.get("id")) != _string(profile.get("id")):
                return False
            self._profile = normalize_profile(copy.deepcopy(profile))
            self._rebuild_mechanic_index_locked(preserve_runtime=True)
            # ids that just got bound leave the inbox
            for sid in list(self._unbound_skills):
                if sid in self._mech_index_by_skill:
                    self._unbound_skills.pop(sid, None)
            self._maybe_anchor_enrage_locked()
        return True

    def get_unbound_skills(self) -> List[Dict[str, Any]]:
        """Binding inbox for the mechanics editor (newest first)."""
        with self._lock:
            rows = sorted(self._unbound_skills.values(),
                          key=lambda r: r["last_ts"], reverse=True)
            return [dict(r) for r in rows]

    def clear_unbound_skill(self, skill_id: Any):
        with self._lock:
            self._unbound_skills.pop(_coerce_int(skill_id, 0), None)

    def get_last_mechanic_event(self) -> Dict[str, Any]:
        with self._lock:
            return dict(self._last_mechanic_event or {})

    # ── observation aggregation (persisted store) ──────────────────────────────
    def _current_scene_locked(self) -> Dict[str, Any]:
        sm = self._state_mgr
        sid = did = 0
        name = ""
        if sm is not None:
            try:
                st = sm.state
                sid = int(getattr(st, "dungeon_scene_id", 0) or 0)
                did = int(getattr(st, "dungeon_id", 0) or 0)
                name = _string(getattr(st, "dungeon_name", "") or "")
            except Exception:
                pass
        return {"scene_key": str(sid or did or 0), "scene_id": sid,
                "dungeon_id": did, "name": name}

    def _boss_hp_pct_locked(self) -> Optional[float]:
        if self._boss_max_hp > 0:
            return max(0.0, min(1.0, self._boss_hp / self._boss_max_hp))
        return None

    def _elapsed_s_locked(self) -> Optional[float]:
        if self._state == self.STATE_RUNNING and self._start_time > 0:
            return max(0.0, time.time() - self._start_time)
        return None

    def _derive_state_tags_locked(self) -> List[str]:
        """Tag a cast with the boss's concurrent special state so the editor can
        mark e.g. a shield/enrage/invincible-coincident skill."""
        tags: List[str] = []
        if self._boss_in_overdrive:
            tags.append("enrage")
        if self._boss_invincible:
            tags.append("invincible")
        if self._boss_shield_active:
            tags.append("shield")
        if self._boss_breaking_stage and self._boss_breaking_stage >= 1:
            tags.append("breaking")
        if self._boss_stun:
            tags.append("stun")
        return tags

    def _observe_to_store_locked(self, obs_id: int, name: str, kind: str,
                                 tags: Optional[List[str]] = None,
                                 duration_ms: Optional[int] = None) -> None:
        if not self._boss_base_id:
            return
        scene = self._current_scene_locked()
        bname = self._observed_boss_names.get(self._boss_base_id, "") \
            or _string((self._last_monster_data or {}).get("name"))
        try:
            self._skill_store.observe(
                scene_key=scene["scene_key"], scene_id=scene["scene_id"],
                dungeon_id=scene["dungeon_id"], scene_name=scene["name"],
                boss_base_id=self._boss_base_id, boss_name=bname,
                obs_id=obs_id, name=name, kind=kind, tags=tags or [],
                duration_ms=duration_ms, hp_pct=self._boss_hp_pct_locked(),
                elapsed_s=self._elapsed_s_locked(), max_hp=int(self._boss_max_hp or 0))
        except Exception:
            pass

    def _record_boss_event_locked(self, event_type: int, label: str, family: str) -> None:
        tag = _EVENT_TAG.get(int(event_type or 0)) or (family or "mechanic")
        nm = label or family or ("机制#%d" % int(event_type or 0))
        self._observe_to_store_locked(int(event_type or 0), nm, KIND_MECHANIC, tags=[tag])

    def _observe_state_transitions_locked(self) -> None:
        """Record enrage / invincibility ONSET (rising edge) with the HP%/elapsed
        at which it happened, so the editor can mark blood-line / timed states."""
        if not self._boss_base_id:
            return
        od = bool(self._boss_in_overdrive)
        if od and not self._obs_prev_overdrive:
            self._observe_to_store_locked(_STATE_ENRAGE, "狂暴/过载", KIND_STATE, tags=["enrage"])
        self._obs_prev_overdrive = od
        inv = bool(self._boss_invincible)
        if inv and not self._obs_prev_invincible:
            self._observe_to_store_locked(_STATE_INVINCIBLE, "无敌", KIND_STATE, tags=["invincible"])
        self._obs_prev_invincible = inv

    def get_observed_scenes(self) -> List[Dict[str, Any]]:
        """Editor scene selector: [{scene_key, scene_id, dungeon_id, name, boss_count}]."""
        return self._skill_store.scenes()

    def get_observed_boss_skills(self, base_id: Optional[int] = None,
                                 scene_key: Optional[str] = None) -> Dict[int, List[Dict[str, Any]]]:
        """Editor data source: {base_id: [observation, ...]} from the persisted
        store, optionally scoped to a scene. Observations carry tags / hp-line /
        timed markers."""
        store = self._skill_store
        if base_id is not None:
            return {int(base_id): store.observations(scene_key, int(base_id))}
        out: Dict[int, List[Dict[str, Any]]] = {}
        for b in store.bosses(scene_key):
            bid = int(b["base_id"])
            if bid not in out:
                out[bid] = store.observations(scene_key, bid)
        return out

    def get_observed_bosses(self, scene_key: Optional[str] = None) -> List[Dict[str, Any]]:
        """Editor boss selector: [{base_id, name, scene_key, observed_count}]."""
        return self._skill_store.bosses(scene_key)

    def on_self_dead_change(self, is_dead: bool):
        """Mem bridge -> player death gate. Suppresses auto-dodge/offense while dead."""
        with self._lock:
            self._self_dead = bool(is_dead)

    def get_profile_phases(self) -> List[Dict[str, Any]]:
        """当前档案的 phases 副本 — UI 全量推送用 (updateFull 契约要 phases 数组)。"""
        with self._lock:
            return [dict(p) for p in (self._profile or {}).get("phases") or []]

    def get_status(self, include_entities: bool = True) -> Dict[str, Any]:
        """Return current engine status dict. `include_entities=False` skips the
        O(N) per-entity list build — used by the boss-reactions editor path and
        the 250ms game-state push, which need only the boss scalars (this is the
        crowd / 20-player lag fix: the entity build no longer runs under the lock
        for consumers that never read it)."""
        with self._lock:
            return self._build_status_locked(include_entities=include_entities)

    # ── Internal ──

    def _build_status_locked(self, include_entities: bool = True) -> Dict[str, Any]:
        now = time.time()
        profile = self._profile or {}
        elapsed = (now - self._start_time) if self._state == self.STATE_RUNNING else 0.0
        phase_elapsed = (now - self._phase_start_time) if self._state == self.STATE_RUNNING else 0.0
        enrage_st = self._enrage_state_locked(now)
        enrage_remaining = enrage_st["remaining_s"] if enrage_st["time_s"] > 0 else 0.0
        dps = int(self._total_damage / max(0.001, elapsed)) if elapsed > 0 else 0
        boss_hp_profile = int(profile.get("boss_total_hp") or 0)

        # Prefer real HP from packets; fall back to damage-estimate
        if self._boss_max_hp > 0:
            boss_hp_pct = max(0.0, self._boss_hp / self._boss_max_hp)
            boss_total_hp = self._boss_max_hp
            boss_current_hp = self._boss_hp
            hp_source = "packet"
        elif boss_hp_profile > 0:
            boss_hp_pct = max(0.0, 1.0 - (self._total_damage / boss_hp_profile))
            boss_total_hp = boss_hp_profile
            boss_current_hp = max(0, boss_hp_profile - self._total_damage)
            hp_source = "estimate"
        else:
            boss_hp_pct = 1.0
            boss_total_hp = 0
            boss_current_hp = 0
            hp_source = "none"

        phases = list(profile.get("phases") or [])
        current_phase = phases[self._current_phase_idx] if self._current_phase_idx < len(phases) else {}

        return {
            "state": self._state,
            "profile_name": _string(profile.get("profile_name")),
            "elapsed_s": round(elapsed, 1),
            "phase_idx": self._current_phase_idx,
            "phase_name": _string(current_phase.get("name")),
            "phase_elapsed_s": round(phase_elapsed, 1),
            "total_damage": self._total_damage,
            "dps": dps,
            "enrage_remaining_s": round(enrage_remaining, 1),
            "enrage_armed": enrage_st["armed"],
            "enrage_urgency": enrage_st["urgency"],
            "enrage_source": enrage_st.get("source", "profile"),
            "mechanic_event": dict(self._last_mechanic_event or {}),
            "mechanic_countdowns": list(self._mech_countdowns_push),
            "unbound_skill_count": len(self._unbound_skills),
            "boss_hp_est_pct": round(boss_hp_pct, 4),
            "boss_total_hp": boss_total_hp,
            "boss_current_hp": boss_current_hp,
            "boss_hp_source": hp_source,
            "boss_uuid": self._boss_uuid,
            "boss_base_id": self._boss_base_id,
            "boss_shield_active": self._boss_shield_active,
            "boss_shield_pct": round(self._boss_shield_pct, 4),
            "boss_breaking_stage": self._boss_breaking_stage,
            "boss_extinction_pct": round(self._boss_extinction_pct, 4),
            "boss_in_overdrive": self._boss_in_overdrive,
            "boss_invincible": self._boss_invincible,
            "last_boss_event": dict(self._last_boss_event or {}),
            "last_boss_mechanic_key": self._last_boss_mechanic_key,
            "last_boss_mechanic_label": self._last_boss_mechanic_label,
            "last_boss_trigger_family": self._last_boss_trigger_family,
            "boss_cast_skill_id": self._boss_cast_skill_id,
            "boss_cast_skill_name": self._boss_cast_skill_name,
            "boss_cast_active": self._boss_cast_active,
            "boss_cast_duration_ms": self._boss_cast_duration_ms,
            "boss_stun": self._boss_stun,
            "self_dead": self._self_dead,
            "entities": self._get_entities_locked() if include_entities else [],
        }

    def _run_loop(self):
        while self._running:
            time.sleep(0.25)
            self._skill_store.maybe_save()   # throttled flush during long fights
            with self._lock:
                if self._state == self.STATE_RUNNING:
                    try:
                        self._tick_locked()
                    except Exception as e:
                        print(f"[BossRaid] tick error: {e}")
                    # v3.1.7 round 16: drain any pending entity-update set by
                    # `on_damage_event`. Fires at most once per 250ms loop
                    # iteration; combined with the on_damage_event dirty-bit
                    # this keeps per-event cost down without losing UI updates.
                    self._maybe_flush_entity_update_locked()
            # tick-fired mechanic dodges dispatch outside the lock
            self._dispatch_mech_forwards()

    def _maybe_flush_entity_update_locked(self) -> None:
        if not self._entity_update_dirty:
            return
        if self._on_entity_update_cb is None:
            self._entity_update_dirty = False
            return
        now = time.time()
        if now - self._last_entity_update_emit_at < self.ENTITY_UPDATE_MIN_INTERVAL_S:
            return  # too soon; keep dirty so a later tick flushes
        self._last_entity_update_emit_at = now
        self._entity_update_dirty = False
        entities = self._get_entities_locked()
        try:
            self._on_entity_update_cb(entities)
        except Exception:
            pass

    def _tick_locked(self):
        now = time.time()
        profile = self._profile
        if not profile:
            return

        elapsed = now - self._start_time
        phase_elapsed = now - self._phase_start_time

        # ── Mechanics upkeep (hp/time anchors, countdowns, enrage milestones) ──
        self._tick_mechanics_locked(now, elapsed, phase_elapsed)

        # ── Enrage check ──
        enrage_st = self._enrage_state_locked(now)
        if enrage_st["armed"] and enrage_st["remaining_s"] <= 0:
            self._state = self.STATE_COMPLETED
            self._fire_alert_unlocked("Boss Raid", "⚠ ENRAGE — TIME UP!")
            self._fire_sound_unlocked("boss_alert")
            self._push_game_state_locked(now)
            return

        # ── Phase auto-transition ──
        phases = list(profile.get("phases") or [])
        if self._current_phase_idx < len(phases) - 1:
            next_idx = self._current_phase_idx + 1
            if next_idx < len(phases):
                next_phase = phases[next_idx]
                trigger = next_phase.get("trigger") or {}
                trigger_type = _string(trigger.get("type"))
                trigger_value = _coerce_float(trigger.get("value"), 0.0)

                should_advance = False
                if trigger_type == "time" and trigger_value > 0:
                    should_advance = elapsed >= trigger_value
                elif trigger_type == "dps_total" and trigger_value > 0:
                    should_advance = self._total_damage >= trigger_value
                elif trigger_type == "hp_pct" and trigger_value > 0:
                    # Prefer real HP from packets; fall back to damage estimate
                    if self._boss_max_hp > 0:
                        current_pct = max(0.0, self._boss_hp / self._boss_max_hp)
                    else:
                        boss_hp = int(profile.get("boss_total_hp") or 0)
                        current_pct = max(0.0, 1.0 - (self._total_damage / boss_hp)) if boss_hp > 0 else 1.0
                    should_advance = current_pct <= (trigger_value / 100.0)
                elif trigger_type == "extinction_pct" and trigger_value > 0:
                    # Breaking bar fill percentage (0-100)
                    should_advance = self._boss_extinction_pct >= (trigger_value / 100.0)
                elif trigger_type == "breaking_stage":
                    # Boss breaking stage counter
                    should_advance = self._boss_breaking_stage >= int(trigger_value)
                elif trigger_type == "overdrive":
                    # Polled overdrive check (reactive also in on_boss_event)
                    should_advance = self._boss_in_overdrive

                if should_advance:
                    self._advance_phase()
                    return

        # ── Timeline triggers ──
        current_phase = phases[self._current_phase_idx] if self._current_phase_idx < len(phases) else {}
        for tl in (current_phase.get("timelines") or []):
            tl_id = _string(tl.get("id"))
            time_s = _coerce_float(tl.get("time_s"), 0.0)
            repeat = _coerce_float(tl.get("repeat_interval_s"), 0.0)
            pre_warn_s = _coerce_float(tl.get("pre_warn_s"), 0.0)
            duration_s = _coerce_float(tl.get("duration_s"), 0.0)

            # Evaluate condition gate — skip this timeline if condition not met
            cond = tl.get("condition") or {}
            cond_type = _string(cond.get("type"))
            if cond_type and cond_type != "always":
                cond_comp = _string(cond.get("comparator")) or ">="
                cond_val = _coerce_float(cond.get("value"), 0.0)
                if cond_type == "hp_pct":
                    if self._boss_max_hp > 0:
                        cur = self._boss_hp / self._boss_max_hp * 100
                    else:
                        boss_hp = int(profile.get("boss_total_hp") or 0)
                        cur = max(0.0, (1.0 - self._total_damage / boss_hp) * 100) if boss_hp > 0 else 100.0
                    if not self._eval_comparator(cur, cond_comp, cond_val):
                        continue
                elif cond_type == "shield_active":
                    if not self._boss_shield_active:
                        continue
                elif cond_type == "breaking":
                    if self._boss_breaking_stage < 1:
                        continue

            key = (self._current_phase_idx, tl_id)
            fire_count = self._fired_timelines.get(key, 0)

            # Pre-warning check
            pre_key = (self._current_phase_idx, tl_id, "pre")
            pre_fired = self._fired_timelines.get(pre_key, 0)
            if pre_warn_s > 0 and pre_fired == 0 and time_s > pre_warn_s:
                pre_time = time_s - pre_warn_s
                if phase_elapsed >= pre_time and fire_count == 0:
                    self._fired_timelines[pre_key] = 1
                    label = _string(tl.get("label")) or "Timeline Alert"
                    self._fire_alert_unlocked("Pre-warn", f"⏳ {label} in {pre_warn_s:.0f}s")
                    self._fire_sound_unlocked("boss_prewarn")

            if repeat > 0 and fire_count > 0:
                # Repeating: check if next repeat time reached
                next_fire = time_s + repeat * fire_count
                if phase_elapsed >= next_fire:
                    self._fired_timelines[key] = fire_count + 1
                    label = _string(tl.get("label")) or "Timeline Alert"
                    alert_type = _string(tl.get("alert_type")) or "both"
                    if alert_type in ("visual", "both"):
                        suffix = f" ({duration_s:.0f}s)" if duration_s > 0 else ""
                        self._fire_alert_unlocked("Boss Raid", f"{label}{suffix}")
                    if alert_type in ("sound", "both"):
                        self._fire_sound_unlocked("boss_alert")
            elif fire_count == 0 and phase_elapsed >= time_s:
                # First fire
                self._fired_timelines[key] = 1
                label = _string(tl.get("label")) or "Timeline Alert"
                alert_type = _string(tl.get("alert_type")) or "both"
                if alert_type in ("visual", "both"):
                    suffix = f" ({duration_s:.0f}s)" if duration_s > 0 else ""
                    self._fire_alert_unlocked("Boss Raid", f"{label}{suffix}")
                if alert_type in ("sound", "both"):
                    self._fire_sound_unlocked("boss_alert")

        # ── Push state ──
        self._push_game_state_locked(now)

    def _advance_phase(self):
        """Advance to next phase (called under lock)."""
        profile = self._profile or {}
        phases = list(profile.get("phases") or [])
        if self._current_phase_idx >= len(phases) - 1:
            self._state = self.STATE_COMPLETED
            self._fire_alert_unlocked("Boss Raid", "✓ COMPLETED")
            self._fire_sound_unlocked("boss_phase")
            return
        self._current_phase_idx += 1
        self._phase_start_time = time.time()
        self._maybe_anchor_enrage_locked()
        phase = phases[self._current_phase_idx]
        phase_name = _string(phase.get("name")) or f"P{self._current_phase_idx + 1}"
        self._fire_alert_unlocked("Boss Raid", f"→ {phase_name}")
        self._fire_sound_unlocked("boss_phase")

    def _push_game_state_locked(self, now: float):
        """Push boss raid fields to GameStateManager (called under lock)."""
        if self._state_mgr is None:
            return
        status = self._build_status_locked(include_entities=False)
        enrage_rem = status["enrage_remaining_s"]
        # Only surface a boss timer while a user-started raid profile is RUNNING.
        # During free-combat observation (no profile, or an enrage-boss the user
        # hasn't started a raid for) the HUD must keep its wall clock, not boss time.
        if self._state != self.STATE_RUNNING:
            timer_text = ''
        elif enrage_rem > 0:
            timer_text = f"{int(enrage_rem) // 60}:{int(enrage_rem) % 60:02d}"
        else:
            elapsed = status["elapsed_s"]
            timer_text = f"{int(elapsed) // 60}:{int(elapsed) % 60:02d}"

        self._state_mgr.update(
            boss_raid_active=True,
            boss_raid_phase=status["phase_idx"],
            boss_raid_phase_name=status["phase_name"],
            boss_enrage_remaining=enrage_rem,
            boss_enrage_urgency=status["enrage_urgency"],
            boss_mechanic_event=status["mechanic_event"],
            boss_mechanic_countdowns=status["mechanic_countdowns"],
            boss_timer_text=timer_text,
            boss_total_damage=status["total_damage"],
            boss_dps=status["dps"],
            boss_hp_est_pct=status["boss_hp_est_pct"],
            boss_current_hp=status["boss_current_hp"],
            boss_total_hp=status["boss_total_hp"],
            boss_hp_source=status["boss_hp_source"],
            boss_shield_active=status["boss_shield_active"],
            boss_shield_pct=status["boss_shield_pct"],
            boss_breaking_stage=status["boss_breaking_stage"],
            boss_extinction_pct=status["boss_extinction_pct"],
            boss_in_overdrive=status["boss_in_overdrive"],
            boss_invincible=status["boss_invincible"],
            boss_cast_skill_id=status["boss_cast_skill_id"],
            boss_cast_skill_name=status["boss_cast_skill_name"],
            boss_cast_active=status["boss_cast_active"],
            boss_cast_duration_ms=status["boss_cast_duration_ms"],
            boss_stun=status["boss_stun"],
            self_dead=status["self_dead"],
            last_boss_event=status.get("last_boss_event") or {},
        )

    def _push_game_state_clear(self):
        """Clear boss raid fields from GameState."""
        self._state_mgr.update(
            boss_raid_active=False,
            boss_raid_phase=0,
            boss_raid_phase_name='',
            boss_enrage_remaining=0.0,
            boss_enrage_urgency='',
            boss_mechanic_event={},
            boss_mechanic_countdowns=[],
            boss_timer_text='',
            boss_total_damage=0,
            boss_dps=0,
            boss_hp_est_pct=1.0,
            boss_current_hp=0,
            boss_total_hp=0,
            boss_hp_source='none',
            boss_shield_active=False,
            boss_shield_pct=0.0,
            boss_breaking_stage=0,
            boss_extinction_pct=0.0,
            boss_in_overdrive=False,
            boss_invincible=False,
            boss_cast_skill_id=0,
            boss_cast_skill_name='',
            boss_cast_active=False,
            boss_cast_duration_ms=0,
            boss_stun=False,
            self_dead=self._self_dead,
            last_boss_event={},
        )

    def _fire_alert(self, title: str, message: str):
        if self._on_alert:
            try:
                self._on_alert(title, message)
            except Exception:
                pass

    def _fire_alert_unlocked(self, title: str, message: str):
        """Fire alert — safe to call from within locked context (deferred to thread)."""
        if self._on_alert:
            threading.Thread(target=self._fire_alert, args=(title, message),
                             daemon=True).start()

    @staticmethod
    def _eval_comparator(current: float, comp: str, target: float) -> bool:
        """Evaluate a numeric comparator for timeline conditions."""
        if comp == ">=":
            return current >= target
        elif comp == "<=":
            return current <= target
        elif comp == ">":
            return current > target
        elif comp == "<":
            return current < target
        elif comp == "==":
            return abs(current - target) < 0.01
        return True  # Unknown comparator → pass

    def _fire_sound(self, name: str):
        if self._on_sound:
            try:
                self._on_sound(name)
            except Exception:
                pass

    def _fire_sound_unlocked(self, name: str):
        if self._on_sound:
            threading.Thread(target=self._fire_sound, args=(name,),
                             daemon=True).start()
