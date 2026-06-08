# -*- coding: utf-8 -*-
"""Boss Raid → Auto Key linkage controller.

Listens to boss raid phase/timeline events and triggers mapped auto-key actions.
Supports cooldown, dedup, execution lock, and debug logging.
"""

import copy
import json
import threading
import time
import uuid
from typing import Any, Callable, Dict, List, Optional

# ════════════════════════════════════════
#  Helpers
# ════════════════════════════════════════

def _s(v: Any) -> str:
    return str(v or "").strip()


def _bool(v: Any, default: bool = False) -> bool:
    if isinstance(v, bool):
        return v
    if isinstance(v, str):
        t = v.strip().lower()
        return t in ("1", "true", "yes", "on")
    return bool(v) if isinstance(v, (int, float)) else default


def _float(v: Any, default: float = 0.0, lo: Optional[float] = None,
           hi: Optional[float] = None) -> float:
    try:
        r = float(v)
    except Exception:
        r = default
    if lo is not None:
        r = max(lo, r)
    if hi is not None:
        r = min(hi, r)
    return r


def _int(v: Any, default: int = 0, lo: Optional[int] = None,
         hi: Optional[int] = None) -> int:
    try:
        r = int(v)
    except Exception:
        r = default
    if lo is not None:
        r = max(lo, r)
    if hi is not None:
        r = min(hi, r)
    return r


def _new_id() -> str:
    return f"lnk_{uuid.uuid4().hex[:12]}"


# ════════════════════════════════════════
#  Data model
# ════════════════════════════════════════

TRIGGER_TYPES = (
    # legacy, text-classified by on_boss_raid_alert (kept for back-compat):
    "phase_enter",      # boss raid enters a phase matching trigger_match
    "timeline_alert",   # timeline fires an alert matching trigger_match
    "breaking",         # boss enters breaking state (alert text)
    "enrage",           # enrage timer reached
    # memory-driven, skill_id-accurate, fired by on_boss_action:
    "boss_cast",        # boss begins casting skill_id (auto-dodge — the reliable signal)
    "boss_breaking",    # offensive window: boss enters/changes breaking stage
    "boss_overdrive",   # offensive window: boss enters overdrive
    "boss_stun",        # offensive window: boss stunned
)

# trigger types driven by the mem boss-action feed (not the alert text path)
MEM_TRIGGER_TYPES = ("boss_cast", "boss_breaking", "boss_overdrive", "boss_stun")


def _norm_sequence(raw: Any) -> list:
    out = []
    for step in (raw or []):
        if not isinstance(step, dict):
            continue
        k = _s(step.get("key")).upper()
        if not k:
            continue
        out.append({"key": k, "delay_ms": _int(step.get("delay_ms"), 0, 0, 10000)})
    return out


def make_default_mapping() -> Dict[str, Any]:
    return {
        "id": _new_id(),
        "enabled": True,
        "trigger_type": "boss_cast",
        "trigger_match": "",     # legacy text match (phase_enter/timeline_alert)
        "skill_id": 0,           # boss_cast: 0 = any skill, else exact match
        "boss_base_id": 0,       # 0 = any boss, else scope to this boss template
        "delay_ms": 0,           # react N ms AFTER the edge (late press)
        "lead_ms": 0,            # react N ms BEFORE cast_end (needs cast_duration)
        "action_key": "",        # key to press (e.g. "1", "Q", "SPACE")
        "action_label": "",      # human-readable label
        "press_mode": "tap",     # tap | hold
        "hold_ms": 80,
        "press_count": 1,
        "sequence": [],          # [{key, delay_ms}] offensive combo; non-empty overrides press_count
        "cooldown_s": 3.0,
    }


def normalize_mapping(raw: Any) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    trigger = _s(src.get("trigger_type")).lower()
    if trigger not in TRIGGER_TYPES:
        trigger = "phase_enter"
    press_mode = _s(src.get("press_mode")).lower()
    if press_mode not in ("tap", "hold"):
        press_mode = "tap"
    return {
        "id": _s(src.get("id")) or _new_id(),
        "enabled": _bool(src.get("enabled"), True),
        "trigger_type": trigger,
        "trigger_match": _s(src.get("trigger_match")),
        "skill_id": _int(src.get("skill_id"), 0, 0),
        "boss_base_id": _int(src.get("boss_base_id"), 0, 0),
        "delay_ms": _int(src.get("delay_ms"), 0, 0, 60000),
        "lead_ms": _int(src.get("lead_ms"), 0, 0, 60000),
        "action_key": _s(src.get("action_key")).upper(),
        "action_label": _s(src.get("action_label")),
        "press_mode": press_mode,
        "hold_ms": _int(src.get("hold_ms"), 80, 0, 10000),
        "press_count": _int(src.get("press_count"), 1, 1, 20),
        "sequence": _norm_sequence(src.get("sequence")),
        "cooldown_s": _float(src.get("cooldown_s"), 3.0, 0.0, 600.0),
    }


