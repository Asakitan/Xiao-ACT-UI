"""
SAO-UI WebView GUI 

"""

import os
import sys
import time
import threading
import json
import copy
import ctypes
import math
import numpy as np
import logging
from pathlib import Path

logger = logging.getLogger(__name__)
from typing import Any, Dict, List, Optional

from act_platform.runtime import (
    act_action_log_copy,
    act_action_log_filter,
    act_action_log_jump_to_time,
    act_action_log_search,
    act_action_log_status,
    act_aggregate_status,
    act_mem_scope_status,
    act_mem_search,
    act_mem_search_status,
    act_mem_narrow,
    act_mem_search_cancel,
    act_mem_attr_map,
    act_combatant_drilldown_back,
    act_combatant_drilldown_filter,
    act_combatant_drilldown_focus_target,
    act_combatant_drilldown_status,
    act_data_source_diagnose,
    act_data_source_health,
    act_death_recap_copy,
    act_death_recap_status,
    act_graph_timeseries_export,
    act_graph_timeseries_filter,
    act_graph_timeseries_select_metric,
    act_graph_timeseries_status,
    act_graph_timeseries_zoom,
    act_history_delete,
    act_history_load,
    act_history_status,
    act_mini_parse_copy,
    act_mini_parse_preview,
    act_mini_parse_status,
    act_offline_import_file,
    act_offline_import_status,
    act_plugin_disable,
    act_plugin_enable,
    act_plugin_hotkeys,
    act_plugin_import,
    act_plugin_import_dialog,
    act_plugin_list,
    act_plugin_menu,
    act_plugin_pin,
    act_plugin_reload,
    act_plugin_set_hotkey,
    act_plugin_status,
    act_plugin_uninstall,
    act_render_apply_hooks,
    act_render_overlays,
    act_render_surfaces,
    act_report_copy,
    act_report_export,
    act_report_status,
    act_selective_parsing_clear,
    act_selective_parsing_status,
    act_selective_parsing_update,
    act_skill_drilldown_back,
    act_skill_drilldown_copy,
    act_skill_drilldown_filter,
    act_skill_drilldown_status,
    act_timeline_filter,
    act_timeline_pause,
    act_timeline_play,
    act_timeline_seek,
    act_timeline_set_speed,
    act_timeline_status,
    act_timeline_step,
    act_trigger_disable,
    act_trigger_enable,
    act_trigger_reload,
    act_trigger_status,
    act_trigger_test,
    ensure_act_event_bus,
    ensure_act_plugin_manager,
)
from config import (
    DEFAULT_HOTKEYS,
    HOTKEY_MOD_ALL_VKS,
    hotkey_mods_down,
    parse_hotkey,
    select_hotkey_match,
    WEB_DIR,
    resource_path,
)
try:
    from render.gpu_capture import capture_monitor_bgr_for_point, ensure_session, get_latest_bgr
except Exception:
    capture_monitor_bgr_for_point = None  # type: ignore
    ensure_session = None  # type: ignore
    get_latest_bgr = None  # type: ignore

# ── 延迟导入 pywebview ──
webview = None
_DOTNET_TRANSPARENCY_DONE = set()


def _ensure_webview():
    global webview
    if webview is None:
        import webview as wv
        webview = wv


def is_webview_available() -> bool:
    try:
        _ensure_webview()
        return True
    except ImportError:
        return False


def _get_icon_path() -> Optional[str]:
    icon_path = resource_path('icon.ico')
    return icon_path if os.path.exists(icon_path) else None


def _setting_bool_value(value: Any) -> bool:
    if isinstance(value, str):
        text = value.strip().lower()
        if text in {"0", "false", "off", "no", "disabled"}:
            return False
        if text in {"1", "true", "on", "yes", "enabled"}:
            return True
    return bool(value)


def _sound_volume_value(volume_pct: Any) -> int:
    try:
        value = float(volume_pct)
        if not math.isfinite(value):
            value = 70.0
    except Exception:
        value = 70.0
    return max(0, min(100, int(value)))



def _web_file_uri(filename: str) -> str:
    return Path(os.path.join(WEB_DIR, filename)).resolve().as_uri()


def _webview_extension_or_web_file_uri(owner: Any, filename: str) -> str:
    try:
        extension = getattr(owner, '_webview_extension', None)
        resolve = getattr(extension, 'resolve_web_uri', None)
        if callable(resolve):
            uri = resolve(filename)
            if uri:
                return str(uri)
    except Exception:
        pass
    return _web_file_uri(filename)


# Round 71 of sao_gui split refactor: _set_process_app_id deduplicated —
# moved into gui_modules.sao_panel_ui alongside the other Win32 helpers.
from gui_modules.sao_panel_ui import _set_process_app_id  # noqa: E402


# ════════════════════════════════════════════════
#  Win32 透明窗口工具
# ════════════════════════════════════════════════
_GWL_EXSTYLE = -20
_WS_EX_LAYERED = 0x00080000
_WS_EX_TRANSPARENT = 0x00000020
_LWA_COLORKEY = 0x00000001
_LWA_ALPHA = 0x00000002
_COLORREF_KEY = 0x00010001  # RGB(1,0,1) → COLORREF 0x00BBGGRR


def _make_transparent_ctypes(hwnd: int):
    """Win32 LWA_COLORKEY 透明 (ctypes 降级方案)"""
    try:
        u = ctypes.windll.user32
        ex = u.GetWindowLongW(hwnd, _GWL_EXSTYLE)
        u.SetWindowLongW(hwnd, _GWL_EXSTYLE, ex | _WS_EX_LAYERED)
        u.SetLayeredWindowAttributes(hwnd, _COLORREF_KEY, 0, _LWA_COLORKEY)
    except Exception as e:
        print(f"[SAO] ctypes transparency failed: {e}")


def _setup_dotnet_transparency(form):
    """用 .NET / WinForms 设置色键透明 + WebView2 透明背景.

    TransparencyKey 让颜色 rgb(1,0,1) 的区域桌面穿透;
    DefaultBackgroundColor=Transparent 让 WebView2 不遮盖 Form 背景.
    注意: 必须在 GUI 线程调用 (或通过 _invoke_dotnet_transparency 封装).
    """
    try:
        form_key = None
        try:
            form_key = int(form.Handle)
        except Exception:
            form_key = id(form)
        if form_key in _DOTNET_TRANSPARENCY_DONE:
            return True
        from System.Drawing import Color as DColor # type: ignore
        key = DColor.FromArgb(255, 1, 0, 1)
        form.BackColor = key
        form.TransparencyKey = key
        # WebView2 控件 — 找到并设置透明背景
        for i in range(form.Controls.Count):
            ctrl = form.Controls[i]
            if hasattr(ctrl, 'DefaultBackgroundColor'):
                ctrl.DefaultBackgroundColor = DColor.Transparent
                break
        _DOTNET_TRANSPARENCY_DONE.add(form_key)
        return True
    except Exception as e:
        print(f"[SAO] .NET transparency: {e}")
        return False


def _invoke_dotnet_transparency(win_obj, _retries_left=60):
    """从后台线程安全地在 GUI 线程设置 Form 色键透明.

    原理:
      HTML transparent 区域穿透到 Form 背景色.
      原本 Form BackColor = 白色 → 白底可见.
      设置 BackColor = TransparencyKey = rgb(1,0,1) 后,
      Form 背景变成 key color, Win32 COLORKEY 再将 key color 穿透到桌面.

    win_obj.native 是 pywebview BrowserForm 实例
    (winforms.py BrowserForm.__init__: self.pywebview_window.native = self).
    通过 form.Invoke 投递到 GUI 线程执行, 避免跨线程 .NET 访问死锁.

    冷启动时 WebView2 初始化可能需要 10-30 秒, native 和 Handle
    都不会立即就绪, 因此需要持续重试 (最多 ~30 秒).
    """
    try:
        form = getattr(win_obj, 'native', None)
        if form is None:
            # WebView2 尚未初始化, native 未赋值 — 持续重试
            if _retries_left > 0:
                t = threading.Timer(0.5, lambda: _invoke_dotnet_transparency(win_obj, _retries_left - 1))
                t.daemon = True
                t.start()
            return
        # 避免 "在创建窗口句柄之前，不能在控件上调用 Invoke" 错误
        if not form.IsHandleCreated:
            # 句柄尚未创建，延迟重试 (常见于 startup 阶段)
            if _retries_left > 0:
                t = threading.Timer(0.2, lambda: _invoke_dotnet_transparency(win_obj, _retries_left - 1))
                t.daemon = True
                t.start()
            return
        from System import Action # type: ignore
        form.Invoke(Action(lambda: _setup_dotnet_transparency(form)))
    except Exception as e:
        print(f"[SAO] invoke dotnet transparency: {e}")
# ════════════════════════════════════════════════
def _capture_fisheye_base64(strength: float = 0.25, quality: int = 60) -> Optional[str]:
    try:
        from PIL import Image
        import base64, io

        try:
            import mss
            with mss.mss() as sct:
                mon = sct.monitors[0]
                raw = sct.grab(mon)
                img = Image.frombytes('RGB', raw.size, raw.rgb)
        except Exception:
            from PIL import ImageGrab
            img = ImageGrab.grab()

        w, h = img.size
        scale = min(960 / w, 540 / h, 1.0)
        if scale < 1.0:
            img = img.resize((int(w * scale), int(h * scale)), Image.LANCZOS)
        w, h = img.size

        arr = np.array(img)
        cy, cx = h / 2, w / 2
        Y, X = np.mgrid[0:h, 0:w].astype(np.float32)
        X -= cx
        Y -= cy
        r = np.sqrt(X * X + Y * Y)
        max_r = np.sqrt(cx * cx + cy * cy)
        rn = r / max_r
        barrel = 1.0 + strength * rn * rn
        src_x = (X * barrel + cx).clip(0, w - 1).astype(np.int32)
        src_y = (Y * barrel + cy).clip(0, h - 1).astype(np.int32)
        out = arr[src_y, src_x]

        result = Image.fromarray(out)
        buf = io.BytesIO()
        result.save(buf, format='JPEG', quality=quality)
        b64 = base64.b64encode(buf.getvalue()).decode('ascii')
        return b64
    except Exception as e:
        print(f"[SAO] fisheye capture failed: {e}")
        return None


# ════════════════════════════════════════════════
#  Settings (共用)
# ════════════════════════════════════════════════
class SettingsManager:
    """Lightweight settings manager for webview mode.

    Uses atomic write (write-to-temp + os.replace) to prevent data loss
    when the process is killed via os._exit() during shutdown.
    """
    def __init__(self):
        if getattr(sys, 'frozen', False):
            base = os.path.dirname(sys.executable)
        else:
            base = os.path.dirname(os.path.abspath(__file__))
        self._path = os.path.join(base, 'settings.json')
        self._data = {}
        self._load()

    def _load(self):
        # Clean up stale temp files from interrupted atomic saves
        try:
            base = os.path.dirname(self._path)
            if base:
                for f in os.listdir(base):
                    if f.endswith(".tmp.json") and f.startswith("tmp"):
                        try:
                            os.remove(os.path.join(base, f))
                        except Exception:
                            pass
        except Exception:
            pass
        try:
            if os.path.exists(self._path):
                with open(self._path, 'r', encoding='utf-8') as f:
                    self._data = json.load(f)
        except Exception:
            self._data = {}

    def get(self, key, default=None):
        return self._data.get(key, default)

    def set(self, key, value):
        self._data[key] = value

    def save(self):
        """Atomic write: write to temp file, then os.replace to target.

        Prevents truncation when os._exit() kills the process mid-write.
        """
        try:
            import tempfile
            dir_name = os.path.dirname(self._path) or os.getcwd()
            with tempfile.NamedTemporaryFile(
                mode='w', dir=dir_name, delete=False,
                encoding='utf-8', suffix='.tmp.json'
            ) as tmp:
                json.dump(self._data, tmp, indent=2, ensure_ascii=False)
                tmp.flush()
                os.fsync(tmp.fileno())
                tmp_path = tmp.name
            os.replace(tmp_path, self._path)
        except Exception:
            # Clean up orphaned temp file if os.replace failed
            try:
                if tmp_path and os.path.exists(tmp_path):
                    os.remove(tmp_path)
            except Exception:
                pass
            # fallback to direct write
            try:
                with open(self._path, 'w', encoding='utf-8') as f:
                    json.dump(self._data, f, indent=2, ensure_ascii=False)
                    f.flush()
                    os.fsync(f.fileno())
            except Exception:
                pass


# ════════════════════════════════════════════════
#  JS API Bridge
# ════════════════════════════════════════════════
def _safe_timeline_speed(value: Any, fallback: float = 1.0) -> float:
    try:
        speed = float(value)
    except (TypeError, ValueError):
        speed = float(fallback)
    if not math.isfinite(speed):
        speed = float(fallback)
    return max(0.1, min(speed, 8.0))


