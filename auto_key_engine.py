# -*- coding: utf-8 -*-
"""Auto-key script model, storage helpers, and runtime engine."""

import copy
import ctypes
import ctypes.wintypes
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from typing import Any, Callable, Dict, List, Optional, Tuple

from config import BASE_DIR, GAME_PROCESS_NAMES
from window_locator import WindowLocator, _get_process_name

AUTO_KEY_SCHEMA_VERSION = 1
DEFAULT_AUTO_KEY_SERVER_URL = ""
AUTO_KEY_EXPORT_DIR = os.path.join(BASE_DIR, "exports", "auto_keys")
AUTO_KEY_IMPORT_DIR = BASE_DIR

DEFAULT_ACTION_KEY_MAP = {idx: str(idx) for idx in range(1, 10)}

user32 = ctypes.windll.user32

INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", ctypes.wintypes.WORD),
        ("wScan", ctypes.wintypes.WORD),
        ("dwFlags", ctypes.wintypes.DWORD),
        ("time", ctypes.wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class INPUT_UNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", ctypes.wintypes.DWORD), ("u", INPUT_UNION)]


VK_NAME_MAP = {
    "SPACE": 0x20,
    "TAB": 0x09,
    "ENTER": 0x0D,
    "ESC": 0x1B,
    "SHIFT": 0x10,
    "CTRL": 0x11,
    "ALT": 0x12,
}
for _idx in range(10):
    VK_NAME_MAP[str(_idx)] = 0x30 + _idx
for _idx in range(26):
    _char = chr(ord("A") + _idx)
    VK_NAME_MAP[_char] = ord(_char)
for _idx in range(1, 13):
    VK_NAME_MAP[f"F{_idx}"] = 0x6F + _idx


def _utc_now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _slugify_filename(text: str) -> str:
    value = (text or "auto_key_profile").strip().replace(" ", "_")
    out = []
    for ch in value:
        if ch.isalnum() or ch in ("-", "_"):
            out.append(ch)
        elif "\u4e00" <= ch <= "\u9fff":
            out.append(ch)
        else:
            out.append("_")
    cleaned = "".join(out).strip("_")
    return cleaned or "auto_key_profile"


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


def _coerce_int(value: Any, default: int = 0, minimum: Optional[int] = None, maximum: Optional[int] = None) -> int:
    try:
        result = int(value)
    except Exception:
        result = int(default)
    if minimum is not None:
        result = max(minimum, result)
    if maximum is not None:
        result = min(maximum, result)
    return result


def _coerce_float(value: Any, default: float = 0.0, minimum: Optional[float] = None, maximum: Optional[float] = None) -> float:
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


def _mask_token(token: str) -> str:
    token = _string(token)
    if not token:
        return ""
    if len(token) <= 8:
        return "*" * len(token)
    return f"{token[:4]}...{token[-4:]}"


def normalize_author_snapshot(author: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "player_uid": _string(author.get("player_uid") or author.get("uid")),
        "player_name": _string(author.get("player_name") or author.get("name")),
        "profession_id": _coerce_int(author.get("profession_id"), 0, 0),
        "profession_name": _string(author.get("profession_name") or author.get("profession")),
    }


def snapshot_author_from_state(gs) -> Dict[str, Any]:
    return normalize_author_snapshot({
        "player_uid": getattr(gs, "player_id", ""),
        "player_name": getattr(gs, "player_name", ""),
        "profession_id": getattr(gs, "profession_id", 0),
        "profession_name": getattr(gs, "profession_name", ""),
    })


def build_identity_state(author_snapshot: Optional[Dict[str, Any]] = None, source: str = "unknown") -> Dict[str, Any]:
    author = normalize_author_snapshot(author_snapshot or {})
    missing = []
    if not author["player_uid"]:
        missing.append("player_uid")
    if not author["player_name"]:
        missing.append("player_name")
    if int(author["profession_id"] or 0) <= 0:
        missing.append("profession_id")
    return {
        "player_uid": author["player_uid"],
        "player_name": author["player_name"],
        "profession_id": int(author["profession_id"] or 0),
        "profession_name": author["profession_name"],
        "source": _string(source) or "unknown",
        "ready": not missing,
        "missing": missing,
    }


def default_upload_auth_state() -> Dict[str, Any]:
    return {
        "token": "",
        "ready": False,
        "token_masked": "",
        "expires_at": "",
        "error": "",
        "mode": "",
        "identity": build_identity_state({}, source="unknown"),
    }


def normalize_upload_auth_state(raw: Any, identity_state: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    token = _string(src.get("token"))
    identity = src.get("identity") or identity_state or {}
    identity_source = _string((src.get("identity") or {}).get("source")) or _string((identity_state or {}).get("source")) or "unknown"
    return {
        "ready": bool(src.get("ready")) and bool(token),
        "token_masked": _string(src.get("token_masked")) or _mask_token(token),
        "expires_at": _string(src.get("expires_at")),
        "error": _string(src.get("error")),
        "mode": _string(src.get("mode")),
        "identity": build_identity_state(identity, source=identity_source),
    }


def make_default_action(slot_index: int = 1) -> Dict[str, Any]:
    slot_index = _coerce_int(slot_index, 1, 1, 9)
    return {
        "id": _new_id("action"),
        "label": f"Action {slot_index}",
        "enabled": True,
        "slot_index": slot_index,
        "key": DEFAULT_ACTION_KEY_MAP.get(slot_index, str(slot_index)),
        "press_mode": "tap",
        "press_count": 1,
        "press_interval_ms": 40,
        "hold_ms": 80,
        "ready_delay_ms": 0,
        "min_rearm_ms": 800,
        "post_delay_ms": 120,
        "conditions": [],
    }


def make_default_profile(author_snapshot: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    author = normalize_author_snapshot(author_snapshot or {})
    return {
        "id": _new_id("profile"),
        "schema_version": AUTO_KEY_SCHEMA_VERSION,
        "profile_name": "New Auto Key Profile",
        "description": "",
        "profession_id": _coerce_int(author.get("profession_id"), 0, 0),
        "profession_name": _string(author.get("profession_name")),
        "source": "local",
        "remote_id": None,
        "created_at": _utc_now_iso(),
        "updated_at": _utc_now_iso(),
        "author_snapshot": author,
        "engine": {
            "tick_ms": 50,
            "require_foreground": True,
            "pause_on_death": True,
        },
        "actions": [make_default_action(1)],
    }


def normalize_condition(raw: Any) -> Optional[Dict[str, Any]]:
    if not isinstance(raw, dict):
        return None
    condition_type = _string(raw.get("type")).lower()
    if not condition_type:
        return None
    normalized: Dict[str, Any] = {"type": condition_type}
    if condition_type in ("hp_pct_gte", "hp_pct_lte", "sta_pct_gte"):
        normalized["value"] = _coerce_float(raw.get("value"), 0.0, 0.0, 1.0)
    elif condition_type == "burst_ready_is":
        normalized["value"] = _coerce_bool(raw.get("value"), False)
    elif condition_type == "slot_state_is":
        normalized["slot_index"] = _coerce_int(raw.get("slot_index"), 0, 0, 9)
        normalized["state"] = _string(raw.get("state")).lower() or "ready"
    elif condition_type in ("profession_is", "player_name_is"):
        normalized["value"] = _string(raw.get("value"))
    else:
        return None
    return normalized


def normalize_action(raw: Any, fallback_slot: int = 1) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    slot_index = _coerce_int(src.get("slot_index"), fallback_slot, 1, 9)
    press_mode = _string(src.get("press_mode")).lower()
    if press_mode not in ("tap", "hold"):
        press_mode = "tap"
    conditions = []
    for item in src.get("conditions", []) if isinstance(src.get("conditions"), list) else []:
        normalized = normalize_condition(item)
        if normalized:
            conditions.append(normalized)
    return {
        "id": _string(src.get("id")) or _new_id("action"),
        "label": _string(src.get("label")) or f"Action {slot_index}",
        "enabled": _coerce_bool(src.get("enabled"), True),
        "slot_index": slot_index,
        "key": (_string(src.get("key")) or DEFAULT_ACTION_KEY_MAP.get(slot_index, str(slot_index))).upper(),
        "press_mode": press_mode,
        "press_count": _coerce_int(src.get("press_count"), 1, 1, 20),
        "press_interval_ms": _coerce_int(src.get("press_interval_ms"), 40, 0, 10_000),
        "hold_ms": _coerce_int(src.get("hold_ms"), 80, 0, 10_000),
        "ready_delay_ms": _coerce_int(src.get("ready_delay_ms"), 0, 0, 60_000),
        "min_rearm_ms": _coerce_int(src.get("min_rearm_ms"), 800, 0, 120_000),
        "post_delay_ms": _coerce_int(src.get("post_delay_ms"), 120, 0, 120_000),
        "conditions": conditions,
    }


def normalize_profile(raw: Any, author_snapshot: Optional[Dict[str, Any]] = None, source: Optional[str] = None) -> Dict[str, Any]:
    base = raw if isinstance(raw, dict) else {}
    default_profile = make_default_profile(author_snapshot)
    profile_source = _string(source or base.get("source") or default_profile["source"]).lower()
    if profile_source not in ("local", "downloaded", "uploaded"):
        profile_source = "local"
    author = normalize_author_snapshot(base.get("author_snapshot") or author_snapshot or {})
    profession_name = _string(base.get("profession_name")) or author.get("profession_name", "")
    profession_id = _coerce_int(base.get("profession_id") or author.get("profession_id"), 0, 0)
    actions = []
    actions_raw = base.get("actions")
    if isinstance(actions_raw, list):
        for idx, item in enumerate(actions_raw, start=1):
            actions.append(normalize_action(item, idx))
    if not actions:
        actions = [make_default_action(1)]
    engine = base.get("engine") if isinstance(base.get("engine"), dict) else {}
    return {
        "id": _string(base.get("id")) or default_profile["id"],
        "schema_version": AUTO_KEY_SCHEMA_VERSION,
        "profile_name": _string(base.get("profile_name")) or default_profile["profile_name"],
        "description": _string(base.get("description")),
        "profession_id": profession_id,
        "profession_name": profession_name,
        "source": profile_source,
        "remote_id": _string(base.get("remote_id")) or None,
        "created_at": _string(base.get("created_at")) or default_profile["created_at"],
        "updated_at": _utc_now_iso(),
        "author_snapshot": author,
        "engine": {
            "tick_ms": _coerce_int(engine.get("tick_ms"), 50, 10, 1000),
            "require_foreground": _coerce_bool(engine.get("require_foreground"), True),
            "pause_on_death": _coerce_bool(engine.get("pause_on_death"), True),
        },
        "actions": actions,
    }


def default_auto_key_config() -> Dict[str, Any]:
    return {
        "enabled": False,
        "active_profile_id": "",
        "server_url": DEFAULT_AUTO_KEY_SERVER_URL,
        "profiles": [],
        "last_remote_search": {
            "query": {
                "q": "",
                "profile_name": "",
                "player_uid": "",
                "player_name": "",
                "profession_name": "",
                "page": 1,
                "page_size": 20,
            },
            "results": [],
            "error": "",
            "fetched_at": "",
        },
    }


def normalize_auto_key_config(raw: Any, state_snapshot: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    author = normalize_author_snapshot(state_snapshot or {})
    result = default_auto_key_config()
    result["enabled"] = _coerce_bool(src.get("enabled"), False)
    result["active_profile_id"] = _string(src.get("active_profile_id"))
    result["server_url"] = _string(src.get("server_url")) or DEFAULT_AUTO_KEY_SERVER_URL
    profiles = []
    for item in src.get("profiles", []) if isinstance(src.get("profiles"), list) else []:
        profiles.append(normalize_profile(item, author_snapshot=author))
    result["profiles"] = profiles
    if result["active_profile_id"] and not any(p["id"] == result["active_profile_id"] for p in profiles):
        result["active_profile_id"] = ""
    if not result["active_profile_id"] and profiles:
        result["active_profile_id"] = profiles[0]["id"]
    search = src.get("last_remote_search") if isinstance(src.get("last_remote_search"), dict) else {}
    query = search.get("query") if isinstance(search.get("query"), dict) else {}
    result["last_remote_search"] = {
        "query": {
            "q": _string(query.get("q")),
            "profile_name": _string(query.get("profile_name")),
            "player_uid": _string(query.get("player_uid")),
            "player_name": _string(query.get("player_name")),
            "profession_name": _string(query.get("profession_name")),
            "page": _coerce_int(query.get("page"), 1, 1),
            "page_size": _coerce_int(query.get("page_size"), 20, 1, 100),
        },
        "results": list(search.get("results") or []),
        "error": _string(search.get("error")),
        "fetched_at": _string(search.get("fetched_at")),
    }
    return result


def load_auto_key_config(settings, state_snapshot: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    return normalize_auto_key_config(settings.get("auto_key", {}), state_snapshot=state_snapshot)


def save_auto_key_config(settings, config: Dict[str, Any]) -> Dict[str, Any]:
    normalized = normalize_auto_key_config(config)
    settings.set("auto_key", normalized)
    settings.save()
    return normalized


def find_profile(config: Dict[str, Any], profile_id: str) -> Optional[Dict[str, Any]]:
    profile_id = _string(profile_id)
    if not profile_id:
        return None
    for profile in config.get("profiles", []) or []:
        if _string(profile.get("id")) == profile_id:
            return profile
    return None


def active_profile(config: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    return find_profile(config, config.get("active_profile_id", ""))


def upsert_profile(config: Dict[str, Any], profile: Dict[str, Any], activate: bool = False) -> Dict[str, Any]:
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
    profile_id = _string(profile_id)
    profiles = [item for item in list(config.get("profiles", []) or []) if _string(item.get("id")) != profile_id]
    config["profiles"] = profiles
    if _string(config.get("active_profile_id")) == profile_id:
        config["active_profile_id"] = profiles[0]["id"] if profiles else ""
    return config


def clone_profile(config: Dict[str, Any], profile_id: str, author_snapshot: Optional[Dict[str, Any]] = None) -> Optional[Dict[str, Any]]:
    profile = find_profile(config, profile_id)
    if not profile:
        return None
    cloned = copy.deepcopy(profile)
    cloned["id"] = _new_id("profile")
    cloned["profile_name"] = f'{cloned.get("profile_name") or "Profile"} Copy'
    cloned["source"] = "local"
    cloned["remote_id"] = None
    cloned["created_at"] = _utc_now_iso()
    cloned["updated_at"] = _utc_now_iso()
    if author_snapshot:
        cloned["author_snapshot"] = normalize_author_snapshot(author_snapshot)
    for action in cloned.get("actions", []) or []:
        action["id"] = _new_id("action")
    upsert_profile(config, normalize_profile(cloned, author_snapshot=author_snapshot), activate=False)
    return cloned


def summarize_profile(profile: Dict[str, Any]) -> Dict[str, Any]:
    actions = list(profile.get("actions", []) or [])
    return {
        "id": profile.get("id"),
        "profile_name": profile.get("profile_name"),
        "description": profile.get("description"),
        "profession_id": profile.get("profession_id", 0),
        "profession_name": profile.get("profession_name", ""),
        "source": profile.get("source", "local"),
        "remote_id": profile.get("remote_id"),
        "updated_at": profile.get("updated_at", ""),
        "action_count": len(actions),
        "enabled_action_count": len([item for item in actions if _coerce_bool(item.get("enabled"), True)]),
    }


def build_auto_key_state(config: Dict[str, Any], engine_status: Optional[Dict[str, Any]] = None,
                         identity_snapshot: Optional[Dict[str, Any]] = None,
                         upload_auth: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    active = active_profile(config)
    identity_state = build_identity_state(
        identity_snapshot or {},
        source=_string((identity_snapshot or {}).get("source")) or "unknown",
    )
    return {
        "enabled": _coerce_bool(config.get("enabled"), False),
        "active_profile_id": _string(config.get("active_profile_id")),
        "active_profile_name": _string((active or {}).get("profile_name")),
        "profiles": [summarize_profile(item) for item in list(config.get("profiles", []) or [])],
        "profiles_full": copy.deepcopy(list(config.get("profiles", []) or [])),
        "active_profile": copy.deepcopy(active) if active else None,
        "local_profile_count": len(list(config.get("profiles", []) or [])),
        "server_url": _string(config.get("server_url")) or DEFAULT_AUTO_KEY_SERVER_URL,
        "identity": identity_state,
        "upload_auth": normalize_upload_auth_state(upload_auth or {}, identity_state=identity_state),
        "last_remote_search": copy.deepcopy(config.get("last_remote_search") or {}),
        "runtime": copy.deepcopy(engine_status or {}),
    }


def export_profile_json(profile: Dict[str, Any]) -> str:
    payload = {"schema_version": AUTO_KEY_SCHEMA_VERSION, "profile": copy.deepcopy(profile)}
    return json.dumps(payload, ensure_ascii=False, indent=2)


def ensure_export_dir() -> str:
    os.makedirs(AUTO_KEY_EXPORT_DIR, exist_ok=True)
    return AUTO_KEY_EXPORT_DIR


def export_profile_to_default_path(profile: Dict[str, Any]) -> str:
    export_dir = ensure_export_dir()
    stamp = time.strftime("%Y%m%d_%H%M%S", time.localtime())
    filename = f'{_slugify_filename(profile.get("profile_name"))}_{stamp}.json'
    path = os.path.join(export_dir, filename)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(export_profile_json(profile))
    return path


def import_profile_from_path(path: str, author_snapshot: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    profile_data = data.get("profile") if isinstance(data, dict) else {}
    if not profile_data and isinstance(data, dict):
        profile_data = data
    profile = normalize_profile(profile_data, author_snapshot=author_snapshot, source="local")
    profile["id"] = _new_id("profile")
    profile["remote_id"] = None
    profile["source"] = "local"
    profile["created_at"] = _utc_now_iso()
    profile["updated_at"] = _utc_now_iso()
    for action in profile.get("actions", []) or []:
        action["id"] = _new_id("action")
    return profile


class AutoKeyCloudClient:
    def __init__(self, base_url: str, timeout: float = 5.0):
        self.base_url = (base_url or DEFAULT_AUTO_KEY_SERVER_URL).rstrip("/")
        self.timeout = float(timeout)

    def _request(self, method: str, path: str, query: Optional[Dict[str, Any]] = None,
                 body: Optional[Dict[str, Any]] = None, headers: Optional[Dict[str, str]] = None) -> Dict[str, Any]:
        url = f"{self.base_url}{path}"
        if query:
            clean_query = {k: v for k, v in query.items() if v not in (None, "", [])}
            if clean_query:
                url = f"{url}?{urllib.parse.urlencode(clean_query)}"
        payload_bytes = None
        req_headers = {"Accept": "application/json"}
        if headers:
            req_headers.update(headers)
        if body is not None:
            payload_bytes = json.dumps(body, ensure_ascii=False).encode("utf-8")
            req_headers["Content-Type"] = "application/json; charset=utf-8"
        request = urllib.request.Request(url, data=payload_bytes, headers=req_headers, method=method.upper())
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                raw = response.read().decode("utf-8")
                return json.loads(raw) if raw else {}
        except urllib.error.HTTPError as exc:
            text = exc.read().decode("utf-8", errors="ignore")
            try:
                parsed = json.loads(text) if text else {}
            except Exception:
                parsed = {}
            raise RuntimeError(parsed.get("detail") or text or f"HTTP {exc.code}")
        except urllib.error.URLError as exc:
            raise RuntimeError(str(exc.reason))

    def search_scripts(self, query: Dict[str, Any]) -> Dict[str, Any]:
        return self._request("GET", "/api/scripts", query=query)

    def get_script(self, script_id: Any) -> Dict[str, Any]:
        return self._request("GET", f"/api/scripts/{script_id}")

    def issue_upload_token(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        return self._request("POST", "/api/upload-token/issue", body=payload)

    def upload_script(self, payload: Dict[str, Any], upload_token: str) -> Dict[str, Any]:
        headers = {"X-SAO-Upload-Token": upload_token or ""}
        return self._request("POST", "/api/scripts", body=payload, headers=headers)


class AutoKeyEngine:
    def __init__(self, state_mgr, settings, extra_gate: Optional[Callable[[], bool]] = None):
        self._state_mgr = state_mgr
        self._settings = settings
        self._extra_gate = extra_gate
        self._locator = WindowLocator()
        self._running = False
        self._thread = None
        self._next_loop_at = 0.0
        self._ready_since: Dict[Tuple[str, str], float] = {}
        self._last_fire_at: Dict[Tuple[str, str], float] = {}
        self._status_lock = threading.Lock()
        self._status: Dict[str, Any] = {
            "active": False,
            "active_profile_id": "",
            "active_profile_name": "",
            "last_action_id": "",
            "last_action_label": "",
            "last_fire_at": 0.0,
            "last_reason": "idle",
        }

    def start(self):
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True, name="auto_key_engine")
        self._thread.start()

    def stop(self):
        self._running = False
        thread = self._thread
        if thread and thread.is_alive():
            thread.join(timeout=1.0)
        self._thread = None

    def invalidate(self):
        self._ready_since.clear()
        self._last_fire_at.clear()
        self._next_loop_at = 0.0

    def get_status(self) -> Dict[str, Any]:
        with self._status_lock:
            return copy.deepcopy(self._status)

    def _set_status(self, **kwargs):
        with self._status_lock:
            self._status.update(kwargs)

    def _run(self):
        while self._running:
            time.sleep(0.01)
            now = time.time()
            if now < self._next_loop_at:
                continue
            try:
                self._tick(now)
            except Exception as exc:
                self._set_status(last_reason=f"error: {exc}")

    def _tick(self, now: float):
        gs = self._state_mgr.state
        config = load_auto_key_config(self._settings, state_snapshot=snapshot_author_from_state(gs))
        profile = active_profile(config)
        tick_ms = _coerce_int((((profile or {}).get("engine") or {}).get("tick_ms")), 50, 10, 1000)
        self._next_loop_at = now + (tick_ms / 1000.0)

        enabled = _coerce_bool(config.get("enabled"), False)
        if self._extra_gate and not self._extra_gate():
            self._set_status(active=False, last_reason="gate-blocked")
            return
        if not enabled or not profile:
            self._set_status(active=False, active_profile_id="", active_profile_name="", last_reason="disabled")
            return

        self._set_status(
            active=True,
            active_profile_id=profile.get("id", ""),
            active_profile_name=profile.get("profile_name", ""),
        )
        engine_cfg = profile.get("engine") or {}
        if _coerce_bool(engine_cfg.get("pause_on_death"), True) and self._is_dead(gs):
            self._set_status(last_reason="dead")
            return
        if not bool(getattr(gs, "recognition_ok", False)):
            self._set_status(last_reason="recognition-off")
            return
        if _coerce_bool(engine_cfg.get("require_foreground"), True) and not self._is_game_foreground():
            self._set_status(last_reason="background")
            return

        slot_map = self._slot_map(gs)
        for action in list(profile.get("actions", []) or []):
            if not _coerce_bool(action.get("enabled"), True):
                continue
            if self._action_ready(profile, action, gs, slot_map, now):
                self._fire_action(profile, action, now)
                post_delay = _coerce_int(action.get("post_delay_ms"), 120, 0) / 1000.0
                self._next_loop_at = max(self._next_loop_at, time.time() + post_delay)
                return
        self._set_status(last_reason="idle")

    def _is_dead(self, gs) -> bool:
        try:
            hp_max = int(getattr(gs, "hp_max", 0) or 0)
            hp_current = int(getattr(gs, "hp_current", 0) or 0)
            hp_pct = float(getattr(gs, "hp_pct", 1.0) or 0.0)
        except Exception:
            return False
        return hp_max > 0 and hp_current <= 0 and hp_pct <= 0.001

    def _is_game_foreground(self) -> bool:
        try:
            hwnd = user32.GetForegroundWindow()
            if not hwnd:
                return False
            exe = _get_process_name(hwnd)
            if exe and exe in [item.lower() for item in GAME_PROCESS_NAMES]:
                return True
            found = self._locator.find_game_window()
            return bool(found and int(found[0]) == int(hwnd))
        except Exception:
            return False

    def _slot_map(self, gs) -> Dict[int, Dict[str, Any]]:
        mapping: Dict[int, Dict[str, Any]] = {}
        for slot in getattr(gs, "skill_slots", []) or []:
            if not isinstance(slot, dict):
                continue
            idx = _coerce_int(slot.get("index"), 0, 0)
            if idx > 0:
                mapping[idx] = slot
        return mapping

    def _normalized_slot_state(self, slot: Optional[Dict[str, Any]]) -> str:
        if not slot:
            return "unknown"
        state = _string(slot.get("state")).lower()
        if state in ("ready", "active", "cooldown", "unknown", "insufficient_energy"):
            return state
        return "ready" if self._slot_is_ready(slot) else "cooldown"

    def _slot_is_ready(self, slot: Optional[Dict[str, Any]]) -> bool:
        if not slot:
            return False
        state = _string(slot.get("state")).lower()
        if state in ("ready", "active"):
            return True
        if _coerce_bool(slot.get("active"), False):
            return True
        if _coerce_int(slot.get("charge_count"), 0, 0) > 0:
            return True
        if _coerce_int(slot.get("remaining_ms"), 999999) <= 120:
            return True
        return _coerce_float(slot.get("cooldown_pct"), 1.0, 0.0, 1.0) <= 0.02

    def _conditions_match(self, action: Dict[str, Any], gs, slot_map: Dict[int, Dict[str, Any]]) -> bool:
        for condition in list(action.get("conditions", []) or []):
            cond_type = _string(condition.get("type")).lower()
            if cond_type == "hp_pct_gte":
                if float(getattr(gs, "hp_pct", 0.0) or 0.0) < _coerce_float(condition.get("value"), 0.0, 0.0, 1.0):
                    return False
            elif cond_type == "hp_pct_lte":
                if float(getattr(gs, "hp_pct", 1.0) or 0.0) > _coerce_float(condition.get("value"), 1.0, 0.0, 1.0):
                    return False
            elif cond_type == "sta_pct_gte":
                if float(getattr(gs, "stamina_pct", 0.0) or 0.0) < _coerce_float(condition.get("value"), 0.0, 0.0, 1.0):
                    return False
            elif cond_type == "burst_ready_is":
                if bool(getattr(gs, "burst_ready", False)) != _coerce_bool(condition.get("value"), False):
                    return False
            elif cond_type == "slot_state_is":
                slot_index = _coerce_int(condition.get("slot_index"), action.get("slot_index", 0), 0, 9)
                if self._normalized_slot_state(slot_map.get(slot_index)) != (_string(condition.get("state")).lower() or "ready"):
                    return False
            elif cond_type == "profession_is":
                if _string(getattr(gs, "profession_name", "")) != _string(condition.get("value")):
                    return False
            elif cond_type == "player_name_is":
                if _string(getattr(gs, "player_name", "")) != _string(condition.get("value")):
                    return False
        return True

    def _action_ready(self, profile: Dict[str, Any], action: Dict[str, Any], gs,
                      slot_map: Dict[int, Dict[str, Any]], now: float) -> bool:
        slot_index = _coerce_int(action.get("slot_index"), 0, 1, 9)
        slot = slot_map.get(slot_index)
        key = (profile.get("id", ""), action.get("id", ""))
        if not self._slot_is_ready(slot):
            self._ready_since.pop(key, None)
            return False
        self._ready_since.setdefault(key, now)
        ready_delay = _coerce_int(action.get("ready_delay_ms"), 0, 0) / 1000.0
        if ready_delay > 0 and (now - self._ready_since.get(key, now)) < ready_delay:
            return False
        last_fire = self._last_fire_at.get(key, 0.0)
        min_rearm = _coerce_int(action.get("min_rearm_ms"), 0, 0) / 1000.0
        if min_rearm > 0 and last_fire > 0 and (now - last_fire) < min_rearm:
            return False
        return self._conditions_match(action, gs, slot_map)

    def _send_vk(self, vk: int, key_up: bool = False):
        extra = ctypes.c_ulong(0)
        ki = KEYBDINPUT(wVk=int(vk), wScan=0, dwFlags=KEYEVENTF_KEYUP if key_up else 0,
                        time=0, dwExtraInfo=ctypes.pointer(extra))
        event = INPUT(type=INPUT_KEYBOARD, ki=ki)
        ctypes.windll.user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT))

    def _resolve_vk(self, key_name: str) -> int:
        key_name = _string(key_name).upper()
        if key_name in VK_NAME_MAP:
            return VK_NAME_MAP[key_name]
        if len(key_name) == 1 and key_name.isalpha():
            return ord(key_name)
        raise ValueError(f"Unsupported key: {key_name}")

    def _fire_action(self, profile: Dict[str, Any], action: Dict[str, Any], now: float):
        vk = self._resolve_vk(action.get("key"))
        press_mode = _string(action.get("press_mode")).lower()
        press_count = _coerce_int(action.get("press_count"), 1, 1, 20)
        interval = _coerce_int(action.get("press_interval_ms"), 40, 0, 10_000) / 1000.0
        hold_s = _coerce_int(action.get("hold_ms"), 80, 0, 10_000) / 1000.0
        for idx in range(press_count):
            self._send_vk(vk, key_up=False)
            time.sleep(hold_s if press_mode == "hold" and hold_s > 0 else 0.015)
            self._send_vk(vk, key_up=True)
            if idx < press_count - 1 and interval > 0:
                time.sleep(interval)
        action_key = (profile.get("id", ""), action.get("id", ""))
        self._last_fire_at[action_key] = now
        self._set_status(
            last_action_id=action.get("id", ""),
            last_action_label=action.get("label", ""),
            last_fire_at=now,
            last_reason="fired",
        )


__all__ = [
    "AUTO_KEY_SCHEMA_VERSION",
    "DEFAULT_AUTO_KEY_SERVER_URL",
    "AUTO_KEY_EXPORT_DIR",
    "AUTO_KEY_IMPORT_DIR",
    "AutoKeyCloudClient",
    "AutoKeyEngine",
    "active_profile",
    "build_auto_key_state",
    "build_identity_state",
    "clone_profile",
    "default_auto_key_config",
    "default_upload_auth_state",
    "delete_profile",
    "ensure_export_dir",
    "export_profile_to_default_path",
    "find_profile",
    "import_profile_from_path",
    "load_auto_key_config",
    "make_default_action",
    "make_default_profile",
    "normalize_action",
    "normalize_auto_key_config",
    "normalize_profile",
    "normalize_upload_auth_state",
    "save_auto_key_config",
    "snapshot_author_from_state",
    "upsert_profile",
]
