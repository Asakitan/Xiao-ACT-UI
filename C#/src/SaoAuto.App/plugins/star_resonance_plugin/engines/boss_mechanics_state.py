# -*- coding: utf-8 -*-
"""机制编辑器共享契约：Tk 简单面板 / Tk 详细面板 / Web raid_editor 三处消费同一
state dict (仿 build_boss_reactions_state 模式)。编辑目标=当前激活档案的
mechanics[]；总开关聚合 TTS / 横幅 / 自动躲避三组设置。"""

import copy
import threading
from typing import Any, Dict, List, Optional

# 所有 mechanics 写操作 load→modify→save 序列化, 防双窗口/pywebview 多线程并发互覆盖。
# RLock: create_mechanic_from_skill 内嵌 upsert_mechanic。
_MUTATE_LOCK = threading.RLock()

from plugins.star_resonance_plugin.engines.boss_raid_engine import (
    active_profile,
    find_profile,
    load_boss_raid_config,
    normalize_mechanic,
    make_default_mechanic,
    save_boss_raid_config,
    _utc_now_iso,
)
from plugins.star_resonance_plugin.engines.boss_autokey_linkage import (
    load_linkage_config,
    set_dodge_enabled,
    _lookup,
    _name_resolver,
    _resolve_skill_name,
    _resolve_boss_name,
    _resolve_scene_name,
)
from utils import sao_tts


def _s(v: Any) -> str:
    return str(v or "").strip()


def _i(v: Any, default: int = 0) -> int:
    try:
        return int(v)
    except Exception:
        return default


# 编辑器技能库搜索覆盖的离线名字表 kind (system_skill 含离线枚举的 raid boss 技能)
_CATALOG_KINDS = ("system_skill", "boss_skill", "boss_mechanic_skill",
                  "monster_skill", "ultimate_skill", "scripted_skill", "skill")
# buff 表 (点名/分摊/标记类机制全是 buff, 检测走 detect.buff_ids)
_CATALOG_BUFF_KINDS = ("buff",)


def _target_profile(config: Dict[str, Any],
                    profile_id: Optional[str] = None) -> Optional[Dict[str, Any]]:
    if _s(profile_id):
        return find_profile(config, profile_id)
    return active_profile(config)


def _resolve_detect_name(nm, sid: int) -> str:
    """检测 id 既可能是技能也可能是 buff (点名机制全是 buff), 两类表都查。
    都查不到也返回前缀名(如 Boss技能#12345), 不留空白让卡片显示裸 #id。"""
    name = _resolve_skill_name(nm, sid) or _lookup(nm, "buff", sid)
    if name or not nm or not sid:
        return name
    try:
        return nm.resolve("skill", sid) or ""
    except Exception:
        return ""


def mechanic_summary(mech: Dict[str, Any],
                     directional_on: bool = True) -> Dict[str, Any]:
    """卡片行的轻量摘要 (双端同样渲染)。directional_on=总开关「定向移动」状态,
    关时方向摘要标注「(总开关未开·不执行)」, 不谎报生效 (修审计 UI P1)。"""
    alert = mech.get("alert") or {}
    dodge = mech.get("dodge") or {}
    inline = dodge.get("inline") or {}
    det = mech.get("detect") or {}
    skill_ids = list(det.get("skill_ids") or []) + list(det.get("buff_ids") or [])
    dodge_desc = ""
    if dodge.get("enabled"):
        seq = inline.get("sequence") or []
        keys = {(_s(s.get("key"))).upper() for s in seq}
        parts = []
        direction = _s(inline.get("direction"))
        if direction:
            from plugins.star_resonance_plugin.engines.auto_dodge_director import direction_label
            lbl = direction_label(direction) or (
                "远离" if direction.startswith("away") else direction)
            if not directional_on:
                lbl += "(总开关未开·不执行)"
            parts.append("移动:" + lbl)
        if seq and len(seq) >= 2 and len(keys) == 1 and "" not in keys:
            parts.append("冲刺 %s×%d" % (next(iter(keys)), len(seq)))
        elif seq:
            parts.append("序列×%d" % len(seq))
        elif _s(inline.get("action_key")):
            mode = "按住%dms" % _i(inline.get("hold_ms"), 0) \
                if _s(inline.get("press_mode")) == "hold" else "轻点"
            parts.append("%s %s" % (_s(inline.get("action_key")), mode))
        dodge_desc = " ".join(parts)
    if not alert.get("enabled", True):
        alert_desc = "提醒:关"
    else:
        atype = _s(alert.get("alert_type")) or "both"
        label = {"both": "TTS·横幅", "sound": "TTS", "visual": "横幅"}.get(atype, "TTS·横幅")
        alert_desc = "%s %.0fs/预警%.0fs" % (
            label, float(alert.get("countdown_s") or 0), float(alert.get("pre_warn_s") or 0))
    return {
        "bound_count": len(skill_ids),
        "alert_desc": alert_desc,
        "dodge_desc": dodge_desc or "躲避:关",
    }