class SAOWebAPI:
    """pywebview js_api — 暴露给 JavaScript 的 Python 接口."""

    def __init__(self, gui: 'SAOWebViewGUI'):
        self._g = gui

    def __getattr__(self, name: str):
        if not isinstance(name, str) or name.startswith('_'):
            raise AttributeError(name)
        extension = getattr(self._g, '_webview_extension', None)
        if not callable(getattr(extension, name, None)):
            raise AttributeError(name)

        def _forward(*args, **kwargs):
            call = getattr(self._g, '_webview_extension_call', None)
            if callable(call):
                return call(name, *args, **kwargs)
            current = getattr(getattr(self._g, '_webview_extension', None), name, None)
            if not callable(current):
                raise AttributeError(name)
            return current(*args, **kwargs)
        _forward.__name__ = name
        return _forward

    def toggle_menu(self):
        threading.Thread(target=self._g._toggle_menu, daemon=True).start()

    def context_action(self, action: str):
        threading.Thread(target=self._g._context_action, args=(action,), daemon=True).start()

    def menu_action(self, action: str):
        threading.Thread(target=self._g._menu_action, args=(action,), daemon=True).start()

    def alert_ok(self):
        pass

    def play_sound(self, name: str):
        threading.Thread(target=self._g._play_sound, args=(name,), daemon=True).start()

    # ── License API ──

    def license_status(self):
        try:
            from license import get_license_manager
            return json.dumps(get_license_manager().get_status(), ensure_ascii=False)
        except Exception as e:
            return json.dumps({"tier": "free", "is_paid": False, "error": str(e)})

    def license_activate(self, key):
        try:
            from license import get_license_manager
            result = get_license_manager().activate(str(key or '').strip())
            return json.dumps(result, ensure_ascii=False)
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    def license_done(self, dismissed=False):
        if dismissed:
            try:
                from gui_modules.sao_gui_license import _dismiss_license_dialog
                _dismiss_license_dialog()
            except Exception:
                pass
        threading.Thread(target=self._g._close_license_panel, daemon=True).start()

    def exit_app(self):
        threading.Thread(target=self._g._exit_with_animation, daemon=True).start()

    def getDataSourceHealth(self):
        """Phase 10: webview menu diagnostic — exposes mem_probe / TCP health."""
        try:
            return act_data_source_health(self._g)
        except Exception as e:
            return {"available": False, "error": str(e)}

    def get_data_source_health(self):
        return json.dumps(act_data_source_health(self._g), ensure_ascii=False)

    def diagnose_data_source(self):
        return json.dumps(act_data_source_diagnose(self._g), ensure_ascii=False)

    def copy_data_source_health(self):
        payload = act_data_source_diagnose(self._g)
        return json.dumps({
            'ok': True,
            'text': json.dumps(payload, ensure_ascii=False, indent=2),
            'status': payload,
        }, ensure_ascii=False)

    def get_report_export_status(self, limit=20, fmt='json'):
        return json.dumps(act_report_status(self._g, limit=int(limit or 20), fmt=str(fmt or 'json')), ensure_ascii=False)

    def export_last_report(self, fmt='json'):
        return json.dumps(act_report_export(self._g, fmt=str(fmt or 'json')), ensure_ascii=False)

    def copy_report_export(self, fmt='json'):
        return json.dumps(act_report_copy(self._g, fmt=str(fmt or 'json')), ensure_ascii=False)

    def get_mini_parse_status(self, formatter_id='summary_table'):
        return json.dumps(act_mini_parse_status(self._g, formatter_id=str(formatter_id or 'summary_table')), ensure_ascii=False)

    def preview_mini_parse(self, formatter_id='summary_table'):
        return json.dumps(act_mini_parse_preview(self._g, formatter_id=str(formatter_id or 'summary_table')), ensure_ascii=False)

    def copy_mini_parse(self, formatter_id='summary_table'):
        return json.dumps(act_mini_parse_copy(self._g, formatter_id=str(formatter_id or 'summary_table')), ensure_ascii=False)

    def get_selective_parsing_status(self):
        return json.dumps(act_selective_parsing_status(self._g), ensure_ascii=False)

    def update_selective_parsing(self, policy=None):
        if isinstance(policy, str):
            try:
                policy = json.loads(policy or '{}')
            except Exception:
                policy = {}
        return json.dumps(act_selective_parsing_update(self._g, policy if isinstance(policy, dict) else {}), ensure_ascii=False)

    def clear_selective_parsing(self):
        return json.dumps(act_selective_parsing_clear(self._g), ensure_ascii=False)

    def get_history_status(self, limit=20, query=''):
        return json.dumps(act_history_status(self._g, limit=int(limit or 20), query=str(query or '')), ensure_ascii=False)

    def load_history_report(self, index=0, show=True):
        return json.dumps(act_history_load(self._g, index=int(index or 0), show=bool(show)), ensure_ascii=False)

    def delete_history_report(self, index=0):
        return json.dumps(act_history_delete(self._g, index=int(index or 0)), ensure_ascii=False)

    def clear_history_reports(self):
        return json.dumps(act_history_delete(self._g, clear=True), ensure_ascii=False)

    def choose_offline_import_file(self):
        try:
            _ensure_webview()
            win = getattr(self._g, 'offline_import_win', None) if getattr(self._g, '_offline_import_visible', False) else None
            if win is None:
                win = getattr(self._g, 'report_export_win', None)
            if win is None:
                windows = getattr(webview, 'windows', []) if webview is not None else []
                win = windows[0] if windows else None
            dialog = getattr(win, 'create_file_dialog', None)
            if not callable(dialog):
                raise RuntimeError('File dialog is unavailable')
            file_types = (
                'ACT replay/report (*.json;*.jsonl;*.ndjson;*.xml;*.xml.gz;*.xml.zip;*.zip)',
                'SAO ACT XML report (*.xml;*.xml.gz;*.xml.zip;*.zip)',
                'JSON (*.json)',
                'JSONL/NDJSON (*.jsonl;*.ndjson)',
                'All files (*.*)',
            )
            try:
                paths = dialog(
                    getattr(webview, 'OPEN_DIALOG', 10),
                    allow_multiple=False,
                    file_types=file_types,
                )
            except TypeError:
                paths = dialog(getattr(webview, 'OPEN_DIALOG', 10), '', False, '', file_types)
            if not paths:
                return json.dumps({'ok': False, 'cancelled': True, 'path': '', 'message': 'No file selected'}, ensure_ascii=False)
            path = paths if isinstance(paths, str) else list(paths)[0]
            return json.dumps({'ok': True, 'path': str(path)}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'path': '', 'message': str(e), 'errors': [str(e)]}, ensure_ascii=False)

    def import_offline_report(self, path, persist=True, show=True):
        try:
            result = act_offline_import_file(
                self._g,
                str(path or ''),
                persist=bool(persist),
                show=bool(show),
            )
            return json.dumps(result, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e), 'errors': [str(e)]}, ensure_ascii=False)

    def get_offline_import_status(self, history_limit=20):
        try:
            return json.dumps(act_offline_import_status(self._g, history_limit=int(history_limit or 20)), ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e), 'accepted_formats': [], 'selected_file': '', 'progress': 0.0, 'status': 'error', 'last_result': {}, 'history': {}, 'errors': [str(e)]}, ensure_ascii=False)

    def get_timeline_status(self, limit=80, query=''):
        return json.dumps(act_timeline_status(self._g, limit=int(limit or 80), query=str(query or '')), ensure_ascii=False)

    def get_aggregate_status(self, limit=1000, query='', source='live', window_ms=1000, top_n=20, encounter_id='', group_by='skill', group_field=''):
        return json.dumps(
            act_aggregate_status(
                self._g,
                limit=int(limit or 1000),
                query=str(query or ''),
                source=str(source or 'live'),
                window_ms=int(window_ms or 1000),
                top_n=int(top_n or 20),
                encounter_id=str(encounter_id or ''),
                group_by=str(group_by or 'skill'),
                group_field=str(group_field or ''),
            ),
            ensure_ascii=False,
        )

    def get_mem_scope_status(self, query='', dtype='i32', job_id=''):
        return json.dumps(
            act_mem_scope_status(self._g, query=str(query or ''), dtype=str(dtype or 'i32'),
                                 job_id=str(job_id or '')),
            ensure_ascii=False,
        )

    def mem_search(self, value='', dtype='i32', align=0):
        return json.dumps(act_mem_search(self._g, value=value, dtype=str(dtype or 'i32'),
                                         align=int(align or 0)), ensure_ascii=False)

    def mem_search_status(self, job_id=''):
        return json.dumps(act_mem_search_status(self._g, job_id=str(job_id or '')), ensure_ascii=False)

    def mem_narrow(self, job_id='', value=''):
        return json.dumps(act_mem_narrow(self._g, job_id=str(job_id or ''), value=value), ensure_ascii=False)

    def mem_search_cancel(self, job_id=''):
        return json.dumps(act_mem_search_cancel(self._g, job_id=str(job_id or '')), ensure_ascii=False)

    def mem_attr_map(self, ent_addr=''):
        return json.dumps(act_mem_attr_map(self._g, ent_addr=ent_addr), ensure_ascii=False)

    def play_timeline(self, speed=1.0):
        return json.dumps(act_timeline_play(self._g, speed=_safe_timeline_speed(speed)), ensure_ascii=False)

    def pause_timeline(self):
        return json.dumps(act_timeline_pause(self._g), ensure_ascii=False)

    def step_timeline(self, delta_ms=1000):
        return json.dumps(act_timeline_step(self._g, delta_ms=int(delta_ms or 0)), ensure_ascii=False)

    def seek_timeline(self, cursor_ms=0):
        return json.dumps(act_timeline_seek(self._g, cursor_ms=int(cursor_ms or 0)), ensure_ascii=False)

    def set_timeline_speed(self, speed=1.0):
        return json.dumps(act_timeline_set_speed(self._g, speed=_safe_timeline_speed(speed)), ensure_ascii=False)

    def filter_timeline(self, query=''):
        return json.dumps(act_timeline_filter(self._g, query=str(query or '')), ensure_ascii=False)

    def get_action_log_status(self, limit=80, query='', topic='', cursor_ms=None, source='live', encounter_id='', offset=0):
        kwargs = {
            'limit': int(limit or 80),
            'query': str(query or ''),
            'topic': str(topic or ''),
            'source': str(source or 'live'),
            'encounter_id': str(encounter_id or ''),
            'offset': int(offset or 0),
        }
        if cursor_ms is not None:
            kwargs['cursor_ms'] = int(cursor_ms or 0)
        return json.dumps(act_action_log_status(self._g, **kwargs), ensure_ascii=False)

    def search_action_log(self, query='', limit=80, source='live', encounter_id='', offset=0):
        return json.dumps(act_action_log_search(
            self._g,
            query=str(query or ''),
            limit=int(limit or 80),
            source=str(source or 'live'),
            encounter_id=str(encounter_id or ''),
            offset=int(offset or 0),
        ), ensure_ascii=False)

    def filter_action_log(self, topic='', query=None, limit=80, source='live', encounter_id='', offset=0):
        return json.dumps(act_action_log_filter(
            self._g,
            topic=str(topic or ''),
            query=None if query is None else str(query or ''),
            limit=int(limit or 80),
            source=str(source or 'live'),
            encounter_id=str(encounter_id or ''),
            offset=int(offset or 0),
        ), ensure_ascii=False)

    def jump_action_log_time(self, cursor_ms=0, limit=80, source='live', encounter_id='', offset=0, topic=None):
        return json.dumps(act_action_log_jump_to_time(
            self._g,
            cursor_ms=int(cursor_ms or 0),
            limit=int(limit or 80),
            source=str(source or 'live'),
            encounter_id=str(encounter_id or ''),
            offset=int(offset or 0),
            topic=None if topic is None else str(topic or ''),
        ), ensure_ascii=False)

    def show_action_log_at(self, cursor_ms=0, source='live', encounter_id='', topic=None):
        """Ensure the Action Log window is SHOWN (never toggled off) and focus its
        cursor at cursor_ms — the drill target for graph-point / timeline clicks."""
        try:
            if not getattr(self._g, '_action_log_visible', False):
                self._g._show_action_log()
        except Exception:
            pass
        result = act_action_log_jump_to_time(
            self._g,
            cursor_ms=int(cursor_ms or 0),
            limit=80,
            source=str(source or 'live'),
            encounter_id=str(encounter_id or ''),
            offset=0,
            topic=None if topic is None else str(topic or ''),
        )
        try:
            self._g._eval_action_log('if(window.ActionLog&&ActionLog.refresh)ActionLog.refresh()')
        except Exception:
            pass
        return json.dumps(result, ensure_ascii=False)

    def copy_action_log(self, limit=80, query='', topic='', source='live', encounter_id='', offset=0):
        return json.dumps(act_action_log_copy(
            self._g,
            limit=int(limit or 80),
            query=str(query or ''),
            topic=str(topic or ''),
            source=str(source or 'live'),
            encounter_id=str(encounter_id or ''),
            offset=int(offset or 0),
        ), ensure_ascii=False)

    def get_death_recap_status(self, limit=80, window_s=8.0, entity_id=None):
        return json.dumps(
            act_death_recap_status(
                self._g,
                limit=int(limit or 80),
                window_s=float(window_s or 8.0),
                entity_id=entity_id,
            ),
            ensure_ascii=False,
        )

    def copy_death_recap(self, limit=80, window_s=8.0, entity_id=None):
        return json.dumps(
            act_death_recap_copy(
                self._g,
                limit=int(limit or 80),
                window_s=float(window_s or 8.0),
                entity_id=entity_id,
            ),
            ensure_ascii=False,
        )

    def get_graph_timeseries_status(self, metric=None, limit=120, query=None, topic=None, time_range_ms=None):
        kwargs = {'limit': int(limit or 120)}
        if metric is not None:
            kwargs['metric'] = str(metric or '')
        if query is not None:
            kwargs['query'] = str(query or '')
        if topic is not None:
            kwargs['topic'] = str(topic or '')
        if time_range_ms is not None:
            kwargs['time_range_ms'] = int(time_range_ms or 0)
        return json.dumps(act_graph_timeseries_status(self._g, **kwargs), ensure_ascii=False)

    def select_graph_metric(self, metric='damage', limit=120):
        return json.dumps(act_graph_timeseries_select_metric(self._g, metric=str(metric or 'damage'), limit=int(limit or 120)), ensure_ascii=False)

    def zoom_graph_timeseries(self, time_range_ms=0, limit=120):
        return json.dumps(act_graph_timeseries_zoom(self._g, time_range_ms=int(time_range_ms or 0), limit=int(limit or 120)), ensure_ascii=False)

    def filter_graph_timeseries(self, query=None, topic=None, limit=120):
        return json.dumps(act_graph_timeseries_filter(self._g, query=None if query is None else str(query or ''), topic=None if topic is None else str(topic or ''), limit=int(limit or 120)), ensure_ascii=False)

    def export_graph_timeseries(self, metric=None, limit=120, query=None, topic=None):
        return json.dumps(act_graph_timeseries_export(self._g, metric=None if metric is None else str(metric or ''), limit=int(limit or 120), query=None if query is None else str(query or ''), topic=None if topic is None else str(topic or '')), ensure_ascii=False)

    def get_combatant_drilldown_status(self, combatant_id=None, query=None, focus_target=None):
        return json.dumps(act_combatant_drilldown_status(self._g, combatant_id=combatant_id, query=query, focus_target=focus_target), ensure_ascii=False)

    def filter_combatant_drilldown(self, combatant_id=None, query=''):
        return json.dumps(act_combatant_drilldown_filter(self._g, combatant_id=combatant_id, query=str(query or '')), ensure_ascii=False)

    def focus_combatant_target(self, combatant_id=None, target_id=''):
        return json.dumps(act_combatant_drilldown_focus_target(self._g, combatant_id=combatant_id, target_id=str(target_id or '')), ensure_ascii=False)

    def back_combatant_drilldown(self):
        return json.dumps(act_combatant_drilldown_back(self._g), ensure_ascii=False)

    def get_skill_drilldown_status(self, combatant_id=None, skill_id=None, query=None, limit=80):
        return json.dumps(act_skill_drilldown_status(self._g, combatant_id=combatant_id, skill_id=skill_id, query=query, limit=int(limit or 80)), ensure_ascii=False)

    def filter_skill_drilldown(self, combatant_id=None, skill_id=None, query='', limit=80):
        return json.dumps(act_skill_drilldown_filter(self._g, combatant_id=combatant_id, skill_id=skill_id, query=str(query or ''), limit=int(limit or 80)), ensure_ascii=False)

    def copy_skill_drilldown(self, combatant_id=None, skill_id=None, query=None, limit=80):
        return json.dumps(act_skill_drilldown_copy(self._g, combatant_id=combatant_id, skill_id=skill_id, query=query, limit=int(limit or 80)), ensure_ascii=False)

    def back_skill_drilldown(self):
        return json.dumps(act_skill_drilldown_back(self._g), ensure_ascii=False)

    def open_skill_drilldown(self, combatant_id=None, skill_id=None):
        """Open the Skill Drilldown window focused on (combatant_id, skill_id).

        Webview parity for the entity combatant `_open_skill` -> SkillDrilldown
        select path: clicking a combatant skill row must actually OPEN the skill
        window and show that skill (the user's '点开进不去')."""
        cid = str(combatant_id or '')
        sid = str(skill_id or '')
        try:
            if not getattr(self._g, '_skill_drilldown_visible', False):
                self._g._show_skill_drilldown()
        except Exception:
            pass
        status = act_skill_drilldown_status(self._g, combatant_id=cid or None, skill_id=sid or None, query=None, limit=80)
        try:
            js = (
                "(function(){var c=document.getElementById('combatantId'),s=document.getElementById('skillId');"
                "if(c)c.value=" + json.dumps(cid) + ";if(s)s.value=" + json.dumps(sid) + ";"
                "if(window.SkillDrilldown&&SkillDrilldown.refresh)SkillDrilldown.refresh();})()"
            )
            self._g._eval_skill_drilldown(js)
        except Exception:
            pass
        return json.dumps(status, ensure_ascii=False)

    def list_plugins(self):
        return json.dumps(act_plugin_list(self._g), ensure_ascii=False)

    def get_plugin_status(self):
        return json.dumps(act_plugin_status(self._g), ensure_ascii=False)

    def enable_plugin(self, plugin_id):
        return json.dumps(act_plugin_enable(self._g, str(plugin_id or '')), ensure_ascii=False)

    def disable_plugin(self, plugin_id):
        return json.dumps(act_plugin_disable(self._g, str(plugin_id or '')), ensure_ascii=False)

    def reload_plugins(self, plugin_id=''):
        return json.dumps(act_plugin_reload(self._g, str(plugin_id or '')), ensure_ascii=False)

    def pin_plugin(self, plugin_id, pinned=True):
        return json.dumps(act_plugin_pin(self._g, str(plugin_id or ''), bool(pinned)), ensure_ascii=False)

    def import_plugin_dialog(self):
        """Open a native file picker for a .zip plugin and one-click import it."""
        return json.dumps(act_plugin_import_dialog(self._g), ensure_ascii=False)

    def import_plugin(self, archive_path=''):
        """Import a .zip plugin by path (no dialog)."""
        return json.dumps(act_plugin_import(self._g, str(archive_path or '')), ensure_ascii=False)

    def uninstall_plugin(self, plugin_id):
        """Uninstall a user-installed plugin (delete its user_plugins dir)."""
        return json.dumps(act_plugin_uninstall(self._g, str(plugin_id or '')), ensure_ascii=False)

    def get_plugin_hotkeys(self):
        return json.dumps(act_plugin_hotkeys(self._g), ensure_ascii=False)

    def set_plugin_hotkey(self, action, key=''):
        return json.dumps(act_plugin_set_hotkey(self._g, str(action or ''), str(key or '')), ensure_ascii=False)

    def act_render_overlays(self, surface):
        return json.dumps(act_render_overlays(self._g, str(surface or '')), ensure_ascii=False)

    def act_render_apply_hooks(self, surface, payload=''):
        return json.dumps(act_render_apply_hooks(self._g, str(surface or ''), payload), ensure_ascii=False)

    def act_render_surfaces(self):
        return json.dumps(act_render_surfaces(self._g), ensure_ascii=False)

    def get_trigger_status(self):
        return json.dumps(act_trigger_status(self._g), ensure_ascii=False)

    def enable_trigger(self, rule_id):
        return json.dumps(act_trigger_enable(self._g, str(rule_id or '')), ensure_ascii=False)

    def disable_trigger(self, rule_id):
        return json.dumps(act_trigger_disable(self._g, str(rule_id or '')), ensure_ascii=False)

    def reload_triggers(self):
        return json.dumps(act_trigger_reload(self._g), ensure_ascii=False)

    def test_trigger(self, rule_id):
        return json.dumps(act_trigger_test(self._g, str(rule_id or '')), ensure_ascii=False)

    def toggle_plugin_manager(self):
        """Show/hide the ACT plugin manager overlay."""
        def _do():
            if self._g._plugin_manager_visible:
                self._g._hide_plugin_manager()
            else:
                self._g._show_plugin_manager()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_trigger_timer_manager(self):
        """Show/hide the ACT trigger/timer manager overlay."""
        def _do():
            if self._g._trigger_timer_manager_visible:
                self._g._hide_trigger_timer_manager()
            else:
                self._g._show_trigger_timer_manager()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_data_source_health(self):
        """Show/hide the ACT data-source health overlay."""
        def _do():
            if self._g._data_source_health_visible:
                self._g._hide_data_source_health()
            else:
                self._g._show_data_source_health()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_report_export(self):
        """Show/hide the ACT report/export overlay."""
        def _do():
            if self._g._report_export_visible:
                self._g._hide_report_export()
            else:
                self._g._show_report_export()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_offline_import(self):
        """Show/hide the ACT offline import wizard overlay."""
        def _do():
            if self._g._offline_import_visible:
                self._g._hide_offline_import()
            else:
                self._g._show_offline_import()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_timeline_vcr(self):
        """Show/hide the ACT timeline/VCR overlay."""
        def _do():
            if self._g._timeline_vcr_visible:
                self._g._hide_timeline_vcr()
            else:
                self._g._show_timeline_vcr()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_act_aggregate(self):
        """Show/hide the ACT semantic aggregate cockpit overlay."""
        def _do():
            if self._g._act_aggregate_visible:
                self._g._hide_act_aggregate()
            else:
                self._g._show_act_aggregate()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_mem_scope(self):
        """Show/hide the memory-scan explorer (Mem Scope) overlay."""
        def _do():
            if self._g._mem_scope_visible:
                self._g._hide_mem_scope()
            else:
                self._g._show_mem_scope()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_action_log(self):
        """Show/hide the ACT action-log overlay."""
        def _do():
            if self._g._action_log_visible:
                self._g._hide_action_log()
            else:
                self._g._show_action_log()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_death_recap(self):
        """Show/hide the ACT death-recap overlay."""
        def _do():
            if self._g._death_recap_visible:
                self._g._hide_death_recap()
            else:
                self._g._show_death_recap()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_graph_timeseries(self):
        """Show/hide the ACT graph/timeseries overlay."""
        def _do():
            if self._g._graph_timeseries_visible:
                self._g._hide_graph_timeseries()
            else:
                self._g._show_graph_timeseries()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_combatant_drilldown(self):
        """Show/hide the ACT combatant drilldown overlay."""
        def _do():
            if self._g._combatant_drilldown_visible:
                self._g._hide_combatant_drilldown()
            else:
                self._g._show_combatant_drilldown()
        threading.Thread(target=_do, daemon=True).start()

    def toggle_skill_drilldown(self):
        """Show/hide the ACT skill drilldown overlay."""
        def _do():
            if self._g._skill_drilldown_visible:
                self._g._hide_skill_drilldown()
            else:
                self._g._show_skill_drilldown()
        threading.Thread(target=_do, daemon=True).start()

    def switch_to_entity(self):
        """切换到 Entity (tkinter) UI 模式."""
        threading.Thread(target=lambda: self._g._transition_with_animation('entity'), daemon=True).start()

    # ---- 远程更新 ----
    def get_update_status(self):
        try:
            from updater.sao_updater import get_manager
            return get_manager().snapshot().to_json()
        except Exception as e:
            return {"state": "error", "error": str(e)}

    def check_update(self):
        try:
            from updater.sao_updater import get_manager
            get_manager().check_async()
            return True
        except Exception as e:
            print(f"[SAO WebView] check_update failed: {e}")
            return False

    def download_update(self):
        try:
            from updater.sao_updater import get_manager
            get_manager().download_async()
            return True
        except Exception as e:
            print(f"[SAO WebView] download_update failed: {e}")
            return False

    def apply_update(self):
        """应用已下载的更新包，会退出当前进程。"""
        try:
            from updater.sao_updater import has_pending_update, schedule_apply_on_exit
            if not has_pending_update():
                return False
            if not schedule_apply_on_exit():
                return False
            threading.Thread(target=self._g._exit_with_animation, daemon=True).start()
            return True
        except Exception as e:
            print(f"[SAO WebView] apply_update failed: {e}")
            return False

    def skip_update(self):
        try:
            from updater.sao_updater import get_manager
            get_manager().skip_current()
            return True
        except Exception:
            return False

    # ---- 面板主题 ----
    def get_panel_themes(self):
        """返回当前面板主题设置 (JS 调用获取初始主题)."""
        try:
            cfg = getattr(self._g, '_cfg_settings_ref', None) or self._g.settings
            defaults = {
                'hp': 'dark',
                'alert': 'dark',
                'act': 'dark',
            }
            plugin_defaults = self._g._dispatch_webview_extension('panel_theme_defaults', default={}) or {}
            if isinstance(plugin_defaults, dict):
                defaults.update({
                    str(k): str(v) for k, v in plugin_defaults.items()
                    if str(v) in ('light', 'dark')
                })
            themes = dict(cfg.get('panel_themes', {}) or {})
            defaults.update({k: v for k, v in themes.items() if v in ('light', 'dark')})
            return defaults
        except Exception:
            return {'hp': 'dark', 'alert': 'dark', 'act': 'dark'}

    def set_panel_theme(self, panel: str, theme: str):
        """从 JS 端设置面板主题并保存."""
        try:
            panel = str(panel or '').lower()
            theme = str(theme or '').lower()
            allowed = {'hp', 'alert', 'act'}
            plugin_allowed = self._g._dispatch_webview_extension('panel_theme_keys', default=()) or ()
            allowed.update(str(item) for item in plugin_allowed)
            if panel not in allowed:
                return json.dumps({'ok': False, 'panel': panel, 'message': 'Unknown panel'}, ensure_ascii=False)
            if theme not in {'light', 'dark'}:
                theme = 'dark'
            cfg = getattr(self._g, '_cfg_settings_ref', None) or self._g.settings
            themes = dict(cfg.get('panel_themes', {}))
            themes[panel] = theme
            cfg.set('panel_themes', themes)
            cfg.save()
            self._g._dispatch_webview_extension('on_panel_theme_changed', panel, theme)
            # 即时推送到对应 webview 面板窗口
            _win_map = {
                'hp': getattr(self._g, 'hp_win', None),
                'alert': getattr(self._g, 'alert_win', None),
            }
            _act_wins = (
                getattr(self._g, 'plugin_manager_win', None),
                getattr(self._g, 'trigger_timer_win', None),
                getattr(self._g, 'data_source_health_win', None),
                getattr(self._g, 'report_export_win', None),
                getattr(self._g, 'offline_import_win', None),
                getattr(self._g, 'timeline_vcr_win', None),
                getattr(self._g, 'act_aggregate_win', None),
                getattr(self._g, 'mem_scope_win', None),
                getattr(self._g, 'action_log_win', None),
                getattr(self._g, 'death_recap_win', None),
                getattr(self._g, 'graph_timeseries_win', None),
                getattr(self._g, 'combatant_drilldown_win', None),
                getattr(self._g, 'skill_drilldown_win', None),
            )
            if panel == 'act':
                targets = _act_wins
            else:
                targets = (_win_map.get(panel) or self._g._plugin_surface_win(panel),)
            for w in targets:
                if w:
                    try:
                        w.evaluate_js(f'window._applyPanelTheme&&window._applyPanelTheme("{theme}")')
                    except Exception:
                        pass
            return json.dumps({'ok': True, 'panel': panel, 'theme': theme, 'panel_themes': themes}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def window_drag(self, dx, dy):
        """HP 窗口固定, 不允许拖拽 — 此方法保留但不执行."""
        pass

    def set_ctx_menu_active(self, active, bounds=None):
        """控制 HP 窗口 click-through 区域 (右键菜单开关)"""
        self._g._ctx_menu_active = bool(active)
        self._g._ctx_menu_bounds = bounds if active and isinstance(bounds, dict) else None
        self._g._set_hp_region(expanded=bool(active), menu_bounds=self._g._ctx_menu_bounds)

    def set_hit_regions(self, regions):
        """接收前端上报的实际可点击 UI 区域"""
        if isinstance(regions, str):
            try:
                regions = json.loads(regions)
            except Exception:
                regions = {}

        def _sanitize(rects):
            if not isinstance(rects, list):
                return []
            sane = []
            for rect in rects:
                if not isinstance(rect, dict):
                    continue
                try:
                    width = int(rect.get('width', 0))
                    height = int(rect.get('height', 0))
                except Exception:
                    continue
                if width < 8 or height < 8:
                    continue
                sane.append(rect)
            return sane

        got_region_payload = isinstance(regions, dict)
        if got_region_payload:
            display_regions = _sanitize(regions.get('display_regions', []))
            click_regions = _sanitize(regions.get('click_regions', []))
        else:
            display_regions = _sanitize(regions if isinstance(regions, list) else [])
            click_regions = list(display_regions)

        if got_region_payload or display_regions:
            self._g._hp_display_regions = display_regions
            self._g._hp_hit_regions = list(display_regions)
        elif not getattr(self._g, '_hp_display_regions', None):
            self._g._hp_display_regions = []

        if got_region_payload or click_regions:
            self._g._hp_click_regions = click_regions
        elif not getattr(self._g, '_hp_click_regions', None):
            self._g._hp_click_regions = []

        if display_regions or click_regions:
            self._g._hp_hit_regions_ready = True
            self._g._hp_last_hit_region_ts = time.time()

        if (not got_region_payload and not display_regions and not click_regions
                and getattr(self._g, '_hp_hit_regions', None)):
            return
        if not getattr(self._g, '_hp_hit_regions', None):
            self._g._hp_hit_regions = []
        self._g._set_hp_region(expanded=self._g._ctx_menu_active, menu_bounds=self._g._ctx_menu_bounds)

    def notify_hp_hit_regions_ready(self):
        self._g._hp_js_hit_regions_ready = True
        self._g._hp_hit_regions_ready = True
        self._g._hp_last_hit_region_ts = time.time()
        self._g._set_hp_region(expanded=self._g._ctx_menu_active, menu_bounds=self._g._ctx_menu_bounds)

    # ── Sound settings ──
    def set_sound_enabled(self, enabled):
        """Global SFX on/off."""
        state = _setting_bool_value(enabled)
        try:
            from utils.sao_sound import set_sound_enabled
            set_sound_enabled(state)
            self._g._set_setting('sound_enabled', state)
            return json.dumps({'ok': True, 'enabled': state}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'enabled': state, 'message': str(e)}, ensure_ascii=False)

    def set_sound_volume(self, volume_pct):
        """Global volume 0-100."""
        volume = _sound_volume_value(volume_pct)
        try:
            from utils.sao_sound import set_sound_volume
            set_sound_volume(volume)
            self._g._set_setting('sound_volume', volume)
            return json.dumps({'ok': True, 'volume': volume}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'volume': volume, 'message': str(e)}, ensure_ascii=False)

    def browse_dir(self, path: str) -> str:
        """文件选择器: 返回目录内容 JSON"""
        try:
            items = sorted(os.listdir(path), key=lambda x: x.lower())
            parent = os.path.dirname(path)
            parent = parent if parent != path else None
            dirs = [d for d in items
                    if os.path.isdir(os.path.join(path, d)) and not d.startswith('.')]
            files = [f for f in items if os.path.isfile(os.path.join(path, f))]
            return json.dumps({
                'current': path,
                'parent': parent,
                'dirs': [{'name': d, 'path': os.path.join(path, d)} for d in dirs],
                'files': [{'name': f, 'path': os.path.join(path, f),
                           'size': os.path.getsize(os.path.join(path, f))} for f in files],
            }, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'current': path, 'parent': None,
                               'dirs': [], 'files': [], 'error': str(e)})


# ════════════════════════════════════════════════
#  面板 JS API Bridge
# ════════════════════════════════════════════════
class PanelAPI:
    """pywebview js_api for panel windows (control/piano/status/viz)."""

    def __init__(self, gui: 'SAOWebViewGUI', panel_type: str):
        self._g = gui
        self._type = panel_type

    def window_drag(self, dx, dy):
        try:
            win = self._g._panel_wins.get(self._type)
            if win:
                x = win.x + int(dx)
                y = win.y + int(dy)
                win.move(x, y)
                self._g._panel_origins[self._type] = (x, y)
        except Exception:
            pass

    def close_panel(self):
        threading.Thread(target=self._g._toggle_panel, args=(self._type,), daemon=True).start()

    def panel_action(self, action):
        threading.Thread(target=self._g._menu_action, args=(action,), daemon=True).start()

    def play_sound(self, name):
        threading.Thread(target=self._g._play_sound, args=(name,), daemon=True).start()

