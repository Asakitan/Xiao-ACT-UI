# SAO-UI WebView GUI

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

import settings_crypto

logger = logging.getLogger(__name__)
from typing import Any, Dict, List, Optional

from act_platform.runtime import (
    act_plugin_action,
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
    render_apply_hooks as platform_render_apply_hooks,
    render_overlays as platform_render_overlays,
    render_surfaces as platform_render_surfaces,
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
    # Win32 LWA_COLORKEY 透明 (ctypes 降级方案)
    try:
        u = ctypes.windll.user32
        ex = u.GetWindowLongW(hwnd, _GWL_EXSTYLE)
        u.SetWindowLongW(hwnd, _GWL_EXSTYLE, ex | _WS_EX_LAYERED)
        u.SetLayeredWindowAttributes(hwnd, _COLORREF_KEY, 0, _LWA_COLORKEY)
    except Exception as e:
        print(f"[SAO] ctypes transparency failed: {e}")


def _setup_dotnet_transparency(form):
    # 用 .NET / WinForms 设置色键透明 + WebView2 透明背景.
    #
    # TransparencyKey 让颜色 rgb(1,0,1) 的区域桌面穿透;
    # DefaultBackgroundColor=Transparent 让 WebView2 不遮盖 Form 背景.
    # 注意: 必须在 GUI 线程调用 (或通过 _invoke_dotnet_transparency 封装).
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
    # 从后台线程安全地在 GUI 线程设置 Form 色键透明.
    #
    # 原理:
    # HTML transparent 区域穿透到 Form 背景色.
    # 原本 Form BackColor = 白色 → 白底可见.
    # 设置 BackColor = TransparencyKey = rgb(1,0,1) 后,
    # Form 背景变成 key color, Win32 COLORKEY 再将 key color 穿透到桌面.
    #
    # win_obj.native 是 pywebview BrowserForm 实例
    # (winforms.py BrowserForm.__init__: self.pywebview_window.native = self).
    # 通过 form.Invoke 投递到 GUI 线程执行, 避免跨线程 .NET 访问死锁.
    #
    # 冷启动时 WebView2 初始化可能需要 10-30 秒, native 和 Handle
    # 都不会立即就绪, 因此需要持续重试 (最多 ~30 秒).
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
    # Lightweight settings manager for webview mode.
    #
    # Uses atomic write (write-to-temp + os.replace) to prevent data loss
    # when the process is killed via os._exit() during shutdown.
    def __init__(self):
        # config.BASE_DIR is frozen-aware (packaged: the EXE lives in
        # <root>\runtime\ but user data belongs at <root>\).
        try:
            from config import BASE_DIR as base
        except Exception:
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
                with open(self._path, 'rb') as f:
                    raw = f.read()
                data = settings_crypto.decode_settings(raw)
                if not isinstance(data, dict):
                    raise ValueError('settings root must be an object')
                self._data = data
                if settings_crypto.is_legacy_plaintext(raw):
                    self.save()
        except Exception:
            self._data = {}

    def get(self, key, default=None):
        return self._data.get(key, default)

    def set(self, key, value):
        self._data[key] = value

    def save(self):
        # Atomic write: write to temp file, then os.replace to target.
        #
        # Prevents truncation when os._exit() kills the process mid-write.
        tmp_path = ''
        try:
            import tempfile
            blob = settings_crypto.encode_settings(self._data)
            dir_name = os.path.dirname(self._path) or os.getcwd()
            with tempfile.NamedTemporaryFile(
                mode='wb', dir=dir_name, delete=False,
                suffix='.tmp.json'
            ) as tmp:
                tmp.write(blob)
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
                blob = settings_crypto.encode_settings(self._data)
                with open(self._path, 'wb') as f:
                    f.write(blob)
                    f.flush()
                    os.fsync(f.fileno())
            except Exception:
                pass


# ════════════════════════════════════════════════
#  JS API Bridge
# ════════════════════════════════════════════════
class SAOWebAPI:
    # pywebview js_api — 暴露给 JavaScript 的 Python 接口.

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

    def cmd(self, name: str, payload=None):
        # Generic WebView command entrypoint for dynamically injected plugin UI.
        #
        # Platform-owned commands are handled here. Feature/game commands are
        # forwarded to the registered WebView extension instead of being
        # declared as platform API names.
        command = str(name or '').strip()
        data = self._command_payload(payload)
        try:
            return self._dispatch_command(command, data)
        except Exception as exc:
            return {'ok': False, 'command': command, 'message': str(exc), 'errors': [str(exc)]}

    def _command_payload(self, payload):
        if payload is None:
            return {}
        if isinstance(payload, str):
            try:
                parsed = json.loads(payload or '{}')
            except Exception:
                parsed = {}
            return parsed if isinstance(parsed, dict) else {}
        return payload if isinstance(payload, dict) else {}

    def _dispatch_command(self, command: str, payload: dict):
        platform_commands = {
            'sound.play': lambda p: self.play_sound(p.get('name', '')),
            'sound.set_enabled': lambda p: self.set_sound_enabled(p.get('enabled', False)),
            'sound.set_volume': lambda p: self.set_sound_volume(p.get('volume', 70)),
            'ui.exit': lambda p: self.exit_app(),
            'ui.toggle_menu': lambda p: self.toggle_menu(),
            'ui.context_action': lambda p: self.context_action(p.get('action', '')),
            'ui.menu_action': lambda p: self.menu_action(p.get('action', '')),
            'ui.window_drag': lambda p: self.window_drag(p.get('dx', 0), p.get('dy', 0)),
            'ui.set_ctx_menu_active': lambda p: self.set_ctx_menu_active(p.get('active', False), p.get('bounds')),
            'ui.get_panel_themes': lambda p: self.get_panel_themes(),
            'ui.set_panel_theme': lambda p: self.set_panel_theme(p.get('panel', ''), p.get('theme', '')),
            'file.browse_dir': lambda p: self.browse_dir(str(p.get('path', '') or '')),
            'updater.download': lambda p: self.download_update(),
            'updater.apply': lambda p: self.apply_update(),
            'updater.skip': lambda p: self.skip_update(),
            'plugins.status': lambda p: self.get_plugin_status(),
            'plugins.list': lambda p: self.list_plugins(),
            'plugins.enable': lambda p: self.enable_plugin(p.get('plugin_id', '')),
            'plugins.disable': lambda p: self.disable_plugin(p.get('plugin_id', '')),
            'plugins.reload': lambda p: self.reload_plugins(p.get('plugin_id', '')),
            'plugins.pin': lambda p: self.pin_plugin(p.get('plugin_id', ''), p.get('pinned', True)),
            'plugins.import_dialog': lambda p: self.import_plugin_dialog(),
            'plugins.import': lambda p: self.import_plugin(p.get('archive_path', '')),
            'plugins.uninstall': lambda p: self.uninstall_plugin(p.get('plugin_id', '')),
            'plugins.open_workshop': lambda p: self._open_workshop(),
            'plugins.resize_window': lambda p: self.resize_window(p.get('width', 800), p.get('height', 600)),
            'plugins.hotkeys': lambda p: self.get_plugin_hotkeys(),
            'plugins.set_hotkey': lambda p: self.set_plugin_hotkey(p.get('action', ''), p.get('key', '')),
            'plugins.render_ui_panel': lambda p: self.render_ui_panel(p.get('panel_id', ''), p.get('payload', '')),
            'plugins.invoke_ui_action': lambda p: self.invoke_ui_action(p.get('panel_id', ''), p.get('action_id', ''), p.get('payload', '')),
            'render.surfaces': lambda p: self.render_surfaces(),
            'render.overlays': lambda p: self.render_overlays(p.get('surface', '')),
            'render.apply_hooks': lambda p: self.render_apply_hooks(p.get('surface', ''), p.get('payload', '')),
        }
        fn = platform_commands.get(command)
        if callable(fn):
            return fn(payload)

        extension = getattr(self._g, '_webview_extension', None)
        for handler_name in ('cmd', 'bridge_call', 'webview_command', 'handle_webview_command'):
            handler = getattr(extension, handler_name, None)
            if callable(handler):
                return handler(command, payload)
        return {'ok': False, 'command': command, 'message': 'Unknown WebView command'}

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
        # Open a native file picker for a .zip plugin and one-click import it.
        return json.dumps(act_plugin_import_dialog(self._g), ensure_ascii=False)

    def import_plugin(self, archive_path=''):
        # Import a .zip plugin by path (no dialog).
        return json.dumps(act_plugin_import(self._g, str(archive_path or '')), ensure_ascii=False)

    def uninstall_plugin(self, plugin_id):
        # Uninstall a user-installed plugin (delete its user_plugins dir).
        return json.dumps(act_plugin_uninstall(self._g, str(plugin_id or '')), ensure_ascii=False)

    def _open_workshop(self):
        from act_platform.runtime import act_open_workshop
        return json.dumps(act_open_workshop(self._g), ensure_ascii=False)

    def get_plugin_hotkeys(self):
        return json.dumps(act_plugin_hotkeys(self._g), ensure_ascii=False)

    def set_plugin_hotkey(self, action, key=''):
        return json.dumps(act_plugin_set_hotkey(self._g, str(action or ''), str(key or '')), ensure_ascii=False)

    def render_overlays(self, surface):
        return json.dumps(platform_render_overlays(self._g, str(surface or '')), ensure_ascii=False)

    def render_apply_hooks(self, surface, payload=''):
        return json.dumps(platform_render_apply_hooks(self._g, str(surface or ''), payload), ensure_ascii=False)

    def render_surfaces(self):
        return json.dumps(platform_render_surfaces(self._g), ensure_ascii=False)

    def toggle_plugin_manager(self):
        # Show/hide the plugin manager overlay.
        def _do():
            if self._g._plugin_manager_visible:
                self._g._hide_plugin_manager()
            else:
                self._g._show_plugin_manager()
        threading.Thread(target=_do, daemon=True).start()

    def switch_to_entity(self):
        # 切换到 Entity (tkinter) UI 模式.
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
        # 应用已下载的更新包，会退出当前进程。
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
        # 返回当前面板主题设置 (JS 调用获取初始主题).
        try:
            cfg = getattr(self._g, '_cfg_settings_ref', None) or self._g.settings
            defaults = {}
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
            return {}

    def set_panel_theme(self, panel: str, theme: str):
        # 从 JS 端设置面板主题并保存.
        try:
            panel = str(panel or '').lower()
            theme = str(theme or '').lower()
            allowed = set()
            plugin_allowed = self._g._dispatch_webview_extension('panel_theme_keys', default=()) or ()
            allowed.update(str(item) for item in plugin_allowed)
            cfg = getattr(self._g, '_cfg_settings_ref', None) or self._g.settings
            allowed.update(str(item) for item in (cfg.get('panel_themes', {}) or {}).keys())
            if panel not in allowed:
                return json.dumps({'ok': False, 'panel': panel, 'message': 'Unknown panel'}, ensure_ascii=False)
            if theme not in {'light', 'dark'}:
                theme = 'dark'
            themes = dict(cfg.get('panel_themes', {}))
            themes[panel] = theme
            cfg.set('panel_themes', themes)
            cfg.save()
            self._g._dispatch_webview_extension('on_panel_theme_changed', panel, theme)
            targets = self._g._plugin_surface_theme_targets(panel)
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
        # HP 窗口固定, 不允许拖拽 — 此方法保留但不执行.
        pass

    def resize_window(self, width, height):
        # Resize the plugin manager window (called from plugin_manager.html resize handle).
        try:
            win = getattr(self._g, 'plugin_manager_win', None)
            if win:
                win.resize(max(580, int(width)), max(420, int(height)))
        except Exception:
            pass

    def set_ctx_menu_active(self, active, bounds=None):
        # 控制 HP 窗口 click-through 区域 (右键菜单开关)
        self._g._ctx_menu_active = bool(active)
        self._g._ctx_menu_bounds = bounds if active and isinstance(bounds, dict) else None
        self._g._set_hp_region(expanded=bool(active), menu_bounds=self._g._ctx_menu_bounds)

    def set_hit_regions(self, regions):
        # 接收前端上报的实际可点击 UI 区域
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
        # Global SFX on/off.
        state = _setting_bool_value(enabled)
        try:
            from utils.sao_sound import set_sound_enabled
            set_sound_enabled(state)
            self._g._set_setting('sound_enabled', state)
            return json.dumps({'ok': True, 'enabled': state}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'enabled': state, 'message': str(e)}, ensure_ascii=False)

    def set_sound_volume(self, volume_pct):
        # Global volume 0-100.
        volume = _sound_volume_value(volume_pct)
        try:
            from utils.sao_sound import set_sound_volume
            set_sound_volume(volume)
            self._g._set_setting('sound_volume', volume)
            return json.dumps({'ok': True, 'volume': volume}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'volume': volume, 'message': str(e)}, ensure_ascii=False)

    def browse_dir(self, path: str) -> str:
        # 文件选择器: 返回目录内容 JSON
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
    # pywebview js_api for panel windows (control/piano/status/viz).

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
    # 基于 pywebview 的 SAO-UI 自动化覆盖层.
    #
    # 窗口:
    # hp_win  — 悬浮 HP 栏 (430×500, 色键透明, 上部 68px 可见, 固定位置)
    # menu_win — 全屏 SAO 菜单 (初始隐藏)
    # LinkStart 使用 SAOLinkStart (tkinter / ModernGL) 在 webview 启动前运行.

    def __init__(self):
        _ensure_webview()
        _set_process_app_id('sao.auto.overlay')

        self.settings = SettingsManager()
        self._cfg_settings_ref = self.settings
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
        self._act_plugin_lifecycle_token = ''
        self._vision_paused_for_death = False
        self._last_dead_state = False
        self._recog_lock = threading.Lock()  # 保护 _recognition_active 切换

        # 菜单
        self._menu_visible = False

        # 窗口
        self.hp_win = None
        self.menu_win = None
        self.alert_win = None
        self._mech_alert_controller = None
        # Game/plugin-owned WebView surfaces are injected dynamically by plugins.
        self._plugin_surfaces = {}
        self._plugin_surface_meta = {}
        self._plugin_surface_order = []

        # Plugin manager panel
        self.plugin_manager_win = None
        self._plugin_manager_visible = False

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
        self._cfg_settings_ref = getattr(self, '_cfg_settings_ref', None) or self.settings
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
        # Persist a setting to cfg_settings and save.
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
        # Read a setting.
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            return self._cfg_settings_ref.get(key, default)
        return default

    def _bootstrap_runtime_state(self):
        if getattr(self, '_cfg_settings_ref', None):
            ensure_act_event_bus(self)
            ensure_act_plugin_manager(self, load=False)
            self._ensure_plugin_lifecycle_subscription()
            return
        try:
            from config import SettingsManager as CfgSettings

            if not getattr(self, '_cfg_settings_ref', None):
                self._cfg_settings_ref = CfgSettings()
            ensure_act_event_bus(self)
            ensure_act_plugin_manager(self, load=False)
            self._ensure_plugin_lifecycle_subscription()
        except Exception as e:
            print(f'[SAO] Runtime state bootstrap failed: {e}')

    def _ensure_plugin_lifecycle_subscription(self) -> bool:
        if getattr(self, '_act_plugin_lifecycle_token', ''):
            return True
        try:
            token = ensure_act_event_bus(self).subscribe(
                'plugin_lifecycle',
                self._handle_plugin_lifecycle,
                owner_id='sao_webview_plugin_lifecycle',
            )
            self._act_plugin_lifecycle_token = token
            return True
        except Exception:
            self._act_plugin_lifecycle_token = ''
            return False

    def _release_plugin_lifecycle_subscription(self) -> None:
        token = str(getattr(self, '_act_plugin_lifecycle_token', '') or '')
        if not token:
            return
        try:
            ensure_act_event_bus(self).unsubscribe(token)
        except Exception:
            pass
        self._act_plugin_lifecycle_token = ''

    def _handle_plugin_lifecycle(self, event) -> None:
        payload = event.get('payload') if isinstance(event, dict) else None
        if not isinstance(payload, dict):
            return
        action = str(payload.get('action') or '').lower()
        if action not in {'unloaded', 'disabled', 'load_failed', 'uninstalled', 'forgotten'}:
            return
        self._destroy_plugin_surfaces_for_plugin(str(payload.get('plugin_id') or ''))

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
        # Register a plugin-owned WebView window without teaching the platform its game semantics.
        key = str(surface or '').strip()
        if not key:
            return None
        title = str(title or getattr(win, 'title', '') or key)
        self._plugin_surfaces[key] = win
        merged = dict(self._plugin_surface_meta.get(key, {}) or {})
        merged.update(meta)
        merged['title'] = title
        merged.setdefault('theme_key', key)
        merged.setdefault('plugin_layer', True)
        self._plugin_surface_meta[key] = merged
        if key not in self._plugin_surface_order:
            self._plugin_surface_order.append(key)
        return win

    def _plugin_surface_win(self, surface: str):
        return self._plugin_surfaces.get(str(surface or '').strip())

    def _plugin_surface_title(self, surface: str) -> str:
        meta = self._plugin_surface_meta.get(str(surface or '').strip(), {}) or {}
        return str(meta.get('title') or surface or '')

    def _plugin_surface_hwnd(self, surface: str) -> int:
        key = str(surface or '').strip()
        meta = self._plugin_surface_meta.get(key, {}) or {}
        try:
            hwnd = int(meta.get('hwnd') or 0)
        except Exception:
            hwnd = 0
        if hwnd:
            return hwnd
        title = self._plugin_surface_title(key)
        if not title:
            return 0
        try:
            hwnd = int(ctypes.windll.user32.FindWindowW(None, title) or 0)
        except Exception:
            hwnd = 0
        if hwnd:
            meta['hwnd'] = hwnd
            self._plugin_surface_meta[key] = meta
        return hwnd

    def _plugin_surface_items(self):
        surfaces = getattr(self, '_plugin_surfaces', None)
        order = getattr(self, '_plugin_surface_order', None)
        if not isinstance(surfaces, dict):
            self._plugin_surfaces = {}
            surfaces = self._plugin_surfaces
        if not isinstance(order, list):
            self._plugin_surface_order = []
            order = self._plugin_surface_order
        for surface in list(order):
            win = surfaces.get(surface)
            if win:
                yield win, surface

    def _plugin_surface_theme_targets(self, theme_key: str):
        key = str(theme_key or '').strip().lower()
        if not key:
            return []
        targets = []
        seen = set()
        for win, surface in self._plugin_surface_items():
            meta = self._plugin_surface_meta.get(surface, {}) or {}
            current = str(meta.get('theme_key') or surface or '').strip().lower()
            if current != key:
                continue
            token = id(win)
            if token in seen:
                continue
            seen.add(token)
            targets.append(win)
        return targets

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

    def _set_plugin_surface_icon(self, surface: str):
        title = self._plugin_surface_title(surface)
        if title:
            self._set_window_icon(title)

    def _native_fade_plugin_surface(self, surface: str, duration_ms: int = 260, steps: int = 12):
        title = self._plugin_surface_title(surface)
        if title:
            self._native_fade_window(title, duration_ms=duration_ms, steps=steps)

    def _show_plugin_surface(self, surface: str):
        try:
            win = self._plugin_surface_win(surface)
            if win:
                win.show()
        except Exception:
            pass
        try:
            from render.webview_proxy import get_webview_proxy
            proxy = get_webview_proxy(surface)
            if proxy:
                proxy.show()
        except Exception:
            pass

    def _hide_plugin_surface(self, surface: str):
        try:
            win = self._plugin_surface_win(surface)
            if win:
                win.hide()
        except Exception:
            pass
        try:
            from render.webview_proxy import get_webview_proxy
            proxy = get_webview_proxy(surface)
            if proxy:
                proxy.hide()
        except Exception:
            pass

    def _destroy_plugin_surfaces(self) -> bool:
        try:
            from render.webview_proxy import stop_all_proxies
            if not stop_all_proxies():
                return False
        except Exception:
            return False
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
        return True

    def _destroy_plugin_surfaces_for_plugin(self, plugin_id: str):
        pid = str(plugin_id or '').strip()
        if not pid:
            return
        surfaces = getattr(self, '_plugin_surfaces', None)
        meta = getattr(self, '_plugin_surface_meta', None)
        order = getattr(self, '_plugin_surface_order', None)
        if not isinstance(surfaces, dict) or not isinstance(meta, dict) or not isinstance(order, list):
            return
        stale = [
            surface for surface in list(order)
            if str((meta.get(surface) or {}).get('plugin_id') or '') == pid
        ]
        for surface in stale:
            try:
                from render.webview_proxy import unregister_webview_proxy
                if not unregister_webview_proxy(surface):
                    continue
            except Exception:
                continue
            try:
                win = surfaces.get(surface)
                if win:
                    win.destroy()
            except Exception:
                pass
            surfaces.pop(surface, None)
            meta.pop(surface, None)
            try:
                while surface in order:
                    order.remove(surface)
            except Exception:
                pass

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

    def _proxy_surfaces_to_compositor(self) -> None:
        # In unified overlay mode, proxy all webview windows through
        # the compositor. Each window is moved off-screen and captured
        # via PrintWindow; content is presented as a compositor layer.
        try:
            from render.gpu_overlay_window import get_unified_overlay_mode
            if not get_unified_overlay_mode():
                return
        except Exception:
            return

        from render.webview_proxy import register_webview_proxy

        for surface, _win in list(self._plugin_surface_items()):
            meta = self._plugin_surface_meta.get(surface, {}) or {}
            hwnd = self._plugin_surface_hwnd(surface)
            if not hwnd:
                continue
            ct = bool(meta.get('click_through', False))
            w = int(meta.get('width', 0) or 400)
            h = int(meta.get('height', 0) or 300)
            x = int(meta.get('x', 0) or 0)
            y = int(meta.get('y', 0) or 0)
            z = 150 if ct else 250
            fps = 15.0 if ct else 10.0
            proxy = register_webview_proxy(
                hwnd, surface, w, h, x, y,
                z=z, click_through=ct, capture_fps=fps,
            )
            proxy.start()
            visible_attr = meta.get('visible_attr', '')
            if visible_attr and getattr(self, visible_attr, False):
                proxy.show()

        # Also proxy the menu and plugin manager
        for title_attr, name, z in [
            ('menu_win', 'sao_menu', 160),
            ('plugin_manager_win', 'plugin_manager', 260),
        ]:
            win = getattr(self, title_attr, None)
            if win is None:
                continue
            meta = {}
            try:
                title = str(getattr(win, 'title', name))
                hwnd = ctypes.windll.user32.FindWindowW(None, title)
            except Exception:
                hwnd = 0
            if not hwnd:
                continue
            try:
                rect = wt.RECT()
                ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(rect))
                w = rect.right - rect.left
                h = rect.bottom - rect.top
                x, y = rect.left, rect.top
            except Exception:
                continue
            proxy = register_webview_proxy(
                hwnd, name, w, h, x, y,
                z=z, click_through=False, capture_fps=10.0,
            )
            proxy.start()

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
        try:
            from act_platform.runtime import shutdown_act_plugin_manager
            shutdown_act_plugin_manager(self)
        except Exception:
            pass
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
        self._dispatch_webview_extension('on_recognition_changed', bool(self._recognition_active))
        self._vision_paused_for_death = False
        self._last_dead_state = False
        if not preserve_packet:
            self._recognition_active = False

    def _reconfigure_data_engines(self, restart_packet: bool = True):
        # Restart packet/vision engines to match the current per-component source map.
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
        menu_url = _web_file_uri('menu.html')

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

        self.menu_win = self._register_plugin_surface(
            'menu',
            webview.create_window(
                'SAO Menu', menu_url,
                frameless=True,
                easy_drag=False,
                transparent=True,
                hidden=True,
                js_api=self._api,
            ),
            'SAO Menu',
            dotnet_transparency=False,
            click_through=False,
            on_top=True,
            surface_group='platform',
            theme_key='menu',
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

        # Plugin manager — center, shared API with plugin-owned panels
        plugin_manager_url = _web_file_uri('plugin_manager.html')
        _pm_w = max(620, int(min(_sw, 1920) * 0.38))
        _pm_h = max(520, int(min(_sh, 1080) * 0.52))
        _pm_x = max(16, int(monitor_left + (_sw - _pm_w) / 2))
        _pm_y = max(24, int(monitor_top + (_sh - _pm_h) * 0.20))
        self.plugin_manager_win = self._register_plugin_surface(
            'plugin_manager',
            webview.create_window(
                'SAO-PluginManager', plugin_manager_url,
                width=_pm_w, height=_pm_h,
                x=_pm_x, y=_pm_y,
                frameless=True,
                easy_drag=False,
                transparent=True,
                hidden=True,
                on_top=True,
                js_api=self._api,
            ),
            'SAO-PluginManager',
            click_through=False,
            on_top=True,
            surface_group='plugins',
            theme_key='plugin_manager',
            visible_attr='_plugin_manager_visible',
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
        # 计算 HUD 目标位置 (x, y) — 与 Entity 模式对齐: x=4%屏宽, y=目标显示器底部。
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
        # Win32 LWA_COLORKEY + .NET Form BackColor 色键透明.
        #
        # 两路并用:
        # 1. Win32 COLORKEY — rgb(1,0,1) 像素穿透到桌面
        # 2. .NET via Invoke — form.BackColor = key color, 让 HTML 透明区域
        # 穿透 Form 背景色, 再由 Win32 COLORKEY 穿透到桌面, 彻底消除白底.
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

        self._apply_plugin_surfaces_transparency()

    def _reassert_hp_transparency(self, alpha: float = 1.0, retries: int = 4, delay: float = 0.18):
        # 反复重置 HP 窗口透明状态，修复热切换后偶发白底。
        def _apply_once():
            try:
                self._apply_webview2_transparency()
                # reveal 未完成时不设置 alpha, 避免与 reveal 定时器冲突
                if not getattr(self, '_hp_reveal_pending', False):
                    self._set_plugin_surface_alpha('hp', alpha)
                self._setup_click_through()
                self._ensure_hp_on_top()
                self._request_hp_hit_regions()
            except Exception:
                pass

        _apply_once()
        for i in range(1, max(1, retries)):
            threading.Timer(delay * i, _apply_once).start()

    def _request_hp_hit_regions(self):
        # 主动向前端请求重新上报可点击区域.
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
                        _found = self._plugin_surface_hwnd('hp')
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
                    self._dispatch_webview_extension('maintain_transient_surfaces')

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
        # Fallback display regions contributed by the active plugin.
        try:
            win_w = int(getattr(self, '_win_w_phys', 0) or 0)
            win_h = int(getattr(self, '_win_h_phys', 0) or 0)
            if win_w <= 0 or win_h <= 0:
                return []
            viewport_h = max(1, win_h - int(getattr(self, '_hp_viewport_offset_y', 0) or 0))
            regions = self._dispatch_webview_extension(
                'build_default_display_regions', win_w, win_h, viewport_h, default=None)
            return regions if isinstance(regions, list) else []
        except Exception:
            return []

    def _default_hp_hot_regions(self):
        # Fallback clickable regions when JS hit-regions have not registered yet.
        #
        # 返回所有 display regions 作为默认热区, 确保整个 HP 面板内容区域
        # 在 JS 报告真实点击区域之前就可以接收点击事件,
        # 从而触发 JS 注册精确的 hit regions.
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
            hwnd = self._plugin_surface_hwnd('hp')
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
        # 安全检查: 确保 HP 窗口未被意外设为鼠标穿透.
        #
        # 当 passthrough poller 已在运行时, 穿透状态完全由 poller 管理, 此方法不干预.
        # 仅在 poller 未启用时才移除 WS_EX_TRANSPARENT.
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
        # 确保所有当前隐藏的面板窗口保持 WS_EX_TRANSPARENT, 防止意外拦截点击.
        #
        # 在 position guard 中周期性调用.
        for win, surface in list(self._plugin_surface_items()):
            try:
                meta = self._plugin_surface_meta.get(surface, {}) or {}
                vis_attr = str(meta.get('visible_attr') or '').strip()
                if not vis_attr or getattr(self, vis_attr, False):
                    continue
                title = self._plugin_surface_title(surface)
                hwnd = self._plugin_surface_hwnd(surface)
                if not hwnd:
                    continue
                user32 = ctypes.windll.user32
                ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
                if not (ex & _WS_EX_TRANSPARENT):
                    user32.SetWindowLongW(
                        hwnd, _GWL_EXSTYLE,
                        ex | _WS_EX_TRANSPARENT | _WS_EX_LAYERED)
            except Exception:
                pass

    def _wait_and_apply_click_through(self, title: str, timeout: float = 2.0):
        # Block (up to *timeout* seconds) until the window hwnd is findable.
        #
        # This ensures that FindWindowW-based click-through setup can succeed
        # before the window is shown, preventing a brief non-passthrough window.
        # Intended to be called from background init thread only.
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
        # 用 GetWindowRect + SetWindowPos 强制 HP 窗口贴屏幕底部 (物理像素)。
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
        # Win32 LWA_ALPHA — 设置窗口整体透明度 (0.0~1.0), 保留色键透明.
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
        # Map every SAO WebView window to its plugin render-surface id.
        #
        # Injecting ``plugin_layer.js`` into each window gives plugins a universal
        # overlay layer (and, for opt-in pages, render-hook taps) on the main UI
        # and every floating window — not just plugin-owned panels.
        items = []
        seen = set()
        for win, surface in self._plugin_surface_items():
            meta = self._plugin_surface_meta.get(surface, {}) or {}
            if not bool(meta.get('plugin_layer', True)):
                continue
            token = id(win)
            if token in seen:
                continue
            seen.add(token)
            items.append((win, surface))
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
        # Inject plugin_layer.js into every live WebView window (idempotent).
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
            self._set_plugin_surface_alpha('hp', 0.0)
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
                self._set_plugin_surface_alpha('hp', 1.0)
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
            self._set_plugin_surface_icon('hp')
            self._set_window_icon('SAO Menu')
            self._set_window_icon('SAO-PluginManager')
            for _, surface in list(self._plugin_surface_items()):
                meta = self._plugin_surface_meta.get(surface, {}) or {}
                if meta.get('surface_group') == 'plugin':
                    self._set_plugin_surface_icon(surface)
            # 菜单窗口在启动阶段保持完全透明, 避免偶发白色方框闪现
            self._set_window_alpha('SAO Menu', 0.0)
            try:
                self._wait_and_apply_click_through('SAO-PluginManager', timeout=0.5)
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
                for win, surface in list(self._plugin_surface_items()):
                    meta = self._plugin_surface_meta.get(surface, {}) or {}
                    if meta.get('surface_group') != 'plugin' or not meta.get('visible_attr'):
                        continue
                    setattr(self, str(meta.get('visible_attr')), False)
                    self._set_plugin_surface_alpha(surface, 0.0)
                    if win:
                        win.hide()
                self._ensure_hidden_panels_passthrough()
            except Exception:
                pass
            # 重新触发 HP 入场动态模糊 (避免页面预加载时动画已经跑完)
            self._eval_hp('if (window.HP && HP.retriggerEntryBlur) HP.retriggerEntryBlur()')
            # HP 窗口入场动画: 从中央滑到固定位置
            self._animate_hp_entry()
            # Unified overlay: proxy webview windows through compositor
            try:
                self._proxy_surfaces_to_compositor()
            except Exception:
                pass
            # 启动识别引擎
            self._start_recognition()
        threading.Timer(0.5, _init).start()

        threading.Thread(target=self._recognition_loop, daemon=True).start()
        threading.Thread(target=self._save_position_loop, daemon=True).start()
        self._setup_hotkeys()

    def _start_recognition(self):
        # 启动游戏数据引擎 (抓包 + 纯识图)
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

    def _toggle_recognition(self):
        # 切换识别开关 — 线程安全
        with self._recog_lock:
            self._recognition_active = not self._recognition_active
            state = "ON" if self._recognition_active else "OFF"
            if not self._recognition_active:
                self._dispatch_webview_extension('on_recognition_changed', bool(self._recognition_active))
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
        # 插件快捷键 {action: {'key': 'CTRL+F8', 'callback': fn}} — 镜像
        # Entity 端 _plugin_hotkey_map。用户在 settings['hotkeys'] 的覆盖优先
        # 于插件声明的 default_key; 每次按键现解析, 热加载的插件即时生效。
        # webview 无 Tk 主循环, 回调在监听/轮询线程直接派发。
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
        # 全部活动绑定 [(parsed, (callback, vk))] — 内置在前 (并列特异度时
        # 内置优先), 插件映射在后; 每次按键/轮询现解析。
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
        # Poll GetAsyncKeyState for F-key presses (called from recognition loop).
        #
        # This is a fallback for when pynput's keyboard hook fails to receive events
        # (common with DirectInput games). Detects rising edges only.
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

    def _on_mechanic_event(self, evt):
        # 引擎机制事件 → TTS + 顶部横幅 (controller 缺位时静默丢弃)。
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
        # 热键标签按 {**DEFAULT_HOTKEYS, **saved} 解析 — 标签跟随用户改键不漂移。
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
        # Remove WS_EX_TRANSPARENT so the plugin manager receives clicks.
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

    def _viewport_to_css(self, viewport: dict) -> dict:
        # Convert viewport/callout coords from physical pixels to CSS pixels for JS rendering.
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
        # Convert a slot rect from physical pixels to CSS pixels.
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
            self._native_fade_plugin_surface('hp', duration_ms=240, steps=12)
        except Exception:
            pass
        self._dispatch_webview_extension('before_platform_shutdown')
        try:
            self._native_fade_window('SAO-PluginManager', duration_ms=140, steps=8)
        except Exception:
            pass
        for _, surface in list(self._plugin_surface_items()):
            try:
                meta = self._plugin_surface_meta.get(surface, {}) or {}
                if meta.get('surface_group') == 'plugin':
                    self._native_fade_plugin_surface(surface, duration_ms=140, steps=8)
            except Exception:
                pass

        try:
            self._destroy_all_panels()
        except Exception:
            pass

        time.sleep(0.46 if self._menu_visible else 0.28)

        # Capture producers must be proven stopped before their source HWNDs
        # or compositor layers are destroyed.
        if not self._destroy_plugin_surfaces():
            self._exit_animating = False
            return

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
        self._release_plugin_lifecycle_subscription()

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
        # 快速截屏 → 低分辨率 JPEG base64，用于 WebGL 鱼眼纹理 (目标 <10ms)
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
        # 实时鱼眼背景循环 — 目标 16ms (60fps) 刷新
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
            info = {}
        self._eval_menu(f'SAO.updateInfo({json.dumps(info, ensure_ascii=False)})')
        # Sync menu settings (watched slots, sound, mode, etc.)
        self._sync_menu_settings()
        self._sync_all_panels()
        self._ensure_updater_listener()

    def _push_update_state(self, snapshot=None):
        # 将 UpdateManager 快照推送到 menu (SAO.updateUpdaterState)。
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
        self._dispatch_webview_extension(
            'show_platform_notice',
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
        # Push current settings to menu so UI toggles reflect saved state.
        try:
            cfg = {
                'hotkey_labels': {
                    'toggle_recognition': self._resolved_hotkey('toggle_recognition', 'F5'),
                },
            }
            try:
                cfg['panel_themes'] = self._api.get_panel_themes()
            except Exception:
                cfg['panel_themes'] = {}
            cfg['plugin_manager_visible'] = bool(self._plugin_manager_visible)
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
        action = str(action or "")
        if action in {'show_plugins', 'toggle_plugin_manager', 'show_license_panel', 'switch_to_entity'}:
            try:
                if self._menu_visible:
                    self._close_menu()
            except Exception:
                pass
        _map = {
            'toggle_recognition': self._toggle_recognition,
            'show_plugins': lambda: (self._show_plugin_manager() if not self._plugin_manager_visible else self._hide_plugin_manager()),
            'toggle_plugin_manager': lambda: (self._show_plugin_manager() if not self._plugin_manager_visible else self._hide_plugin_manager()),
            'show_license_panel': self._show_license_panel_from_menu,
            'switch_to_entity': lambda: self._transition_with_animation('entity'),
            'exit': self._exit_with_animation,
        }
        def _plugin_action():
            result = act_plugin_action(self, action)
            if not isinstance(result, dict) or not result.get('ok'):
                return self._dispatch_webview_extension('menu_action', action)
            return result

        fn = _map.get(action) or _plugin_action
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
        # 后台识别循环 — 读取状态并调用插件渲染钩子。
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
        # 热切换到目标 UI 模式 (entity).
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
        if not self._destroy_plugin_surfaces():
            return
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
        self._release_plugin_lifecycle_subscription()