def build_mechanics_state(settings, engine, state_mgr,
                          scene_key: Any = None,
                          boss_base_id: Any = None,
                          profile_id: Any = None) -> Dict[str, Any]:
    """机制编辑器单一数据契约 (Tk×2 + Web 1:1)。"""
    config = load_boss_raid_config(settings)
    linkage = load_linkage_config(settings)
    nm = _name_resolver()
    profile = _target_profile(config, _s(profile_id))

    def _setting(key, default):
        try:
            v = settings.get(key, default)
            return default if v is None else v
        except Exception:
            return default

    def _panic_hotkey() -> str:
        # 急停键即主 UI 的 toggle_auto_dodge 绑定 — 跟随用户改键, 标签
        # 才不会和实际监听漂移。读 hotkeys 必须 {**DEFAULT_HOTKEYS, **saved}。
        try:
            from config import DEFAULT_HOTKEYS
        except Exception:
            DEFAULT_HOTKEYS = {}
        hk = _setting("hotkeys", {})
        merged = {**DEFAULT_HOTKEYS, **(hk if isinstance(hk, dict) else {})}
        value = merged.get("toggle_auto_dodge")
        if isinstance(value, dict):
            value = value.get("key") or value.get("name")
        return str(value or "").strip().upper() or "F12"

    health = sao_tts.peek_tts_health()
    master = {
        "tts_enabled": bool(_setting("tts_enabled", True)),
        "tts_volume": _i(_setting("tts_volume", 80), 80),
        "banner_enabled": bool(_setting("mech_banner_enabled", True)),
        "dodge_enabled": bool(linkage.get("dodge_enabled", False)),
        "directional_dodge_enabled": bool(_setting("directional_dodge_enabled", False)),
        "auto_walk_enabled": bool(_setting("auto_walk_enabled", False)),
        "linkage_enabled": bool(linkage.get("enabled", False)),
        "panic_hotkey": _panic_hotkey(),
        "zh_voice": health.get("zh_voice"),
    }

    out: Dict[str, Any] = {
        "ok": True,
        "master": master,
        "profiles": [{"id": _s(p.get("id")), "name": _s(p.get("profile_name")),
                      "map_name": _s(p.get("map_name")),
                      "mechanic_count": len(p.get("mechanics") or [])}
                     for p in (config.get("profiles") or [])],
        "active_profile_id": _s(config.get("active_profile_id")),
        "profile": None,
        "phases": [],
        "mechanics": [],
        "inbox": [],
        "observed": [],
    }
    if not profile:
        out["ok"] = False
        out["error"] = "no_profile"
        return out

    enrage = profile.get("enrage") if isinstance(profile.get("enrage"), dict) else {}
    out["profile"] = {
        "id": _s(profile.get("id")),
        "name": _s(profile.get("profile_name")),
        "map_name": _s(profile.get("map_name")),
        "enrage_time_s": _i(profile.get("enrage_time_s"), 0),
        "enrage": copy.deepcopy(enrage),
        "dungeon_id": _i(profile.get("dungeon_id"), 0),
        "difficulty": _s(profile.get("difficulty")),
        "source": _s(profile.get("source")),
    }
    out["phases"] = [{"id": _s(p.get("id")), "name": _s(p.get("name"))}
                     for p in (profile.get("phases") or [])]

    mechanics = []
    bound_ids = set()
    for mech in profile.get("mechanics") or []:
        if not isinstance(mech, dict):
            continue
        m = copy.deepcopy(mech)
        det = m.get("detect") or {}
        names = {}
        for sid in list(det.get("skill_ids") or []) + list(det.get("buff_ids") or []):
            bound_ids.add(_i(sid))
            names[str(sid)] = _resolve_detect_name(nm, _i(sid))
        m["skill_names"] = names
        m["summary"] = mechanic_summary(
            m, directional_on=bool(master.get("directional_dodge_enabled")))
        mechanics.append(m)
    out["mechanics"] = mechanics

    # 未绑定技能收件箱 (引擎运行时记录, 名字读时解析)
    inbox = []
    if engine is not None and hasattr(engine, "get_unbound_skills"):
        try:
            for rec in engine.get_unbound_skills()[:10]:
                sid = _i(rec.get("skill_id"))
                if sid in bound_ids:
                    continue
                row = dict(rec)
                if not _s(row.get("name")):
                    row["name"] = _resolve_skill_name(nm, sid)
                inbox.append(row)
        except Exception:
            pass
    out["inbox"] = inbox

    # 已观测技能 (绑定下拉数据源, 含离线枚举写入的 config 标签观测)
    sel_boss = _i(boss_base_id)
    selected_scene = _s(scene_key) or None
    if not sel_boss:
        # 智能默认: 机制里出现最多的 boss_base_id (示例档案绑 103309 → 直接
        # 给出 P3 boss 的候选技能, 而不是观测库里的第一个 P1 boss)
        counts: Dict[int, int] = {}
        for mech in mechanics:
            bid = _i((mech.get("detect") or {}).get("boss_base_id"))
            if bid > 0:
                counts[bid] = counts.get(bid, 0) + 1
        if counts:
            sel_boss = max(counts, key=lambda k: counts[k])
    observed: List[Dict[str, Any]] = []
    if engine is not None:
        try:
            if not sel_boss:
                bosses = engine.get_observed_bosses(selected_scene) or []
                if bosses:
                    sel_boss = _i(bosses[0].get("base_id"))
            if sel_boss:
                obs_map = engine.get_observed_boss_skills(sel_boss, selected_scene) or {}
                obs = list(obs_map.get(sel_boss) or obs_map.get(str(sel_boss)) or [])
                for o in obs:
                    if o.get("kind") != "skill":
                        continue
                    sid = _i(o.get("skill_id") or o.get("id"))
                    if sid <= 0:
                        continue
                    observed.append({
                        "id": sid,
                        "name": _s(o.get("name")) or _resolve_skill_name(nm, sid),
                        "count": _i(o.get("count")),
                        "last_cast_duration_ms": o.get("last_cast_duration_ms"),
                        "tags": list(o.get("tags") or []),
                        "bound": sid in bound_ids,
                    })
        except Exception:
            pass
    observed.sort(key=lambda r: (-int(r.get("count") or 0), r["id"]))
    out["observed"] = observed[:40]
    out["selected_boss_base_id"] = sel_boss

    # 场景/Boss 选择器 (复用观测库)
    scenes = []
    bosses = []
    if engine is not None:
        try:
            scenes = [dict(s) for s in (engine.get_observed_scenes() or [])]
            for s in scenes:
                if not _s(s.get("name")):
                    s["name"] = _resolve_scene_name(
                        nm, _i(s.get("scene_id")), _i(s.get("dungeon_id")))
            bosses = [dict(b) for b in (engine.get_observed_bosses(selected_scene) or [])]
            for b in bosses:
                resolved = _resolve_boss_name(nm, _i(b.get("base_id")))
                if resolved:
                    b["name"] = resolved
        except Exception:
            pass
    out["scenes"] = scenes
    out["bosses"] = bosses
    out["selected_scene_key"] = selected_scene or ""
    return out


