# -*- coding: utf-8 -*-
"""Boss Raid timeline engine — profile model, state machine, DPS tracking."""

import copy
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from typing import Any, Callable, Dict, List, Optional, Tuple

from config import BASE_DIR

from perf_probe import probe as _probe

BOSS_RAID_SCHEMA_VERSION = 1
DEFAULT_BOSS_RAID_SERVER_URL = "http://47.82.157.220:9320"
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
    if minimum is not None:
        result = max(minimum, result)
    if maximum is not None:
        result = min(maximum, result)
    return result


def _string(value: Any) -> str:
    return str(value or "").strip()


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


def make_default_phase_trigger() -> Dict[str, Any]:
    return {
        "type": "manual",   # manual | time | dps_total | hp_pct | breaking | buff_event | shield_broken | overdrive | extinction_pct | breaking_stage
        "value": 0,
    }


def normalize_phase_trigger(raw: Any) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    trigger_type = _string(src.get("type")).lower()
    if trigger_type not in ("manual", "time", "dps_total", "hp_pct", "breaking", "buff_event",
                            "shield_broken", "overdrive", "extinction_pct", "breaking_stage"):
        trigger_type = "manual"
    return {
        "type": trigger_type,
        "value": _coerce_float(src.get("value"), 0.0, 0.0),
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
        "simple_mode": True,
        "target_name_pattern": "",
        "phases": [make_default_phase(1)],
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
    return {
        "id": _string(base.get("id")) or default["id"],
        "schema_version": BOSS_RAID_SCHEMA_VERSION,
        "profile_name": _string(base.get("profile_name")) or default["profile_name"],
        "description": _string(base.get("description")),
        "boss_total_hp": _coerce_int(base.get("boss_total_hp"), 0, 0),
        "enrage_time_s": _coerce_int(base.get("enrage_time_s"), 600, 0, 86400),
        "simple_mode": _coerce_bool(base.get("simple_mode"), True),
        "target_name_pattern": _string(base.get("target_name_pattern")),
        "phases": phases,
        "source": profile_source,
        "remote_id": _string(base.get("remote_id")) or None,
        "created_at": _string(base.get("created_at")) or default["created_at"],
        "updated_at": _utc_now_iso(),
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


def load_boss_raid_config(settings, state_snapshot=None) -> Dict[str, Any]:
    return normalize_boss_raid_config(settings.get("boss_raid", {}),
                                       state_snapshot=state_snapshot)


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
    for phase in cloned.get("phases", []) or []:
        phase["id"] = _new_id("phase")
        for tl in phase.get("timelines", []) or []:
            tl["id"] = _new_id("tl")
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
        "source": profile.get("source", "local"),
        "remote_id": profile.get("remote_id"),
        "updated_at": profile.get("updated_at", ""),
    }


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
    for phase in profile.get("phases", []) or []:
        phase["id"] = _new_id("phase")
        for tl in phase.get("timelines", []) or []:
            tl["id"] = _new_id("tl")
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
                 on_entity_update: Optional[Callable[[List[Dict[str, Any]]], None]] = None):
        """
        Args:
            state_mgr: GameStateManager instance
            settings: SettingsManager instance
            on_alert: callback(title, message) for visual alert
            on_sound: callback(sound_name) for playing sound
            on_entity_update: callback([entity_dict, ...]) for visual editor entity list
        """
        self._state_mgr = state_mgr
        self._settings = settings
        self._on_alert = on_alert
        self._on_sound = on_sound
        self._on_entity_update_cb = on_entity_update

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

        # Thread
        self._running = False
        self._thread: Optional[threading.Thread] = None

    # ── Public API ──

    def start(self, profile: Optional[Dict[str, Any]] = None):
        """Start (or restart) a boss raid with the given profile."""
        with self._lock:
            if profile:
                self._profile = copy.deepcopy(profile)
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
            self._last_monster_data = None
            # Reset multi-entity tracking
            self._entities.clear()
            self._entity_order.clear()
            self._boss_manually_set = False

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
            self._entities.clear()
            self._entity_order.clear()
            self._boss_manually_set = False
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
            self._last_monster_data = None
            self._entities.clear()
            self._entity_order.clear()
            self._boss_manually_set = False
        self._push_game_state_clear()

    # ── Entity role management (for visual editor) ──

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
            target_is_monster = event.get("target_is_monster")
            attacker_is_self = event.get("attacker_is_self")
            is_immune = event.get("is_immune", False)
            is_absorbed = event.get("is_absorbed", False)
            damage = max(0, int(event.get("damage") or 0))

            if attacker_is_self and target_is_monster:
                target_uuid = int(event.get("target_uuid", 0))
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
                        'name': _string(event.get('target_name', '')),
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
                    if not (is_immune or is_absorbed) and not event.get('is_heal'):
                        ent['damage_dealt'] += damage
                        ent['hit_count'] += 1
                    if not ent.get('name'):
                        ent['name'] = _string(event.get('target_name', ''))

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
                    if not event.get("is_heal"):
                        self._total_damage += damage

                # Notify visual editor of entity list change
                self._fire_entity_update_locked()

    @_probe.decorate('boss.on_monster_update')
    def on_monster_update(self, monster_data: Dict[str, Any]):
        """Called from packet_parser monster update callback. Updates real boss HP/shield/breaking.

        Also updates multi-entity tracking for all known monsters.
        """
        if not monster_data:
            return
        with self._lock:
            if self._state != self.STATE_RUNNING:
                # Still track even when idle so we can show info
                pass
            uuid = int(monster_data.get("uuid", 0))
            hp = int(monster_data.get("hp") or 0)
            max_hp = int(monster_data.get("max_hp") or 0)

            # ── Update multi-entity tracking ──
            if uuid and uuid in self._entities:
                ent = self._entities[uuid]
                if max_hp > 0:
                    ent['hp'] = hp
                    ent['max_hp'] = max_hp
                ent['shield_active'] = bool(monster_data.get('shield_active'))
                ent['shield_pct'] = float(monster_data.get('shield_pct') or 0.0)
                ent['breaking_stage'] = int(monster_data.get('breaking_stage') or 0)
                ent['extinction_pct'] = float(monster_data.get('extinction_pct') or 0.0)
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
            if max_hp > 0:
                self._boss_hp = hp
                self._boss_max_hp = max_hp
            self._boss_breaking_stage = int(monster_data.get("breaking_stage") or 0)
            self._boss_extinction_pct = float(monster_data.get("extinction_pct") or 0.0)
            self._boss_in_overdrive = bool(monster_data.get("in_overdrive"))
            self._boss_shield_active = bool(monster_data.get("shield_active"))
            self._boss_shield_pct = float(monster_data.get("shield_pct") or 0.0)

    def on_boss_event(self, event: Dict[str, Any]):
        """Called from packet_parser boss event callback. Handles buff-based phase triggers."""
        if not event:
            return
        with self._lock:
            if self._state != self.STATE_RUNNING:
                return
            event_type = event.get("event_type", 0)

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

    def get_status(self) -> Dict[str, Any]:
        """Return current engine status dict."""
        with self._lock:
            return self._build_status_locked()

    # ── Internal ──

    def _build_status_locked(self) -> Dict[str, Any]:
        now = time.time()
        profile = self._profile or {}
        elapsed = (now - self._start_time) if self._state == self.STATE_RUNNING else 0.0
        phase_elapsed = (now - self._phase_start_time) if self._state == self.STATE_RUNNING else 0.0
        enrage_time = int(profile.get("enrage_time_s") or 0)
        enrage_remaining = max(0.0, enrage_time - elapsed) if enrage_time > 0 else 0.0
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
            "boss_hp_est_pct": round(boss_hp_pct, 4),
            "boss_total_hp": boss_total_hp,
            "boss_current_hp": boss_current_hp,
            "boss_hp_source": hp_source,
            "boss_uuid": self._boss_uuid,
            "boss_shield_active": self._boss_shield_active,
            "boss_shield_pct": round(self._boss_shield_pct, 4),
            "boss_breaking_stage": self._boss_breaking_stage,
            "boss_extinction_pct": round(self._boss_extinction_pct, 4),
            "boss_in_overdrive": self._boss_in_overdrive,
            "boss_invincible": self._boss_invincible,
            "entities": self._get_entities_locked(),
        }

    def _run_loop(self):
        while self._running:
            time.sleep(0.25)
            with self._lock:
                if self._state != self.STATE_RUNNING:
                    continue
                try:
                    self._tick_locked()
                except Exception as e:
                    print(f"[BossRaid] tick error: {e}")

    def _tick_locked(self):
        now = time.time()
        profile = self._profile
        if not profile:
            return

        elapsed = now - self._start_time
        phase_elapsed = now - self._phase_start_time

        # ── Enrage check ──
        enrage_time = int(profile.get("enrage_time_s") or 0)
        if enrage_time > 0 and elapsed >= enrage_time:
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
        phase = phases[self._current_phase_idx]
        phase_name = _string(phase.get("name")) or f"P{self._current_phase_idx + 1}"
        self._fire_alert_unlocked("Boss Raid", f"→ {phase_name}")
        self._fire_sound_unlocked("boss_phase")

    def _push_game_state_locked(self, now: float):
        """Push boss raid fields to GameStateManager (called under lock)."""
        status = self._build_status_locked()
        enrage_rem = status["enrage_remaining_s"]
        if enrage_rem > 0:
            mins = int(enrage_rem) // 60
            secs = int(enrage_rem) % 60
            timer_text = f"{mins}:{secs:02d}"
        else:
            elapsed = status["elapsed_s"]
            mins = int(elapsed) // 60
            secs = int(elapsed) % 60
            timer_text = f"{mins}:{secs:02d}"

        self._state_mgr.update(
            boss_raid_active=True,
            boss_raid_phase=status["phase_idx"],
            boss_raid_phase_name=status["phase_name"],
            boss_enrage_remaining=enrage_rem,
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
        )

    def _push_game_state_clear(self):
        """Clear boss raid fields from GameState."""
        self._state_mgr.update(
            boss_raid_active=False,
            boss_raid_phase=0,
            boss_raid_phase_name='',
            boss_enrage_remaining=0.0,
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
