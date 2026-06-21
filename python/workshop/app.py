# -*- coding: utf-8 -*-
"""SAO Creative Workshop — pywebview GUI application.

Launch:
    python -m workshop.app            # standalone
    XiaoACTUI.exe --workshop          # from frozen app

From Plugin Manager, the "创意工坊" tab calls ``launch()`` which opens
the pywebview window in a subprocess (Tk owns main thread).
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import threading
from typing import Any, Dict, List, Optional

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_WINDOW_TITLE = "SAO Creative Workshop"
_MIN_SIZE = (800, 520)
_POS_FILE = os.path.join(os.path.expanduser("~"), ".sao", "workshop_pos.json")

_running_window = None


def _html_path() -> str:
    candidates = [
        os.path.join(_ROOT, "web", "workshop.html"),
    ]
    if getattr(sys, "frozen", False):
        base = os.path.dirname(sys.executable)
        candidates.insert(0, os.path.join(base, "web", "workshop.html"))
        candidates.insert(1, os.path.join(base, "runtime", "web", "workshop.html"))
    for c in candidates:
        if os.path.isfile(c):
            return c
    return candidates[0]


def _load_window_pos() -> dict:
    try:
        with open(_POS_FILE, "r") as f:
            return json.load(f)
    except Exception:
        return {}


def _save_window_pos(x: int, y: int, w: int, h: int):
    try:
        os.makedirs(os.path.dirname(_POS_FILE), exist_ok=True)
        with open(_POS_FILE, "w") as f:
            json.dump({"x": x, "y": y, "width": w, "height": h}, f)
    except Exception:
        pass


def _get_server_url() -> str:
    try:
        cfg_path = os.path.join(_ROOT, "dev_publish_config.json")
        if os.path.isfile(cfg_path):
            with open(cfg_path, "r", encoding="utf-8") as f:
                data = json.load(f)
            host = data.get("host", "").strip()
            if host:
                return host
    except Exception:
        pass
    try:
        from config import WORKSHOP_SERVER_URL
        return WORKSHOP_SERVER_URL
    except Exception:
        pass
    return "http://doi.sakisense.top:15018"


class WorkshopAPI:
    """Python backend exposed to JS via ``window.pywebview.api.*``."""

    def __init__(self, gui_ref: Any = None):
        self._gui_ref = gui_ref
        self._window = None
        self._client = None

    def set_window(self, window, geo: dict | None = None):
        self._window = window

    def _ensure_client(self):
        if self._client is None:
            from workshop.client import WorkshopClient
            self._client = WorkshopClient(
                base_url=_get_server_url(),
                api_key=_get_api_key(),
                is_paid=_is_paid_user(),
            )
        return self._client

    def load_config(self) -> Dict:
        return {
            "server": _get_server_url(),
            "version": _get_app_version(),
            "has_api_key": bool(_get_api_key()),
            "is_paid": _is_paid_user(),
        }

    def browse(self, game_id: str = "", tag: str = "", search: str = "",
               page: int = 1, per_page: int = 40) -> Dict:
        try:
            return self._ensure_client().catalog(
                game_id=game_id, search=search, tag=tag,
                page=page, per_page=per_page,
            )
        except Exception as e:
            return {"ok": False, "error": str(e), "plugins": [], "total": 0}

    def detail(self, plugin_id: str) -> Dict:
        try:
            return self._ensure_client().detail(plugin_id)
        except Exception as e:
            return {"ok": False, "error": str(e)}

    def install(self, plugin_id: str, version: str = "") -> Dict:
        try:
            client = self._ensure_client()
            detail = client.detail(plugin_id)
            plugin_meta = detail.get("plugin", {})
            sha256 = plugin_meta.get("sha256", "")

            with tempfile.TemporaryDirectory() as tmp:
                zip_path = client.download(
                    plugin_id, tmp,
                    version=version,
                    expected_sha256=sha256,
                )
                try:
                    from act_platform.runtime import act_plugin_import
                    result = act_plugin_import(self._gui_ref, zip_path)
                    return result if isinstance(result, dict) else {"ok": True}
                except ImportError:
                    from act_platform.plugin_install import install_plugin_archive
                    user_plugins = os.path.join(_ROOT, "user_plugins")
                    os.makedirs(user_plugins, exist_ok=True)
                    result = install_plugin_archive(zip_path, user_plugins)
                    return result
        except Exception as e:
            return {"ok": False, "error": str(e)}

    def get_installed(self) -> List[str]:
        try:
            from act_platform.plugins import PluginManager
            mgr = PluginManager.instance()
            if mgr:
                return [r.plugin_id for r in mgr.list_plugins()]
        except Exception:
            pass
        ids = []
        for base in (os.path.join(_ROOT, "plugins"), os.path.join(_ROOT, "user_plugins")):
            if not os.path.isdir(base):
                continue
            for name in os.listdir(base):
                pdir = os.path.join(base, name)
                if os.path.isfile(os.path.join(pdir, "plugin.json")):
                    ids.append(name)
        return ids

    def get_game_list(self) -> List[Dict]:
        try:
            resp = self._ensure_client().catalog(per_page=1)
            game_ids = resp.get("game_ids", {})
            return [{"id": k, "count": v} for k, v in sorted(game_ids.items(), key=lambda x: -x[1])]
        except Exception:
            return []

    def open_in_editor(self, plugin_id: str) -> Dict:
        try:
            from ai_editor.app import launch as ai_launch
            plugin_path = ""
            for base in (os.path.join(_ROOT, "user_plugins"), os.path.join(_ROOT, "plugins")):
                candidate = os.path.join(base, plugin_id)
                if os.path.isdir(candidate):
                    plugin_path = candidate
                    break
            if not plugin_path:
                return {"ok": False, "error": f"plugin {plugin_id} not found locally"}
            ai_launch(gui_ref=self._gui_ref)
            return {"ok": True, "path": plugin_path}
        except Exception as e:
            return {"ok": False, "error": str(e)}

    def upload(self, zip_path: str, metadata: Dict) -> Dict:
        try:
            client = self._ensure_client()
            if not client.api_key:
                return {"ok": False, "error": "未配置 API Key，无法上传"}
            return client.publish(zip_path, metadata)
        except Exception as e:
            return {"ok": False, "error": str(e)}

    def get_local_plugins(self) -> List[Dict]:
        results = []
        for base in (os.path.join(_ROOT, "user_plugins"), os.path.join(_ROOT, "plugins")):
            if not os.path.isdir(base):
                continue
            for name in sorted(os.listdir(base)):
                pdir = os.path.join(base, name)
                mf = os.path.join(pdir, "plugin.json")
                if not os.path.isfile(mf):
                    continue
                try:
                    with open(mf, "r", encoding="utf-8") as f:
                        data = json.load(f)
                    results.append({
                        "plugin_id": data.get("id", name),
                        "name": data.get("name", name),
                        "version": data.get("version", "0.0.0"),
                        "description": data.get("description", ""),
                        "game_ids": data.get("game_ids", []),
                        "path": pdir,
                    })
                except Exception:
                    results.append({"plugin_id": name, "name": name, "version": "?", "path": pdir})
        return results

    def pick_file(self) -> str:
        try:
            result = self._window.create_file_dialog(
                dialog_type=1,
                allow_multiple=False,
                file_types=("Zip Archives (*.zip)",),
            )
            if result and len(result) > 0:
                return str(result[0])
        except Exception:
            pass
        return ""

    def close_window(self):
        if self._window:
            try:
                _save_window_pos(self._window.x, self._window.y,
                                 self._window.width, self._window.height)
                self._window.destroy()
            except Exception:
                pass


def _get_hwid() -> str:
    try:
        from license.hwid import collect_hwid
        return collect_hwid()
    except Exception:
        pass
    import hashlib, uuid
    return hashlib.sha256(str(uuid.getnode()).encode()).hexdigest()


def generate_plugin_id(plugin_name: str) -> str:
    import hashlib
    hwid = _get_hwid()[:16]
    name_hash = hashlib.sha256(plugin_name.strip().lower().encode("utf-8")).hexdigest()[:8]
    return f"{name_hash}-{hwid[:8]}"


def _get_api_key() -> str:
    try:
        cfg_path = os.path.join(_ROOT, "dev_publish_config.json")
        if os.path.isfile(cfg_path):
            with open(cfg_path, "r", encoding="utf-8") as f:
                data = json.load(f)
            return str(data.get("publish_api_key", "")).strip()
    except Exception:
        pass
    return os.environ.get("SAO_UPDATE_API_KEY", "").strip()


def _is_paid_user() -> bool:
    try:
        from license import get_license_manager
        return bool(get_license_manager().is_paid)
    except Exception:
        return False


def _get_app_version() -> str:
    try:
        from config import APP_VERSION
        return APP_VERSION
    except Exception:
        return ""


def launch(gui_ref: Any = None, blocking: bool = False) -> None:
    global _running_window

    if _running_window is not None:
        try:
            _running_window.show()
            return
        except Exception:
            _running_window = None

    if blocking:
        _launch_webview_blocking(gui_ref)
        return

    _launch_subprocess()


def _launch_subprocess() -> None:
    import subprocess as _sp

    html_file = _html_path()
    if not os.path.isfile(html_file):
        print(f"[Workshop] HTML not found: {html_file}")
        return

    if getattr(sys, "frozen", False):
        cmd = [sys.executable, "--workshop"]
    else:
        cmd = [sys.executable, "-m", "workshop.app"]

    env = dict(os.environ)
    env.setdefault("PYTHONPATH", _ROOT)
    env.setdefault("PYTHONUNBUFFERED", "1")
    cwd = os.path.dirname(sys.executable) if getattr(sys, "frozen", False) else _ROOT

    flags = 0
    if sys.platform == "win32":
        BELOW_NORMAL = 0x00004000
        CREATE_NEW_PROCESS_GROUP = 0x00000200
        flags = BELOW_NORMAL | CREATE_NEW_PROCESS_GROUP

    try:
        log_path = os.path.join(os.path.expanduser("~"), ".sao", "workshop_subprocess.log")
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        with open(log_path, "a", encoding="utf-8") as log:
            log.write(f"\n[Workshop] launch cmd={cmd!r} cwd={cwd!r}\n")
            log.flush()
            _sp.Popen(cmd, cwd=cwd, env=env, creationflags=flags,
                       stdout=log, stderr=log, close_fds=True)
    except Exception as exc:
        print(f"[Workshop] subprocess failed: {exc}")


def _launch_webview_blocking(gui_ref: Any = None) -> None:
    global _running_window
    try:
        import webview

        html_file = _html_path()
        if not os.path.isfile(html_file):
            print(f"[Workshop] HTML not found: {html_file}")
            return

        pos = _load_window_pos()
        w = int(pos.get("width", 1100))
        h = int(pos.get("height", 780))
        x = pos.get("x")
        y = pos.get("y")
        if isinstance(x, (int, float)):
            x = int(x)
        else:
            x = None
        if isinstance(y, (int, float)):
            y = int(y)
        else:
            y = None

        api = WorkshopAPI(gui_ref)
        url = f"file:///{html_file.replace(os.sep, '/')}"

        kwargs = dict(
            title=_WINDOW_TITLE,
            url=url,
            width=w,
            height=h,
            min_size=_MIN_SIZE,
            resizable=True,
            js_api=api,
            frameless=True,
            easy_drag=False,
            text_select=True,
        )
        if x is not None:
            kwargs["x"] = x
        if y is not None:
            kwargs["y"] = y

        window = webview.create_window(**kwargs)
        _running_window = window
        api.set_window(window)

        def _on_closed():
            global _running_window
            try:
                _save_window_pos(window.x, window.y, window.width, window.height)
            except Exception:
                pass
            _running_window = None

        window.events.closed += _on_closed
        webview.start(debug=False)
    except Exception as exc:
        print(f"[Workshop] launch failed: {exc}")
        raise


if __name__ == "__main__":
    launch(blocking=True)