def upsert_mechanic(settings, mechanic: Any,
                    profile_id: Optional[str] = None) -> Dict[str, Any]:
    """新增/更新一条机制 (按 id), 落到目标档案并持久化。返回归一化配置。"""
    with _MUTATE_LOCK:
        config = load_boss_raid_config(settings)
        profile = _target_profile(config, profile_id)
        if not profile:
            return config
        mech = normalize_mechanic(mechanic)
        mechanics = list(profile.get("mechanics") or [])
        for idx, existing in enumerate(mechanics):
            if _s(existing.get("id")) == mech["id"]:
                mechanics[idx] = mech
                break
        else:
            mechanics.append(mech)
        profile["mechanics"] = mechanics
        profile["updated_at"] = _utc_now_iso()
        return save_boss_raid_config(settings, config)


def delete_mechanic(settings, mechanic_id: str,
                    profile_id: Optional[str] = None) -> Dict[str, Any]:
    with _MUTATE_LOCK:
        config = load_boss_raid_config(settings)
        profile = _target_profile(config, profile_id)
        if not profile:
            return config
        mid = _s(mechanic_id)
        profile["mechanics"] = [m for m in (profile.get("mechanics") or [])
                                if _s(m.get("id")) != mid]
        profile["updated_at"] = _utc_now_iso()
        return save_boss_raid_config(settings, config)