# ════════════════════════════════════════════════
#  主类
# ════════════════════════════════════════════════
class SAOWebViewGUI:
    """基于 pywebview 的 SAO-UI 自动化覆盖层.

    窗口:
      hp_win  — 悬浮 HP 栏 (430×500, 色键透明, 上部 68px 可见, 固定位置)
      menu_win — 全屏 SAO 菜单 (初始隐藏)
    LinkStart 使用 SAOLinkStart (tkinter / ModernGL) 在 webview 启动前运行.
    """

    def __init__(self):
        _ensure_webview()
        _set_process_app_id('sao.auto.overlay')

        self.settings = SettingsManager()
        self.settings.set('ui_mode', 'webview')
        self.settings.save()

        # 音效
        self._sound_ok = False
        try:
            from utils import sao_sound
            self._sao_sound = sao_sound
            self._sound_ok = True
        except Exception:
            self._sao_sound = None

        # 角色身份/进度/STA 状态由游戏插件 on_load 通过 webview_bridge 注入；
        # 平台不持有任何游戏字段定义。

        # 识别状态 (识别引擎本身由插件提供, 平台只保留通用开关引用)
        self._recognition_active = False
        self._game_state = None  # 由插件填充的最近一帧游戏状态快照
        self._state_mgr = None
        self._recognition_engine = None
        self._recognition_engines = []
        self._packet_engine = None
        self._vision_engine = None
        self._act_event_bus = None
        self._act_plugin_manager = None
        self._vision_paused_for_death = False
        self._last_dead_state = False
        self._recog_lock = threading.Lock()  # 保护 _recognition_active 切换

        # 菜单
        self._sta_detector_started = False
        self._menu_visible = False

        # 窗口
        self.hp_win = None
        self.menu_win = None
        self.alert_win = None
        # 切换地图中央横幅 (延迟 3s 后淡入地图名)
        self.mapbanner_win = None
        self.mech_banner_win = None
        self._mech_banner_hwnd = 0
        self._mech_banner_shown = False
        self._mech_alert_controller = None
        self._mapbanner_hwnd = 0
        self._mapbanner_nonce = 0
        self._mapbanner_last_name = ''
        self._mapbanner_last_ts = 0.0
        self._mapbanner_timer = None
        # Game/plugin-owned WebView surfaces are injected dynamically by plugins.
        self._plugin_surfaces = {}
        self._plugin_surface_meta = {}
        self._plugin_surface_order = []

        # ACT Plugin Manager panel
        self.plugin_manager_win = None
        self._plugin_manager_visible = False

        # ACT Trigger/Timer Manager panel
        self.trigger_timer_win = None
        self._trigger_timer_manager_visible = False

        # ACT Data Source Health panel
        self.data_source_health_win = None
        self._data_source_health_visible = False

        # ACT Report/Export panel
        self.report_export_win = None
        self._report_export_visible = False

        # ACT Offline Import panel
        self.offline_import_win = None
        self._offline_import_visible = False

        # ACT Timeline/VCR panel
        self.timeline_vcr_win = None
        self._timeline_vcr_visible = False

        # ACT Semantic Aggregate cockpit panel
        self.act_aggregate_win = None
        self._act_aggregate_visible = False

        # Mem Scope — memory-scan explorer panel
        self.mem_scope_win = None
        self._mem_scope_visible = False

        # ACT Action Log panel
        self.action_log_win = None
        self._action_log_visible = False

        # ACT Death Recap panel
        self.death_recap_win = None
        self._death_recap_visible = False

        # ACT Graph/Timeseries panel
        self.graph_timeseries_win = None
        self._graph_timeseries_visible = False

        # ACT Combatant Drilldown panel
        self.combatant_drilldown_win = None
        self._combatant_drilldown_visible = False

        # ACT Skill Drilldown panel
        self.skill_drilldown_win = None
        self._skill_drilldown_visible = False

        # 热切换目标
        self._pending_switch: Optional[str] = None
        self._update_popup_ready = False
        self._pending_update_popup_snapshot = None
        self._last_update_popup_key = ''
        self._identity_alert_kind = ''
        self._persistent_alert_kinds = set()

        # License panel
        self._license_win = None
        self._license_done_event = threading.Event()

        # JS API
        self._api = SAOWebAPI(self)

        # 窗口 click-through
        self._hp_hwnd = 0
        self._ctx_menu_active = False
        self._ctx_menu_bounds = None
        self._hp_hit_regions = []
        self._hp_display_regions = []
        self._hp_click_regions = []
        self._hp_hit_regions_ready = False
        self._hp_js_hit_regions_ready = False
        self._hp_last_hit_region_ts = 0.0
        self._hp_click_bootstrap_started = False
        self._hp_fullscreen = False
        self._hp_reveal_pending = False
        self._hp_mouse_passthrough_started = False
        self._hp_mouse_passthrough = None
        self._alert_hwnd = 0
        self._sta_pixel_detector_enabled = False
        self._hp_viewport_offset_x = 0
        self._hp_viewport_offset_y = 0
        self._fisheye_active = False
        self._fisheye_gen = 0
        self._fisheye_prev_frame = None
        self._panel_wins = {}
        self._panel_float_active = False
        self._panel_origins = {}
        self._hp_visible = False
        self._exit_animating = False
        self._hp_entry_animating = False
        self._hp_position_lock_until = 0.0
        self._hp_position_guard_started = False

        # 识别相关引用
        self._cfg_settings_ref = None
        self._cache_loop_stop = threading.Event()
        self._last_identity_alert_serial = 0
        self._identity_alert_visible = False
        self._identity_alert_nonce = 0

        self._webview_extension = None

        self._bootstrap_runtime_state()
        self._start_packet_engine_early()

    # ─── 音效 ───
    def _play_sound(self, name: str):
        if self._sound_ok and self._sao_sound:
            try:
                self._sao_sound.play_sound(name)
            except Exception:
                pass

    def _set_setting(self, key: str, value):
        """Persist a setting to cfg_settings and save."""
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            self._cfg_settings_ref.set(key, value)
            try:
                self._cfg_settings_ref.save()
                self._setting_save_fail_at = 0.0
            except Exception as exc:
                # 设置在本会话内已生效, 只是没写进磁盘; 持续失败 (磁盘满/权限)
                # 时 60s 内只提示一次, 恢复成功后重新armed。
                now = time.monotonic()
                if now - getattr(self, '_setting_save_fail_at', 0.0) >= 60.0:
                    self._setting_save_fail_at = now
                    print(f'[SAOWebView] settings save failed: {exc}')
                    self._eval_menu('SAO.showToast("设置保存失败 — 修改重启后会丢失")')

    def _get_setting(self, key: str, default=None):
        """Read a setting."""
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            return self._cfg_settings_ref.get(key, default)
        return default

    def _bootstrap_runtime_state(self):
        if getattr(self, '_cfg_settings_ref', None):
            ensure_act_event_bus(self)
            ensure_act_plugin_manager(self, load=False)
            return
        try:
            from config import SettingsManager as CfgSettings

            if not getattr(self, '_cfg_settings_ref', None):
                self._cfg_settings_ref = CfgSettings()
            ensure_act_event_bus(self)
            ensure_act_plugin_manager(self, load=False)
        except Exception as e:
            print(f'[SAO] Runtime state bootstrap failed: {e}')

    def _start_packet_engine_early(self):
        try:
            ensure_act_plugin_manager(self, load=True)
        except Exception as e:
            import traceback
            print(f'[SAO] Plugin startup failed: {e}', flush=True)
            traceback.print_exc()
            self._packet_engine = None

    def _webview_extension_call(self, name: str, *args, **kwargs):
        extension = getattr(self, '_webview_extension', None)
        fn = getattr(extension, name, None)
        if not callable(fn):
            raise RuntimeError(f'WebView extension is not available: {name}')
        return fn(*args, **kwargs)

    def _dispatch_webview_extension(self, name: str, *args, default=None, **kwargs):
        try:
            return self._webview_extension_call(name, *args, **kwargs)
        except Exception:
            return default

    def _register_plugin_surface(self, surface: str, win, title: str = None, **meta):
        """Register a plugin-owned WebView window without teaching the platform its game semantics."""
        key = str(surface or '').strip()
        if not key:
            return None
        title = str(title or getattr(win, 'title', '') or key)
        self._plugin_surfaces[key] = win
        merged = dict(self._plugin_surface_meta.get(key, {}) or {})
        merged.update(meta)
        merged['title'] = title
        self._plugin_surface_meta[key] = merged
        if key not in self._plugin_surface_order:
            self._plugin_surface_order.append(key)
        return win

    def _plugin_surface_win(self, surface: str):
        return self._plugin_surfaces.get(str(surface or '').strip())

    def _plugin_surface_title(self, surface: str) -> str:
        meta = self._plugin_surface_meta.get(str(surface or '').strip(), {}) or {}
        return str(meta.get('title') or surface or '')

    def _plugin_surface_items(self):
        for surface in list(self._plugin_surface_order):
            win = self._plugin_surfaces.get(surface)
            if win:
                yield win, surface

    def _eval_plugin_surface(self, surface: str, js: str):
        try:
            win = self._plugin_surface_win(surface)
            if win:
                win.evaluate_js(js)
        except Exception:
            pass

    def _set_plugin_surface_alpha(self, surface: str, alpha: float):
        title = self._plugin_surface_title(surface)
        if title:
            self._set_window_alpha(title, alpha)

    def _show_plugin_surface(self, surface: str):
        try:
            win = self._plugin_surface_win(surface)
            if win:
                win.show()
        except Exception:
            pass

    def _hide_plugin_surface(self, surface: str):
        try:
            win = self._plugin_surface_win(surface)
            if win:
                win.hide()
        except Exception:
            pass

    def _destroy_plugin_surfaces(self):
        surfaces = getattr(self, '_plugin_surfaces', None)
        meta = getattr(self, '_plugin_surface_meta', None)
        order = getattr(self, '_plugin_surface_order', None)
        if not isinstance(surfaces, dict):
            self._plugin_surfaces = {}
            surfaces = self._plugin_surfaces
        if not isinstance(meta, dict):
            self._plugin_surface_meta = {}
            meta = self._plugin_surface_meta
        if not isinstance(order, list):
            self._plugin_surface_order = []
            order = self._plugin_surface_order
        for surface in list(order):
            try:
                win = surfaces.get(surface)
                if win:
                    win.destroy()
            except Exception:
                pass
        surfaces.clear()
        meta.clear()
        order.clear()

    def _apply_plugin_surfaces_transparency(self):
        for surface, win in list(self._plugin_surface_items()):
            meta = self._plugin_surface_meta.get(surface, {}) or {}
            title = str(meta.get('title') or surface)
            if not title:
                continue
            try:
                hwnd = ctypes.windll.user32.FindWindowW(None, title)
                if hwnd:
                    _make_transparent_ctypes(hwnd)
                    meta['hwnd'] = hwnd
            except Exception:
                pass
            if bool(meta.get('dotnet_transparency', True)):
                try:
                    _invoke_dotnet_transparency(win)
                except Exception:
                    pass

    def _set_plugin_surface_click_through(
            self, surface: str, enabled: bool = True, wait_retries: int = 0,
            ensure_on_top: bool = False):
        key = str(surface or '').strip()
        title = self._plugin_surface_title(key)
        if not title:
            return
        try:
            user32 = ctypes.windll.user32
            hwnd = user32.FindWindowW(None, title)
            if not hwnd and wait_retries > 0:
                threading.Timer(
                    0.1,
                    lambda: self._set_plugin_surface_click_through(
                        key, enabled=enabled, wait_retries=wait_retries - 1,
                        ensure_on_top=ensure_on_top),
                ).start()
                return
            if not hwnd:
                return
            meta = self._plugin_surface_meta.get(key, {}) or {}
            meta['hwnd'] = hwnd
            self._plugin_surface_meta[key] = meta
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if enabled:
                ex |= _WS_EX_TRANSPARENT
            else:
                ex &= ~_WS_EX_TRANSPARENT
            user32.SetWindowLongW(hwnd, _GWL_EXSTYLE, ex | _WS_EX_LAYERED)
            if ensure_on_top or bool(meta.get('on_top', False)):
                self._ensure_plugin_surface_on_top(key)
        except Exception:
            pass

    def _ensure_plugin_surface_on_top(self, surface: str):
        key = str(surface or '').strip()
        title = self._plugin_surface_title(key)
        if not title:
            return
        try:
            meta = self._plugin_surface_meta.get(key, {}) or {}
            hwnd = int(meta.get('hwnd') or 0)
            if not hwnd:
                hwnd = ctypes.windll.user32.FindWindowW(None, title)
                if hwnd:
                    meta['hwnd'] = hwnd
                    self._plugin_surface_meta[key] = meta
            if not hwnd:
                return
            HWND_TOPMOST = ctypes.c_void_p(-1)
            SWP_NOMOVE = 0x0002
            SWP_NOSIZE = 0x0001
            SWP_NOACTIVATE = 0x0010
            ctypes.windll.user32.SetWindowPos(
                hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        except Exception:
            pass

    def _maintain_plugin_surfaces(self):
        for surface, _win in list(self._plugin_surface_items()):
            meta = self._plugin_surface_meta.get(surface, {}) or {}
            if bool(meta.get('click_through', False)):
                self._set_plugin_surface_click_through(
                    surface, enabled=True, ensure_on_top=bool(meta.get('on_top', False)))

    # ── WebView extension hooks ──

    def _persist_cached_identity_state(self, save_now: bool = False):
        self._dispatch_webview_extension('persist_cached_identity_state', save_now)

    def _persistent_alert_kind_set(self) -> set:
        kinds = getattr(self, '_persistent_alert_kinds', None)
        if not isinstance(kinds, set):
            kinds = set(kinds or ())
            self._persistent_alert_kinds = kinds
        return kinds

    def _register_persistent_alert_kind(self, kind: str) -> bool:
        kind = str(kind or '').strip()
        if not kind:
            return False
        self._persistent_alert_kind_set().add(kind)
        return True

    def _unregister_persistent_alert_kind(self, kind: str) -> bool:
        kind = str(kind or '').strip()
        if not kind:
            return False
        self._persistent_alert_kind_set().discard(kind)
        return True

    def _is_persistent_alert_kind(self, kind: str) -> bool:
        return str(kind or '').strip() in self._persistent_alert_kind_set()

    def _is_persistent_alert_active(self) -> bool:
        return (bool(getattr(self, '_identity_alert_visible', False))
                and self._is_persistent_alert_kind(getattr(self, '_identity_alert_kind', '')))

    def _sync_identity_alert(self, gs):
        self._dispatch_webview_extension('sync_identity_alert', gs)

    def _stop_recognition_engines(self, preserve_packet: bool = False):
        self._dispatch_webview_extension('stop_engines', preserve_packet)
        engines = list(getattr(self, '_recognition_engines', []) or [])
        if not engines and self._recognition_engine:
            engines = [self._recognition_engine]
        kept_engines = []
        for engine in engines:
            if preserve_packet and engine is getattr(self, '_packet_engine', None):
                kept_engines.append(engine)
                continue
            try:
                engine.stop()
            except Exception:
                pass
        self._recognition_engines = kept_engines
        self._recognition_engine = kept_engines[0] if kept_engines else None
        if not preserve_packet:
            self._packet_engine = None
        self._vision_engine = None
        self._dispatch_webview_extension('_reset_sta_offline_state')
        self._vision_paused_for_death = False
        self._last_dead_state = False
        if not preserve_packet:
            self._recognition_active = False

    def _reconfigure_data_engines(self, restart_packet: bool = True):
        """Restart packet/vision engines to match the current per-component source map."""
        try:
            return self._webview_extension_call('_reconfigure_data_engines', restart_packet)
        except Exception as exc:
            print(f'[SAO] Plugin data-engine reconfigure failed: {exc}')

    # ════════════════════════════════════════
    #  入口
    # ════════════════════════════════════════
    def run(self):
        # ── Phase 1: LinkStart (tkinter, 阻塞) ──
        self._run_tkinter_link_start()
        self._lock_hp_position(1.0)

        # ── Phase 2: pywebview ──
        hp_url = _web_file_uri('hp.html')
        menu_url = _web_file_uri('menu.html')
        alert_url = _web_file_uri('alert.html')
        mapbanner_url = _web_file_uri('mapbanner.html')

        # HP 固定位置: 平台只按当前显示器/DPI 布局; 目标窗口贴合由插件处理。
        layout_hwnd, layout_rect = 0, None
        monitor_left, monitor_top, monitor_right, monitor_bottom = self._get_monitor_rect_for_target(
            hwnd=layout_hwnd, rect=layout_rect
        )
        self._hud_monitor_rect = (monitor_left, monitor_top, monitor_right, monitor_bottom)
        _sw = max(1, monitor_right - monitor_left)
        _sh = max(1, monitor_bottom - monitor_top)
        _dpi_scale = self._refresh_webview_dpi_scale(hwnd=layout_hwnd, rect=layout_rect)

        # 统一 HUD 窗口: 覆盖左下角 + 中底 HP + STA
        hud_w = int(_sw * 0.75)
        self._hud_w = hud_w

        # 裁剪区域参数 (在 _setup_click_through 后由 _force_hp_to_bottom 根据实际窗口尺寸重新计算)
        self._hp_clip_top = max(200, int(500 - 120 * _dpi_scale))

        # 目标位置
        tx0, ty0 = self._calc_hud_target(_sw, _sh, left=monitor_left, top=monitor_top)
        self._hp_target_x = tx0
        self._hp_target_y = ty0

        # HP 悬浮窗 — 初始放在动画起点, 避免 show() 时先闪到错误位置
        if self._hp_fullscreen:
            cx, cy = monitor_left, monitor_top
        else:
            cx = int(monitor_left + max(0, (_sw - hud_w) / 2))
            cy = int(monitor_top + max(0, (_sh - 500) / 2))

        hp_w = _sw if self._hp_fullscreen else hud_w
        hp_h = _sh if self._hp_fullscreen else 500
        self.hp_win = webview.create_window(
            'SAO-HP', hp_url,
            width=hp_w, height=hp_h,
            x=cx, y=cy,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        self.menu_win = webview.create_window(
            'SAO Menu', menu_url,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            js_api=self._api,
        )

        alert_w, alert_h = 416, 226
        self.alert_win = webview.create_window(
            'SAO Alert', alert_url,
            width=alert_w, height=alert_h,
            x=max(0, int((_sw - alert_w) / 2)),
            y=max(32, int(_sh * 0.16)),
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # Map-name banner — 居中大字横幅 (切换地图时淡入), 纯覆盖层鼠标穿透
        mapbanner_w = max(640, int(_sw * 0.6))
        mapbanner_h = 280
        self.mapbanner_win = webview.create_window(
            'SAO MapBanner', mapbanner_url,
            width=self._to_webview_px(mapbanner_w),
            height=self._to_webview_px(mapbanner_h),
            x=self._to_webview_px(monitor_left + max(0, int((_sw - mapbanner_w) / 2))),
            y=self._to_webview_px(monitor_top + max(0, int((_sh - mapbanner_h) / 2))),
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # Mechanic banner — 顶部居中机制提醒堆叠条 (倒计时进度条), 纯覆盖层鼠标穿透
        mech_banner_url = _web_file_uri('mech_banner.html')
        # 行宽是 CSS-px(DIP, 560)。窗口尺寸也按 DIP 给, 不再二次除 DPI —
        # 否则高 DPI 下窗口被缩到 ~400 DIP, 560 行被裁切。只有 x/y(由物理坐标来)需转 DIP。
        mech_banner_w = 600
        mech_banner_h = 3 * 64 + 2 * 8 + 12
        _mb_mon_w = self._to_webview_px(_sw)
        self.mech_banner_win = webview.create_window(
            'SAO MechBanner', mech_banner_url,
            width=mech_banner_w,
            height=mech_banner_h,
            x=self._to_webview_px(monitor_left) + max(0, (_mb_mon_w - mech_banner_w) // 2),
            y=self._to_webview_px(monitor_top) + max(0, int(self._to_webview_px(_sh) * 0.08)),
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        self._dispatch_webview_extension(
            'create_overlay_windows',
            webview,
            _web_file_uri,
            monitor_left,
            monitor_top,
            _sw,
            _sh,
        )

        # ACT Plugin Manager — center, shared API with Entity plugin panel
        plugin_manager_url = _web_file_uri('plugin_manager.html')
        _pm_w = max(620, int(min(_sw, 1920) * 0.38))
        _pm_h = max(520, int(min(_sh, 1080) * 0.52))
        _pm_x = max(16, int(monitor_left + (_sw - _pm_w) / 2))
        _pm_y = max(24, int(monitor_top + (_sh - _pm_h) * 0.20))
        self.plugin_manager_win = webview.create_window(
            'SAO-PluginManager', plugin_manager_url,
            width=_pm_w, height=_pm_h,
            x=_pm_x, y=_pm_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # ACT Trigger/Timer Manager — same API surface as Entity/Tk panel
        trigger_timer_url = _webview_extension_or_web_file_uri(self, 'trigger_timer_manager.html')
        _tt_w = max(640, int(min(_sw, 1920) * 0.40))
        _tt_h = max(520, int(min(_sh, 1080) * 0.52))
        _tt_x = max(16, int(monitor_left + (_sw - _tt_w) * 0.58))
        _tt_y = max(24, int(monitor_top + (_sh - _tt_h) * 0.23))
        self.trigger_timer_win = webview.create_window(
            'SAO-TriggerTimerManager', trigger_timer_url,
            width=_tt_w, height=_tt_h,
            x=_tt_x, y=_tt_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # ACT Data Source Health — observability panel for PacketBridge + memory fallback
        data_source_health_url = _webview_extension_or_web_file_uri(self, 'data_source_health.html')
        _dh_w = max(640, int(min(_sw, 1920) * 0.40))
        _dh_h = max(500, int(min(_sh, 1080) * 0.50))
        _dh_x = max(16, int(monitor_left + (_sw - _dh_w) * 0.45))
        _dh_y = max(24, int(monitor_top + (_sh - _dh_h) * 0.28))
        self.data_source_health_win = webview.create_window(
            'SAO-DataSourceHealth', data_source_health_url,
            width=_dh_w, height=_dh_h,
            x=_dh_x, y=_dh_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # ACT Report/Export — shared report exporter surface for WebView + Entity parity
        report_export_url = _webview_extension_or_web_file_uri(self, 'act_report_export.html')
        _re_w = max(680, int(min(_sw, 1920) * 0.42))
        _re_h = max(520, int(min(_sh, 1080) * 0.52))
        _re_x = max(16, int(monitor_left + (_sw - _re_w) * 0.40))
        _re_y = max(24, int(monitor_top + (_sh - _re_h) * 0.24))
        self.report_export_win = webview.create_window(
            'SAO-ReportExport', report_export_url,
            width=_re_w, height=_re_h,
            x=_re_x, y=_re_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # ACT Offline Import — standalone import/history playback wizard
        offline_import_url = _webview_extension_or_web_file_uri(self, 'act_offline_import.html')
        _oi_w = max(700, int(min(_sw, 1920) * 0.44))
        _oi_h = max(520, int(min(_sh, 1080) * 0.52))
        _oi_x = max(16, int(monitor_left + (_sw - _oi_w) * 0.42))
        _oi_y = max(24, int(monitor_top + (_sh - _oi_h) * 0.26))
        self.offline_import_win = webview.create_window(
            'SAO-OfflineImport', offline_import_url,
            width=_oi_w, height=_oi_h,
            x=_oi_x, y=_oi_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # ACT Timeline/VCR — shared event timeline controls for WebView parity
        timeline_vcr_url = _webview_extension_or_web_file_uri(self, 'act_timeline_vcr.html')
        _tl_w = max(700, int(min(_sw, 1920) * 0.43))
        _tl_h = max(520, int(min(_sh, 1080) * 0.52))
        _tl_x = max(16, int(monitor_left + (_sw - _tl_w) * 0.34))
        _tl_y = max(24, int(monitor_top + (_sh - _tl_h) * 0.18))
        self.timeline_vcr_win = webview.create_window(
            'SAO-TimelineVCR', timeline_vcr_url,
            width=_tl_w, height=_tl_h,
            x=_tl_x, y=_tl_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # ACT Semantic Aggregate — cockpit parity with Entity/Tk aggregate panel
        act_aggregate_url = _webview_extension_or_web_file_uri(self, 'act_aggregate.html')
        _ag_w = max(820, int(min(_sw, 1920) * 0.54))
        _ag_h = max(620, int(min(_sh, 1080) * 0.62))
        _ag_x = max(16, int(monitor_left + (_sw - _ag_w) * 0.25))
        _ag_y = max(22, int(monitor_top + (_sh - _ag_h) * 0.14))
        self.act_aggregate_win = webview.create_window(
            'SAO-ActAggregate', act_aggregate_url,
            width=_ag_w, height=_ag_h,
            x=_ag_x, y=_ag_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # Mem Scope — memory-scan explorer (CE-lite), parity with Entity panel
        mem_scope_url = _web_file_uri('mem_scope.html')
        _ms_w = max(780, int(min(_sw, 1920) * 0.52))
        _ms_h = max(620, int(min(_sh, 1080) * 0.62))
        _ms_x = max(16, int(monitor_left + (_sw - _ms_w) * 0.30))
        _ms_y = max(22, int(monitor_top + (_sh - _ms_h) * 0.12))
        self.mem_scope_win = webview.create_window(
            'SAO-MemScope', mem_scope_url,
            width=_ms_w, height=_ms_h,
            x=_ms_x, y=_ms_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # ACT Action Log — searchable EventBus table for WebView parity
        action_log_url = _webview_extension_or_web_file_uri(self, 'act_action_log.html')
        _al_w = max(760, int(min(_sw, 1920) * 0.48))
        _al_h = max(520, int(min(_sh, 1080) * 0.54))
        _al_x = max(16, int(monitor_left + (_sw - _al_w) * 0.30))
        _al_y = max(24, int(monitor_top + (_sh - _al_h) * 0.20))
        self.action_log_win = webview.create_window(
            'SAO-ActionLog', action_log_url,
            width=_al_w, height=_al_h,
            x=_al_x, y=_al_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # ACT Death Recap — death-window report surface for WebView parity
        death_recap_url = _webview_extension_or_web_file_uri(self, 'act_death_recap.html')
        _dr_w = max(720, int(min(_sw, 1920) * 0.44))
        _dr_h = max(500, int(min(_sh, 1080) * 0.50))
        _dr_x = max(18, int(monitor_left + (_sw - _dr_w) * 0.32))
        _dr_y = max(26, int(monitor_top + (_sh - _dr_h) * 0.23))
        self.death_recap_win = webview.create_window(
            'SAO-DeathRecap', death_recap_url,
            width=_dr_w, height=_dr_h,
            x=_dr_x, y=_dr_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # ACT Graph/Timeseries — rich chart surface for WebView parity
        graph_timeseries_url = _webview_extension_or_web_file_uri(self, 'act_graph_timeseries.html')
        _gt_w = max(780, int(min(_sw, 1920) * 0.50))
        _gt_h = max(540, int(min(_sh, 1080) * 0.55))
        _gt_x = max(16, int(monitor_left + (_sw - _gt_w) * 0.27))
        _gt_y = max(24, int(monitor_top + (_sh - _gt_h) * 0.22))
        self.graph_timeseries_win = webview.create_window(
            'SAO-GraphTimeseries', graph_timeseries_url,
            width=_gt_w, height=_gt_h,
            x=_gt_x, y=_gt_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # ACT Combatant Drilldown — per-combatant detail surface
        combatant_drilldown_url = _webview_extension_or_web_file_uri(self, 'act_combatant_drilldown.html')
        _cd_w = max(720, int(min(_sw, 1920) * 0.46))
        _cd_h = max(520, int(min(_sh, 1080) * 0.54))
        _cd_x = max(20, int(monitor_left + (_sw - _cd_w) * 0.31))
        _cd_y = max(28, int(monitor_top + (_sh - _cd_h) * 0.25))
        self.combatant_drilldown_win = webview.create_window(
            'SAO-CombatantDrilldown', combatant_drilldown_url,
            width=_cd_w, height=_cd_h,
            x=_cd_x, y=_cd_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # ACT Skill Drilldown — per-skill detail and timeline refs
        skill_drilldown_url = _webview_extension_or_web_file_uri(self, 'act_skill_drilldown.html')
        _sd_w = max(720, int(min(_sw, 1920) * 0.45))
        _sd_h = max(500, int(min(_sh, 1080) * 0.52))
        _sd_x = max(24, int(monitor_left + (_sw - _sd_w) * 0.34))
        _sd_y = max(32, int(monitor_top + (_sh - _sd_h) * 0.28))
        self.skill_drilldown_win = webview.create_window(
            'SAO-SkillDrilldown', skill_drilldown_url,
            width=_sd_w, height=_sd_h,
            x=_sd_x, y=_sd_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        webview.start(self._on_webview_started, debug=False)

        # ── Phase 3: 热切换 ──
        if self._pending_switch:
            self._do_hot_switch(self._pending_switch)

    def _get_monitor_handle_for_target(self, hwnd: int = 0, rect=None):
        try:
            user32 = ctypes.windll.user32
            monitor_default_to_nearest = 2
            if hwnd:
                monitor = user32.MonitorFromWindow(hwnd, monitor_default_to_nearest)
                if monitor:
                    return monitor
            if isinstance(rect, (list, tuple)) and len(rect) == 4:
                class POINT(ctypes.Structure):
                    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]

                center = POINT(
                    int((int(rect[0]) + int(rect[2])) / 2),
                    int((int(rect[1]) + int(rect[3])) / 2),
                )
                monitor = user32.MonitorFromPoint(center, monitor_default_to_nearest)
                if monitor:
                    return monitor
        except Exception:
            pass
        return 0

    def _get_monitor_rect_for_target(self, hwnd: int = 0, rect=None):
        try:
            user32 = ctypes.windll.user32
            monitor = self._get_monitor_handle_for_target(hwnd=hwnd, rect=rect)
            if monitor:
                class _RECT(ctypes.Structure):
                    _fields_ = [
                        ('left', ctypes.c_long),
                        ('top', ctypes.c_long),
                        ('right', ctypes.c_long),
                        ('bottom', ctypes.c_long),
                    ]

                class _MONITORINFO(ctypes.Structure):
                    _fields_ = [
                        ('cbSize', ctypes.c_uint32),
                        ('rcMonitor', _RECT),
                        ('rcWork', _RECT),
                        ('dwFlags', ctypes.c_uint32),
                    ]

                info = _MONITORINFO()
                info.cbSize = ctypes.sizeof(_MONITORINFO)
                if user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
                    return (
                        int(info.rcMonitor.left),
                        int(info.rcMonitor.top),
                        int(info.rcMonitor.right),
                        int(info.rcMonitor.bottom),
                    )
        except Exception:
            pass

        try:
            sw = ctypes.windll.user32.GetSystemMetrics(0)
            sh = ctypes.windll.user32.GetSystemMetrics(1)
        except Exception:
            sw, sh = 1920, 1080
        return (0, 0, int(sw), int(sh))

    def _get_monitor_dpi_for_target(self, hwnd: int = 0, rect=None) -> int:
        user32 = ctypes.windll.user32
        try:
            dpi = int(user32.GetDpiForWindow(hwnd)) if hwnd else 0
            if dpi > 0:
                return dpi
        except Exception:
            pass

        try:
            shcore = ctypes.windll.shcore
            monitor = self._get_monitor_handle_for_target(hwnd=hwnd, rect=rect)
            if monitor:
                xdpi = ctypes.c_uint()
                ydpi = ctypes.c_uint()
                if shcore.GetDpiForMonitor(monitor, 0, ctypes.byref(xdpi), ctypes.byref(ydpi)) == 0:
                    dpi = int(xdpi.value or 0)
                    if dpi > 0:
                        return dpi
        except Exception:
            pass

        try:
            dpi = int(user32.GetDpiForSystem() or 0)
            if dpi > 0:
                return dpi
        except Exception:
            pass
        return 96

    def _refresh_webview_dpi_scale(self, hwnd: int = 0, rect=None) -> float:
        dpi = max(96, int(self._get_monitor_dpi_for_target(hwnd=hwnd, rect=rect) or 96))
        scale = max(1.0, dpi / 96.0)
        self._dpi_scale = scale
        self._hp_clip_top = max(200, int(500 - 120 * scale))
        return scale

    def _calc_hud_target(self, sw: int = 0, sh: int = 0, left: Optional[int] = None, top: Optional[int] = None) -> tuple:
        """计算 HUD 目标位置 (x, y) — 与 Entity 模式对齐: x=4%屏宽, y=目标显示器底部。"""
        monitor_rect = getattr(self, '_hud_monitor_rect', None)
        if (left is None or top is None) and isinstance(monitor_rect, (list, tuple)) and len(monitor_rect) == 4:
            if left is None:
                left = int(monitor_rect[0])
            if top is None:
                top = int(monitor_rect[1])
            if not sw:
                sw = int(monitor_rect[2]) - int(monitor_rect[0])
            if not sh:
                sh = int(monitor_rect[3]) - int(monitor_rect[1])
        if not sw:
            try:
                sw = ctypes.windll.user32.GetSystemMetrics(0)
            except Exception:
                sw = 1920
        if not sh:
            try:
                sh = ctypes.windll.user32.GetSystemMetrics(1)
            except Exception:
                sh = 1080
        if left is None:
            left = 0
        if top is None:
            top = 0
        # 可用户自定义偏移
        offset_pct = 0.04
        if self._cfg_settings_ref:
            offset_pct = self._cfg_settings_ref.get('hud_offset_x', 0.04)
        return int(left + sw * offset_pct), int(top + sh - 500)

    # ─── LinkStart (tkinter) ───
    def _run_tkinter_link_start(self):
        try:
            import tkinter as tk
            from sao_theme import SAOLinkStart
        except Exception as e:
            print(f"[SAO] LinkStart skipped (import): {e}")
            return

        ls_root = None
        try:
            ls_root = tk.Tk()
            ls_root.withdraw()

            # Cover the platform-selected monitor. play() otherwise reads
            # winfo_screenwidth/height off this withdrawn throwaway root,
            # which under PerMonitorV2 may report a different monitor.
            monitor_rect = self._get_monitor_rect_for_target()

            done = threading.Event()

            def on_done():
                done.set()
                try:
                    ls_root.after(50, ls_root.destroy)
                except Exception:
                    pass

            ls = SAOLinkStart(ls_root, on_done=on_done,
                              monitor_rect=monitor_rect)

            # Watchdog: a wedged finish-marshal (post_to_tk never drained) or
            # a GLFW window that never paints must never hang this blocking
            # startup mainloop. Force-finish a little past the intro's natural
            # length; SAOLinkStart._finish() is idempotent (guarded by
            # _finished) and fires on_done -> destroy.
            try:
                watchdog_ms = int((SAOLinkStart._DURATION
                                   + SAOLinkStart._STARTUP_PRELUDE + 3.0) * 1000)
            except Exception:
                watchdog_ms = 15000

            def _watchdog():
                if done.is_set():
                    return
                done.set()
                print("[SAO] LinkStart watchdog fired — forcing finish so "
                      "the HUD can start")
                # In the wedged-GPU-pump failure case, ls._finish() ->
                # GpuOverlayWindow.destroy() blocks up to ~5s waiting on the
                # dead pump. Don't freeze the Tk main thread / startup: null
                # on_done (so _finish can't re-enter Tk from a worker thread)
                # and run the teardown off-thread when GPU present is active.
                ls.on_done = None

                def _bg_finish():
                    try:
                        ls._finish()
                    except Exception:
                        pass

                if getattr(ls, '_gpu_present_enabled', False):
                    threading.Thread(target=_bg_finish, daemon=True).start()
                else:
                    _bg_finish()
                # Break the blocking mainloop now; the finally block destroys
                # ls_root (Tk-only, fast).
                try:
                    ls_root.quit()
                except Exception:
                    pass

            try:
                ls_root.after(watchdog_ms, _watchdog)
            except Exception:
                pass

            try:
                ls.play()
            except Exception:
                import traceback
                traceback.print_exc()
                try:
                    ls._finish()
                except Exception:
                    pass

            ls_root.mainloop()
        except Exception as e:
            print(f"[SAO] LinkStart skipped: {e}")
            import traceback
            traceback.print_exc()
        finally:
            try:
                if ls_root is not None and ls_root.winfo_exists():
                    ls_root.destroy()
            except Exception:
                pass

    # ─── 透明设置 ───
    def _apply_webview2_transparency(self):
        """Win32 LWA_COLORKEY + .NET Form BackColor 色键透明.

        两路并用:
          1. Win32 COLORKEY — rgb(1,0,1) 像素穿透到桌面
          2. .NET via Invoke — form.BackColor = key color, 让 HTML 透明区域
             穿透 Form 背景色, 再由 Win32 COLORKEY 穿透到桌面, 彻底消除白底.
        """
        def _apply_for(title: str, win_obj):
            # 方案1: Win32 色键
            try:
                hwnd = ctypes.windll.user32.FindWindowW(None, title)
                if hwnd:
                    _make_transparent_ctypes(hwnd)
            except Exception:
                pass
            # 方案2: .NET Form BackColor — GUI 线程安全 Invoke
            try:
                _invoke_dotnet_transparency(win_obj)
            except Exception:
                pass

        _apply_for('SAO-HP', self.hp_win)
        _apply_for('SAO Alert', self.alert_win)
        _apply_for('SAO MapBanner', self.mapbanner_win)
        _apply_for('SAO MechBanner', self.mech_banner_win)
        # 菜单窗口只做 Win32 色键, 不设 .NET TransparencyKey
        # (TransparencyKey 会令菜单 HTML 透明区域变成鼠标穿透, 导致按钮无法点击)
        try:
            menu_hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO Menu')
            if menu_hwnd:
                _make_transparent_ctypes(menu_hwnd)
        except Exception:
            pass
        self._apply_plugin_surfaces_transparency()

    def _reassert_hp_transparency(self, alpha: float = 1.0, retries: int = 4, delay: float = 0.18):
        """反复重置 HP 窗口透明状态，修复热切换后偶发白底。"""
        def _apply_once():
            try:
                self._apply_webview2_transparency()
                # reveal 未完成时不设置 alpha, 避免与 reveal 定时器冲突
                if not getattr(self, '_hp_reveal_pending', False):
                    self._set_window_alpha('SAO-HP', alpha)
                self._setup_click_through()
                self._ensure_hp_on_top()
                self._request_hp_hit_regions()
            except Exception:
                pass

        _apply_once()
        for i in range(1, max(1, retries)):
            threading.Timer(delay * i, _apply_once).start()

    def _request_hp_hit_regions(self):
        """主动向前端请求重新上报可点击区域."""
        try:
            self._eval_hp(
                'if (window.scheduleHitRegionReport) { scheduleHitRegionReport(); }'
                ' if (window.scheduleHitRegionBootstrap) { scheduleHitRegionBootstrap(); }'
                ' if (window.startHitRegionBootRetry) { startHitRegionBootRetry(); }'
            )
        except Exception:
            pass

    def _lock_hp_position(self, seconds: float):
        try:
            seconds = float(seconds)
        except Exception:
            seconds = 0.0
        until = time.time() + max(0.0, seconds)
        if until > float(getattr(self, '_hp_position_lock_until', 0.0) or 0.0):
            self._hp_position_lock_until = until

    def _is_hp_position_locked(self) -> bool:
        if getattr(self, '_hp_entry_animating', False):
            return True
        return time.time() < float(getattr(self, '_hp_position_lock_until', 0.0) or 0.0)

    def _start_hp_position_guard(self):
        if getattr(self, '_hp_position_guard_started', False):
            return
        self._hp_position_guard_started = True

        def _loop():
            _tick = 0
            while True:
                time.sleep(0.12)
                _tick += 1
                try:
                    # 如果 hwnd 还没有获取到, 持续尝试 (首次开机可能延迟)
                    if not self._hp_hwnd:
                        _found = ctypes.windll.user32.FindWindowW(None, 'SAO-HP')
                        if _found:
                            self._hp_hwnd = _found
                            self._setup_click_through()
                    if self._hp_hwnd and not self._hp_entry_animating and self._is_hp_position_locked():
                        self._force_hp_to_bottom(force=True, quiet=True)
                except Exception:
                    pass
                # 每 ~0.5s 检查一次点击穿透健康状态
                if _tick % 4 == 0 and self._hp_hwnd:
                    try:
                        self._ensure_hp_clickable()
                    except Exception:
                        pass
                    # 确保隐藏面板保持 WS_EX_TRANSPARENT (防止隐藏窗口意外拦截点击)
                    try:
                        self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                    self._maintain_plugin_surfaces()
                    try:
                        self._setup_mapbanner_click_through(_wait_retries=0)
                    except Exception:
                        pass
                    try:
                        # Alert 只在未显示时设穿透, 避免干扰活动弹窗的按钮点击
                        if not getattr(self, '_identity_alert_visible', False):
                            self._setup_alert_click_through()
                    except Exception:
                        pass

        threading.Thread(target=_loop, daemon=True).start()

    def _start_hp_click_bootstrap(self, duration: float = 8.0):
        if getattr(self, '_hp_click_bootstrap_started', False):
            return
        self._hp_click_bootstrap_started = True

        def _loop():
            deadline = time.time() + max(1.0, float(duration or 0.0))
            while time.time() < deadline:
                try:
                    self._setup_click_through()
                    self._ensure_hp_clickable()
                    self._request_hp_hit_regions()
                    if self._hp_hwnd and getattr(self, '_hp_js_hit_regions_ready', False):
                        break
                except Exception:
                    pass
                time.sleep(0.16)

            # 最终确认
            try:
                self._setup_click_through()
                self._ensure_hp_clickable()
                self._request_hp_hit_regions()
            except Exception:
                pass

        threading.Thread(target=_loop, daemon=True).start()

    def _default_hp_display_regions(self):
        """Fallback display regions when JS layout data is not ready yet."""
        try:
            win_w = int(getattr(self, '_win_w_phys', 0) or 0)
            win_h = int(getattr(self, '_win_h_phys', 0) or 0)
            if win_w <= 0 or win_h <= 0:
                return []
            viewport_h = max(1, win_h - int(getattr(self, '_hp_viewport_offset_y', 0) or 0))
            stage_width = int(win_w * 0.75)

            def _rect(left, top, width, height):
                return {
                    'left': int(left),
                    'top': int(top),
                    'width': int(max(0, width)),
                    'height': int(max(0, height)),
                }

            id_w = int(stage_width * 0.42)
            id_h = 136
            id_left = int(stage_width * 0.01)
            id_top = viewport_h - 146

            hp_w = int(stage_width * 0.34)
            hp_h = 118
            hp_left = int(stage_width * 0.48)
            hp_top = viewport_h - 112

            sta_w = int(stage_width * 0.30)
            sta_h = 38
            sta_left = int(stage_width * 0.53)
            sta_top = viewport_h - 42

            return [
                _rect(id_left, id_top, id_w, id_h),
                _rect(hp_left - 44, hp_top - 18, hp_w + 88, hp_h + 36),
                _rect(hp_left, hp_top, hp_w, hp_h),
                _rect(sta_left, sta_top, sta_w, sta_h),
            ]
        except Exception:
            return []

    def _default_hp_hot_regions(self):
        """Fallback clickable regions when JS hit-regions have not registered yet.

        返回所有 display regions 作为默认热区, 确保整个 HP 面板内容区域
        在 JS 报告真实点击区域之前就可以接收点击事件,
        从而触发 JS 注册精确的 hit regions.
        """
        try:
            display_regions = self._default_hp_display_regions()
            if not display_regions:
                return []
            return list(display_regions)
        except Exception:
            return []

    # ─── 任务栏图标 ───
    def _set_window_icon(self, title: str):
        try:
            icon_path = _get_icon_path()
            if not icon_path:
                return
            IMAGE_ICON = 1
            LR_LOADFROMFILE = 0x10
            LR_DEFAULTSIZE = 0x40
            WM_SETICON = 0x80
            hwnd = ctypes.windll.user32.FindWindowW(None, title)
            if not hwnd:
                return
            hicon = ctypes.windll.user32.LoadImageW(
                None, icon_path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE)
            if hicon:
                if not hasattr(self, '_window_hicons'):
                    self._window_hicons = []
                self._window_hicons.append(hicon)
                ctypes.windll.user32.SendMessageW(hwnd, WM_SETICON, 0, hicon)
                ctypes.windll.user32.SendMessageW(hwnd, WM_SETICON, 1, hicon)
        except Exception as e:
            print(f'[SAO] set icon: {e}')

    # ─── 点击穿透 ───
    def _setup_click_through(self):
        try:
            user32 = ctypes.windll.user32
            hwnd = user32.FindWindowW(None, 'SAO-HP')
            if not hwnd:
                # Retry after a short delay if the window isn't ready yet
                threading.Timer(0.15, self._setup_click_through).start()
                return
            self._hp_hwnd = hwnd
            self._refresh_webview_dpi_scale(hwnd=hwnd)
            # Measure actual physical window dimensions early so fallback
            # regions are accurate on high-DPI displays.
            try:
                class _RECT(ctypes.Structure):
                    _fields_ = [('left', ctypes.c_long), ('top', ctypes.c_long),
                                 ('right', ctypes.c_long), ('bottom', ctypes.c_long)]
                _rc = _RECT()
                if user32.GetWindowRect(hwnd, ctypes.byref(_rc)):
                    _w = _rc.right - _rc.left
                    _h = _rc.bottom - _rc.top
                    if _w > 10 and _h > 10:
                        self._win_w_phys = _w
                        self._win_h_phys = _h
            except Exception:
                pass
            self._set_hp_region(False)
            # 始终启用鼠标穿透轮询: 光标在 UI 热区外时设 WS_EX_TRANSPARENT (穿透),
            # 进入热区时移除 WS_EX_TRANSPARENT (可点击).
            # 这取代了仅依赖 SetWindowRgn 剪裁 + LWA_COLORKEY 的旧方案,
            # 彻底解决 HP 窗口在 JS hit-regions 未就绪时独占点击的问题.
            self._set_hp_mouse_passthrough(True)  # 先穿透, 等 poller 接管
            self._start_hp_mouse_passthrough_poller()
            # 安全网: 始终确认 WS_EX_TRANSPARENT 未被意外设置
            self._ensure_hp_clickable()
        except Exception as e:
            print(f"[SAO] click-through setup failed: {e}")

    def _ensure_hp_clickable(self):
        """安全检查: 确保 HP 窗口未被意外设为鼠标穿透.

        当 passthrough poller 已在运行时, 穿透状态完全由 poller 管理, 此方法不干预.
        仅在 poller 未启用时才移除 WS_EX_TRANSPARENT.
        """
        if not self._hp_hwnd:
            return
        # passthrough poller 已接管, 不干预
        if getattr(self, '_hp_mouse_passthrough_started', False):
            return
        try:
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(self._hp_hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    self._hp_hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _ensure_hidden_panels_passthrough(self):
        """确保所有当前隐藏的面板窗口保持 WS_EX_TRANSPARENT, 防止意外拦截点击.

        在 position guard 中周期性调用.
        """
        _panels = [
            ('SAO-PluginManager', '_plugin_manager_visible', None),
            ('SAO-TriggerTimerManager', '_trigger_timer_manager_visible', None),
            ('SAO-DataSourceHealth', '_data_source_health_visible', None),
            ('SAO-ReportExport', '_report_export_visible', None),
            ('SAO-OfflineImport', '_offline_import_visible', None),
            ('SAO-TimelineVCR', '_timeline_vcr_visible', None),
            ('SAO-ActionLog', '_action_log_visible', None),
            ('SAO-DeathRecap', '_death_recap_visible', None),
            ('SAO-GraphTimeseries', '_graph_timeseries_visible', None),
            ('SAO-CombatantDrilldown', '_combatant_drilldown_visible', None),
            ('SAO-SkillDrilldown', '_skill_drilldown_visible', None),
        ]
        user32 = ctypes.windll.user32
        for title, vis_attr, hwnd_attr in _panels:
            try:
                if getattr(self, vis_attr, False):
                    continue  # 面板已显示, 不干预
                hwnd = getattr(self, hwnd_attr, 0) if hwnd_attr else 0
                if not hwnd:
                    hwnd = user32.FindWindowW(None, title)
                    if hwnd and hwnd_attr:
                        setattr(self, hwnd_attr, hwnd)
                if not hwnd:
                    continue
                ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
                if not (ex & _WS_EX_TRANSPARENT):
                    user32.SetWindowLongW(
                        hwnd, _GWL_EXSTYLE,
                        ex | _WS_EX_TRANSPARENT | _WS_EX_LAYERED)
            except Exception:
                pass

    def _wait_and_apply_click_through(self, title: str, timeout: float = 2.0):
        """Block (up to *timeout* seconds) until the window hwnd is findable.

        This ensures that FindWindowW-based click-through setup can succeed
        before the window is shown, preventing a brief non-passthrough window.
        Intended to be called from background init thread only.
        """
        try:
            user32 = ctypes.windll.user32
            deadline = time.time() + max(0.1, float(timeout))
            hwnd = 0
            while time.time() < deadline:
                hwnd = user32.FindWindowW(None, title)
                if hwnd:
                    break
                time.sleep(0.05)
            if not hwnd:
                return
            # Ensure WS_EX_TRANSPARENT + WS_EX_LAYERED is set
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            user32.SetWindowLongW(
                hwnd, _GWL_EXSTYLE,
                ex | _WS_EX_TRANSPARENT | _WS_EX_LAYERED)
        except Exception:
            pass

    def _set_hp_mouse_passthrough(self, enabled: bool):
        if not self._hp_hwnd:
            return
        enabled = bool(enabled)
        if enabled == getattr(self, '_hp_mouse_passthrough', None):
            return
        try:
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(self._hp_hwnd, _GWL_EXSTYLE)
            if enabled:
                ex |= _WS_EX_TRANSPARENT
            else:
                ex &= ~_WS_EX_TRANSPARENT
            user32.SetWindowLongW(self._hp_hwnd, _GWL_EXSTYLE, ex | _WS_EX_LAYERED)
            self._hp_mouse_passthrough = enabled
        except Exception:
            pass

    def _cursor_over_hp_hot_region(self) -> bool:
        if not self._hp_hwnd:
            return False
        try:
            class POINT(ctypes.Structure):
                _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]

            class RECT(ctypes.Structure):
                _fields_ = [('left', ctypes.c_long), ('top', ctypes.c_long),
                             ('right', ctypes.c_long), ('bottom', ctypes.c_long)]

            user32 = ctypes.windll.user32
            pt = POINT()
            if not user32.GetCursorPos(ctypes.byref(pt)):
                return False
            rc = RECT()
            if not user32.GetWindowRect(self._hp_hwnd, ctypes.byref(rc)):
                return False
            dpi_s = float(getattr(self, '_dpi_scale', 1.0) or 1.0)
            rel_x = int(pt.x - rc.left - int(getattr(self, '_hp_viewport_offset_x', 0) or 0))
            rel_y = int(pt.y - rc.top - int(getattr(self, '_hp_viewport_offset_y', 0) or 0))
            if rel_x < 0 or rel_y < 0 or rel_x > (rc.right - rc.left) or rel_y > (rc.bottom - rc.top):
                return False
            if self._ctx_menu_active and isinstance(self._ctx_menu_bounds, dict):
                try:
                    left = int(float(self._ctx_menu_bounds.get('left', 0)) * dpi_s)
                    top = int(float(self._ctx_menu_bounds.get('top', 0)) * dpi_s)
                    width = int(float(self._ctx_menu_bounds.get('width', 0)) * dpi_s)
                    height = int(float(self._ctx_menu_bounds.get('height', 0)) * dpi_s)
                    pad = int(18 * dpi_s)
                    if (left - pad) <= rel_x <= (left + width + pad) and (top - pad) <= rel_y <= (top + height + pad):
                        return True
                except Exception:
                    pass
            regions = getattr(self, '_hp_click_regions', []) or []
            if not regions:
                if (getattr(self, '_hp_js_hit_regions_ready', False)
                        or getattr(self, '_hp_hit_regions_ready', False)):
                    return False
                regions = self._default_hp_hot_regions()
            for rect in regions:
                if not isinstance(rect, dict):
                    continue
                try:
                    left = int(float(rect.get('left', 0)) * dpi_s)
                    top = int(float(rect.get('top', 0)) * dpi_s)
                    width = int(float(rect.get('width', 0)) * dpi_s)
                    height = int(float(rect.get('height', 0)) * dpi_s)
                except Exception:
                    continue
                if width < 2 or height < 2:
                    continue
                if left <= rel_x <= left + width and top <= rel_y <= top + height:
                    return True
        except Exception:
            return False
        return False

    def _start_hp_mouse_passthrough_poller(self):
        if self._hp_mouse_passthrough_started:
            return
        self._hp_mouse_passthrough_started = True

        def _loop():
            while True:
                time.sleep(0.02)
                try:
                    if not self._hp_hwnd:
                        continue
                    # JS 未报告 hit regions 且无缓存 click regions 时,
                    # 保持窗口可点击, 让 JS 有机会接收事件并注册区域
                    if (not getattr(self, '_hp_js_hit_regions_ready', False)
                            and not getattr(self, '_hp_hit_regions_ready', False)
                            and not getattr(self, '_hp_click_regions', None)):
                        self._set_hp_mouse_passthrough(False)
                        continue
                    self._set_hp_mouse_passthrough(not self._cursor_over_hp_hot_region())
                except Exception:
                    pass

        threading.Thread(target=_loop, daemon=True).start()

    def _set_hp_region(self, expanded=False, menu_bounds=None):
        if not self._hp_hwnd:
            return
        if getattr(self, '_hp_fullscreen', False):
            try:
                user32 = ctypes.windll.user32
                win_w = getattr(self, '_win_w_phys', user32.GetSystemMetrics(0))
                win_h = getattr(self, '_win_h_phys', user32.GetSystemMetrics(1))
            except Exception:
                win_w = getattr(self, '_win_w_phys', 1920)
                win_h = getattr(self, '_win_h_phys', 1080)
        else:
            win_h = getattr(self, '_win_h_phys', 500)
            win_w = getattr(self, '_win_w_phys', getattr(self, '_hud_w', 540))
        dpi_s = getattr(self, '_dpi_scale', 1.0)
        try:
            gdi32 = ctypes.windll.gdi32
            user32 = ctypes.windll.user32
            if getattr(self, '_hp_fullscreen', False):
                hrgn = gdi32.CreateRectRgn(0, 0, max(1, win_w), max(1, win_h))
                user32.SetWindowRgn(self._hp_hwnd, hrgn, True)
                return

            # ── 在 JS 上报实际 hit regions 之前, 不限制窗口区域 ──
            # 依赖 WS_EX_LAYERED + LWA_COLORKEY 让 key-color 像素自动穿透,
            # 避免 fallback 坐标不准导致整个面板无法点击.
            js_ready = getattr(self, '_hp_hit_regions_ready', False)
            has_display = bool(getattr(self, '_hp_display_regions', None))
            if not js_ready and not has_display and not expanded:
                # 移除限制: 让整个窗口矩形有效, COLORKEY 处理穿透
                user32.SetWindowRgn(self._hp_hwnd, 0, True)
                return

            hit_rects = []
            off_x = int(getattr(self, '_hp_viewport_offset_x', 0) or 0)
            off_y = int(getattr(self, '_hp_viewport_offset_y', 0) or 0)
            regions = getattr(self, '_hp_display_regions', []) or []
            if not regions and not js_ready:
                regions = self._default_hp_display_regions()
            for rect in regions:
                if not isinstance(rect, dict):
                    continue
                try:
                    left = int(float(rect.get('left', 0)) * dpi_s) + off_x
                    top = int(float(rect.get('top', 0)) * dpi_s) + off_y
                    width = int(float(rect.get('width', 0)) * dpi_s)
                    height = int(float(rect.get('height', 0)) * dpi_s)
                except Exception:
                    continue
                if width < 2 or height < 2:
                    continue
                right = min(win_w, left + width)
                bottom = min(win_h, top + height)
                left = max(0, left)
                top = max(0, top)
                if right - left >= 2 and bottom - top >= 2:
                    hit_rects.append((left, top, right, bottom))

            if menu_bounds and isinstance(menu_bounds, dict):
                try:
                    left = int(float(menu_bounds.get('left', 0)) * dpi_s) + off_x
                    top = int(float(menu_bounds.get('top', 0)) * dpi_s) + off_y
                    width = int(float(menu_bounds.get('width', 0)) * dpi_s)
                    height = int(float(menu_bounds.get('height', 0)) * dpi_s)
                    pad = int(18 * dpi_s)
                    right = min(win_w, left + width + pad)
                    bottom = min(win_h, top + height + pad)
                    left = max(0, left - pad)
                    top = max(0, top - pad)
                    if right - left >= 2 and bottom - top >= 2:
                        hit_rects.append((left, top, right, bottom))
                except Exception:
                    pass

            if hit_rects:
                hrgn = gdi32.CreateRectRgn(0, 0, 0, 0)
                RGN_OR = 2
                for left, top, right, bottom in hit_rects:
                    rect_rgn = gdi32.CreateRectRgn(left, top, right, bottom)
                    gdi32.CombineRgn(hrgn, hrgn, rect_rgn, RGN_OR)
                    gdi32.DeleteObject(rect_rgn)
            else:
                if js_ready:
                    empty = gdi32.CreateRectRgn(0, 0, 0, 0)
                    user32.SetWindowRgn(self._hp_hwnd, empty, True)
                    return
                # 无精确区域 — 全窗口有效, 依赖 COLORKEY 穿透
                user32.SetWindowRgn(self._hp_hwnd, 0, True)
                return
            user32.SetWindowRgn(self._hp_hwnd, hrgn, True)
        except Exception:
            pass

    def _ensure_hp_on_top(self):
        if not self._hp_hwnd:
            return
        try:
            HWND_TOPMOST = ctypes.c_void_p(-1)
            SWP_NOMOVE = 0x0002
            SWP_NOSIZE = 0x0001
            SWP_NOACTIVATE = 0x0010
            ctypes.windll.user32.SetWindowPos(
                self._hp_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        except Exception:
            pass
        self._maintain_plugin_surfaces()

    def _force_hp_to_bottom(self, force: bool = False, quiet: bool = False):
        """用 GetWindowRect + SetWindowPos 强制 HP 窗口贴屏幕底部 (物理像素)。"""
        if not self._hp_hwnd:
            return
        if not force and self._is_hp_position_locked():
            return
        try:
            user32 = ctypes.windll.user32
            target_hwnd = self._hp_hwnd
            target_rect = None
            monitor_left, monitor_top, monitor_right, monitor_bottom = self._get_monitor_rect_for_target(
                hwnd=target_hwnd, rect=target_rect
            )
            self._hud_monitor_rect = (monitor_left, monitor_top, monitor_right, monitor_bottom)
            self._refresh_webview_dpi_scale(hwnd=target_hwnd, rect=target_rect)
            sw = max(1, monitor_right - monitor_left)
            sh = max(1, monitor_bottom - monitor_top)

            class RECT(ctypes.Structure):
                _fields_ = [('left', ctypes.c_long), ('top', ctypes.c_long),
                             ('right', ctypes.c_long), ('bottom', ctypes.c_long)]
            rc = RECT()
            user32.GetWindowRect(self._hp_hwnd, ctypes.byref(rc))
            win_w = rc.right - rc.left
            win_h = rc.bottom - rc.top
            if win_w < 10 or win_h < 10:
                return

            # 目标: 与 Entity 模式对齐 (x=4%屏宽, 底边贴屏幕底)
            if getattr(self, '_hp_fullscreen', False):
                # WebView/WinForms occasionally leaves a small non-client gutter
                # at the bottom even for frameless transparent windows.
                # Overscan the fullscreen HUD window slightly so the visible
                # display area truly reaches the monitor edge.
                overscan = max(12, int(sh * 0.012))
                target_x = monitor_left
                target_y = monitor_top - overscan
                win_w = sw
                win_h = sh + overscan
                self._hp_viewport_offset_x = 0
                self._hp_viewport_offset_y = overscan
            else:
                target_x, _ = self._calc_hud_target(sw, sh, left=monitor_left, top=monitor_top)
                target_y = monitor_top + sh - win_h
                self._hp_viewport_offset_x = 0
                self._hp_viewport_offset_y = 0

            # 保存实测/目标物理尺寸 (供 _set_hp_region 使用)
            self._win_w_phys = win_w
            self._win_h_phys = win_h

            HWND_TOPMOST = ctypes.c_void_p(-1)
            SWP_NOACTIVATE = 0x0010
            user32.SetWindowPos(
                self._hp_hwnd, HWND_TOPMOST,
                target_x, target_y, win_w, win_h,
                SWP_NOACTIVATE)

            # 用实测尺寸重新设置裁剪区域
            self._set_hp_region(False)
            self._request_hp_hit_regions()
            threading.Timer(0.12, self._request_hp_hit_regions).start()
            threading.Timer(0.32, self._request_hp_hit_regions).start()
            # Extra delayed retries to handle startup race condition
            threading.Timer(0.6, self._request_hp_hit_regions).start()
            threading.Timer(1.2, self._request_hp_hit_regions).start()
            if not quiet:
                print(f'[SAO] force position: screen_h={sh}, win={win_w}x{win_h}, y={target_y}')
        except Exception as e:
            if not quiet:
                print(f'[SAO] force position error: {e}')

    def _set_window_alpha(self, title, alpha):
        """Win32 LWA_ALPHA — 设置窗口整体透明度 (0.0~1.0), 保留色键透明."""
        try:
            user32 = ctypes.windll.user32
            hwnd = user32.FindWindowW(None, title)
            if not hwnd:
                return
            GWL_EXSTYLE = -20
            WS_EX_LAYERED = 0x00080000
            ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            if not (ex & WS_EX_LAYERED):
                user32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED)
            alpha_byte = int(max(0, min(255, alpha * 255)))
            user32.SetLayeredWindowAttributes(hwnd, _COLORREF_KEY, alpha_byte,
                                              _LWA_ALPHA | _LWA_COLORKEY)
        except Exception:
            pass

    def _animate_window_alpha(self, title, start, end, duration_ms=220, steps=8, on_done=None):
        def _run():
            try:
                total_steps = max(1, int(steps))
                sleep_s = max(0.0, float(duration_ms) / 1000.0 / total_steps)
                for i in range(total_steps + 1):
                    t = i / total_steps
                    alpha = start + (end - start) * t
                    self._set_window_alpha(title, alpha)
                    if i < total_steps and sleep_s > 0:
                        time.sleep(sleep_s)
            except Exception:
                pass
            if on_done:
                try:
                    on_done()
                except Exception:
                    pass

        threading.Thread(target=_run, daemon=True).start()

    def _reassert_menu_transparency(self, alpha: float = None):
        try:
            self._apply_webview2_transparency()
            if alpha is None:
                alpha = 1.0 if self._menu_visible else 0.0
            self._set_window_alpha('SAO Menu', alpha)
        except Exception:
            pass

    # ─── HP 入场动画: 从屏幕中央滑到固定位置 ───
    def _animate_hp_entry(self):
        def _slide():
            # Wait for hwnd if not yet available (retry up to 2s)
            for _wait_i in range(20):
                if self._hp_hwnd:
                    break
                time.sleep(0.1)
            if not self._hp_hwnd:
                # hwnd never appeared — force setup and bail
                self._setup_click_through()
                time.sleep(0.3)
                if self._hp_hwnd:
                    self._force_hp_to_bottom(force=True, quiet=True)
                return
            self._hp_entry_animating = True
            try:
                user32 = ctypes.windll.user32
                sw = user32.GetSystemMetrics(0)
                sh = user32.GetSystemMetrics(1)

                class RECT(ctypes.Structure):
                    _fields_ = [('left', ctypes.c_long), ('top', ctypes.c_long),
                                 ('right', ctypes.c_long), ('bottom', ctypes.c_long)]
                rc = RECT()
                user32.GetWindowRect(self._hp_hwnd, ctypes.byref(rc))
                win_w = rc.right - rc.left
                win_h = rc.bottom - rc.top
                if win_w < 10 or win_h < 10:
                    self._force_hp_to_bottom(force=True, quiet=True)
                    return

                HWND_TOPMOST = ctypes.c_void_p(-1)
                SWP_NOACTIVATE = 0x0010
                SWP_NOSIZE = 0x0001

                if getattr(self, '_hp_fullscreen', False):
                    overscan = max(12, int(sh * 0.012))
                    tx, ty = 0, -overscan
                    sx, sy = 0, sh
                    user32.SetWindowPos(
                        self._hp_hwnd, HWND_TOPMOST,
                        sx, sy, sw, sh + overscan,
                        SWP_NOACTIVATE)
                    time.sleep(0.02)
                else:
                    # 起点: 强制移到屏幕中央 (无论当前在哪)
                    sx = (sw - win_w) // 2
                    sy = (sh - win_h) // 2
                    user32.SetWindowPos(
                        self._hp_hwnd, HWND_TOPMOST,
                        sx, sy, 0, 0,
                        SWP_NOACTIVATE | SWP_NOSIZE)
                    time.sleep(0.02)

                    # 终点: 窗口底边贴屏幕底边
                    tx = int(getattr(self, '_hp_target_x', self._calc_hud_target(sw, sh)[0]) or 0)
                    ty = int(getattr(self, '_hp_target_y', sh - win_h) or (sh - win_h))

                HWND_TOPMOST = ctypes.c_void_p(-1)
                SWP_NOACTIVATE = 0x0010
                SWP_NOSIZE = 0x0001

                steps = 28
                duration = 0.65
                dt = duration / steps
                for i in range(1, steps + 1):
                    t = i / steps
                    ease = 1 - (1 - t) ** 3
                    nx = int(sx + (tx - sx) * ease)
                    ny = int(sy + (ty - sy) * ease)
                    try:
                        if getattr(self, '_hp_fullscreen', False):
                            user32.SetWindowPos(
                                self._hp_hwnd, HWND_TOPMOST,
                                nx, ny, sw, sh + overscan,
                                SWP_NOACTIVATE)
                        else:
                            user32.SetWindowPos(
                                self._hp_hwnd, HWND_TOPMOST,
                                nx, ny, 0, 0,
                                SWP_NOACTIVATE | SWP_NOSIZE)
                    except Exception:
                        break
                    time.sleep(dt)

                # 最终精确定位
                self._force_hp_to_bottom(force=True, quiet=True)
            except Exception:
                self._force_hp_to_bottom(force=True, quiet=True)
            finally:
                self._hp_entry_animating = False
                self._lock_hp_position(1.0)
        threading.Thread(target=_slide, daemon=True).start()

    # ─── WebView 就绪 ───
    def _plugin_layer_window_map(self):
        """Map every SAO WebView window to its plugin render-surface id.

        Injecting ``plugin_layer.js`` into each window gives plugins a universal
        overlay layer (and, for opt-in pages, render-hook taps) on the main UI
        and every floating window — not just the ACT panels.
        """
        g = self
        items = [
            (getattr(g, 'hp_win', None), 'hp'),
            (getattr(g, 'menu_win', None), 'menu'),
            (getattr(g, 'alert_win', None), 'alert'),
            (getattr(g, 'mapbanner_win', None), 'mapbanner'),
            (getattr(g, 'mech_banner_win', None), 'mech_banner'),
            (getattr(g, 'trigger_timer_win', None), 'trigger_timer'),
            (getattr(g, 'data_source_health_win', None), 'data_source_health'),
            (getattr(g, 'report_export_win', None), 'report_export'),
            (getattr(g, 'offline_import_win', None), 'offline_import'),
            (getattr(g, 'timeline_vcr_win', None), 'timeline_vcr'),
            (getattr(g, 'act_aggregate_win', None), 'act_aggregate'),
            (getattr(g, 'mem_scope_win', None), 'mem_scope'),
            (getattr(g, 'action_log_win', None), 'action_log'),
            (getattr(g, 'death_recap_win', None), 'death_recap'),
            (getattr(g, 'graph_timeseries_win', None), 'graph_timeseries'),
            (getattr(g, 'combatant_drilldown_win', None), 'combatant_drilldown'),
            (getattr(g, 'skill_drilldown_win', None), 'skill_drilldown'),
        ]
        items.extend(list(self._plugin_surface_items()))
        return items

    def _plugin_layer_source(self):
        src = getattr(self, '_plugin_layer_js_cache', None)
        if src is None:
            try:
                with open(os.path.join(WEB_DIR, 'plugin_layer.js'), 'r', encoding='utf-8') as fp:
                    src = fp.read()
            except Exception:
                src = ''
            self._plugin_layer_js_cache = src
        return src

    def _inject_all_plugin_layers(self):
        """Inject plugin_layer.js into every live WebView window (idempotent)."""
        src = self._plugin_layer_source()
        if not src:
            return
        for win, surface in self._plugin_layer_window_map():
            if not win:
                continue
            try:
                win.evaluate_js(
                    "if(!window.__SAO_PLUGIN_LAYER__){window.__SAO_PLUGIN_LAYER__=1;"
                    f"window.__SAO_SURFACE__={json.dumps(surface)};\n{src}\n}}"
                )
            except Exception:
                pass

    def _close_license_panel(self):
        try:
            if self._license_win:
                self._license_win.destroy()
                self._license_win = None
        except Exception:
            pass
        self._license_done_event.set()

    def _on_webview_started(self):
        def _init():
            self._lock_hp_position(2.0)
            self._hp_hit_regions_ready = False
            self._hp_js_hit_regions_ready = False
            self._hp_last_hit_region_ts = 0.0
            # ── 先应用透明, 再显示窗口 (防止白底/黑底闪现) ──
            self._apply_webview2_transparency()
            time.sleep(0.15)
            self._apply_webview2_transparency()  # 二次确保
            # 显示前设 alpha=0, 防止冷启动时 WebView2 未就绪导致黑底闪现
            self._set_window_alpha('SAO-HP', 0.0)
            try:
                if self.hp_win and not self._hp_visible:
                    self.hp_win.show()
                    self._hp_visible = True
            except Exception:
                pass
            time.sleep(0.18)
            initial_state = getattr(getattr(self, '_state_mgr', None), 'state', None)
            self._dispatch_webview_extension('sync_identity_panel', initial_state)
            self._sync_menu_info()
            self._dispatch_webview_extension('on_webview_started')
            # 注入插件渲染层 (plugin_layer.js) 到所有 WebView 窗口 —— 给插件
            # overlay/hook 能力覆盖主UI与全部悬浮窗, 延迟确保页面已加载.
            try:
                self._inject_all_plugin_layers()
                threading.Timer(2.5, self._inject_all_plugin_layers).start()
            except Exception:
                pass
            # 设置 click-through (延迟确保窗口已完全创建)
            time.sleep(0.3)
            self._setup_click_through()
            self._request_hp_hit_regions()
            self._start_hp_position_guard()
            self._start_hp_click_bootstrap(8.0)
            # WebView2 透明背景 — 持续重试
            self._apply_webview2_transparency()
            # 标记 reveal 未完成, 阻止 _reassert_hp_transparency 修改 alpha
            self._hp_reveal_pending = True
            self._reassert_hp_transparency(1.0, retries=15, delay=0.35)
            # HP 启动时 alpha=0, 0.8s 后淡入 (等待透明应用后再变可见)
            def _reveal_windows():
                self._hp_reveal_pending = False
                self._set_window_alpha('SAO-HP', 1.0)
                self._mark_update_popup_ready()
            threading.Timer(0.8, _reveal_windows).start()
            # Safety: re-run _force_hp_to_bottom after a delay in case hwnd
            # was not available during the first attempt.
            def _safety_force():
                if self._hp_hwnd and not getattr(self, '_win_h_phys', 0):
                    self._force_hp_to_bottom()
            threading.Timer(1.5, _safety_force).start()
            threading.Timer(3.0, _safety_force).start()
            # 首次启动安全网: 12s/20s 后再做一次完整的 click-through + 透明重试,
            # 针对第一次开机 WebView2 初始化极慢的情况.
            def _late_hp_recovery():
                try:
                    self._apply_webview2_transparency()
                    # 如果 hwnd 仍未获取, 再尝试一次
                    if not self._hp_hwnd:
                        self._setup_click_through()
                    self._ensure_hp_clickable()
                    self._request_hp_hit_regions()
                    self._set_hp_region(False)
                    self._maintain_plugin_surfaces()
                except Exception:
                    pass
            threading.Timer(12.0, _late_hp_recovery).start()
            threading.Timer(20.0, _late_hp_recovery).start()
            # 任务栏图标
            self._set_window_icon('SAO-HP')
            self._set_window_icon('SAO Menu')
            self._set_window_icon('SAO Alert')
            self._set_window_icon('SAO-PluginManager')
            self._set_window_icon('SAO-TriggerTimerManager')
            self._set_window_icon('SAO-DataSourceHealth')
            self._set_window_icon('SAO-ReportExport')
            self._set_window_icon('SAO-OfflineImport')
            # 菜单窗口在启动阶段保持完全透明, 避免偶发白色方框闪现
            self._set_window_alpha('SAO Menu', 0.0)
            self._set_window_alpha('SAO Alert', 1.0)
            # Alert window: default click-through so the hidden window never captures clicks
            self._setup_alert_click_through()
            try:
                self._wait_and_apply_click_through('SAO-PluginManager', timeout=0.5)
                self._wait_and_apply_click_through('SAO-TriggerTimerManager', timeout=0.5)
                self._wait_and_apply_click_through('SAO-DataSourceHealth', timeout=0.5)
                self._wait_and_apply_click_through('SAO-ReportExport', timeout=0.5)
                self._wait_and_apply_click_through('SAO-OfflineImport', timeout=0.5)
            except Exception:
                pass
            try:
                self._plugin_manager_visible = False
                if self.plugin_manager_win:
                    self._set_window_alpha('SAO-PluginManager', 0.0)
                    self.plugin_manager_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._trigger_timer_manager_visible = False
                if self.trigger_timer_win:
                    self._set_window_alpha('SAO-TriggerTimerManager', 0.0)
                    self.trigger_timer_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._data_source_health_visible = False
                if self.data_source_health_win:
                    self._set_window_alpha('SAO-DataSourceHealth', 0.0)
                    self.data_source_health_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._report_export_visible = False
                if self.report_export_win:
                    self._set_window_alpha('SAO-ReportExport', 0.0)
                    self.report_export_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._offline_import_visible = False
                if self.offline_import_win:
                    self._set_window_alpha('SAO-OfflineImport', 0.0)
                    self.offline_import_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._timeline_vcr_visible = False
                if self.timeline_vcr_win:
                    self._set_window_alpha('SAO-TimelineVCR', 0.0)
                    self.timeline_vcr_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._act_aggregate_visible = False
                if self.act_aggregate_win:
                    self._set_window_alpha('SAO-ActAggregate', 0.0)
                    self.act_aggregate_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._mem_scope_visible = False
                if self.mem_scope_win:
                    self._set_window_alpha('SAO-MemScope', 0.0)
                    self.mem_scope_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._action_log_visible = False
                if self.action_log_win:
                    self._set_window_alpha('SAO-ActionLog', 0.0)
                    self.action_log_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._death_recap_visible = False
                if self.death_recap_win:
                    self._set_window_alpha('SAO-DeathRecap', 0.0)
                    self.death_recap_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._graph_timeseries_visible = False
                if self.graph_timeseries_win:
                    self._set_window_alpha('SAO-GraphTimeseries', 0.0)
                    self.graph_timeseries_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._combatant_drilldown_visible = False
                if self.combatant_drilldown_win:
                    self._set_window_alpha('SAO-CombatantDrilldown', 0.0)
                    self.combatant_drilldown_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            try:
                self._skill_drilldown_visible = False
                if self.skill_drilldown_win:
                    self._set_window_alpha('SAO-SkillDrilldown', 0.0)
                    self.skill_drilldown_win.hide()
                    self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            # 重新触发 HP 入场动态模糊 (避免页面预加载时动画已经跑完)
            self._eval_hp('if (window.HP && HP.retriggerEntryBlur) HP.retriggerEntryBlur()')
            # HP 窗口入场动画: 从中央滑到固定位置
            self._animate_hp_entry()
            # 启动识别引擎
            self._start_recognition()
        threading.Timer(0.5, _init).start()

        threading.Thread(target=self._recognition_loop, daemon=True).start()
        threading.Thread(target=self._save_position_loop, daemon=True).start()
        self._setup_hotkeys()

    # ─── 识别引擎 ───
    def _start_recognition(self):
        """启动游戏数据引擎 (抓包 + 纯识图)"""
        try:
            self._bootstrap_runtime_state()
            cfg_settings = self._cfg_settings_ref
            self._dispatch_webview_extension('on_recognition_start')

            # Restore sound settings
            try:
                from utils.sao_sound import set_sound_enabled, set_sound_volume
                _snd_on = cfg_settings.get('sound_enabled', True)
                _snd_vol = cfg_settings.get('sound_volume', 70)
                set_sound_enabled(bool(_snd_on) if _snd_on is not None else True)
                set_sound_volume(int(_snd_vol) if _snd_vol is not None else 70)
            except Exception:
                pass

            # 游戏状态缓存恢复由插件 on_load 处理

            # 5.0.0: 引擎由插件 on_load 创建
            self._reconfigure_data_engines(restart_packet=not bool(getattr(self, '_packet_engine', None)))

            # 启动定时缓存保存 (每30秒)
            import threading as _thr
            _stop_evt = self._cache_loop_stop
            def _cache_loop():
                import time as _t
                while not _stop_evt.is_set():
                    _stop_evt.wait(30)
                    if _stop_evt.is_set():
                        break
                    try:
                        self._persist_cached_identity_state(save_now=False)
                        self._state_mgr.save_cache(self._cfg_settings_ref)
                    except Exception:
                        pass
            _thr.Thread(target=_cache_loop, daemon=True, name='cache_saver').start()

        except Exception as e:
            print(f"[SAO] Data engine failed: {e}")
            import traceback; traceback.print_exc()
            self._recognition_active = False

    def _start_sta_pixel_detector(self, cfg_settings):
        """Removed: stamina now updates through the main vision engine only."""
        self._sta_detector_started = False
        return

    def _toggle_recognition(self):
        """切换识别开关 — 线程安全"""
        with self._recog_lock:
            self._recognition_active = not self._recognition_active
            state = "ON" if self._recognition_active else "OFF"
            if not self._recognition_active:
                self._dispatch_webview_extension('_reset_sta_offline_state')
        self._eval_menu(f'SAO.showToast("识别: {state}")')


    # ─── 快捷键 ───
    _FKEY_VK = {
        'F1': 112, 'F2': 113, 'F3': 114, 'F4': 115,
        'F5': 116, 'F6': 117, 'F7': 118, 'F8': 119,
        'F9': 120, 'F10': 121, 'F11': 122, 'F12': 123,
    }

    def _setup_hotkeys(self):
        self._hk_actions = {
            'toggle_recognition': self._toggle_recognition,
            'toggle_topmost': lambda: None,
            'hide_panels': lambda: None,
            'show_plugins': lambda: (self._show_plugin_manager() if not self._plugin_manager_visible else self._hide_plugin_manager()),
        }
        self._hk_pressed = set()
        self._hk_listener = None
        self._hk_poll_prev = {}  # previous state for GetAsyncKeyState polling
        try:
            from pynput.keyboard import Listener as KbListener, Key, KeyCode
            self._hk_Key = Key
            self._hk_KeyCode = KeyCode
            self._hk_listener = KbListener(
                on_press=self._hk_on_press, on_release=self._hk_on_release)
            self._hk_listener.daemon = True
            self._hk_listener.start()
            self._hotkeys_ok = True
            print('[SAO WebView] Hotkeys (pynput): enabled')
        except Exception as e:
            self._hotkeys_ok = False
            print(f'[SAO WebView] Hotkeys (pynput) unavailable: {e}')

        # ── GetAsyncKeyState polling fallback ──
        # pynput's WH_KEYBOARD_LL hook can fail silently when the game uses
        # DirectInput / exclusive input. Polling GetAsyncKeyState always works.
        self._hk_poll_ok = False
        try:
            import ctypes
            self._hk_GetAsyncKeyState = ctypes.windll.user32.GetAsyncKeyState
            self._hk_poll_ok = True
            print('[SAO WebView] Hotkeys (poll fallback): enabled')
        except Exception as e:
            print(f'[SAO WebView] Hotkeys poll fallback unavailable: {e}')

    def _hk_on_press(self, key):
        try:
            if isinstance(key, self._hk_KeyCode) and key.vk:
                self._hk_pressed.add(key.vk)
            elif isinstance(key, self._hk_Key):
                self._hk_pressed.add(key.value.vk if hasattr(key.value, 'vk') else str(key))
        except Exception:
            pass
        try:
            self._hk_check()
        except Exception:
            pass

    def _hk_on_release(self, key):
        try:
            if isinstance(key, self._hk_KeyCode) and key.vk:
                self._hk_pressed.discard(key.vk)
            elif isinstance(key, self._hk_Key):
                self._hk_pressed.discard(key.value.vk if hasattr(key.value, 'vk') else str(key))
        except Exception:
            pass

    def _hk_plugin_map(self):
        """插件快捷键 {action: {'key': 'CTRL+F8', 'callback': fn}} — 镜像
        Entity 端 _plugin_hotkey_map。用户在 settings['hotkeys'] 的覆盖优先
        于插件声明的 default_key; 每次按键现解析, 热加载的插件即时生效。
        webview 无 Tk 主循环, 回调在监听/轮询线程直接派发。
        """
        out = {}
        try:
            from act_platform.runtime import ensure_act_plugin_manager
            mgr = ensure_act_plugin_manager(self, load=False)
            saved = getattr(self, '_cfg_settings_ref', None)
            saved = {} if saved is None else (saved.get('hotkeys') or {})
            for hk in mgr.list_hotkeys():
                if not hk.get('active'):
                    continue
                action = str(hk.get('action') or '')
                key = ''
                if isinstance(saved, dict) and action in saved:
                    v = saved[action]
                    key = (v.get('key') or v.get('name')) if isinstance(v, dict) else str(v)
                key = str(key or hk.get('default_key') or '').upper()
                if not key:
                    continue
                out[action] = {
                    'key': key,
                    'callback': (lambda a=action, m=mgr: m.dispatch_hotkey(a)),
                }
        except Exception:
            pass
        return out

    def _hk_clear_pressed_main(self):
        # 触发后只清主键、保留修饰键 — pynput 不会为仍按住的 Ctrl 重发
        # press 事件, 全清会让紧接着的下一个 Ctrl+F 组合丢失 Ctrl 状态。
        self._hk_pressed = {k for k in self._hk_pressed
                            if k in HOTKEY_MOD_ALL_VKS}

    def _hk_bindings(self):
        """全部活动绑定 [(parsed, (callback, vk))] — 内置在前 (并列特异度时
        内置优先), 插件映射在后; 每次按键/轮询现解析。"""
        saved = getattr(self, '_cfg_settings_ref', None)
        user_hotkeys = {} if saved is None else (saved.get('hotkeys') or {})
        # Merge saved hotkeys with defaults so newly added keys are always
        # available even if settings.json doesn't contain them.
        hotkeys = {**DEFAULT_HOTKEYS, **user_hotkeys}
        bindings = []
        for action, info in hotkeys.items():
            cb = self._hk_actions.get(action)
            if cb is None:
                continue
            parsed = parse_hotkey(info)
            if parsed:
                bindings.append((parsed, (cb, parsed['vk'])))
        for action, info in self._hk_plugin_map().items():
            parsed = parse_hotkey(info)
            cb = info.get('callback')
            if parsed and cb:
                bindings.append((parsed, (cb, parsed['vk'])))
        return bindings

    def _hk_check(self):
        # 'F5' 与 'CTRL+F5' 等组合共存: select_hotkey_match 取最特异命中。
        hit = select_hotkey_match(self._hk_bindings(), self._hk_pressed)
        if hit:
            cb, vk = hit
            # 标记轮询沿状态, 防止 50ms 内 _hk_poll_tick 对同一次物理按键
            # 再发一次 (pynput 钩子健康时两路都活着)。
            self._hk_poll_prev[vk] = True
            threading.Thread(target=cb, daemon=True).start()
            self._hk_clear_pressed_main()

    def _hk_poll_tick(self):
        """Poll GetAsyncKeyState for F-key presses (called from recognition loop).

        This is a fallback for when pynput's keyboard hook fails to receive events
        (common with DirectInput games). Detects rising edges only.
        """
        if not getattr(self, '_hk_poll_ok', False):
            return
        try:
            bindings = self._hk_bindings()
            if not bindings:
                return
            # 同一主键可挂多个组合 (F5 / CTRL+F5): 先按 vk 统一算上升沿,
            # 再对每个新落下的主键选最特异命中。不提前 return —— 同一 tick
            # 内按下的第二个热键的沿不能被吞掉。
            mods_down = hotkey_mods_down()
            fresh = set()
            for vk in {payload[1] for _parsed, payload in bindings}:
                # GetAsyncKeyState returns short; bit 15 = currently pressed
                is_pressed = bool(self._hk_GetAsyncKeyState(vk) & 0x8000)
                if is_pressed and not self._hk_poll_prev.get(vk, False):
                    fresh.add(vk)
                self._hk_poll_prev[vk] = is_pressed
            for vk in fresh:
                hit = select_hotkey_match(
                    [(p, pl) for p, pl in bindings if pl[1] == vk],
                    {vk}, mods_down=mods_down)
                if hit:
                    threading.Thread(target=hit[0], daemon=True).start()
        except Exception:
            pass

    # ════════════════════════════════════════
    #  JS 辅助
    # ════════════════════════════════════════
    def _eval_hp(self, js):
        try:
            self.hp_win.evaluate_js(js)
        except Exception:
            pass

    def _eval_menu(self, js):
        try:
            self.menu_win.evaluate_js(js)
        except Exception:
            pass

    def _eval_alert(self, js):
        try:
            if self.alert_win:
                self.alert_win.evaluate_js(js)
        except Exception:
            pass

    def _eval_mapbanner(self, js):
        try:
            if self.mapbanner_win:
                self.mapbanner_win.evaluate_js(js)
        except Exception:
            pass

    def _eval_mech_banner(self, js):
        try:
            if self.mech_banner_win:
                self.mech_banner_win.evaluate_js(js)
        except Exception:
            pass

    def _push_mech_banner(self, entry):
        """机制横幅推一行 (懒显示窗口, 常驻鼠标穿透覆盖层)。"""
        try:
            if not self.mech_banner_win:
                return
            if not self._mech_banner_shown:
                self._mech_banner_shown = True
                try:
                    self.mech_banner_win.show()
                except Exception:
                    pass
                self._setup_mech_banner_click_through()
            payload = json.dumps(entry or {}, ensure_ascii=False)
            self._eval_mech_banner(
                f'if (window.MechBanner) MechBanner.push({payload})')
        except Exception:
            pass

    def _setup_mech_banner_click_through(self, _wait_retries: int = 20):
        """Make mechanic banner overlay fully click-through (纯覆盖层)。"""
        try:
            user32 = ctypes.windll.user32
            hwnd = user32.FindWindowW(None, 'SAO MechBanner')
            if not hwnd and _wait_retries > 0:
                threading.Timer(0.1, lambda: self._setup_mech_banner_click_through(
                    _wait_retries - 1)).start()
                return
            if not hwnd:
                return
            self._mech_banner_hwnd = hwnd
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            ex |= (_WS_EX_TRANSPARENT | _WS_EX_LAYERED)
            user32.SetWindowLongW(hwnd, _GWL_EXSTYLE, ex)
        except Exception:
            pass

    def _on_mechanic_event(self, evt):
        """引擎机制事件 → TTS + 顶部横幅 (controller 缺位时静默丢弃)。"""
        controller = getattr(self, '_mech_alert_controller', None)
        if controller is not None:
            try:
                controller.on_mechanic(evt)
            except Exception:
                pass

    @staticmethod
    def _safe_js(s: str) -> str:
        if not s:
            return ''
        return s.replace('\\', '\\\\').replace('"', '\\"').replace("'", "\\'").replace('\n', '\\n')

    def _resolved_hotkey(self, action: str, fallback: str) -> str:
        """热键标签按 {**DEFAULT_HOTKEYS, **saved} 解析 — 标签跟随用户改键不漂移。"""
        try:
            from config import DEFAULT_HOTKEYS
            saved = (self._cfg_settings_ref.get('hotkeys', {})
                     if getattr(self, '_cfg_settings_ref', None) else {})
            merged = {**DEFAULT_HOTKEYS, **(saved if isinstance(saved, dict) else {})}
            v = merged.get(action)
            if isinstance(v, dict):
                v = v.get('key') or v.get('name')
            return str(v or fallback).upper()
        except Exception:
            return fallback

    def _eval_plugin_manager(self, js):
        try:
            if self.plugin_manager_win:
                self.plugin_manager_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_plugin_manager_clickable(self):
        """Remove WS_EX_TRANSPARENT so the plugin manager receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-PluginManager')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_plugin_manager(self):
        try:
            if self.plugin_manager_win and not self._plugin_manager_visible:
                self._set_window_alpha('SAO-PluginManager', 0.0)
                self.plugin_manager_win.show()
                self._eval_plugin_manager('if(window.PluginManager&&PluginManager.fadeIn)PluginManager.fadeIn()')
                self._eval_plugin_manager('if(window.PluginManager&&PluginManager.refresh)PluginManager.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-PluginManager', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._plugin_manager_visible = True
                self._ensure_plugin_manager_clickable()
                threading.Timer(0.5, self._ensure_plugin_manager_clickable).start()
        except Exception:
            pass

    def _hide_plugin_manager(self):
        try:
            if self.plugin_manager_win and self._plugin_manager_visible:
                self._eval_plugin_manager('if(window.PluginManager&&PluginManager.fadeOut)PluginManager.fadeOut()')
                def _finish():
                    try:
                        if self.plugin_manager_win:
                            self.plugin_manager_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._plugin_manager_visible = False

    # ── ACT Trigger/Timer Manager panel ──

    def _eval_trigger_timer_manager(self, js):
        try:
            if self.trigger_timer_win:
                self.trigger_timer_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_trigger_timer_manager_clickable(self):
        """Remove WS_EX_TRANSPARENT so the trigger/timer manager receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-TriggerTimerManager')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_trigger_timer_manager(self):
        try:
            if self.trigger_timer_win and not self._trigger_timer_manager_visible:
                self._set_window_alpha('SAO-TriggerTimerManager', 0.0)
                self.trigger_timer_win.show()
                self._eval_trigger_timer_manager('if(window.TriggerTimerManager&&TriggerTimerManager.fadeIn)TriggerTimerManager.fadeIn()')
                self._eval_trigger_timer_manager('if(window.TriggerTimerManager&&TriggerTimerManager.refresh)TriggerTimerManager.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-TriggerTimerManager', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._trigger_timer_manager_visible = True
                self._ensure_trigger_timer_manager_clickable()
                threading.Timer(0.5, self._ensure_trigger_timer_manager_clickable).start()
        except Exception:
            pass

    def _hide_trigger_timer_manager(self):
        try:
            if self.trigger_timer_win and self._trigger_timer_manager_visible:
                self._eval_trigger_timer_manager('if(window.TriggerTimerManager&&TriggerTimerManager.fadeOut)TriggerTimerManager.fadeOut()')
                def _finish():
                    try:
                        if self.trigger_timer_win:
                            self.trigger_timer_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._trigger_timer_manager_visible = False

    # ── ACT Data Source Health panel ──

    def _eval_data_source_health(self, js):
        try:
            if self.data_source_health_win:
                self.data_source_health_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_data_source_health_clickable(self):
        """Remove WS_EX_TRANSPARENT so the data-source health panel receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-DataSourceHealth')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_data_source_health(self):
        try:
            if self.data_source_health_win and not self._data_source_health_visible:
                self._set_window_alpha('SAO-DataSourceHealth', 0.0)
                self.data_source_health_win.show()
                self._eval_data_source_health('if(window.DataSourceHealth&&DataSourceHealth.fadeIn)DataSourceHealth.fadeIn()')
                self._eval_data_source_health('if(window.DataSourceHealth&&DataSourceHealth.refresh)DataSourceHealth.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-DataSourceHealth', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._data_source_health_visible = True
                self._ensure_data_source_health_clickable()
                threading.Timer(0.5, self._ensure_data_source_health_clickable).start()
        except Exception:
            pass

    def _hide_data_source_health(self):
        try:
            if self.data_source_health_win and self._data_source_health_visible:
                self._eval_data_source_health('if(window.DataSourceHealth&&DataSourceHealth.fadeOut)DataSourceHealth.fadeOut()')
                def _finish():
                    try:
                        if self.data_source_health_win:
                            self.data_source_health_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._data_source_health_visible = False

    # ── ACT Report/Export panel ──

    def _eval_report_export(self, js):
        try:
            if self.report_export_win:
                self.report_export_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_report_export_clickable(self):
        """Remove WS_EX_TRANSPARENT so the report/export panel receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-ReportExport')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_report_export(self):
        try:
            if self.report_export_win and not self._report_export_visible:
                self._set_window_alpha('SAO-ReportExport', 0.0)
                self.report_export_win.show()
                self._eval_report_export('if(window.ReportExport&&ReportExport.fadeIn)ReportExport.fadeIn()')
                self._eval_report_export('if(window.ReportExport&&ReportExport.refresh)ReportExport.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-ReportExport', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._report_export_visible = True
                self._ensure_report_export_clickable()
                threading.Timer(0.5, self._ensure_report_export_clickable).start()
        except Exception:
            pass

    def _hide_report_export(self):
        try:
            if self.report_export_win and self._report_export_visible:
                self._eval_report_export('if(window.ReportExport&&ReportExport.fadeOut)ReportExport.fadeOut()')
                def _finish():
                    try:
                        if self.report_export_win:
                            self.report_export_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._report_export_visible = False

    # ── ACT Offline Import panel ──

    def _eval_offline_import(self, js):
        try:
            if self.offline_import_win:
                self.offline_import_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_offline_import_clickable(self):
        """Remove WS_EX_TRANSPARENT so the offline-import panel receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-OfflineImport')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_offline_import(self):
        try:
            if self.offline_import_win and not self._offline_import_visible:
                self._set_window_alpha('SAO-OfflineImport', 0.0)
                self.offline_import_win.show()
                self._eval_offline_import('if(window.OfflineImport&&OfflineImport.fadeIn)OfflineImport.fadeIn()')
                self._eval_offline_import('if(window.OfflineImport&&OfflineImport.refresh)OfflineImport.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-OfflineImport', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._offline_import_visible = True
                self._ensure_offline_import_clickable()
                threading.Timer(0.5, self._ensure_offline_import_clickable).start()
        except Exception:
            pass

    def _hide_offline_import(self):
        try:
            if self.offline_import_win and self._offline_import_visible:
                self._eval_offline_import('if(window.OfflineImport&&OfflineImport.fadeOut)OfflineImport.fadeOut()')
                def _finish():
                    try:
                        if self.offline_import_win:
                            self.offline_import_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._offline_import_visible = False

    # ── ACT Timeline/VCR panel ──

    def _eval_timeline_vcr(self, js):
        try:
            if self.timeline_vcr_win:
                self.timeline_vcr_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_timeline_vcr_clickable(self):
        """Remove WS_EX_TRANSPARENT so the timeline/VCR panel receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-TimelineVCR')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_timeline_vcr(self):
        try:
            if self.timeline_vcr_win and not self._timeline_vcr_visible:
                self._set_window_alpha('SAO-TimelineVCR', 0.0)
                self.timeline_vcr_win.show()
                self._eval_timeline_vcr('if(window.TimelineVcr&&TimelineVcr.fadeIn)TimelineVcr.fadeIn()')
                self._eval_timeline_vcr('if(window.TimelineVcr&&TimelineVcr.refresh)TimelineVcr.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-TimelineVCR', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._timeline_vcr_visible = True
                self._ensure_timeline_vcr_clickable()
                threading.Timer(0.5, self._ensure_timeline_vcr_clickable).start()
        except Exception:
            pass

    def _hide_timeline_vcr(self):
        try:
            if self.timeline_vcr_win and self._timeline_vcr_visible:
                self._eval_timeline_vcr('if(window.TimelineVcr&&TimelineVcr.fadeOut)TimelineVcr.fadeOut()')
                def _finish():
                    try:
                        if self.timeline_vcr_win:
                            self.timeline_vcr_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._timeline_vcr_visible = False

    # ── ACT Semantic Aggregate panel ──

    def _eval_act_aggregate(self, js):
        try:
            if self.act_aggregate_win:
                self.act_aggregate_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_act_aggregate_clickable(self):
        """Remove WS_EX_TRANSPARENT so the aggregate cockpit receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-ActAggregate')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_act_aggregate(self):
        try:
            if self.act_aggregate_win and not self._act_aggregate_visible:
                self._set_window_alpha('SAO-ActAggregate', 0.0)
                self.act_aggregate_win.show()
                self._eval_act_aggregate('if(window.ActAggregate&&ActAggregate.fadeIn)ActAggregate.fadeIn()')
                self._eval_act_aggregate('if(window.ActAggregate&&ActAggregate.refresh)ActAggregate.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-ActAggregate', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._act_aggregate_visible = True
                self._ensure_act_aggregate_clickable()
                threading.Timer(0.5, self._ensure_act_aggregate_clickable).start()
        except Exception:
            pass

    def _hide_act_aggregate(self):
        try:
            if self.act_aggregate_win and self._act_aggregate_visible:
                self._eval_act_aggregate('if(window.ActAggregate&&ActAggregate.fadeOut)ActAggregate.fadeOut()')
                def _finish():
                    try:
                        if self.act_aggregate_win:
                            self.act_aggregate_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._act_aggregate_visible = False

    # ── Mem Scope panel ──

    def _eval_mem_scope(self, js):
        try:
            if self.mem_scope_win:
                self.mem_scope_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_mem_scope_clickable(self):
        """Remove WS_EX_TRANSPARENT so the mem scope panel receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-MemScope')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_mem_scope(self):
        try:
            if self.mem_scope_win and not self._mem_scope_visible:
                self._set_window_alpha('SAO-MemScope', 0.0)
                self.mem_scope_win.show()
                self._eval_mem_scope('if(window.MemScope&&MemScope.fadeIn)MemScope.fadeIn()')
                self._eval_mem_scope('if(window.MemScope&&MemScope.refresh)MemScope.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-MemScope', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._mem_scope_visible = True
                self._ensure_mem_scope_clickable()
                threading.Timer(0.5, self._ensure_mem_scope_clickable).start()
        except Exception:
            pass

    def _hide_mem_scope(self):
        try:
            if self.mem_scope_win and self._mem_scope_visible:
                self._eval_mem_scope('if(window.MemScope&&MemScope.fadeOut)MemScope.fadeOut()')
                def _finish():
                    try:
                        if self.mem_scope_win:
                            self.mem_scope_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._mem_scope_visible = False

    # ── ACT Action Log panel ──

    def _eval_action_log(self, js):
        try:
            if self.action_log_win:
                self.action_log_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_action_log_clickable(self):
        """Remove WS_EX_TRANSPARENT so the action-log panel receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-ActionLog')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_action_log(self):
        try:
            if self.action_log_win and not self._action_log_visible:
                self._set_window_alpha('SAO-ActionLog', 0.0)
                self.action_log_win.show()
                self._eval_action_log('if(window.ActionLog&&ActionLog.fadeIn)ActionLog.fadeIn()')
                self._eval_action_log('if(window.ActionLog&&ActionLog.refresh)ActionLog.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-ActionLog', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._action_log_visible = True
                self._ensure_action_log_clickable()
                threading.Timer(0.5, self._ensure_action_log_clickable).start()
        except Exception:
            pass

    def _hide_action_log(self):
        try:
            if self.action_log_win and self._action_log_visible:
                self._eval_action_log('if(window.ActionLog&&ActionLog.fadeOut)ActionLog.fadeOut()')
                def _finish():
                    try:
                        if self.action_log_win:
                            self.action_log_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._action_log_visible = False

    # ── ACT Death Recap panel ──

    def _eval_death_recap(self, js):
        try:
            if self.death_recap_win:
                self.death_recap_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_death_recap_clickable(self):
        """Remove WS_EX_TRANSPARENT so the death-recap panel receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-DeathRecap')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_death_recap(self):
        try:
            if self.death_recap_win and not self._death_recap_visible:
                self._set_window_alpha('SAO-DeathRecap', 0.0)
                self.death_recap_win.show()
                self._eval_death_recap('if(window.DeathRecap&&DeathRecap.fadeIn)DeathRecap.fadeIn()')
                self._eval_death_recap('if(window.DeathRecap&&DeathRecap.refresh)DeathRecap.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-DeathRecap', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._death_recap_visible = True
                self._ensure_death_recap_clickable()
                threading.Timer(0.5, self._ensure_death_recap_clickable).start()
        except Exception:
            pass

    def _hide_death_recap(self):
        try:
            if self.death_recap_win and self._death_recap_visible:
                self._eval_death_recap('if(window.DeathRecap&&DeathRecap.fadeOut)DeathRecap.fadeOut()')
                def _finish():
                    try:
                        if self.death_recap_win:
                            self.death_recap_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._death_recap_visible = False

    # ── ACT Graph/Timeseries panel ──

    def _eval_graph_timeseries(self, js):
        try:
            if self.graph_timeseries_win:
                self.graph_timeseries_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_graph_timeseries_clickable(self):
        """Remove WS_EX_TRANSPARENT so the graph/timeseries panel receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-GraphTimeseries')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_graph_timeseries(self):
        try:
            if self.graph_timeseries_win and not self._graph_timeseries_visible:
                self._set_window_alpha('SAO-GraphTimeseries', 0.0)
                self.graph_timeseries_win.show()
                self._eval_graph_timeseries('if(window.GraphTimeseries&&GraphTimeseries.fadeIn)GraphTimeseries.fadeIn()')
                self._eval_graph_timeseries('if(window.GraphTimeseries&&GraphTimeseries.refresh)GraphTimeseries.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-GraphTimeseries', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._graph_timeseries_visible = True
                self._ensure_graph_timeseries_clickable()
                threading.Timer(0.5, self._ensure_graph_timeseries_clickable).start()
        except Exception:
            pass

    def _hide_graph_timeseries(self):
        try:
            if self.graph_timeseries_win and self._graph_timeseries_visible:
                self._eval_graph_timeseries('if(window.GraphTimeseries&&GraphTimeseries.fadeOut)GraphTimeseries.fadeOut()')
                def _finish():
                    try:
                        if self.graph_timeseries_win:
                            self.graph_timeseries_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._graph_timeseries_visible = False

    # ── ACT Combatant Drilldown panel ──

    def _eval_combatant_drilldown(self, js):
        try:
            if self.combatant_drilldown_win:
                self.combatant_drilldown_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_combatant_drilldown_clickable(self):
        """Remove WS_EX_TRANSPARENT so the combatant drilldown panel receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-CombatantDrilldown')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_combatant_drilldown(self):
        try:
            if self.combatant_drilldown_win and not self._combatant_drilldown_visible:
                self._set_window_alpha('SAO-CombatantDrilldown', 0.0)
                self.combatant_drilldown_win.show()
                self._eval_combatant_drilldown('if(window.CombatantDrilldown&&CombatantDrilldown.fadeIn)CombatantDrilldown.fadeIn()')
                self._eval_combatant_drilldown('if(window.CombatantDrilldown&&CombatantDrilldown.refresh)CombatantDrilldown.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-CombatantDrilldown', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._combatant_drilldown_visible = True
                self._ensure_combatant_drilldown_clickable()
                threading.Timer(0.5, self._ensure_combatant_drilldown_clickable).start()
        except Exception:
            pass

    def _hide_combatant_drilldown(self):
        try:
            if self.combatant_drilldown_win and self._combatant_drilldown_visible:
                self._eval_combatant_drilldown('if(window.CombatantDrilldown&&CombatantDrilldown.fadeOut)CombatantDrilldown.fadeOut()')
                def _finish():
                    try:
                        if self.combatant_drilldown_win:
                            self.combatant_drilldown_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._combatant_drilldown_visible = False

    # ── ACT Skill Drilldown panel ──

    def _eval_skill_drilldown(self, js):
        try:
            if self.skill_drilldown_win:
                self.skill_drilldown_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_skill_drilldown_clickable(self):
        """Remove WS_EX_TRANSPARENT so the skill drilldown panel receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-SkillDrilldown')
            if not hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _show_skill_drilldown(self):
        try:
            if self.skill_drilldown_win and not self._skill_drilldown_visible:
                self._set_window_alpha('SAO-SkillDrilldown', 0.0)
                self.skill_drilldown_win.show()
                self._eval_skill_drilldown('if(window.SkillDrilldown&&SkillDrilldown.fadeIn)SkillDrilldown.fadeIn()')
                self._eval_skill_drilldown('if(window.SkillDrilldown&&SkillDrilldown.refresh)SkillDrilldown.refresh()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-SkillDrilldown', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._skill_drilldown_visible = True
                self._ensure_skill_drilldown_clickable()
                threading.Timer(0.5, self._ensure_skill_drilldown_clickable).start()
        except Exception:
            pass

    def _hide_skill_drilldown(self):
        try:
            if self.skill_drilldown_win and self._skill_drilldown_visible:
                self._eval_skill_drilldown('if(window.SkillDrilldown&&SkillDrilldown.fadeOut)SkillDrilldown.fadeOut()')
                def _finish():
                    try:
                        if self.skill_drilldown_win:
                            self.skill_drilldown_win.hide()
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.25, _finish).start()
        except Exception:
            pass
        self._skill_drilldown_visible = False

    def _ensure_alert_on_top(self):
        try:
            if not self._alert_hwnd:
                self._alert_hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO Alert')
            if not self._alert_hwnd:
                return
            HWND_TOPMOST = ctypes.c_void_p(-1)
            SWP_NOMOVE = 0x0002
            SWP_NOSIZE = 0x0001
            SWP_NOACTIVATE = 0x0010
            ctypes.windll.user32.SetWindowPos(
                self._alert_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        except Exception:
            pass

    def _get_window_monitor_work_area(self, title: str):
        try:
            user32 = ctypes.windll.user32
            hwnd = user32.FindWindowW(None, title)
            if not hwnd:
                raise RuntimeError('window not ready')
            monitor = user32.MonitorFromWindow(hwnd, 2)
            if not monitor:
                raise RuntimeError('monitor not found')

            class _RECT(ctypes.Structure):
                _fields_ = [
                    ('left', ctypes.c_long),
                    ('top', ctypes.c_long),
                    ('right', ctypes.c_long),
                    ('bottom', ctypes.c_long),
                ]

            class _MONITORINFO(ctypes.Structure):
                _fields_ = [
                    ('cbSize', ctypes.c_uint32),
                    ('rcMonitor', _RECT),
                    ('rcWork', _RECT),
                    ('dwFlags', ctypes.c_uint32),
                ]

            info = _MONITORINFO()
            info.cbSize = ctypes.sizeof(_MONITORINFO)
            if not user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
                raise RuntimeError('GetMonitorInfoW failed')
            return (
                int(info.rcWork.left),
                int(info.rcWork.top),
                int(info.rcWork.right),
                int(info.rcWork.bottom),
            )
        except Exception:
            try:
                sw = ctypes.windll.user32.GetSystemMetrics(0)
                sh = ctypes.windll.user32.GetSystemMetrics(1)
            except Exception:
                sw, sh = 1920, 1080
            return (0, 0, int(sw), int(sh))

    def _calc_alert_window_rect(self, width: int = 416, height: int = 226):
        left, top, right, bottom = self._get_window_monitor_work_area('SAO-HP')
        work_w = max(width, right - left)
        work_h = max(height, bottom - top)
        x = left + max(0, int((work_w - width) / 2))
        y = top + max(28, int(work_h * 0.16))
        max_y = bottom - height - 28
        if max_y >= top:
            y = min(y, max_y)
        return int(x), int(y), int(width), int(height)

    def _position_alert_window(self):
        if not self.alert_win:
            return
        x, y, width, height = self._calc_alert_window_rect()
        try:
            self.alert_win.resize(width, height)
        except Exception:
            pass
        try:
            self.alert_win.move(x, y)
        except Exception:
            pass

    def _show_identity_alert_window(self, title: str, message: str,
                                    duration_ms: int = 9000,
                                    play_sound: bool = True,
                                    alert_kind: str = 'generic'):
        if not self.alert_win:
            return
        # v2.1.2-m: 防"alert 一直弹很多次"问题. 调用方很多,
        # 同一条 (title,message) 在 alert 仍可见 + 4s 窗口内重复触发时,
        # 直接续展当前 alert, 不再 hide+show 闪一下。
        try:
            now_ts = time.time()
        except Exception:
            now_ts = 0.0
        sig = (str(alert_kind or 'generic'), str(title or ''), str(message or ''))
        last_sig = getattr(self, '_last_alert_sig', None)
        last_ts = float(getattr(self, '_last_alert_sig_ts', 0.0) or 0.0)
        if (sig == last_sig
                and getattr(self, '_identity_alert_visible', False)
                and (now_ts - last_ts) < 4.0):
            # 同一条 alert 重复触发: 仅刷新 timestamp / 续 stay 计时, 不重弹
            self._last_alert_sig_ts = now_ts
            return
        self._last_alert_sig = sig
        self._last_alert_sig_ts = now_ts

        self._identity_alert_visible = True
        self._identity_alert_kind = str(alert_kind or 'generic')
        self._identity_alert_nonce = int(getattr(self, '_identity_alert_nonce', 0) or 0) + 1
        nonce = self._identity_alert_nonce
        stay_ms = int(duration_ms or 9000)
        if stay_ms <= 0:
            stay_ms = 9000

        self._position_alert_window()
        try:
            self.alert_win.show()
        except Exception:
            pass
        try:
            self._apply_webview2_transparency()
        except Exception:
            pass
        # Remove click-through so alert buttons can be clicked while visible
        self._remove_alert_click_through()
        self._set_window_alpha('SAO Alert', 1.0)
        self._ensure_alert_on_top()
        if play_sound:
            self._play_sound('alert')

        safe_title = self._safe_js(title or '提示')
        safe_message = self._safe_js(message or '')

        def _push():
            if nonce != int(getattr(self, '_identity_alert_nonce', 0) or 0):
                return
            self._eval_alert(
                f'if (window.AlertPanel && AlertPanel.showAlert) '
                f'AlertPanel.showAlert("{safe_title}", "{safe_message}")'
            )
            self._ensure_alert_on_top()

        _push()
        threading.Timer(0.35, _push).start()
        threading.Timer(stay_ms / 1000.0, lambda: self._hide_identity_alert_window(expected_nonce=nonce)).start()

    def _hide_identity_alert_window(self, expected_nonce: int = None, force: bool = False):
        if not self.alert_win:
            return
        current_nonce = int(getattr(self, '_identity_alert_nonce', 0) or 0)
        if expected_nonce is not None and expected_nonce != current_nonce:
            return

        # While a plugin-registered persistent alert is shown, refuse unforced
        # external dismiss requests so the plugin's refresh loop keeps it alive.
        # The plugin's own stop passes force=True to close it. (Stale nonce-
        # bearing auto-hide timers are already rejected above.)
        if (not force and expected_nonce is None
                and self._is_persistent_alert_kind(getattr(self, '_identity_alert_kind', ''))):
            return

        was_visible = bool(getattr(self, '_identity_alert_visible', False))
        self._identity_alert_visible = False
        self._identity_alert_nonce = current_nonce + 1
        closing_nonce = self._identity_alert_nonce

        if not was_visible:
            try:
                self.alert_win.hide()
            except Exception:
                pass
            self._identity_alert_kind = ''
            self._setup_alert_click_through()
            return

        self._play_sound('alert_close')
        self._eval_alert('if (window.AlertPanel && AlertPanel.beginClose) AlertPanel.beginClose()')

        def _finish_hide():
            if closing_nonce != int(getattr(self, '_identity_alert_nonce', 0) or 0):
                return
            try:
                self.alert_win.hide()
            except Exception:
                pass
            self._identity_alert_kind = ''
            # Restore click-through so hidden alert window never captures mouse
            self._setup_alert_click_through()

        threading.Timer(0.52, _finish_hide).start()

    # ── Map-name banner (切换地图中央横幅) ──

    def _position_mapbanner_window(self):
        """把横幅窗口居中到游戏所在显示器的正中央。"""
        if not self.mapbanner_win:
            return
        try:
            rect = getattr(self, '_hud_monitor_rect', None)
            if rect:
                left, top, right, bottom = rect
            else:
                left, top = 0, 0
                right = ctypes.windll.user32.GetSystemMetrics(0)
                bottom = ctypes.windll.user32.GetSystemMetrics(1)
            sw = max(1, right - left)
            sh = max(1, bottom - top)
            width = max(640, int(sw * 0.6))
            height = 280
            x = left + max(0, int((sw - width) / 2))
            y = top + max(0, int((sh - height) / 2))
            try:
                self.mapbanner_win.resize(self._to_webview_px(width), self._to_webview_px(height))
            except Exception:
                pass
            try:
                self.mapbanner_win.move(self._to_webview_px(x), self._to_webview_px(y))
            except Exception:
                pass
        except Exception:
            pass

    def _schedule_map_banner(self, name: str):
        """检测到切换地图 → 延迟 3 秒后在屏幕中央淡入地图名 (WebView)。

        去重改为时间窗 (5s): 同名且距上次调度 < 5s 视为同一次进图的重复抓包,
        吞掉防止狂闪; 超过 5s 再次进入同一场景会重新弹 (满足"第二次切入同场景
        也要刷出名字")。快速连切到别的图时取消上一个未触发的延迟, 只显示最新。
        """
        name = (name or '').strip()
        if not name or not getattr(self, 'mapbanner_win', None):
            return
        now = time.time()
        last = getattr(self, '_mapbanner_last_name', '')
        last_ts = float(getattr(self, '_mapbanner_last_ts', 0.0) or 0.0)
        if name == last and (now - last_ts) < 5.0:
            return
        self._mapbanner_last_name = name
        self._mapbanner_last_ts = now
        print(f'[MapBanner][webview] schedule name={name!r} (+3s)', flush=True)
        prev = getattr(self, '_mapbanner_timer', None)
        if prev is not None:
            try:
                prev.cancel()
            except Exception:
                pass
        t = threading.Timer(3.0, lambda n=name: self._show_map_banner_window(n))
        t.daemon = True
        self._mapbanner_timer = t
        t.start()

    def _show_map_banner_window(self, name: str):
        name = (name or '').strip()
        if not name or not getattr(self, 'mapbanner_win', None):
            return
        self._mapbanner_nonce = int(getattr(self, '_mapbanner_nonce', 0) or 0) + 1
        nonce = self._mapbanner_nonce

        self._position_mapbanner_window()
        try:
            self.mapbanner_win.show()
        except Exception:
            pass
        try:
            self._apply_webview2_transparency()
        except Exception:
            pass
        self._set_window_alpha('SAO MapBanner', 1.0)
        # 纯覆盖层: 立即设鼠标穿透, 避免拦截游戏点击
        self._setup_mapbanner_click_through()
        threading.Timer(0.5, lambda: self._setup_mapbanner_click_through(_wait_retries=0)).start()

        safe_name = self._safe_js(name)

        def _push():
            if nonce != int(getattr(self, '_mapbanner_nonce', 0) or 0):
                return
            self._eval_mapbanner(
                f'if (window.MapBanner && MapBanner.showBanner) '
                f'MapBanner.showBanner("{safe_name}")'
            )
            self._ensure_mapbanner_on_top()

        _push()
        threading.Timer(0.35, _push).start()
        # 显示约 2.6s 后淡出 (与 mapbanner.html 动画时长配合)
        threading.Timer(3.0, lambda: self._hide_map_banner_window(expected_nonce=nonce)).start()

    def _hide_map_banner_window(self, expected_nonce: int = None):
        if not getattr(self, 'mapbanner_win', None):
            return
        if expected_nonce is not None and expected_nonce != int(getattr(self, '_mapbanner_nonce', 0) or 0):
            return
        self._eval_mapbanner('if (window.MapBanner && MapBanner.beginClose) MapBanner.beginClose()')
        closing_nonce = int(getattr(self, '_mapbanner_nonce', 0) or 0)

        def _finish_hide():
            if closing_nonce != int(getattr(self, '_mapbanner_nonce', 0) or 0):
                return
            try:
                self.mapbanner_win.hide()
            except Exception:
                pass

        threading.Timer(0.6, _finish_hide).start()

    def _setup_alert_click_through(self):
        """Make alert window fully click-through (WS_EX_TRANSPARENT).

        Default state: always click-through. Temporarily removed
        when an alert is actively showing so buttons can be clicked.
        """
        try:
            user32 = ctypes.windll.user32
            hwnd = self._alert_hwnd or user32.FindWindowW(None, 'SAO Alert')
            if not hwnd:
                return
            self._alert_hwnd = hwnd
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            ex |= (_WS_EX_TRANSPARENT | _WS_EX_LAYERED)
            user32.SetWindowLongW(hwnd, _GWL_EXSTYLE, ex)
        except Exception:
            pass

    def _remove_alert_click_through(self):
        """Temporarily remove WS_EX_TRANSPARENT so alert buttons are clickable."""
        try:
            user32 = ctypes.windll.user32
            hwnd = self._alert_hwnd or user32.FindWindowW(None, 'SAO Alert')
            if not hwnd:
                return
            self._alert_hwnd = hwnd
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            user32.SetWindowLongW(hwnd, _GWL_EXSTYLE,
                                  (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _setup_mapbanner_click_through(self, _wait_retries: int = 20):
        """Make Map-name banner overlay fully click-through (纯覆盖层, 永不挡点击)."""
        try:
            user32 = ctypes.windll.user32
            hwnd = user32.FindWindowW(None, 'SAO MapBanner')
            if not hwnd and _wait_retries > 0:
                threading.Timer(0.1, lambda: self._setup_mapbanner_click_through(_wait_retries - 1)).start()
                return
            if not hwnd:
                return
            self._mapbanner_hwnd = hwnd
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            ex |= (_WS_EX_TRANSPARENT | _WS_EX_LAYERED)
            user32.SetWindowLongW(hwnd, _GWL_EXSTYLE, ex)
            self._ensure_mapbanner_on_top()
        except Exception:
            pass

    def _ensure_mapbanner_on_top(self):
        try:
            if not self._mapbanner_hwnd:
                self._mapbanner_hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO MapBanner')
            if not self._mapbanner_hwnd:
                return
            HWND_TOPMOST = ctypes.c_void_p(-1)
            SWP_NOMOVE = 0x0002
            SWP_NOSIZE = 0x0001
            SWP_NOACTIVATE = 0x0010
            ctypes.windll.user32.SetWindowPos(
                self._mapbanner_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        except Exception:
            pass

    def _viewport_to_css(self, viewport: dict) -> dict:
        """Convert viewport/callout coords from physical pixels to CSS pixels for JS rendering."""
        dpi_s = max(1.0, float(getattr(self, '_dpi_scale', 1.0) or 1.0))
        if dpi_s <= 1.001:
            return viewport
        vp = dict(viewport)
        for key in ('width', 'height', 'padding_x', 'padding_y'):
            if key in vp and isinstance(vp[key], (int, float)):
                vp[key] = int(round(vp[key] / dpi_s))
        if 'callout' in vp and isinstance(vp['callout'], dict):
            co = dict(vp['callout'])
            for key in ('x', 'y', 'w', 'h'):
                if key in co and isinstance(co[key], (int, float)):
                    co[key] = int(round(co[key] / dpi_s))
            vp['callout'] = co
        return vp

    def _to_webview_px(self, value) -> int:
        dpi_s = max(1.0, float(getattr(self, '_dpi_scale', 1.0) or 1.0))
        try:
            px = float(value)
        except Exception:
            px = 0.0
        if dpi_s <= 1.001:
            return int(round(px))
        return int(round(px / dpi_s))

    def _rect_to_css(self, rect: dict) -> dict:
        """Convert a slot rect from physical pixels to CSS pixels."""
        dpi_s = max(1.0, float(getattr(self, '_dpi_scale', 1.0) or 1.0))
        if dpi_s <= 1.001:
            return rect
        return {k: (int(round(v / dpi_s)) if isinstance(v, (int, float)) else v) for k, v in rect.items()}

    def _toggle_menu(self):
        now = time.time()
        if hasattr(self, '_menu_cd') and now - self._menu_cd < 0.6:
            return
        self._menu_cd = now
        if self._menu_visible:
            self._close_menu()
        else:
            self._open_menu()

    def _open_menu(self):
        self._menu_visible = True
        self._fisheye_prev_frame = None
        self._sync_menu_info()
        self._play_sound('menu_open')

        try:
            _hwnd2 = ctypes.windll.user32.FindWindowW(None, 'SAO Menu')
            if _hwnd2:
                GWL_EXSTYLE = -20; WS_EX_LAYERED = 0x80000
                _ex2 = ctypes.windll.user32.GetWindowLongW(_hwnd2, GWL_EXSTYLE)
                # 确保 WS_EX_LAYERED 已设置
                if not (_ex2 & WS_EX_LAYERED):
                    ctypes.windll.user32.SetWindowLongW(_hwnd2, GWL_EXSTYLE, _ex2 | WS_EX_LAYERED)
                # 确保 WS_EX_TRANSPARENT 已移除 (菜单必须可点击)
                _ex2 = ctypes.windll.user32.GetWindowLongW(_hwnd2, GWL_EXSTYLE)
                if _ex2 & _WS_EX_TRANSPARENT:
                    ctypes.windll.user32.SetWindowLongW(
                        _hwnd2, GWL_EXSTYLE,
                        (_ex2 & ~_WS_EX_TRANSPARENT) | WS_EX_LAYERED)
                ctypes.windll.user32.SetLayeredWindowAttributes(_hwnd2, _COLORREF_KEY, 0,
                                                                _LWA_ALPHA | _LWA_COLORKEY)
                # 设为 TOPMOST, 确保菜单在所有 HUD 窗口之上
                HWND_TOPMOST = ctypes.c_void_p(-1)
                SWP_NOMOVE = 0x0002; SWP_NOSIZE = 0x0001; SWP_NOACTIVATE = 0x0010
                ctypes.windll.user32.SetWindowPos(
                    _hwnd2, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE)
                # 激活菜单窗口让它接收输入焦点
                ctypes.windll.user32.SetForegroundWindow(_hwnd2)
        except Exception:
            pass
        try:
            self.menu_win.show()
        except Exception:
            pass
        try:
            self.menu_win.maximize()
        except Exception:
            pass
        self._reassert_menu_transparency(0.0)
        self._ensure_hp_on_top()

        self._push_fisheye_background()
        self._fisheye_active = True
        self._fisheye_gen += 1
        _gen = self._fisheye_gen
        threading.Thread(target=self._fisheye_loop, args=(_gen,), daemon=True).start()

        def _init_menu():
            self._reassert_menu_transparency(0.0)
            time.sleep(0.12)
            self._eval_menu('SAO.openMenu()')
            try:
                hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO Menu')
                if hwnd:
                    for step in range(1, 9):
                        ctypes.windll.user32.SetLayeredWindowAttributes(
                            hwnd, _COLORREF_KEY, int(255 * step / 8), _LWA_ALPHA | _LWA_COLORKEY)
                        time.sleep(0.015)
            except Exception:
                pass
        threading.Thread(target=_init_menu, daemon=True).start()

    def _close_menu(self):
        self._menu_visible = False
        self._fisheye_active = False
        self._fisheye_prev_frame = None
        self._play_sound('menu_close')
        self._eval_menu('SAO.closeMenu()')

        def _fade_and_hide():
            try:
                hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO Menu')
                GWL_EXSTYLE = -20; WS_EX_LAYERED = 0x80000
                if hwnd:
                    _ex = ctypes.windll.user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
                    if not (_ex & WS_EX_LAYERED):
                        ctypes.windll.user32.SetWindowLongW(hwnd, GWL_EXSTYLE, _ex | WS_EX_LAYERED)
                    # 等 TV-off CSS 动画 (0.55s) 大部分播完再开始原生淡出
                    time.sleep(0.42)
                    steps = 8
                    for i in range(steps):
                        alpha = int(255 * (1 - (i + 1) / steps))
                        ctypes.windll.user32.SetLayeredWindowAttributes(hwnd, _COLORREF_KEY, alpha,
                                                                        _LWA_ALPHA | _LWA_COLORKEY)
                        time.sleep(0.018)
                    ctypes.windll.user32.SetLayeredWindowAttributes(hwnd, _COLORREF_KEY, 0,
                                                                    _LWA_ALPHA | _LWA_COLORKEY)
            except Exception:
                pass
            time.sleep(0.05)
            try:
                self.menu_win.restore()
            except Exception:
                pass
            time.sleep(0.05)
            try:
                self.menu_win.hide()
            except Exception:
                pass
            try:
                hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO Menu')
                if hwnd:
                    ctypes.windll.user32.ShowWindow(hwnd, 0)
                    # 还原为非 TOPMOST, 避免隐藏菜单仍占据 Z-order 顶层
                    HWND_NOTOPMOST = ctypes.c_void_p(-2)
                    SWP_NOMOVE = 0x0002; SWP_NOSIZE = 0x0001; SWP_NOACTIVATE = 0x0010
                    ctypes.windll.user32.SetWindowPos(
                        hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
            except Exception:
                pass
            self._ensure_hp_on_top()
        threading.Thread(target=_fade_and_hide, daemon=True).start()

    def _native_fade_window(self, title: str, duration_ms: int = 260, steps: int = 12):
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, title)
            if not hwnd:
                return
            GWL_EXSTYLE = -20
            WS_EX_LAYERED = 0x80000
            exstyle = ctypes.windll.user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            if not (exstyle & WS_EX_LAYERED):
                ctypes.windll.user32.SetWindowLongW(hwnd, GWL_EXSTYLE, exstyle | WS_EX_LAYERED)
            for i in range(steps):
                alpha = int(255 * (1 - (i + 1) / max(1, steps)))
                ctypes.windll.user32.SetLayeredWindowAttributes(
                    hwnd, _COLORREF_KEY, max(0, alpha), _LWA_ALPHA | _LWA_COLORKEY)
                time.sleep(max(0.01, duration_ms / max(1, steps) / 1000.0))
            # 淡出完成后立即隐藏窗口, 防止 destroy() 重置样式导致底板闪现
            ctypes.windll.user32.ShowWindow(hwnd, 0)  # SW_HIDE
        except Exception:
            pass

    def _transition_with_animation(self, next_ui: Optional[str] = None):
        if self._exit_animating:
            return
        self._exit_animating = True
        if next_ui:
            self._pending_switch = next_ui
        self._recognition_active = False
        self._stop_recognition_engines()

        # 停止后台缓存保存线程, 防止退出后继续写入 stale 数据
        self._cache_loop_stop.set()

        # v2.1.18: 在销毁 webview 之前先把 ui_mode 与最新 game_cache 同步到磁盘.
        # 1) 必须用 _cfg_settings_ref (与 game_cache 同一个 SettingsManager 实例) 写入,
        #    否则 self.settings 和 _cfg_settings_ref 是两个独立实例 → 各自持有不同的内存
        #    快照, 后写入的实例会用 stale 数据覆盖前一次写入 (例如 ui_mode='entity' 会被
        #    _cfg_settings_ref 残留的 ui_mode='webview' 覆盖, 或 game_cache 会被
        #    self.settings 中没有 game_cache 的快照覆盖).
        # 2) save_now=True 让插件缓存立刻持久化, 切换到 entity 后不用再等一次抓包.
        try:
            target_mode = 'entity' if self._pending_switch == 'entity' else 'webview'
            settings_ref = getattr(self, '_cfg_settings_ref', None)
            if settings_ref is not None:
                settings_ref.set('ui_mode', target_mode)
                # 同步到 self.settings 内存, 防止后续逻辑读取到 stale 值
                if hasattr(self, 'settings') and self.settings is not None and \
                        self.settings is not settings_ref:
                    try:
                        self.settings.set('ui_mode', target_mode)
                    except Exception:
                        pass
            elif hasattr(self, 'settings') and self.settings:
                # 兜底: 万一 _cfg_settings_ref 还没构建完成, 至少写到 self.settings
                self.settings.set('ui_mode', target_mode)
                self.settings.save()
        except Exception as e:
            print(f'[SAO-WV] ui_mode pre-save failed: {e}')
        try:
            # _persist_cached_identity_state 内部会写到 _cfg_settings_ref 并 save();
            # 把 ui_mode 也合并到同一次 save 中, 保证 ui_mode 与 game_cache 一起落盘.
            self._persist_cached_identity_state(save_now=True)
        except Exception as e:
            print(f'[SAO-WV] identity pre-save failed: {e}')

        # 退出前通知插件保存自身缓存
        self._dispatch_webview_extension('_save_game_cache', quiet=False)

        # preExit CSS 已由 JS exitApplication() 触发, 此处不再重复调用

        if self._menu_visible:
            try:
                self._close_menu()
            except Exception:
                pass
        else:
            try:
                self._native_fade_window('SAO Menu', duration_ms=220, steps=10)
            except Exception:
                pass

        try:
            self._native_fade_window('SAO-HP', duration_ms=240, steps=12)
        except Exception:
            pass
        try:
            self._native_fade_window('SAO Alert', duration_ms=180, steps=10)
        except Exception:
            pass
        self._dispatch_webview_extension('before_platform_shutdown')
        try:
            self._native_fade_window('SAO-PluginManager', duration_ms=140, steps=8)
        except Exception:
            pass
        try:
            self._native_fade_window('SAO-TriggerTimerManager', duration_ms=140, steps=8)
        except Exception:
            pass
        try:
            self._native_fade_window('SAO-DataSourceHealth', duration_ms=140, steps=8)
        except Exception:
            pass
        try:
            self._native_fade_window('SAO-ReportExport', duration_ms=140, steps=8)
        except Exception:
            pass
        try:
            self._native_fade_window('SAO-OfflineImport', duration_ms=140, steps=8)
        except Exception:
            pass

        try:
            self._destroy_all_panels()
        except Exception:
            pass

        time.sleep(0.46 if self._menu_visible else 0.28)

        try:
            self.hp_win.destroy()
        except Exception:
            pass
        try:
            self.menu_win.destroy()
        except Exception:
            pass
        try:
            if self.alert_win:
                self.alert_win.destroy()
        except Exception:
            pass
        try:
            if self.plugin_manager_win:
                self.plugin_manager_win.destroy()
        except Exception:
            pass
        self._destroy_plugin_surfaces()
        try:
            if self.plugin_manager_win:
                self.plugin_manager_win.destroy()
        except Exception:
            pass
        try:
            if self.trigger_timer_win:
                self.trigger_timer_win.destroy()
        except Exception:
            pass
        try:
            if self.data_source_health_win:
                self.data_source_health_win.destroy()
        except Exception:
            pass
        try:
            if self.report_export_win:
                self.report_export_win.destroy()
        except Exception:
            pass
        try:
            if self.offline_import_win:
                self.offline_import_win.destroy()
        except Exception:
            pass
        try:
            if self.timeline_vcr_win:
                self.timeline_vcr_win.destroy()
        except Exception:
            pass
        try:
            if self.act_aggregate_win:
                self.act_aggregate_win.destroy()
        except Exception:
            pass
        try:
            if self.mem_scope_win:
                self.mem_scope_win.destroy()
        except Exception:
            pass
        try:
            if self.action_log_win:
                self.action_log_win.destroy()
        except Exception:
            pass
        try:
            if self.death_recap_win:
                self.death_recap_win.destroy()
        except Exception:
            pass
        try:
            if self.graph_timeseries_win:
                self.graph_timeseries_win.destroy()
        except Exception:
            pass
        try:
            if self.combatant_drilldown_win:
                self.combatant_drilldown_win.destroy()
        except Exception:
            pass
        try:
            if self.skill_drilldown_win:
                self.skill_drilldown_win.destroy()
        except Exception:
            pass

        # 强制退出进程 — webview/.NET 内部线程无法自行终止
        # UI 切换也走新进程: 动画结束后立即拉起 Entity, 不再等待 webview.start() 返回.
        try:
            settings_ref = getattr(self, '_cfg_settings_ref', None)
            if settings_ref is not None:
                settings_ref.save()
            elif hasattr(self, 'settings') and self.settings is not None:
                self.settings.save()
        except Exception:
            pass

        if self._pending_switch:
            self._do_hot_switch(self._pending_switch)
            return

        def _force_exit():
            time.sleep(0.5)
            os._exit(0)
        t = threading.Thread(target=_force_exit, daemon=True)
        t.start()

    def _exit_with_animation(self):
        self._transition_with_animation()

    # ─── 鱼眼截屏 ───
    def _capture_current_monitor_b64(self, quality=82):
        """快速截屏 → 低分辨率 JPEG base64，用于 WebGL 鱼眼纹理 (目标 <10ms)"""
        try:
            import base64 as b64mod, io
            from PIL import Image

            hwnd = 0
            hx, hy = 0, 0
            try:
                if hx == 0 and hy == 0 and self.hp_win and hasattr(self.hp_win, 'x'):
                    hx = self.hp_win.x or 0
                    hy = self.hp_win.y or 0
            except Exception:
                pass

            img = None
            if ensure_session is not None and get_latest_bgr is not None and hwnd > 0:
                try:
                    if ensure_session(hwnd):
                        frame = get_latest_bgr(hwnd, max_age_s=0.2)
                        if frame is not None and frame.size > 0:
                            img = Image.fromarray(frame[:, :, ::-1])
                except Exception:
                    img = None
            if capture_monitor_bgr_for_point is not None:
                try:
                    if img is None:
                        frame = capture_monitor_bgr_for_point(
                            hx, hy, timeout_ms=16, max_age_s=0.2)
                        if frame is not None and frame.size > 0:
                            img = Image.fromarray(frame[:, :, ::-1])
                except Exception:
                    img = None
            try:
                if img is None:
                    import mss
                    with mss.mss() as sct:
                        if not sct.monitors or len(sct.monitors) < 2:
                            raise RuntimeError('no monitors')
                        target = sct.monitors[1]
                        for m in sct.monitors[1:]:
                            if (m['left'] <= hx < m['left'] + m['width'] and
                                    m['top'] <= hy < m['top'] + m['height']):
                                target = m
                                break
                        raw = sct.grab(target)
                        if raw is None:
                            raise RuntimeError('grab returned None')
                        img = Image.frombytes('RGB', raw.size, raw.rgb)
            except Exception:
                try:
                    from PIL import ImageGrab
                    img = ImageGrab.grab()
                except Exception:
                    return None

            if img is None:
                return None

            # 低分辨率以达到 16ms 帧时间 — WebGL 会放大+鱼眼扭曲
            w, h = img.size
            tw = min(1920, w)
            th = int(tw * h / max(1, w))
            if tw < w:
                img = img.resize((tw, th), Image.LANCZOS)

            buf = io.BytesIO()
            img.save(buf, format='JPEG', quality=quality)
            return b64mod.b64encode(buf.getvalue()).decode('ascii')
        except Exception:
            return None

    def _push_fisheye_background(self):
        b64 = self._capture_current_monitor_b64()
        if b64:
            js = f'SAO.setFisheyeBg("data:image/jpeg;base64,{b64}")'
            self._eval_menu(js)

    def _fisheye_loop(self, gen: int):
        """实时鱼眼背景循环 — 目标 16ms (60fps) 刷新"""
        import time as _time
        while (self._fisheye_active and self._menu_visible
               and gen == self._fisheye_gen
               and not self._pending_switch):
            t0 = _time.time()
            try:
                self._push_fisheye_background()
            except Exception:
                break
            elapsed = _time.time() - t0
            sleep_t = max(0.002, 0.016 - elapsed)
            _time.sleep(sleep_t)

    # ─── 面板管理 ───
    def _toggle_panel(self, panel_type):
        if panel_type in self._panel_wins:
            win = self._panel_wins.pop(panel_type, None)
            self._panel_origins.pop(panel_type, None)
            # 防止重复关闭后立即又创建
            self._panel_closing = getattr(self, '_panel_closing', set())
            self._panel_closing.add(panel_type)
            if win:
                try:
                    self.settings.set(f'wv_{panel_type}_x', win.x)
                    self.settings.set(f'wv_{panel_type}_y', win.y)
                    self.settings.save()
                except Exception:
                    pass
                try:
                    win.evaluate_js('if (window.Panel && Panel.preClose) { Panel.preClose(); } else { document.documentElement.style.opacity="0"; }')
                except Exception:
                    pass
                time.sleep(0.20)
                try:
                    win.destroy()
                except Exception:
                    pass
            self._panel_closing.discard(panel_type)
            return
        # 如果刚关闭完毕的面板被另一个线程重新调用, 不创建
        self._panel_closing = getattr(self, '_panel_closing', set())
        if panel_type in self._panel_closing:
            return
        self._create_panel_window(panel_type)

    def _create_panel_window(self, panel_type):
        url = _web_file_uri('panel.html')

        sizes = {
            'control': (280, 240),
            'piano': (700, 120),
            'status': (220, 240),
            'viz': (240, 400),
        }
        w, h = sizes.get(panel_type, (280, 200))

        try:
            if getattr(self, '_hp_fullscreen', False):
                hx, hy = self._calc_hud_target()
            else:
                hx, hy = self.hp_win.x or 100, self.hp_win.y or 100
        except Exception:
            hx, hy = 100, 100

        key_x = f'wv_{panel_type}_x'
        key_y = f'wv_{panel_type}_y'
        sx = self.settings.get(key_x, max(0, hx - w - 20))
        sy = self.settings.get(key_y, hy)

        api = PanelAPI(self, panel_type)
        win = webview.create_window(
            f'SAO {panel_type}', url,
            width=w, height=h,
            x=int(sx), y=int(sy),
            frameless=True,
            transparent=True,
            on_top=True,
            js_api=api,
        )
        self._panel_wins[panel_type] = win
        self._panel_origins[panel_type] = (int(sx), int(sy))

        def _init():
            time.sleep(1.0)
            title = f'SAO {panel_type}'
            try:
                gui_obj = getattr(win, 'gui', None)
                form = getattr(gui_obj, 'BrowserForm', None) if gui_obj else None
                if form:
                    _setup_dotnet_transparency(form)
            except Exception:
                pass
            try:
                state = self._get_panel_state()
                win.evaluate_js(f'Panel.init("{panel_type}", {json.dumps(state)})')
            except Exception:
                pass
            self._set_window_icon(title)
            self._set_window_alpha(title, 0.95)
            time.sleep(0.1)
            try:
                gui_obj2 = getattr(win, 'gui', None)
                form2 = getattr(gui_obj2, 'BrowserForm', None) if gui_obj2 else None
                if form2:
                    _setup_dotnet_transparency(form2)
            except Exception:
                pass
            self._set_window_alpha(title, 0.95)
        threading.Thread(target=_init, daemon=True).start()

    def _get_panel_state(self):
        return {
            'speed': 1.0,
            'transpose': 0,
            'melody': True,
            'bass': True,
            'directc': False,
            'glissando': False,
            'sustain': False,
            'play_state': '识别中' if self._recognition_active else '待机',
            'mode': 'SAO Auto',
            'bpm': 0,
            'kb_mode': '正常',
        }

    def _sync_all_panels(self):
        state = self._get_panel_state()
        for pt, win in list(self._panel_wins.items()):
            try:
                win.evaluate_js(f'Panel.update({json.dumps(state)})')
            except Exception:
                pass

    def _destroy_all_panels(self):
        for win in list(self._panel_wins.values()):
            try:
                win.evaluate_js('if (window.Panel && Panel.preClose) { Panel.preClose(); } else { document.documentElement.style.opacity="0"; }')
            except Exception:
                pass
        time.sleep(0.20)
        for win in list(self._panel_wins.values()):
            try:
                win.destroy()
            except Exception:
                pass
        self._panel_wins.clear()
        self._panel_origins.clear()

    # ─── 同步信息 ───
    def _sync_menu_info(self):
        info = self._dispatch_webview_extension('build_menu_info', default=None)
        if not isinstance(info, dict):
            info = {'username': '', 'level': '', 'profession': '',
                    'hp': '--', 'sta': '--', 'des': '', 'file': ''}
        self._eval_menu(f'SAO.updateInfo({json.dumps(info, ensure_ascii=False)})')
        # Sync menu settings (watched slots, sound, mode, etc.)
        self._sync_menu_settings()
        self._sync_all_panels()
        self._ensure_updater_listener()

    def _push_update_state(self, snapshot=None):
        """将 UpdateManager 快照推送到 menu (SAO.updateUpdaterState)。"""
        try:
            if snapshot is None:
                from updater.sao_updater import get_manager
                snapshot = get_manager().snapshot()
            data = snapshot.to_json() if hasattr(snapshot, 'to_json') else dict(snapshot)
            self._eval_menu(f'if(window.SAO&&SAO.updateUpdaterState)SAO.updateUpdaterState({json.dumps(data, ensure_ascii=False)})')
        except Exception:
            pass

    def _mark_update_popup_ready(self):
        self._update_popup_ready = True
        pending = getattr(self, '_pending_update_popup_snapshot', None)
        if pending is None:
            return
        self._pending_update_popup_snapshot = None
        self._maybe_show_update_popup(pending)

    def _build_update_popup_payload(self, snapshot=None):
        if snapshot is None:
            try:
                from updater.sao_updater import get_manager
                snapshot = get_manager().snapshot()
            except Exception:
                snapshot = None
        state = str(getattr(snapshot, 'state', 'idle') or 'idle') if snapshot else 'idle'
        latest_version = str(getattr(snapshot, 'latest_version', '') or '') if snapshot else ''
        force_required = bool(getattr(snapshot, 'force_required', False)) if snapshot else False
        skipped_version = str(getattr(snapshot, 'skipped_version', '') or '') if snapshot else ''
        progress = 0
        try:
            progress = int(round(float(getattr(snapshot, 'progress', 0.0) or 0.0) * 100.0)) if snapshot else 0
        except Exception:
            progress = 0
        if state == 'available':
            if latest_version and skipped_version == latest_version and not force_required:
                return None
            body = f'检测到新版本 v{latest_version or "?"}'
            if force_required:
                body += '\n此更新为强制更新，请尽快在 关于 > 检查更新 中完成下载。'
            else:
                body += '\n打开 SAO 菜单 > 关于 > 检查更新 可开始下载。'
            return {
                'key': f'available:{latest_version}:{int(force_required)}',
                'title': 'SYSTEM UPDATE',
                'message': body,
                'duration_ms': 6500,
            }
        if state == 'downloading':
            return {
                'key': f'downloading:{latest_version}',
                'title': 'DOWNLOADING UPDATE',
                'message': f'正在下载更新包 v{latest_version or "?"}\n当前进度 {progress}% ，完成后会提示重启应用。',
                'duration_ms': 5000,
            }
        if state == 'ready':
            return {
                'key': f'ready:{latest_version}',
                'title': 'UPDATE READY',
                'message': f'更新包 v{latest_version or "?"} 已下载完成\n打开 SAO 菜单 > 关于 > 检查更新 可立即重启应用。',
                'duration_ms': 6500,
            }
        if state == 'error':
            # 与 entity/Tk 端一致 (sao_gui_status_updater_mixin._build_update_popup_payload):
            # 自动检查在 DNS/网络预热期可能瞬时失败, 这类错误留在更新面板与手动检查
            # 反馈里即可, 不再弹一个吓人的 identity alert。错误状态仍随 snapshot 推到
            # menu 的更新面板 (sao-updater-meta/badge) 显示, 不丢信息。
            return None
        return None

    def _maybe_show_update_popup(self, snapshot=None):
        # v2.1.2-m: 防 sao_alert 反复弹窗:
        #   1) downloading 状态完全静音 (进度由 SAO 菜单/状态面板显示)
        #   2) 同一个 popup_key 不重复弹 (依旧依赖 _last_update_popup_key)
        #   3) 当前 alert 还在显示 → 跳过 (避免无意义重叠)
        # 注: error 状态现已不产生 popup (与 Tk 端一致, 见 _build_update_popup_payload),
        # 故此处只剩 available / ready 两类一次性事件提示。
        payload = self._build_update_popup_payload(snapshot)
        if not payload:
            return
        popup_key = str(payload.get('key') or '')
        if not popup_key or popup_key == getattr(self, '_last_update_popup_key', ''):
            return
        if popup_key.startswith('downloading:'):
            self._last_update_popup_key = popup_key
            return
        if getattr(self, '_identity_alert_visible', False):
            self._last_update_popup_key = popup_key
            return
        self._last_update_popup_key = popup_key
        self._show_identity_alert_window(
            str(payload.get('title') or 'SYSTEM UPDATE'),
            str(payload.get('message') or ''),
            int(payload.get('duration_ms') or 5000),
        )

    def _handle_update_snapshot(self, snapshot=None):
        self._push_update_state(snapshot)
        if not getattr(self, '_update_popup_ready', False):
            self._pending_update_popup_snapshot = snapshot
            return
        self._maybe_show_update_popup(snapshot)

    def _ensure_updater_listener(self):
        if getattr(self, '_updater_listener_installed', False):
            return
        try:
            from updater.sao_updater import get_manager
            mgr = get_manager()

            def _listener(snapshot):
                self._handle_update_snapshot(snapshot)

            mgr.add_listener(_listener)
            self._updater_listener_installed = True
            self._handle_update_snapshot(mgr.snapshot())
        except Exception:
            pass

    def _sync_menu_settings(self):
        """Push current settings to menu so UI toggles reflect saved state."""
        try:
            cfg = {
                'hotkey_labels': {
                    'toggle_recognition': self._resolved_hotkey('toggle_recognition', 'F5'),
                },
            }
            try:
                cfg['panel_themes'] = self._api.get_panel_themes()
            except Exception:
                cfg['panel_themes'] = {
                    'hp': 'dark',
                    'alert': 'dark',
                    'act': 'dark',
                }
            cfg['plugin_manager_visible'] = bool(self._plugin_manager_visible)
            cfg['trigger_timer_manager_visible'] = bool(self._trigger_timer_manager_visible)
            cfg['data_source_health_visible'] = bool(self._data_source_health_visible)
            cfg['report_export_visible'] = bool(self._report_export_visible)
            cfg['offline_import_visible'] = bool(self._offline_import_visible)
            plugin_cfg = self._dispatch_webview_extension('build_menu_settings', default={}) or {}
            if isinstance(plugin_cfg, dict):
                cfg.update(plugin_cfg)
            self._eval_menu(f'SAO.restoreMenuSettings({json.dumps(cfg)})')
        except Exception:
            pass

    # ════════════════════════════════════════
    #  动作分发
    # ════════════════════════════════════════
    def _context_action(self, action: str):
        _map = {
            'menu': self._toggle_menu,
            'toggle_recognition': self._toggle_recognition,
            'switch_to_entity': lambda: self._transition_with_animation('entity'),
            'exit': self._exit_with_animation,
        }
        fn = _map.get(action)
        if fn:
            threading.Thread(target=fn, daemon=True).start()

    def _menu_action(self, action: str):
        _map = {
            'toggle_recognition': self._toggle_recognition,
            'show_plugins': lambda: (self._show_plugin_manager() if not self._plugin_manager_visible else self._hide_plugin_manager()),
            'toggle_plugin_manager': lambda: (self._show_plugin_manager() if not self._plugin_manager_visible else self._hide_plugin_manager()),
            'toggle_trigger_timer_manager': lambda: (self._show_trigger_timer_manager() if not self._trigger_timer_manager_visible else self._hide_trigger_timer_manager()),
            'toggle_data_source_health': lambda: (self._show_data_source_health() if not self._data_source_health_visible else self._hide_data_source_health()),
            'toggle_report_export': lambda: (self._show_report_export() if not self._report_export_visible else self._hide_report_export()),
            'toggle_offline_import': lambda: (self._show_offline_import() if not self._offline_import_visible else self._hide_offline_import()),
            'toggle_timeline_vcr': lambda: (self._show_timeline_vcr() if not self._timeline_vcr_visible else self._hide_timeline_vcr()),
            'toggle_act_aggregate': lambda: (self._show_act_aggregate() if not self._act_aggregate_visible else self._hide_act_aggregate()),
            'toggle_mem_scope': lambda: (self._show_mem_scope() if not self._mem_scope_visible else self._hide_mem_scope()),
            'toggle_action_log': lambda: (self._show_action_log() if not self._action_log_visible else self._hide_action_log()),
            'toggle_death_recap': lambda: (self._show_death_recap() if not self._death_recap_visible else self._hide_death_recap()),
            'toggle_graph_timeseries': lambda: (self._show_graph_timeseries() if not self._graph_timeseries_visible else self._hide_graph_timeseries()),
            'toggle_combatant_drilldown': lambda: (self._show_combatant_drilldown() if not self._combatant_drilldown_visible else self._hide_combatant_drilldown()),
            'toggle_skill_drilldown': lambda: (self._show_skill_drilldown() if not self._skill_drilldown_visible else self._hide_skill_drilldown()),
            'show_license_panel': self._show_license_panel_from_menu,
            'switch_to_entity': lambda: self._transition_with_animation('entity'),
            'exit': self._exit_with_animation,
        }
        fn = _map.get(action) or (lambda: self._dispatch_webview_extension('menu_action', action))
        if fn:
            threading.Thread(target=fn, daemon=True).start()

    def _show_license_panel_from_menu(self):
        from gui_modules.sao_gui_license import reset_license_dialog_dismissed
        reset_license_dialog_dismissed()
        try:
            import webview
            license_url = _web_file_uri('license_panel.html')
            self._license_done_event = threading.Event()
            self._license_win = webview.create_window(
                'SAO-License', license_url,
                width=460, height=420,
                frameless=True,
                easy_drag=True,
                transparent=True,
                on_top=True,
                js_api=self._api,
            )
        except Exception as e:
            print(f'[license] panel show failed: {e}', flush=True)

    # ════════════════════════════════════════
    #  识别状态循环
    # ════════════════════════════════════════
    def _recognition_loop(self):
        """后台识别循环 — 读取状态并调用插件渲染钩子。"""
        _panel_tick = 0
        while True:
            time.sleep(0.05)
            try:
                if self.hp_win is None:
                    return
                _ = self.hp_win.x
            except Exception:
                return
            # ── Hotkey polling (GetAsyncKeyState fallback) ──
            try:
                self._hk_poll_tick()
            except Exception:
                pass
            gs = None
            if hasattr(self, '_state_mgr'):
                try:
                    gs = self._state_mgr.state
                    self._dispatch_webview_extension('sync_vision_lifecycle', gs)
                except Exception as e:
                    print(f'[SAO-WV] vision lifecycle sync error: {e}')
                    gs = None
            self._dispatch_webview_extension('render_tick', gs, bool(self._recognition_active))
            _panel_tick += 1
            if _panel_tick % 5 == 0:
                self._dispatch_webview_extension('render_slow_tick', gs, bool(self._recognition_active))
            if _panel_tick >= 10 and self._panel_wins:
                _panel_tick = 0
                self._sync_all_panels()

    def _save_position_loop(self):
        while True:
            time.sleep(5)
            try:
                if self.hp_win is None:
                    return
                for pt, win in list(self._panel_wins.items()):
                    try:
                        px, py = win.x, win.y
                        if px is not None and py is not None:
                            self.settings.set(f'wv_{pt}_x', px)
                            self.settings.set(f'wv_{pt}_y', py)
                    except Exception:
                        pass
                self.settings.save()
            except Exception:
                return

    # ════════════════════════════════════════
    #  退出
    # ════════════════════════════════════════
    def _do_hot_switch(self, target: str):
        """热切换到目标 UI 模式 (entity)."""
        if target != 'entity':
            print(f'[SAO WebView] Unknown switch target: {target}')
            return
        try:
            import subprocess
            if getattr(sys, 'frozen', False):
                args = [sys.executable]
                cwd = os.path.dirname(os.path.abspath(sys.executable))
            else:
                app_dir = os.path.dirname(os.path.abspath(__file__))
                args = [sys.executable, os.path.join(app_dir, 'main.py')]
                cwd = app_dir
            creationflags = 0
            if os.name == 'nt':
                creationflags = getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
            subprocess.Popen(
                args,
                cwd=cwd or None,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                close_fds=True,
                creationflags=creationflags,
            )
        except Exception as e:
            print(f"[SAO] Hot switch spawn Entity failed: {e}")
            import traceback; traceback.print_exc()
        finally:
            # WebView/.NET threads can leave window/font state behind; the
            # entity UI is launched in a clean process above, then this host
            # exits hard so stale WebView state cannot poison SAOMenu.
            os._exit(0)

    def _exit(self):
        if self._exit_animating:
            return
        self._exit_animating = True
        self._recognition_active = False
        self._stop_recognition_engines()
        # 停止后台缓存保存线程
        self._cache_loop_stop.set()
        # 退出前通知插件保存自身缓存
        try:
            self._persist_cached_identity_state(save_now=False)
        except Exception:
            pass
        self._dispatch_webview_extension('_save_game_cache', quiet=False)
        self._destroy_all_panels()
        try:
            self.hp_win.destroy()
        except Exception:
            pass
        try:
            self.menu_win.destroy()
        except Exception:
            pass
        try:
            if self.alert_win:
                self.alert_win.destroy()
        except Exception:
            pass
        self._destroy_plugin_surfaces()