def default_linkage_config() -> Dict[str, Any]:
    return {
        "enabled": False,
        "global_cooldown_s": 1.0,
        "debug_log": False,
        "mappings": [],
    }


def normalize_linkage_config(raw: Any) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    mappings = []
    for item in (src.get("mappings") or []):
        if isinstance(item, dict):
            mappings.append(normalize_mapping(item))
    return {
        "enabled": _bool(src.get("enabled"), False),
        "global_cooldown_s": _float(src.get("global_cooldown_s"), 1.0, 0.0, 60.0),
        "debug_log": _bool(src.get("debug_log"), False),
        "mappings": mappings,
    }


def load_linkage_config(settings) -> Dict[str, Any]:
    return normalize_linkage_config(settings.get("boss_autokey_linkage", {}))


def save_linkage_config(settings, config: Dict[str, Any]) -> Dict[str, Any]:
    normalized = normalize_linkage_config(config)
    settings.set("boss_autokey_linkage", normalized)
    settings.save()
    return normalized


def build_linkage_state(config: Dict[str, Any],
                        engine_status: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    return {
        "enabled": _bool(config.get("enabled"), False),
        "global_cooldown_s": config.get("global_cooldown_s", 1.0),
        "debug_log": _bool(config.get("debug_log"), False),
        "mappings": copy.deepcopy(config.get("mappings", [])),
        "runtime": copy.deepcopy(engine_status or {}),
    }


# ════════════════════════════════════════
#  Runtime Controller
# ════════════════════════════════════════

class BossAutoKeyLinkage:
    """Bridges boss raid events to auto-key actions.

    Wraps the on_alert callback of the boss raid engine to intercept
    phase/timeline events and fire mapped keystrokes.
    """

    def __init__(self, settings,
                 send_key: Optional[Callable[[str, str, int, int], None]] = None,
                 on_log: Optional[Callable[[str], None]] = None):
        """
        Args:
            settings: SettingsManager instance
            send_key: callable(key, press_mode, hold_ms, press_count) → fires a keystroke
            on_log: callable(message) → debug log output
        """
        self._settings = settings
        self._send_key = send_key
        self._on_log = on_log

        self._lock = threading.Lock()
        self._last_fire: Dict[str, float] = {}   # mapping_id → last fire time
        self._global_last_fire: float = 0.0
        self._fire_count: int = 0

    def on_boss_raid_alert(self, title: str, message: str):
        """Called when boss raid engine fires an alert (phase change, timeline, enrage, etc.).

        Determines if the alert matches any configured mapping and fires the action.
        """
        config = load_linkage_config(self._settings)
        if not _bool(config.get("enabled"), False):
            return

        trigger_type, trigger_label = self._classify_alert(title, message)
        if not trigger_type:
            return

        self._debug(config, f"[Linkage] Alert: type={trigger_type} label='{trigger_label}' msg='{message}'")

        with self._lock:
            now = time.time()
            global_cd = _float(config.get("global_cooldown_s"), 1.0)
            if global_cd > 0 and (now - self._global_last_fire) < global_cd:
                self._debug(config, f"[Linkage] Skipped: global cooldown ({global_cd}s)")
                return

            for mapping in config.get("mappings", []):
                if not _bool(mapping.get("enabled"), True):
                    continue
                if _s(mapping.get("trigger_type")) != trigger_type:
                    continue
                match_pattern = _s(mapping.get("trigger_match"))
                if match_pattern and match_pattern.lower() not in trigger_label.lower():
                    continue
                key = _s(mapping.get("action_key"))
                if not key:
                    continue

                # Cooldown check
                mid = _s(mapping.get("id"))
                per_cooldown = _float(mapping.get("cooldown_s"), 3.0)
                last = self._last_fire.get(mid, 0.0)
                if per_cooldown > 0 and (now - last) < per_cooldown:
                    self._debug(config, f"[Linkage] Skipped '{mapping.get('action_label', key)}': cooldown ({per_cooldown}s)")
                    continue

                # Fire
                self._last_fire[mid] = now
                self._global_last_fire = now
                self._fire_count += 1

                press_mode = _s(mapping.get("press_mode")) or "tap"
                hold_ms = _int(mapping.get("hold_ms"), 80)
                press_count = _int(mapping.get("press_count"), 1)
                label = _s(mapping.get("action_label")) or key

                self._debug(config, f"[Linkage] FIRE: '{label}' key={key} mode={press_mode} count={press_count}")

                if self._send_key:
                    threading.Thread(
                        target=self._send_key,
                        args=(key, press_mode, hold_ms, press_count),
                        daemon=True,
                    ).start()
                break  # Only fire first matching mapping per alert

    def on_boss_action(self, action: Dict[str, Any]):
        """Mem-feed driven, skill_id-accurate boss reaction. Parallel to
        on_boss_raid_alert; reuses the same cooldown/dedup/global-cooldown machinery.

        Fires `boss_cast` on a cast-start edge (matched by skill_id, scoped by
        boss_base_id) and the offensive windows `boss_breaking`/`boss_overdrive`/
        `boss_stun` only on the rising edge (the engine sets *_edge flags)."""
        config = load_linkage_config(self._settings)
        if not _bool(config.get("enabled"), False):
            return
        if not isinstance(action, dict):
            return

        base_id = _int(action.get("boss_base_id"), 0)
        skill_id = _int(action.get("skill_id"), 0)
        cast_edge = _s(action.get("cast_edge")).lower()
        cast_dur = action.get("cast_duration_ms")

        fired_types = []
        # fire on any cast-start edge, including instant skills (skill_id 0, e.g. a
        # counterattack); skill_id matching below still scopes per-skill mappings.
        if cast_edge == "start":
            fired_types.append("boss_cast")
        if action.get("breaking_edge"):
            fired_types.append("boss_breaking")
        if action.get("overdrive_edge"):
            fired_types.append("boss_overdrive")
        if action.get("stun_edge"):
            fired_types.append("boss_stun")
        if not fired_types:
            return

        with self._lock:
            now = time.time()
            global_cd = _float(config.get("global_cooldown_s"), 1.0)
            if global_cd > 0 and (now - self._global_last_fire) < global_cd:
                return
            for trig_type in fired_types:
                for mapping in config.get("mappings", []):
                    if not _bool(mapping.get("enabled"), True):
                        continue
                    if _s(mapping.get("trigger_type")) != trig_type:
                        continue
                    m_base = _int(mapping.get("boss_base_id"), 0)
                    if m_base and base_id and m_base != base_id:
                        continue
                    if trig_type == "boss_cast":
                        m_skill = _int(mapping.get("skill_id"), 0)
                        if m_skill and m_skill != skill_id:
                            continue
                    key = _s(mapping.get("action_key"))
                    seq = mapping.get("sequence") or []
                    if not key and not seq:
                        continue
                    mid = _s(mapping.get("id"))
                    per_cd = _float(mapping.get("cooldown_s"), 3.0)
                    if per_cd > 0 and (now - self._last_fire.get(mid, 0.0)) < per_cd:
                        continue
                    self._last_fire[mid] = now
                    self._global_last_fire = now
                    self._fire_count += 1
                    self._debug(config, f"[Linkage] BOSS_ACTION fire: type={trig_type} "
                                         f"skill={skill_id} base={base_id} "
                                         f"label='{mapping.get('action_label') or key}'")
                    self._dispatch_mapping(mapping, cast_dur)
                    return   # one fire per action

    def _dispatch_mapping(self, mapping: Dict[str, Any], cast_dur: Any):
        """Spawn the key send on a thread, honoring delay_ms / lead_ms / sequence."""
        delay_ms = _int(mapping.get("delay_ms"), 0)
        lead_ms = _int(mapping.get("lead_ms"), 0)
        wait_s = max(0.0, delay_ms / 1000.0)
        if lead_ms and isinstance(cast_dur, (int, float)) and cast_dur > 0:
            wait_s = max(0.0, (float(cast_dur) - lead_ms) / 1000.0)
        seq = list(mapping.get("sequence") or [])
        key = _s(mapping.get("action_key"))
        press_mode = _s(mapping.get("press_mode")) or "tap"
        hold_ms = _int(mapping.get("hold_ms"), 80)
        press_count = _int(mapping.get("press_count"), 1)
        send = self._send_key
        if not send:
            return

        def _run():
            try:
                if wait_s > 0:
                    time.sleep(wait_s)
                if seq:
                    for step in seq:
                        d = _int(step.get("delay_ms"), 0)
                        if d > 0:
                            time.sleep(d / 1000.0)
                        sk = _s(step.get("key"))
                        if sk:
                            send(sk, "tap", 0, 1)
                elif key:
                    send(key, press_mode, hold_ms, press_count)
            except Exception:
                pass

        threading.Thread(target=_run, daemon=True).start()

    def _classify_alert(self, title: str, message: str):
        """Determine trigger_type and label from an alert."""
        msg = _s(message)
        if not msg:
            return None, ""

        # Phase enter: "→ P2" or "▶ Profile — START"
        if msg.startswith("→ "):
            return "phase_enter", msg[2:].strip()
        if msg.startswith("▶ "):
            return "phase_enter", msg[2:].strip()

        # Enrage
        if "ENRAGE" in msg.upper():
            return "enrage", msg

        # Completed
        if "COMPLETED" in msg.upper():
            return None, ""  # Don't trigger on completion

        # Timeline alert — anything else from "Boss Raid" title
        if _s(title).lower() in ("boss raid",):
            return "timeline_alert", msg

        return None, ""

    def get_status(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "total_fires": self._fire_count,
                "last_global_fire": self._global_last_fire,
            }

    def reset(self):
        with self._lock:
            self._last_fire.clear()
            self._global_last_fire = 0.0
            self._fire_count = 0

    def _debug(self, config: Dict[str, Any], msg: str):
        if _bool(config.get("debug_log"), False) and self._on_log:
            try:
                self._on_log(msg)
            except Exception:
                pass


# ════════════════════════════════════════
#  Boss Reactions editor (shared by Tk + WebView)
# ════════════════════════════════════════

# ── name resolution (authoritative live cache → static tables, pure dict lookup) ──

# skill ids can live in any of several name tables (a boss cast / mechanic / monster
# skill); probe them in order and take the first real hit.
_SKILL_NAME_KINDS = ("skill", "boss_skill", "boss_mechanic_skill",
                     "monster_skill", "ultimate_skill", "scripted_skill")


def _name_resolver():
    try:
        from tools.tablekit.name_tables import names
        return names
    except Exception:
        return None


def _lookup(nm, kind: str, id_: int) -> str:
    if not nm or not id_:
        return ""
    fn = getattr(nm, kind, None)
    if not callable(fn):
        return ""
    try:
        return (fn(id_, default="") or "").strip()
    except Exception:
        return ""


def _resolve_skill_name(nm, sid: int) -> str:
    for kind in _SKILL_NAME_KINDS:
        v = _lookup(nm, kind, sid)
        if v:
            return v
    return ""


def _resolve_boss_name(nm, bid: int) -> str:
    return _lookup(nm, "boss", bid) or _lookup(nm, "monster", bid)


def _resolve_scene_name(nm, scene_id: int, dungeon_id: int) -> str:
    return _lookup(nm, "dungeon", dungeon_id) or _lookup(nm, "dungeon", scene_id)


def _build_boss_detail(sel_boss: int, boss_name: str, obs: list) -> Dict[str, Any]:
    """Group one boss's observations into the aggregation views the editor renders:
    casts (the buffs it applies), mechanics/states, and a time-ordered timeline.
    Plus a per-boss-unit summary."""
    skills = [o for o in obs if o.get("kind") == "skill"]
    mechanics = [o for o in obs if o.get("kind") in ("mechanic", "state")]
    timeline = []
    for o in obs:
        at = o.get("time_fixed_s")
        fixed = at is not None
        if at is None:
            at = o.get("elapsed_s")
        if at is None:
            continue
        timeline.append({
            "id": _int(o.get("id"), 0),
            "skill_id": _int(o.get("skill_id") or o.get("id"), 0),
            "name": _s(o.get("name")), "kind": _s(o.get("kind")),
            "at_s": round(float(at), 1), "is_fixed": bool(fixed),
            "count": _int(o.get("count"), 0), "tags": list(o.get("tags") or []),
            "hp_line_pct": o.get("hp_line_pct"),
        })
    timeline.sort(key=lambda r: r["at_s"])
    durs = [int(o["last_cast_duration_ms"]) for o in skills if o.get("last_cast_duration_ms")]
    hp_lines = sorted({round(float(o["hp_line_pct"]), 4) for o in obs
                       if o.get("hp_line_pct") is not None}, reverse=True)
    summary = {
        "skill_count": len(skills),
        "mechanic_count": len(mechanics),
        "hp_line_count": len(hp_lines),
        "hp_lines": hp_lines,
        "approx_duration_ms": max(durs) if durs else None,
        "total_observed": sum(_int(o.get("count"), 0) for o in obs),
    }
    return {"base_id": int(sel_boss), "name": boss_name, "summary": summary,
            "skills": skills, "mechanics": mechanics, "timeline": timeline}


def build_boss_reactions_state(settings, engine, state_mgr,
                               scene_key: Any = None,
                               boss_base_id: Any = None) -> Dict[str, Any]:
    """Single data contract for the dual-UI Boss Reactions editor.

    Assembles the linkage mappings + the engine's live cast state + the persisted
    per-scene/per-boss observed skills & mechanics (tagged, name-resolved) grouped
    into casts/mechanics/timeline aggregation views, so the Tk panel and the
    WebView raid editor render identically. `scene_key` selects which map/scene to
    browse (None = live); `boss_base_id` selects which boss (None = first/live).

    Names (scene / boss / skill) are resolved at read time from the authoritative
    name cache, so historical observations recorded before a name was known still
    display correctly. Observations are scoped to the SELECTED boss only (not every
    boss in the scene) — combined with the entity-free status read, this is what
    keeps opening the editor cheap in crowded raids."""
    cfg = load_linkage_config(settings)
    nm = _name_resolver()
    gs = getattr(state_mgr, "state", None) if state_mgr else None
    cur_scene_id = _int(getattr(gs, "dungeon_scene_id", 0), 0) if gs else 0
    cur_dungeon_id = _int(getattr(gs, "dungeon_id", 0), 0) if gs else 0
    cur_scene_name = _s(getattr(gs, "dungeon_name", "")) if gs else ""
    cur_scene_key = str(cur_scene_id or cur_dungeon_id or 0)
    selected = str(scene_key) if scene_key not in (None, "") else cur_scene_key

    try:
        scenes = [dict(s) for s in (engine.get_observed_scenes() if engine else [])]
    except Exception:
        scenes = []
    for s in scenes:
        if not _s(s.get("name")):
            s["name"] = _resolve_scene_name(nm, _int(s.get("scene_id"), 0),
                                            _int(s.get("dungeon_id"), 0))
    # always surface the live scene so the user sees where they are, even pre-obs
    if cur_scene_key and cur_scene_key not in {str(s.get("scene_key")) for s in scenes}:
        scenes = [{"scene_key": cur_scene_key, "scene_id": cur_scene_id,
                   "dungeon_id": cur_dungeon_id,
                   "name": cur_scene_name or _resolve_scene_name(nm, cur_scene_id, cur_dungeon_id),
                   "boss_count": 0, "last_seen": 0.0}] + scenes
    try:
        bosses = [dict(b) for b in (engine.get_observed_bosses(selected) if engine else [])]
    except Exception:
        bosses = []
    for b in bosses:
        stored = _s(b.get("name"))
        resolved = _resolve_boss_name(nm, _int(b.get("base_id"), 0))
        if resolved:
            b["name"] = resolved          # authoritative (live cache → table) wins
        elif stored.isdigit():
            b["name"] = ""                # bogus uuid-ish stored name → UI falls back to #base_id
    try:
        status = engine.get_status(include_entities=False) if engine else {}
    except TypeError:
        status = engine.get_status() if engine else {}   # engine predates the kwarg
    except Exception:
        status = {}

    # selected boss: explicit → first recorded → live target
    sel_boss = _int(boss_base_id, 0)
    if not sel_boss and bosses:
        sel_boss = _int(bosses[0].get("base_id"), 0)
    if not sel_boss:
        sel_boss = _int(status.get("boss_base_id"), 0)
    observed_skills: Dict[str, Any] = {}
    boss_detail = None
    if engine is not None and sel_boss:
        try:
            obs_map = engine.get_observed_boss_skills(sel_boss, selected) or {}
            obs = list(obs_map.get(sel_boss) or obs_map.get(str(sel_boss)) or [])
        except Exception:
            obs = []
        for o in obs:
            if o.get("kind") == "skill" and not _s(o.get("name")):
                o["name"] = _resolve_skill_name(nm, _int(o.get("id"), 0))
        bname = next((_s(b.get("name")) for b in bosses
                      if _int(b.get("base_id"), 0) == sel_boss), "") \
            or _resolve_boss_name(nm, sel_boss)
        observed_skills = {str(sel_boss): obs}
        boss_detail = _build_boss_detail(sel_boss, bname, obs)

    # Availability is gated on the user's SELECTED data-source mode, not the live
    # gs.data_source (which GameState never populates — the old check was always
    # False, hiding recorded skills behind the "switch to hybrid" banner). Boss
    # skills are now recorded in BOTH tcp (buff_list diff) and hybrid/memory feeds,
    # so the editor is usable whenever recognition is running (engine present).
    def _setting(key):
        try:
            return str(settings.get(key) or "")
        except Exception:
            return ""
    data_source = (_setting("mem_data_source") or _setting("data_source") or "tcp").lower()
    mem_available = bool(engine is not None and data_source in ("tcp", "memory", "hybrid", "auto"))
    mappings = cfg.get("mappings", [])
    cast_name = _s(status.get("boss_cast_skill_name")) \
        or _resolve_skill_name(nm, _int(status.get("boss_cast_skill_id"), 0))
    current = {
        "uuid": str(_int(status.get("boss_uuid"), 0)),
        "base_id": _int(status.get("boss_base_id"), 0),
        "cast_skill_id": _int(status.get("boss_cast_skill_id"), 0),
        "cast_skill_name": cast_name,
        "cast_active": _bool(status.get("boss_cast_active"), False),
        "breaking_stage": status.get("boss_breaking_stage"),
        "in_overdrive": _bool(status.get("boss_in_overdrive"), False),
        "stun": _bool(status.get("boss_stun"), False),
        "hp_pct": round(_float(status.get("boss_hp_est_pct"), 0.0), 4),
    }
    return {
        "ok": True,
        "enabled": _bool(cfg.get("enabled"), False),
        "global_cooldown_s": cfg.get("global_cooldown_s", 1.0),
        "data_source": data_source or "tcp",
        "mem_available": mem_available,
        "self_dead": _bool(status.get("self_dead"), False),
        "current_scene": {"scene_key": cur_scene_key, "scene_id": cur_scene_id,
                          "dungeon_id": cur_dungeon_id,
                          "name": cur_scene_name or _resolve_scene_name(nm, cur_scene_id, cur_dungeon_id)},
        "selected_scene_key": selected,
        "selected_boss_base_id": int(sel_boss),
        "scenes": scenes,
        "current_boss": current,
        "bosses": bosses,
        "observed_skills": observed_skills,
        "boss_detail": boss_detail,
        "mappings": mappings,
        "offensive_windows": [m for m in mappings
                              if _s(m.get("trigger_type")) in ("boss_breaking", "boss_overdrive", "boss_stun")],
    }


def upsert_mapping(settings, mapping: Any) -> Dict[str, Any]:
    """Insert or replace one linkage mapping (by id) and persist. Returns config."""
    cfg = load_linkage_config(settings)
    m = normalize_mapping(mapping)
    mappings = cfg.get("mappings", [])
    for i, ex in enumerate(mappings):
        if ex.get("id") == m["id"]:
            mappings[i] = m
            break
    else:
        mappings.append(m)
    cfg["mappings"] = mappings
    return save_linkage_config(settings, cfg)


def delete_mapping(settings, mapping_id: str) -> Dict[str, Any]:
    cfg = load_linkage_config(settings)
    cfg["mappings"] = [m for m in cfg.get("mappings", []) if m.get("id") != str(mapping_id)]
    return save_linkage_config(settings, cfg)


__all__ = [
    "BossAutoKeyLinkage",
    "build_linkage_state",
    "build_boss_reactions_state",
    "upsert_mapping",
    "delete_mapping",
    "default_linkage_config",
    "load_linkage_config",
    "make_default_mapping",
    "normalize_linkage_config",
    "normalize_mapping",
    "save_linkage_config",
]