def create_mechanic_from_skill(settings, skill_id: Any, skill_name: str = "",
                               cast_duration_ms: Any = None,
                               profile_id: Optional[str] = None) -> Dict[str, Any]:
    """收件箱一键建机制：用技能名做机制名, 施法时长换算默认倒计时。"""
    mech = make_default_mechanic()
    sid = _i(skill_id)
    name = _s(skill_name) or ("技能#%d" % sid)
    mech["name"] = name
    mech["detect"]["skill_ids"] = [sid] if sid > 0 else []
    if isinstance(cast_duration_ms, (int, float)) and cast_duration_ms > 500:
        mech["alert"]["countdown_s"] = round(float(cast_duration_ms) / 1000.0, 1)
    return upsert_mechanic(settings, mech, profile_id)


def bind_skill_to_mechanic(settings, mechanic_id: str, skill_id: Any,
                           profile_id: Optional[str] = None) -> Dict[str, Any]:
    with _MUTATE_LOCK:
        config = load_boss_raid_config(settings)
        profile = _target_profile(config, profile_id)
        if not profile:
            return config
        sid = _i(skill_id)
        mid = _s(mechanic_id)
        for mech in profile.get("mechanics") or []:
            if _s(mech.get("id")) != mid:
                continue
            det = mech.setdefault("detect", {})
            ids = list(det.get("skill_ids") or [])
            if sid > 0 and sid not in ids:
                ids.append(sid)
                det["skill_ids"] = sorted(ids)
            profile["updated_at"] = _utc_now_iso()
            break
        return save_boss_raid_config(settings, config)


def unbind_skill_from_mechanic(settings, mechanic_id: str, skill_id: Any,
                               profile_id: Optional[str] = None) -> Dict[str, Any]:
    with _MUTATE_LOCK:
        config = load_boss_raid_config(settings)
        profile = _target_profile(config, profile_id)
        if not profile:
            return config
        sid = _i(skill_id)
        mid = _s(mechanic_id)
        for mech in profile.get("mechanics") or []:
            if _s(mech.get("id")) != mid:
                continue
            det = mech.setdefault("detect", {})
            det["skill_ids"] = [x for x in (det.get("skill_ids") or []) if _i(x) != sid]
            det["buff_ids"] = [x for x in (det.get("buff_ids") or []) if _i(x) != sid]
            profile["updated_at"] = _utc_now_iso()
            break
        return save_boss_raid_config(settings, config)


def hot_apply_to_engine(settings, engine, profile_id: Optional[str] = None) -> bool:
    """编辑保存/收件箱绑定后把新机制热应用进正在运行的引擎 (无需重按 START)。"""
    if engine is None or not hasattr(engine, "reload_active_profile"):
        return False
    config = load_boss_raid_config(settings)
    profile = _target_profile(config, _s(profile_id))
    if not profile:
        return False
    try:
        return bool(engine.reload_active_profile(profile))
    except Exception:
        return False


def set_mechanics_master(settings, flags: Any) -> Dict[str, Any]:
    """总开关行：tts_enabled / tts_volume / banner_enabled / dodge_enabled。"""
    flags = flags if isinstance(flags, dict) else {}
    if "tts_enabled" in flags:
        val = bool(flags["tts_enabled"])
        settings.set("tts_enabled", val)
        sao_tts.set_tts_enabled(val)
    if "tts_volume" in flags:
        vol = max(0, min(100, _i(flags["tts_volume"], 80)))
        settings.set("tts_volume", vol)
        sao_tts.set_tts_volume(vol)
    if "banner_enabled" in flags:
        settings.set("mech_banner_enabled", bool(flags["banner_enabled"]))
    if "directional_dodge_enabled" in flags:
        settings.set("directional_dodge_enabled", bool(flags["directional_dodge_enabled"]))
    if "auto_walk_enabled" in flags:
        settings.set("auto_walk_enabled", bool(flags["auto_walk_enabled"]))
    try:
        settings.save()
    except Exception:
        pass
    if "dodge_enabled" in flags:
        set_dodge_enabled(settings, bool(flags["dodge_enabled"]))
    return {
        "tts_enabled": bool(settings.get("tts_enabled", True)),
        "tts_volume": _i(settings.get("tts_volume", 80), 80),
        "banner_enabled": bool(settings.get("mech_banner_enabled", True)),
        "dodge_enabled": bool(load_linkage_config(settings).get("dodge_enabled", False)),
        "directional_dodge_enabled": bool(settings.get("directional_dodge_enabled", False)),
        "auto_walk_enabled": bool(settings.get("auto_walk_enabled", False)),
    }


def search_skill_catalog(query: str, limit: int = 30) -> List[Dict[str, Any]]:
    """离线技能/buff 库子串搜索 (名字或 id 前缀)；从不整表外发。
    buff 结果带 is_buff=True → UI 绑到 detect.buff_ids (点名机制全是 buff)。"""
    q = _s(query)
    if not q:
        return []
    nm = _name_resolver()
    if nm is None:
        return []
    results: List[Dict[str, Any]] = []
    seen = set()
    q_lower = q.lower()
    q_is_num = q.isdigit()
    # 技能表在前(命中优先), buff 表在后 — buff 名最多, 限额避免淹没技能
    for kind in _CATALOG_KINDS + _CATALOG_BUFF_KINDS:
        is_buff = kind in _CATALOG_BUFF_KINDS
        try:
            table = nm.kind_map(kind) or {}
        except Exception:
            continue
        for sid, name in table.items():
            try:
                sid_i = int(sid)
            except Exception:
                continue
            if sid_i in seen:
                continue
            name_s = _s(name)
            hit = (q_lower in name_s.lower()) or (q_is_num and str(sid_i).startswith(q))
            if not hit:
                continue
            seen.add(sid_i)
            results.append({"id": sid_i, "name": name_s, "kind": kind, "is_buff": is_buff})
            if len(results) >= max(1, int(limit)):
                return results
    return results
