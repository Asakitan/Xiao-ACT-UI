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
import numpy as np
import logging
from pathlib import Path

logger = logging.getLogger(__name__)
from typing import Any, Dict, List, Optional

from auto_key_engine import (
    AutoKeyCloudClient,
    AutoKeyEngine,
    DEFAULT_AUTO_KEY_SERVER_URL,
    build_auto_key_state,
    build_identity_state,
    clone_profile,
    default_upload_auth_state,
    delete_profile as delete_auto_key_profile,
    export_profile_to_default_path,
    find_profile as find_auto_key_profile,
    import_profile_from_path,
    load_auto_key_config,
    make_default_profile,
    normalize_profile,
    save_auto_key_config,
    snapshot_author_from_state,
    upsert_profile,
)
from boss_raid_engine import (
    BossRaidCloudClient,
    BossRaidEngine,
    DEFAULT_BOSS_RAID_SERVER_URL,
    build_boss_raid_state,
    clone_profile as clone_br_profile,
    delete_profile as delete_br_profile,
    export_profile_to_default_path as export_br_profile_path,
    find_profile as find_br_profile,
    import_profile_from_path as import_br_profile_path,
    load_boss_raid_config,
    make_default_profile as make_default_br_profile,
    normalize_profile as normalize_br_profile,
    save_boss_raid_config,
    upsert_profile as upsert_br_profile,
)
from boss_autokey_linkage import (
    BossAutoKeyLinkage,
    build_linkage_state,
    default_linkage_config,
    load_linkage_config,
    make_default_mapping,
    normalize_linkage_config,
    save_linkage_config,
)
from dps_tracker import DpsTracker
from config import (
    DEFAULT_HOTKEYS,
    get_skill_slot_rects,
    WEB_DIR,
    resource_path,
)
try:
    from gpu_capture import capture_monitor_bgr_for_point, ensure_session, get_latest_bgr
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


def _web_file_uri(filename: str) -> str:
    return Path(os.path.join(WEB_DIR, filename)).resolve().as_uri()


def _set_process_app_id(app_id: str):
    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(app_id)
    except Exception:
        pass


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
class SAOWebAPI:
    """pywebview js_api — 暴露给 JavaScript 的 Python 接口."""

    def __init__(self, gui: 'SAOWebViewGUI'):
        self._g = gui

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

    def exit_app(self):
        threading.Thread(target=self._g._exit_with_animation, daemon=True).start()

    def switch_to_entity(self):
        """切换到 Entity (tkinter) UI 模式."""
        threading.Thread(target=lambda: self._g._transition_with_animation('entity'), daemon=True).start()

    # ---- 远程更新 ----
    def get_update_status(self):
        try:
            from sao_updater import get_manager
            return get_manager().snapshot().to_json()
        except Exception as e:
            return {"state": "error", "error": str(e)}

    def check_update(self):
        try:
            from sao_updater import get_manager
            get_manager().check_async()
            return True
        except Exception as e:
            print(f"[SAO WebView] check_update failed: {e}")
            return False

    def download_update(self):
        try:
            from sao_updater import get_manager
            get_manager().download_async()
            return True
        except Exception as e:
            print(f"[SAO WebView] download_update failed: {e}")
            return False

    def apply_update(self):
        """应用已下载的更新包，会退出当前进程。"""
        try:
            from sao_updater import has_pending_update, schedule_apply_on_exit
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
            from sao_updater import get_manager
            get_manager().skip_current()
            return True
        except Exception:
            return False

    # ---- 面板主题 ----
    def get_panel_themes(self):
        """返回当前面板主题设置 (JS 调用获取初始主题)."""
        try:
            cfg = getattr(self._g, '_cfg_settings_ref', None) or self._g.settings
            return cfg.get('panel_themes', {})
        except Exception:
            return {}

    def set_panel_theme(self, panel: str, theme: str):
        """从 JS 端设置面板主题并保存."""
        try:
            cfg = getattr(self._g, '_cfg_settings_ref', None) or self._g.settings
            themes = dict(cfg.get('panel_themes', {}))
            themes[panel] = theme
            cfg.set('panel_themes', themes)
            cfg.save()
            # 即时推送到对应 webview 面板窗口
            _win_map = {
                'dps': getattr(self._g, 'dps_win', None),
                'hp': getattr(self._g, 'hp_win', None),
                'bosshp': getattr(self._g, 'boss_hp_win', None),
                'skillfx': getattr(self._g, 'skillfx_win', None),
                'alert': getattr(self._g, 'alert_win', None),
            }
            w = _win_map.get(panel)
            if w:
                try:
                    w.evaluate_js(f'window._applyPanelTheme&&window._applyPanelTheme("{theme}")')
                except Exception:
                    pass
            return True
        except Exception:
            return False

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

    def get_state(self):
        """供 JS 查询当前识别状态 (JSON 格式)"""
        gs = self._g._game_state
        try:
            sta_pct = int(round(max(0.0, min(1.0, float(getattr(gs, 'stamina_pct', 0.0) or 0.0))) * 100.0))
        except Exception:
            sta_pct = 0
        return json.dumps({
            'recognition_active': self._g._recognition_active,
            'hp': gs.hp_current if gs and hasattr(gs, 'hp_current') else 0,
            'hp_max': gs.hp_max if gs and hasattr(gs, 'hp_max') else 0,
            'stamina': sta_pct,
            'stamina_max': 100,
            'level': gs.level_base if gs and hasattr(gs, 'level_base') else 0,
        })

    # ── Skill Effects settings ──
    def set_watched_slots(self, slots):
        """Set which skill slots to watch for Burst Mode Ready."""
        if isinstance(slots, str):
            try:
                slots = json.loads(slots)
            except Exception:
                slots = []
        if not isinstance(slots, list):
            slots = []
        slots = [int(x) for x in slots if isinstance(x, (int, float))]
        self._g._set_setting('watched_skill_slots', slots)
        self._g._reset_burst_tracking()

    def set_burst_enabled(self, enabled):
        """Enable/disable Burst Mode Ready alerts."""
        self._g._set_setting('burst_enabled', bool(enabled))

    def set_auto_key_enabled(self, enabled):
        try:
            config = self._g._load_auto_key_config()
            config['enabled'] = bool(enabled)
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def get_auto_key_state(self):
        try:
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def create_auto_key_profile(self):
        try:
            config = self._g._load_auto_key_config()
            profile = make_default_profile(self._g._auto_key_author_snapshot())
            upsert_profile(config, profile, activate=True)
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def copy_auto_key_profile(self, profile_id):
        try:
            config = self._g._load_auto_key_config()
            created = clone_profile(config, profile_id, self._g._auto_key_author_snapshot())
            if not created:
                raise RuntimeError('Profile not found')
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def save_auto_key_profile(self, profile_payload):
        try:
            if isinstance(profile_payload, str):
                profile_payload = json.loads(profile_payload)
            config = self._g._load_auto_key_config()
            profile = normalize_profile(profile_payload, author_snapshot=self._g._auto_key_author_snapshot())
            existing = find_auto_key_profile(config, profile.get('id'))
            if existing and existing.get('created_at'):
                profile['created_at'] = existing.get('created_at')
            upsert_profile(config, profile, activate=str(config.get('active_profile_id') or '') == str(profile.get('id') or ''))
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def delete_auto_key_profile(self, profile_id):
        try:
            config = self._g._load_auto_key_config()
            delete_auto_key_profile(config, profile_id)
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def activate_auto_key_profile(self, profile_id):
        try:
            config = self._g._load_auto_key_config()
            if not find_auto_key_profile(config, profile_id):
                raise RuntimeError('Profile not found')
            config['active_profile_id'] = str(profile_id or '')
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def export_auto_key_profile(self, profile_id=None):
        try:
            config = self._g._load_auto_key_config()
            profile = find_auto_key_profile(config, profile_id) if profile_id else None
            if profile is None:
                profile = find_auto_key_profile(config, config.get('active_profile_id'))
            if profile is None:
                raise RuntimeError('No active profile')
            path = export_profile_to_default_path(profile)
            return json.dumps({'ok': True, 'path': path}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def start_auto_key_import_picker(self, path=None):
        try:
            root = str(path or (os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else os.path.dirname(os.path.abspath(__file__))))
            self._g._auto_key_picker_purpose = 'auto_key_import'
            data = json.loads(self.browse_dir(root))
            data['mode'] = 'file'
            return json.dumps({'ok': True, 'browser': data}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def select_file(self, path):
        try:
            purpose = getattr(self._g, '_auto_key_picker_purpose', '')
            if purpose == 'auto_key_import':
                config = self._g._load_auto_key_config()
                profile = import_profile_from_path(str(path), self._g._auto_key_author_snapshot())
                upsert_profile(config, profile, activate=False)
                self._g._save_auto_key_config(config)
                self._g._auto_key_picker_purpose = ''
                self._g._sync_auto_key_menu()
                return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
            purpose_br = getattr(self._g, '_boss_raid_picker_purpose', '')
            if purpose_br == 'boss_raid_import':
                config = self._g._load_boss_raid_config()
                profile = import_br_profile_path(str(path), self._g._boss_raid_author_snapshot())
                upsert_br_profile(config, profile, activate=False)
                self._g._save_boss_raid_config(config)
                self._g._boss_raid_picker_purpose = ''
                self._g._sync_boss_raid_menu()
                return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
            raise RuntimeError('No file picker action pending')
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def select_folder(self, path):
        return json.dumps({'ok': False, 'message': 'Folder selection not used here'}, ensure_ascii=False)

    def set_auto_key_upload_token(self, token):
        try:
            identity_state = self._g._auto_key_identity_state()
            config = self._g._load_auto_key_config()
            self._g._set_auto_key_upload_auth(
                token=str(token or '').strip(),
                expires_at='',
                error='',
                mode='manual',
                identity=identity_state,
                server_url=str(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL),
            )
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def set_auto_key_server_url(self, url):
        try:
            config = self._g._load_auto_key_config()
            config['server_url'] = str(url or '').strip()
            self._g._save_auto_key_config(config)
            self._g._set_auto_key_upload_auth(
                token='',
                expires_at='',
                error='',
                mode='',
                identity=self._g._auto_key_identity_state(),
                server_url=str(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL),
            )
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def refresh_auto_key_upload_auth(self, force=False):
        try:
            auth = self._g._refresh_auto_key_upload_auth(force=bool(force))
            self._g._sync_auto_key_menu()
            message = str((auth or {}).get('error') or '').strip()
            if not message and not bool((auth or {}).get('ready')):
                identity = (auth or {}).get('identity') or {}
                missing = ', '.join(identity.get('missing') or [])
                message = f'Upload auth is not ready{": " + missing if missing else ""}'
            return json.dumps({
                'ok': bool(auth.get('ready')),
                'message': message,
                'upload_auth': auth,
                'state': self._g._get_auto_key_menu_state(),
            }, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e), 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)

    def search_remote_profiles(self, query_payload):
        try:
            if isinstance(query_payload, str):
                query_payload = json.loads(query_payload or '{}')
            config = self._g._load_auto_key_config()
            client = AutoKeyCloudClient(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL)
            query = {
                'q': str((query_payload or {}).get('q', '') or '').strip(),
                'profile_name': str((query_payload or {}).get('profile_name', '') or '').strip(),
                'player_uid': str((query_payload or {}).get('player_uid', '') or '').strip(),
                'player_name': str((query_payload or {}).get('player_name', '') or '').strip(),
                'profession_name': str((query_payload or {}).get('profession_name', '') or '').strip(),
                'page': int((query_payload or {}).get('page', 1) or 1),
                'page_size': int((query_payload or {}).get('page_size', 20) or 20),
            }
            result = client.search_scripts(query)
            config['last_remote_search'] = {
                'query': query,
                'results': result.get('items', []),
                'error': '',
                'fetched_at': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime()),
            }
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'results': result.get('items', []), 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            config = self._g._load_auto_key_config()
            last = config.get('last_remote_search', {}) or {}
            last['error'] = str(e)
            config['last_remote_search'] = last
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': False, 'message': str(e), 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)

    def download_remote_profile(self, remote_id):
        try:
            config = self._g._load_auto_key_config()
            client = AutoKeyCloudClient(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL)
            result = client.get_script(remote_id)
            profile_raw = result.get('profile') or {}
            profile = normalize_profile(profile_raw, author_snapshot=self._g._auto_key_author_snapshot(), source='downloaded')
            profile['id'] = profile.get('id') or ''
            profile['id'] = profile['id'] if profile['id'] not in {item.get('id') for item in config.get('profiles', []) or []} else ''
            if not profile['id']:
                profile['id'] = f'profile_{int(time.time())}'
            profile['remote_id'] = result.get('id')
            profile['source'] = 'downloaded'
            profile['updated_at'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            upsert_profile(config, profile, activate=False)
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def upload_auto_key_profile(self, profile_id=None):
        try:
            config = self._g._load_auto_key_config()
            profile = find_auto_key_profile(config, profile_id) if profile_id else None
            if profile is None:
                profile = find_auto_key_profile(config, config.get('active_profile_id'))
            if profile is None:
                raise RuntimeError('No active profile')
            auth = self._g._refresh_auto_key_upload_auth(force=False)
            token = str((auth or {}).get('token') or '').strip()
            if not token:
                raise RuntimeError(str((auth or {}).get('error') or 'Upload token is empty'))
            client = AutoKeyCloudClient(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL)
            author = self._g._auto_key_author_snapshot()
            payload = {
                'profile_name': profile.get('profile_name', ''),
                'description': profile.get('description', ''),
                'profession_id': author.get('profession_id') or profile.get('profession_id', 0),
                'profession_name': author.get('profession_name') or profile.get('profession_name', ''),
                'player_uid': author.get('player_uid', ''),
                'player_name': author.get('player_name', ''),
                'schema_version': 1,
                'profile': profile,
            }
            result = client.upload_script(payload, token)
            profile['source'] = 'uploaded'
            profile['remote_id'] = result.get('id')
            upsert_profile(config, profile, activate=str(config.get('active_profile_id') or '') == str(profile.get('id') or ''))
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'remote_id': result.get('id'), 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e), 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)

    # ── Boss Raid API ──

    def get_boss_raid_state(self):
        try:
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def set_boss_raid_enabled(self, enabled):
        try:
            config = self._g._load_boss_raid_config()
            config['enabled'] = bool(enabled)
            self._g._save_boss_raid_config(config)
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def set_boss_raid_server_url(self, url):
        try:
            config = self._g._load_boss_raid_config()
            config['server_url'] = str(url or '').strip()
            self._g._set_boss_raid_upload_auth(
                token='',
                expires_at='',
                error='',
                mode='',
                identity=self._g._boss_raid_identity_state(),
                server_url=str(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL),
            )
            self._g._save_boss_raid_config(config)
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def activate_boss_raid_profile(self, profile_id):
        try:
            config = self._g._load_boss_raid_config()
            if not find_br_profile(config, profile_id):
                raise RuntimeError('Profile not found')
            config['active_profile_id'] = str(profile_id or '')
            self._g._save_boss_raid_config(config)
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def create_boss_raid_profile(self):
        try:
            config = self._g._load_boss_raid_config()
            profile = make_default_br_profile(self._g._boss_raid_author_snapshot())
            upsert_br_profile(config, profile, activate=True)
            self._g._save_boss_raid_config(config)
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def save_boss_raid_profile(self, profile_payload):
        try:
            if isinstance(profile_payload, str):
                profile_payload = json.loads(profile_payload)
            config = self._g._load_boss_raid_config()
            profile = normalize_br_profile(profile_payload, author_snapshot=self._g._boss_raid_author_snapshot())
            existing = find_br_profile(config, profile.get('id'))
            if existing and existing.get('created_at'):
                profile['created_at'] = existing.get('created_at')
            upsert_br_profile(config, profile,
                              activate=str(config.get('active_profile_id') or '') == str(profile.get('id') or ''))
            self._g._save_boss_raid_config(config)
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def delete_boss_raid_profile(self, profile_id):
        try:
            config = self._g._load_boss_raid_config()
            delete_br_profile(config, profile_id)
            self._g._save_boss_raid_config(config)
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def export_boss_raid_profile(self, profile_id=None):
        try:
            config = self._g._load_boss_raid_config()
            profile = find_br_profile(config, profile_id) if profile_id else None
            if profile is None:
                profile = find_br_profile(config, config.get('active_profile_id'))
            if profile is None:
                raise RuntimeError('No active profile')
            path = export_br_profile_path(profile)
            return json.dumps({'ok': True, 'path': path}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def start_boss_raid_import_picker(self, path=None):
        try:
            root = str(path or (os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else os.path.dirname(os.path.abspath(__file__))))
            self._g._boss_raid_picker_purpose = 'boss_raid_import'
            data = json.loads(self.browse_dir(root))
            data['mode'] = 'file'
            return json.dumps({'ok': True, 'browser': data}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def boss_raid_start(self):
        """Start boss raid from active profile via JS."""
        try:
            self._g._toggle_boss_raid()
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def boss_raid_stop(self):
        """Stop boss raid."""
        try:
            if self._g._boss_raid_engine:
                self._g._boss_raid_engine.stop()
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def boss_raid_next_phase(self):
        """Advance boss raid to next phase via JS."""
        try:
            self._g._boss_raid_next_phase()
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def boss_raid_reset(self):
        """Reset boss raid to idle."""
        try:
            if self._g._boss_raid_engine:
                self._g._boss_raid_engine.reset()
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def search_boss_raid_remote(self, query_payload):
        try:
            if isinstance(query_payload, str):
                query_payload = json.loads(query_payload or '{}')
            config = self._g._load_boss_raid_config()
            client = BossRaidCloudClient(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL)
            query = {
                'q': str((query_payload or {}).get('q', '') or '').strip(),
                'profile_name': str((query_payload or {}).get('profile_name', '') or '').strip(),
                'player_uid': str((query_payload or {}).get('player_uid', '') or '').strip(),
                'player_name': str((query_payload or {}).get('player_name', '') or '').strip(),
                'page': int((query_payload or {}).get('page', 1) or 1),
                'page_size': int((query_payload or {}).get('page_size', 20) or 20),
            }
            result = client.search(query)
            config['last_remote_search'] = {
                'query': query,
                'results': result.get('items', []),
                'error': '',
                'fetched_at': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime()),
            }
            self._g._save_boss_raid_config(config)
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'results': result.get('items', []), 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            config = self._g._load_boss_raid_config()
            last = config.get('last_remote_search', {}) or {}
            last['error'] = str(e)
            config['last_remote_search'] = last
            self._g._save_boss_raid_config(config)
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': False, 'message': str(e), 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)

    def download_boss_raid_remote(self, remote_id):
        try:
            config = self._g._load_boss_raid_config()
            client = BossRaidCloudClient(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL)
            result = client.get(remote_id)
            profile_raw = result.get('profile') or {}
            profile = normalize_br_profile(profile_raw, author_snapshot=self._g._boss_raid_author_snapshot(), source='downloaded')
            profile['id'] = profile.get('id') or ''
            profile['id'] = profile['id'] if profile['id'] not in {item.get('id') for item in config.get('profiles', []) or []} else ''
            if not profile['id']:
                import uuid as _uuid
                profile['id'] = f'boss_{_uuid.uuid4().hex[:12]}'
            profile['remote_id'] = result.get('id')
            profile['source'] = 'downloaded'
            profile['updated_at'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            upsert_br_profile(config, profile, activate=False)
            self._g._save_boss_raid_config(config)
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def upload_boss_raid_profile(self, profile_id=None):
        try:
            config = self._g._load_boss_raid_config()
            profile = find_br_profile(config, profile_id) if profile_id else None
            if profile is None:
                profile = find_br_profile(config, config.get('active_profile_id'))
            if profile is None:
                raise RuntimeError('No active profile')
            auth = self._g._refresh_boss_raid_upload_auth(force=False)
            token = str((auth or {}).get('token') or '').strip()
            if not token:
                raise RuntimeError(str((auth or {}).get('error') or 'Upload token is empty'))
            client = BossRaidCloudClient(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL)
            author = self._g._boss_raid_author_snapshot()
            payload = {
                'profile_name': profile.get('profile_name', ''),
                'description': profile.get('description', ''),
                'boss_total_hp': int(profile.get('boss_total_hp') or 0),
                'enrage_time_s': int(profile.get('enrage_time_s') or 0),
                'player_uid': author.get('player_uid', ''),
                'player_name': author.get('player_name', ''),
                'schema_version': 1,
                'profile': profile,
            }
            result = client.upload(payload, token)
            profile['source'] = 'uploaded'
            profile['remote_id'] = result.get('id')
            upsert_br_profile(config, profile,
                              activate=str(config.get('active_profile_id') or '') == str(profile.get('id') or ''))
            self._g._save_boss_raid_config(config)
            self._g._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'remote_id': result.get('id'), 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e), 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)

    def refresh_boss_raid_upload_auth(self, force=False):
        try:
            auth = self._g._refresh_boss_raid_upload_auth(force=bool(force))
            self._g._sync_boss_raid_menu()
            return json.dumps({
                'ok': bool(auth.get('ready')),
                'message': str(auth.get('error') or ''),
                'upload_auth': auth,
                'state': self._g._get_boss_raid_menu_state(),
            }, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e), 'state': self._g._get_boss_raid_menu_state()}, ensure_ascii=False)

    # ── Boss ↔ AutoKey Linkage API ──

    def get_linkage_state(self):
        try:
            config = load_linkage_config(self._g._cfg_settings_ref)
            linkage = getattr(self._g, '_boss_autokey_linkage', None)
            status = linkage.get_status() if linkage else {}
            return json.dumps({'ok': True, 'state': build_linkage_state(config, status)}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def set_linkage_enabled(self, enabled):
        try:
            config = load_linkage_config(self._g._cfg_settings_ref)
            config['enabled'] = bool(enabled)
            save_linkage_config(self._g._cfg_settings_ref, config)
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def set_linkage_debug(self, enabled):
        try:
            config = load_linkage_config(self._g._cfg_settings_ref)
            config['debug_log'] = bool(enabled)
            save_linkage_config(self._g._cfg_settings_ref, config)
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def set_linkage_global_cooldown(self, seconds):
        try:
            config = load_linkage_config(self._g._cfg_settings_ref)
            config['global_cooldown_s'] = max(0.0, min(60.0, float(seconds)))
            save_linkage_config(self._g._cfg_settings_ref, config)
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def save_linkage_mappings(self, mappings_json):
        try:
            mappings = json.loads(mappings_json) if isinstance(mappings_json, str) else mappings_json
            config = load_linkage_config(self._g._cfg_settings_ref)
            config['mappings'] = list(mappings) if isinstance(mappings, list) else []
            save_linkage_config(self._g._cfg_settings_ref, config)
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def reset_linkage(self):
        try:
            linkage = getattr(self._g, '_boss_autokey_linkage', None)
            if linkage:
                linkage.reset()
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    # ── Sound settings ──
    def set_sound_enabled(self, enabled):
        """Global SFX on/off."""
        from sao_sound import set_sound_enabled
        set_sound_enabled(bool(enabled))
        self._g._set_setting('sound_enabled', bool(enabled))

    def set_sound_volume(self, volume_pct):
        """Global volume 0-100."""
        from sao_sound import set_sound_volume
        set_sound_volume(int(volume_pct))
        self._g._set_setting('sound_volume', int(volume_pct))

    # ── Boss bar mode ──
    def set_boss_bar_mode(self, mode):
        """Set boss bar display mode: 'always' | 'boss_raid' | 'off'."""
        mode = str(mode or 'boss_raid').strip()
        if mode not in ('always', 'boss_raid', 'off'):
            mode = 'boss_raid'
        self._g._set_setting('boss_bar_mode', mode)

    def get_boss_bar_mode(self):
        """Return current boss bar display mode."""
        return self._g._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'

    def set_dps_enabled(self, enabled):
        on = bool(enabled)
        self._g._set_setting('dps_enabled', on)
        if not on:
            self._g._hide_dps_window()
        else:
            tracker = getattr(self._g, '_dps_tracker', None)
            try:
                if tracker and tracker.has_recent_damage(self._g._combat_damage_timeout_s()):
                    self._g._show_dps_live_snapshot(tracker.get_snapshot())
            except Exception:
                pass
        self._g._sync_menu_settings()
        return json.dumps({'ok': True, 'enabled': on}, ensure_ascii=False)

    def set_dps_fade_timeout(self, seconds):
        val = max(0, int(seconds or 0))
        self._g._set_setting('dps_fade_timeout_s', val)
        self._g._sync_menu_settings()
        return json.dumps({'ok': True, 'timeout': val}, ensure_ascii=False)

    def get_dps_enabled(self):
        return bool(self._g._get_setting('dps_enabled', True))

    def show_last_dps_report(self):
        try:
            if self._g._show_dps_last_report():
                self._g._sync_dps_report_availability()
                return json.dumps({'ok': True}, ensure_ascii=False)
            return json.dumps({
                'ok': False,
                'message': 'No last combat report yet.',
            }, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def boss_hp_hit_regions(self, regions):
        """Receive display regions from boss_hp.html for pass-through."""
        pass  # Boss HP is always fully click-through

    # ── Raid Editor toggle ──
    def toggle_raid_editor(self):
        """Show/hide the Raid Editor overlay."""
        def _do():
            if self._g._raid_editor_visible:
                self._g._hide_raid_editor()
            else:
                self._g._show_raid_editor()
        threading.Thread(target=_do, daemon=True).start()

    def get_raid_editor_visible(self):
        return bool(self._g._raid_editor_visible)

    # ── AutoKey Editor toggle ──
    def toggle_autokey_editor(self):
        """Show/hide the AutoKey Editor overlay."""
        def _do():
            if self._g._autokey_editor_visible:
                self._g._hide_autokey_editor()
            else:
                self._g._show_autokey_editor()
        threading.Thread(target=_do, daemon=True).start()

    def get_autokey_editor_visible(self):
        return bool(self._g._autokey_editor_visible)

    # ── Hide & Seek toggle ──
    def toggle_hide_seek(self):
        """Toggle the Hide & Seek automation on/off.

        v2.1.20: 不再额外 spawn daemon thread. JS 桥本身就在 pywebview 的 worker
        线程被调用, 再嵌套一层 thread 会让 _show_identity_alert_window 内部的
        alert_win.show() / pythonnet form.Invoke 与 alert_win 自身的 evaluate_js
        在两个不同的非主线程上并发, 在某些机器上会触发 WebView2 native crash.
        engine.start() 自己会创建后台线程, 直接同步调用即可。
        """
        try:
            self._g._toggle_hide_seek()
        except Exception as e:
            print(f'[SAO-WV] toggle_hide_seek failed: {e}')
            import traceback; traceback.print_exc()

    def get_hide_seek_active(self):
        engine = getattr(self._g, '_hide_seek_engine', None)
        return bool(engine and engine.running)

    # ── Data source mode ──
    def set_data_source(self, mode):
        """Legacy no-op: stamina and skills are fixed to vision now."""
        self._g._sync_menu_info()

    def set_component_source(self, component, mode):
        """Legacy no-op: per-component source switching is no longer exposed."""
        self._g._sync_menu_info()

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

    # ── DPS Meter settings ──
    def set_dps_enabled(self, enabled):
        """Toggle DPS meter visibility."""
        on = bool(enabled)
        self._g._set_setting('dps_enabled', on)
        if not on:
            self._g._hide_dps_window()
        else:
            tracker = getattr(self._g, '_dps_tracker', None)
            try:
                if tracker and tracker.has_recent_damage(self._g._combat_damage_timeout_s()):
                    self._g._show_dps_live_snapshot(tracker.get_snapshot())
            except Exception:
                pass
        self._g._sync_menu_settings()

    def set_dps_fade_timeout(self, seconds):
        """Set DPS fade-out idle timeout in seconds (0 = never fade)."""
        val = max(0, int(seconds or 0))
        self._g._set_setting('dps_fade_timeout_s', val)
        self._g._sync_menu_settings()

    def get_dps_enabled(self):
        return bool(self._g._get_setting('dps_enabled', True))


class DpsWindowAPI:
    """pywebview js_api for the DPS meter window — supports dragging and data queries."""

    def __init__(self, gui: 'SAOWebViewGUI'):
        self._g = gui

    def window_drag(self, dx, dy):
        """Move DPS window by delta pixels."""
        try:
            win = self._g.dps_win
            if win:
                win.move(win.x + int(dx), win.y + int(dy))
        except Exception:
            pass

    def reset_dps(self):
        """Reset the DPS tracker encounter."""
        try:
            tracker = getattr(self._g, '_dps_tracker', None)
            if tracker:
                tracker.reset()
                self._g._sync_dps_report_availability()
                if getattr(self._g, '_dps_mode', 'hidden') != 'report':
                    self._g._eval_dps(
                        f'DpsMeter.showLive({json.dumps(tracker.get_snapshot(), ensure_ascii=False)})'
                    )
        except Exception:
            pass

    def request_live_snapshot(self):
        def _push():
            try:
                tracker = getattr(self._g, '_dps_tracker', None)
                snapshot = tracker.get_snapshot() if tracker else None
                if snapshot is None:
                    snapshot = {
                        'encounter_active': False,
                        'elapsed_s': 0.0,
                        'total_damage': 0,
                        'total_heal': 0,
                        'total_dps': 0,
                        'total_hps': 0,
                        'entities': [],
                    }
                self._g._dps_mode = 'live'
                self._g._eval_dps(
                    f'DpsMeter.showLive({json.dumps(snapshot, ensure_ascii=False)})'
                )
            except Exception:
                pass
        threading.Thread(target=_push, daemon=True).start()

    def show_last_report(self):
        try:
            tracker = getattr(self._g, '_dps_tracker', None)
            report = tracker.get_last_report() if tracker else None
            if not report:
                return json.dumps({
                    'ok': False,
                    'message': 'No last combat report yet.',
                }, ensure_ascii=False)
            self._g._dps_mode = 'report'
            self._g._eval_dps(
                f'DpsMeter.showLastReport({json.dumps(report, ensure_ascii=False)})'
            )
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def get_entity_detail(self, uid):
        """Fetch detailed entity stats (with skill breakdown) and push to JS."""
        def _fetch():
            try:
                tracker = getattr(self._g, '_dps_tracker', None)
                if tracker:
                    detail = tracker.get_entity_detail(int(uid))
                    if detail:
                        self._g._eval_dps(
                            f'DpsMeter.updateDetail({json.dumps(detail, ensure_ascii=False)})'
                        )
            except Exception:
                pass
        threading.Thread(target=_fetch, daemon=True).start()

    def set_detail_mode(self, active):
        """Resize the DPS window between compact and detailed layouts."""
        try:
            self._g._set_dps_detail_mode(bool(active))
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def resize_dps(self, width, height, commit=False):
        """Resize the detailed DPS panel; commit=True persists the size."""
        try:
            self._g._resize_dps_window(int(width), int(height), persist=bool(commit))
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def play_sound(self, name: str):
        threading.Thread(target=self._g._play_sound, args=(name,), daemon=True).start()


class RaidEditorAPI:
    """pywebview js_api for the Raid Editor overlay — entity role, phase control."""

    def __init__(self, gui: 'SAOWebViewGUI'):
        self._g = gui

    def window_drag(self, dx, dy):
        try:
            win = self._g.raid_editor_win
            if win:
                win.move(win.x + int(dx), win.y + int(dy))
        except Exception:
            pass

    def set_entity_role(self, uuid, role):
        """Mark a tracked entity as 'boss' or 'enemy'."""
        try:
            engine = getattr(self._g, '_boss_raid_engine', None)
            if engine:
                engine.set_entity_role(int(uuid), str(role))
        except Exception:
            pass

    def raid_next_phase(self):
        """Force-advance to the next phase."""
        try:
            engine = getattr(self._g, '_boss_raid_engine', None)
            if engine:
                engine.next_phase()
        except Exception:
            pass

    def raid_reset(self):
        """Reset the raid engine."""
        try:
            engine = getattr(self._g, '_boss_raid_engine', None)
            if engine:
                engine.reset()
                self._g._push_raid_editor_full()
        except Exception:
            pass

    def play_sound(self, name: str):
        threading.Thread(target=self._g._play_sound, args=(name,), daemon=True).start()


class CommanderAPI:
    """pywebview js_api for the Commander panel — team overview & self CDs."""

    def __init__(self, gui: 'SAOWebViewGUI'):
        self._g = gui

    def window_drag(self, dx, dy):
        try:
            win = self._g.commander_win
            if win:
                win.move(win.x + int(dx), win.y + int(dy))
        except Exception:
            pass

    def close_commander(self):
        """Hide the commander panel."""
        try:
            self._g._hide_commander()
        except Exception:
            pass

    def request_data(self):
        """JS->Python: request a fresh commander data push."""
        def _push():
            try:
                self._g._push_commander_data()
            except Exception:
                pass
        threading.Thread(target=_push, daemon=True).start()

    def play_sound(self, name: str):
        threading.Thread(target=self._g._play_sound, args=(name,), daemon=True).start()


class AutoKeyEditorAPI:
    """pywebview js_api for the AutoKey Editor overlay — skill recording."""

    def __init__(self, gui: 'SAOWebViewGUI'):
        self._g = gui

    def window_drag(self, dx, dy):
        try:
            win = self._g.autokey_editor_win
            if win:
                win.move(win.x + int(dx), win.y + int(dy))
        except Exception:
            pass

    def save_autokey_actions(self, actions_json):
        """Save recorded burst-ready → skill trigger actions.
        actions_json: JSON string of [{trigger_slot, action_slot}, ...]
        """
        try:
            actions = json.loads(actions_json) if isinstance(actions_json, str) else actions_json
            engine = getattr(self._g, '_autokey_engine', None)
            if engine:
                engine.set_burst_actions(actions)
            self._g._set_setting('autokey_burst_actions', actions)
            self._g._sync_menu_settings()
        except Exception:
            pass

    def get_autokey_actions(self):
        """Return current burst-ready actions as JSON."""
        try:
            actions = self._g._get_setting('autokey_burst_actions', [])
            return json.dumps(actions, ensure_ascii=False)
        except Exception:
            return '[]'

    def play_sound(self, name: str):
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

        from character_profile import load_profile

        self.settings = SettingsManager()
        self.settings.set('ui_mode', 'webview')
        self.settings.save()

        # 音效
        self._sound_ok = False
        try:
            import sao_sound
            self._sao_sound = sao_sound
            self._sound_ok = True
        except Exception:
            self._sao_sound = None

        # 角色 (从上次保存的 profile 加载用户名/职业, 等级来自抓包)
        profile = load_profile()
        self._username = profile.get('username', '') or 'Player'
        self._profession = profile.get('profession', '剑士')
        self._level = max(1, int(profile.get('level', 1) or 1))
        self._last_displayed_level_base = 0  # 用于检测等级变化并触发升级动画
        self._sta_offline_armed = False

        # 识别状态
        self._recognition_active = False
        self._game_state = None  # GameState dataclass
        self._state_mgr = None
        self._recognition_engine = None
        self._recognition_engines = []
        self._packet_engine = None
        self._vision_engine = None
        self._vision_paused_for_death = False
        self._last_dead_state = False
        self._recog_lock = threading.Lock()  # 保护 _recognition_active 切换

        # 菜单
        self._sta_detector_started = False
        self._menu_visible = False
        self._session_players = {}
        self._session_players_self_uid = 0
        self._session_players_version = 0
        self._session_players_last_sig = None
        self._session_players_last_push_ts = 0.0

        # 窗口
        self.hp_win = None
        self.menu_win = None
        self.skillfx_win = None
        self.alert_win = None
        self.boss_hp_win = None
        self._boss_hp_hwnd = 0
        self._boss_hp_visible = False
        self._boss_hp_bar_shown = False  # JS-level visibility
        self._boss_hp_geometry = None
        self._bb_last_target_uuid = 0     # UUID of last monster damaged by self
        self._bb_last_damage_ts = 0.0     # timestamp of last self→monster damage
        self._bb_recent_targets = {}      # uuid -> last_damage_ts for multi-unit secondary panels
        self._bb_damage_timeout = 60.0    # seconds before boss bar fades out
        self._pending_combat_reset_after = 0.0
        self._pending_combat_reset_reason = ''
        self._damage_self_fallback_log_ts = 0.0

        # DPS Meter
        self.dps_win = None
        self._dps_hwnd = 0
        self._dps_visible = False
        self._dps_faded = False
        self._dps_fade_seq = 0
        self._dps_mode = 'hidden'
        self._dps_tracker = None
        self._dps_api = None
        self._dps_last_report_available = False
        self._dps_base_w = 0
        self._dps_base_h = 0
        self._dps_detail_w = 760
        self._dps_detail_h = 560
        self._dps_detail_mode = False

        # Raid Editor overlay
        self.raid_editor_win = None
        self._raid_editor_visible = False
        self._raid_editor_api = None

        # AutoKey Editor overlay
        self.autokey_editor_win = None
        self._autokey_editor_visible = False
        self._autokey_editor_api = None

        # Commander panel
        self.commander_win = None
        self._commander_visible = False
        self._commander_api = None

        # Hide & Seek engine
        self._hide_seek_engine = None
        self._hide_seek_alert_timer = None
        self._hide_seek_alert_active = False

        # 热切换目标
        self._pending_switch: Optional[str] = None
        self._update_popup_ready = False
        self._pending_update_popup_snapshot = None
        self._last_update_popup_key = ''
        self._identity_alert_kind = ''

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
        self._skillfx_hwnd = 0
        self._alert_hwnd = 0
        self._skillfx_visible = False
        self._skillfx_slot_count = 9
        self._skillfx_layout = None
        self._sta_pixel_detector_enabled = False
        self._last_ready_slots = {}
        self._burst_seen_cooling = set()
        self._last_watched_signature = ()
        self._last_burst_ready = False
        self._last_burst_slot = 0
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
        self._auto_key_engine = None
        self._auto_key_picker_purpose = ''
        self._auto_key_last_menu_state = None
        self._auto_key_upload_auth = default_upload_auth_state()
        self._last_identity_alert_serial = 0
        self._identity_alert_visible = False
        self._identity_alert_nonce = 0

        # Boss Raid 相关引用
        self._boss_raid_engine = None
        self._boss_raid_last_menu_state = None
        self._boss_raid_upload_auth = default_upload_auth_state()
        self._boss_raid_picker_purpose = ''
        self._last_boss_timer_text = ''
        self._last_boss_timer_urgency = ''

        # Boss ↔ AutoKey 联动
        self._boss_autokey_linkage = None

        self._bootstrap_runtime_state()
        self._start_packet_engine_early()

    # ─── 音效 ───
    def _play_sound(self, name: str):
        if self._sound_ok and self._sao_sound:
            try:
                self._sao_sound.play_sound(name)
            except Exception:
                pass

    def _send_linked_key(self, key: str, press_mode: str = "tap",
                         hold_ms: int = 80, press_count: int = 1):
        """Send a keystroke for boss→autokey linkage, reusing AutoKeyEngine's VK map."""
        try:
            from auto_key_engine import VK_NAME_MAP, INPUT, KEYBDINPUT, INPUT_KEYBOARD, KEYEVENTF_KEYUP
            import ctypes
            key = (key or "").strip().upper()
            vk = VK_NAME_MAP.get(key)
            if vk is None and len(key) == 1 and key.isalpha():
                vk = ord(key)
            if vk is None:
                return
            hold_s = max(0.015, hold_ms / 1000.0) if press_mode == "hold" else 0.015
            extra = ctypes.c_ulong(0)
            for _ in range(max(1, press_count)):
                ki = KEYBDINPUT(wVk=int(vk), wScan=0, dwFlags=0, time=0,
                                dwExtraInfo=ctypes.pointer(extra))
                ev = INPUT(type=INPUT_KEYBOARD, ki=ki)
                ctypes.windll.user32.SendInput(1, ctypes.byref(ev), ctypes.sizeof(INPUT))
                time.sleep(hold_s)
                ki2 = KEYBDINPUT(wVk=int(vk), wScan=0, dwFlags=KEYEVENTF_KEYUP, time=0,
                                 dwExtraInfo=ctypes.pointer(extra))
                ev2 = INPUT(type=INPUT_KEYBOARD, ki=ki2)
                ctypes.windll.user32.SendInput(1, ctypes.byref(ev2), ctypes.sizeof(INPUT))
                if press_count > 1:
                    time.sleep(0.04)
        except Exception as e:
            print(f"[Linkage] send_key error: {e}")

    def _set_setting(self, key: str, value):
        """Persist a setting to cfg_settings and save."""
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            self._cfg_settings_ref.set(key, value)
            try:
                self._cfg_settings_ref.save()
            except Exception:
                pass

    def _get_setting(self, key: str, default=None):
        """Read a setting."""
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            return self._cfg_settings_ref.get(key, default)
        return default

    def _bootstrap_runtime_state(self):
        if getattr(self, '_state_mgr', None) is not None and getattr(self, '_cfg_settings_ref', None):
            return
        try:
            from game_state import GameStateManager
            from config import SettingsManager as CfgSettings

            if getattr(self, '_state_mgr', None) is None:
                self._state_mgr = GameStateManager()
            if not getattr(self, '_cfg_settings_ref', None):
                self._cfg_settings_ref = CfgSettings()
            self._state_mgr.load_cache(self._cfg_settings_ref)
        except Exception as e:
            print(f'[SAO] Runtime state bootstrap failed: {e}')

    def _start_packet_engine_early(self):
        if getattr(self, '_packet_engine', None) is not None:
            return
        if not getattr(self, '_cfg_settings_ref', None) or not getattr(self, '_state_mgr', None):
            return
        try:
            from packet_bridge import PacketBridge

            packet_engine = PacketBridge(
                self._state_mgr,
                self._cfg_settings_ref,
                on_damage=self._on_packet_damage,
                on_monster_update=self._on_monster_update,
                on_boss_event=self._on_boss_event,
                on_scene_change=self._on_scene_change,
            )
            packet_engine.start()
            self._packet_engine = packet_engine
            self._recognition_engines = [packet_engine] + [
                engine for engine in (getattr(self, '_recognition_engines', []) or [])
                if engine is not packet_engine
            ]
            if not getattr(self, '_recognition_engine', None):
                self._recognition_engine = packet_engine
            self._recognition_active = True
            print('[SAO] Packet bridge started early (pre-webview)')
        except Exception as e:
            import traceback
            print(f'[SAO] Early packet bridge FAILED to start: {e}', flush=True)
            traceback.print_exc()
            self._packet_engine = None

    def _auto_key_settings_ref(self):
        return self._cfg_settings_ref or self.settings

    def _auto_key_author_snapshot(self):
        gs = getattr(self, '_game_state', None)
        if gs is not None:
            return snapshot_author_from_state(gs)
        return {
            'player_uid': '',
            'player_name': self._username,
            'profession_id': 0,
            'profession_name': self._profession,
        }

    def _auto_key_identity_state(self):
        source = 'packet' if getattr(self, '_game_state', None) is not None else 'profile'
        return build_identity_state(self._auto_key_author_snapshot(), source=source)

    def _set_auto_key_upload_auth(self, token: str = '', expires_at: str = '', error: str = '',
                                  mode: str = '', identity: Optional[dict] = None,
                                  server_url: str = ''):
        identity_state = identity or self._auto_key_identity_state()
        self._auto_key_upload_auth = {
            'token': str(token or '').strip(),
            'ready': bool(str(token or '').strip()),
            'token_masked': '',
            'expires_at': str(expires_at or '').strip(),
            'error': str(error or '').strip(),
            'mode': str(mode or '').strip(),
            'identity': identity_state,
            'server_url': str(server_url or '').strip(),
        }
        return self._auto_key_upload_auth

    def _auto_key_upload_auth_matches(self, identity_state, config):
        auth = getattr(self, '_auto_key_upload_auth', None) or {}
        cached_identity = auth.get('identity') or {}
        if str(auth.get('server_url') or '') != str((config or {}).get('server_url') or ''):
            return False
        return (
            str(cached_identity.get('player_uid') or '') == str(identity_state.get('player_uid') or '') and
            str(cached_identity.get('player_name') or '') == str(identity_state.get('player_name') or '') and
            int(cached_identity.get('profession_id') or 0) == int(identity_state.get('profession_id') or 0)
        )

    def _auto_key_upload_auth_valid(self, identity_state=None, config=None):
        auth = getattr(self, '_auto_key_upload_auth', None) or {}
        if not auth.get('ready') or not str(auth.get('token') or '').strip():
            return False
        identity_state = identity_state or self._auto_key_identity_state()
        config = config or self._load_auto_key_config()
        if not self._auto_key_upload_auth_matches(identity_state, config):
            return False
        expires_at = str(auth.get('expires_at') or '').strip()
        if expires_at and expires_at <= time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(time.time() + 5)):
            return False
        return True

    def _auto_key_upload_auth_state(self, config=None, identity_state=None):
        config = config or self._load_auto_key_config()
        identity_state = identity_state or self._auto_key_identity_state()
        auth = getattr(self, '_auto_key_upload_auth', None) or default_upload_auth_state()
        if self._auto_key_upload_auth_valid(identity_state, config):
            return auth
        error = str(auth.get('error') or '').strip()
        return self._set_auto_key_upload_auth(
            token='',
            expires_at='',
            error=error,
            mode=str(auth.get('mode') or ''),
            identity=identity_state,
            server_url=str(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL),
        )

    def _refresh_auto_key_upload_auth(self, force: bool = False):
        config = self._load_auto_key_config()
        identity_state = self._auto_key_identity_state()
        server_url = str(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL)
        if not identity_state.get('ready'):
            missing = ', '.join(identity_state.get('missing') or [])
            return self._set_auto_key_upload_auth(
                token='',
                expires_at='',
                error=f'Identity is incomplete: {missing}',
                mode='',
                identity=identity_state,
                server_url=server_url,
            )
        if not force and self._auto_key_upload_auth_valid(identity_state, config):
            return getattr(self, '_auto_key_upload_auth', None) or default_upload_auth_state()
        try:
            client = AutoKeyCloudClient(server_url)
            result = client.issue_upload_token({
                'player_uid': identity_state.get('player_uid', ''),
                'player_name': identity_state.get('player_name', ''),
                'profession_id': int(identity_state.get('profession_id') or 0),
                'profession_name': identity_state.get('profession_name', ''),
            })
            issued_identity = build_identity_state(result.get('identity') or identity_state, source=identity_state.get('source') or 'packet')
            return self._set_auto_key_upload_auth(
                token=str(result.get('token') or '').strip(),
                expires_at=str(result.get('expires_at') or '').strip(),
                error='',
                mode=str(result.get('mode') or '').strip(),
                identity=issued_identity,
                server_url=server_url,
            )
        except Exception as e:
            return self._set_auto_key_upload_auth(
                token='',
                expires_at='',
                error=str(e),
                mode='',
                identity=identity_state,
                server_url=server_url,
            )

    def _load_auto_key_config(self):
        ref = self._auto_key_settings_ref()
        return load_auto_key_config(ref, state_snapshot=self._auto_key_author_snapshot())

    def _save_auto_key_config(self, config):
        ref = self._auto_key_settings_ref()
        saved = save_auto_key_config(ref, config)
        if self._auto_key_engine:
            self._auto_key_engine.invalidate()
        return saved

    def _get_auto_key_menu_state(self):
        config = self._load_auto_key_config()
        status = self._auto_key_engine.get_status() if self._auto_key_engine else {}
        identity_state = self._auto_key_identity_state()
        upload_auth = self._auto_key_upload_auth_state(config=config, identity_state=identity_state)
        return build_auto_key_state(
            config,
            engine_status=status,
            identity_snapshot=identity_state,
            upload_auth=upload_auth,
        )

    def _sync_auto_key_menu(self):
        try:
            state = self._get_auto_key_menu_state()
            if state != getattr(self, '_auto_key_last_menu_state', None):
                self._auto_key_last_menu_state = copy.deepcopy(state)
                self._eval_menu(f'SAO.syncAutoKeyState({json.dumps(state, ensure_ascii=False)})')
        except Exception:
            pass

    # ─── Boss Raid helpers ───

    def _boss_raid_settings_ref(self):
        return self._cfg_settings_ref or self.settings

    def _boss_raid_author_snapshot(self):
        gs = getattr(self, '_game_state', None)
        if gs is not None:
            return snapshot_author_from_state(gs)
        return {
            'player_uid': '',
            'player_name': self._username,
            'profession_id': 0,
            'profession_name': self._profession,
        }

    def _boss_raid_identity_state(self):
        source = 'packet' if getattr(self, '_game_state', None) is not None else 'profile'
        return build_identity_state(self._boss_raid_author_snapshot(), source=source)

    def _set_boss_raid_upload_auth(self, token: str = '', expires_at: str = '', error: str = '',
                                   mode: str = '', identity: Optional[dict] = None,
                                   server_url: str = ''):
        identity_state = identity or self._boss_raid_identity_state()
        self._boss_raid_upload_auth = {
            'token': str(token or '').strip(),
            'ready': bool(str(token or '').strip()),
            'token_masked': '',
            'expires_at': str(expires_at or '').strip(),
            'error': str(error or '').strip(),
            'mode': str(mode or '').strip(),
            'identity': identity_state,
            'server_url': str(server_url or '').strip(),
        }
        return self._boss_raid_upload_auth

    def _boss_raid_upload_auth_matches(self, identity_state, config):
        auth = getattr(self, '_boss_raid_upload_auth', None) or {}
        cached_identity = auth.get('identity') or {}
        if str(auth.get('server_url') or '') != str((config or {}).get('server_url') or ''):
            return False
        return (
            str(cached_identity.get('player_uid') or '') == str(identity_state.get('player_uid') or '') and
            str(cached_identity.get('player_name') or '') == str(identity_state.get('player_name') or '') and
            int(cached_identity.get('profession_id') or 0) == int(identity_state.get('profession_id') or 0)
        )

    def _boss_raid_upload_auth_valid(self, identity_state=None, config=None):
        auth = getattr(self, '_boss_raid_upload_auth', None) or {}
        if not auth.get('ready') or not str(auth.get('token') or '').strip():
            return False
        identity_state = identity_state or self._boss_raid_identity_state()
        config = config or self._load_boss_raid_config()
        if not self._boss_raid_upload_auth_matches(identity_state, config):
            return False
        expires_at = str(auth.get('expires_at') or '').strip()
        if expires_at and expires_at <= time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(time.time() + 5)):
            return False
        return True

    def _boss_raid_upload_auth_state(self, config=None, identity_state=None):
        config = config or self._load_boss_raid_config()
        identity_state = identity_state or self._boss_raid_identity_state()
        auth = getattr(self, '_boss_raid_upload_auth', None) or default_upload_auth_state()
        if self._boss_raid_upload_auth_valid(identity_state, config):
            return auth
        error = str(auth.get('error') or '').strip()
        return self._set_boss_raid_upload_auth(
            token='',
            expires_at='',
            error=error,
            mode=str(auth.get('mode') or ''),
            identity=identity_state,
            server_url=str(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL),
        )

    def _load_boss_raid_config(self):
        ref = self._boss_raid_settings_ref()
        return load_boss_raid_config(ref, state_snapshot=self._boss_raid_author_snapshot())

    def _save_boss_raid_config(self, config):
        ref = self._boss_raid_settings_ref()
        saved = save_boss_raid_config(ref, config)
        return saved

    def _get_boss_raid_menu_state(self):
        config = self._load_boss_raid_config()
        status = self._boss_raid_engine.get_status() if self._boss_raid_engine else {}
        identity_state = self._boss_raid_identity_state()
        upload_auth = self._boss_raid_upload_auth_state(config=config, identity_state=identity_state)
        state = build_boss_raid_state(
            config,
            engine_status=status,
            upload_auth=upload_auth,
        )
        state['identity'] = identity_state
        return state

    def _sync_boss_raid_menu(self):
        try:
            state = self._get_boss_raid_menu_state()
            if state != getattr(self, '_boss_raid_last_menu_state', None):
                self._boss_raid_last_menu_state = copy.deepcopy(state)
                self._eval_menu(f'SAO.syncBossRaidState({json.dumps(state, ensure_ascii=False)})')
        except Exception:
            pass

    def _toggle_boss_raid(self):
        """Hotkey F7: toggle boss raid start/stop."""
        if not self._boss_raid_engine:
            return
        config = self._load_boss_raid_config()
        if not config.get('enabled'):
            config['enabled'] = True
            self._save_boss_raid_config(config)
        status = self._boss_raid_engine.get_status()
        if status.get('state') == 'running':
            self._boss_raid_engine.stop()
            self._eval_menu('SAO.showToast("BOSS RAID: STOPPED")')
        else:
            from boss_raid_engine import active_profile as br_active_profile
            profile = br_active_profile(config)
            if profile:
                self._boss_raid_engine.start(profile)
                self._eval_menu('SAO.showToast("BOSS RAID: START")')
            else:
                self._eval_menu('SAO.showToast("BOSS RAID: No active profile")')
        self._sync_boss_raid_menu()

    def _boss_raid_next_phase(self):
        """Hotkey F8: advance to next phase."""
        if not self._boss_raid_engine:
            return
        self._boss_raid_engine.next_phase()

    # ── Hide & Seek ──

    def _toggle_hide_seek(self):
        """Toggle the Hide & Seek automation engine on/off."""
        if self._hide_seek_engine and self._hide_seek_engine.running:
            self._stop_hide_seek()
        else:
            self._start_hide_seek()

    def _start_hide_seek(self):
        """Start the Hide & Seek engine and show persistent alert."""
        if self._hide_seek_engine and self._hide_seek_engine.running:
            return
        try:
            from hide_seek_engine import HideSeekEngine
            from window_locator import WindowLocator
            locator = getattr(self, '_locator', None)
            if not locator:
                locator = WindowLocator()
            self._hide_seek_engine = HideSeekEngine(
                locator=locator,
                on_status=self._on_hide_seek_status,
            )
            self._hide_seek_engine.start()
            self._eval_menu('SAO.showToast("HIDE & SEEK: ON")')
            # Set flag synchronously so _sync_identity_alert
            # won't dismiss the alert even if engine.running isn't set yet.
            self._hide_seek_alert_active = True
            # Show the initial alert with sound — kind='hide_seek' so the
            # auto-dismiss timer in _hide_identity_alert_window will refuse
            # to close it as long as the engine is still running.
            self._show_identity_alert_window(
                "AUTO HIDE & SEEK", "Auto Hide'seek is on",
                duration_ms=60000, alert_kind='hide_seek')
            # Start periodic refresh to keep the alert visible
            self._schedule_hide_seek_alert_refresh()
        except Exception as e:
            print(f'[SAO] Hide&Seek start failed: {e}')
            import traceback; traceback.print_exc()

    def _stop_hide_seek(self):
        """Stop the Hide & Seek engine and dismiss persistent alert."""
        if self._hide_seek_engine:
            self._hide_seek_engine.stop()
        self._hide_seek_engine = None
        self._hide_hide_seek_persistent_alert()
        self._eval_menu('SAO.showToast("HIDE & SEEK: OFF")')

    def _on_hide_seek_status(self, message: str, step: int):
        """Callback from engine — could push status to UI if needed."""
        pass  # status is already printed by the engine

    def _schedule_hide_seek_alert_refresh(self):
        """Schedule the next alert refresh tick."""
        self._hide_seek_alert_timer = threading.Timer(
            50.0, self._refresh_hide_seek_alert)
        self._hide_seek_alert_timer.daemon = True
        self._hide_seek_alert_timer.start()

    def _refresh_hide_seek_alert(self):
        """Re-show the alert (without sound) every ~50s to prevent the 60s auto-hide.

        The hide & seek game mode can last a very long time (up to 8 min per round),
        so the alert must stay visible for the entire duration of the automation engine.

        IMPORTANT: This timer must NEVER call resume()/restart() or otherwise
        kill the engine thread.  Each detection phase can take many minutes of
        idle waiting; interrupting the thread would reset that wait.
        """
        engine = self._hide_seek_engine
        if not engine:
            self._hide_seek_alert_active = False
            return

        # If the engine object is gone (user stopped it), stop refreshing.
        # But do NOT call resume/restart — the engine might just be waiting
        # a long time for the next UI element to appear.
        if not engine.running:
            # Thread truly died (crash) — just log it, keep alert alive so
            # user knows the mode is still "on" conceptually.  They can
            # toggle off/on manually if needed.
            print('[SAO] Hide&Seek engine thread is no longer running')

        # Re-show without sound — this resets the 60s auto-hide timer via nonce
        self._show_identity_alert_window(
            "AUTO HIDE & SEEK", "Auto Hide'seek is on",
            duration_ms=60000, play_sound=False, alert_kind='hide_seek')
        self._schedule_hide_seek_alert_refresh()

    def _hide_hide_seek_persistent_alert(self):
        """Cancel the persistent alert refresh timer and hide alert."""
        self._hide_seek_alert_active = False
        t = self._hide_seek_alert_timer
        if t:
            t.cancel()
        self._hide_seek_alert_timer = None
        self._hide_identity_alert_window()
        self._sync_boss_raid_menu()

    def _arm_pending_combat_reset(self, scene_event=None):
        """Defer same-instance encounter reset until the next real damage."""
        reason = 'restart'
        delay_s = 3.0
        if isinstance(scene_event, dict):
            reason = str(scene_event.get('reason') or scene_event.get('kind') or reason)
            try:
                delay_s = float(scene_event.get('reset_delay_s', delay_s) or delay_s)
            except Exception:
                delay_s = 3.0
        self._pending_combat_reset_after = time.time() + max(0.0, delay_s)
        self._pending_combat_reset_reason = reason
        self._last_boss_bar_sig = None
        try:
            if self._dps_tracker:
                self._dps_tracker.invalidate_snapshot_cache()
        except Exception:
            pass
        print(
            f'[SAO] ♻ 同副本重开候选({reason}) — 等下一次伤害再重置 boss bar/DPS',
            flush=True,
        )

    def _maybe_apply_pending_combat_reset(self, event, is_self_combat_target: bool) -> bool:
        """Apply deferred encounter reset immediately before the first new hit."""
        try:
            reset_after = float(getattr(self, '_pending_combat_reset_after', 0.0) or 0.0)
        except Exception:
            reset_after = 0.0
        if reset_after <= 0.0 or time.time() < reset_after or not is_self_combat_target:
            return False
        try:
            damage = int(event.get('damage') or 0)
        except Exception:
            damage = 0
        if damage <= 0 or bool(event.get('is_heal', False)):
            return False

        reason = str(getattr(self, '_pending_combat_reset_reason', '') or 'restart')
        self._pending_combat_reset_after = 0.0
        self._pending_combat_reset_reason = ''
        print(
            f'[SAO] ♻ 下一次伤害到达，执行延迟重置: {reason}',
            flush=True,
        )

        self._bb_last_target_uuid = 0
        self._bb_last_damage_ts = 0.0
        self._bb_recent_targets = {}
        self._pending_combat_reset_after = 0.0
        self._pending_combat_reset_reason = ''
        self._last_boss_bar_sig = None
        try:
            gs = self._game_state
            if gs:
                gs.boss_breaking_stage = -1
                gs.boss_extinction_pct = 0.0
                gs.boss_current_hp = 0
                gs.boss_total_hp = 0
                gs.boss_hp_source = 'none'
                gs.boss_hp_est_pct = 1.0
                gs.boss_shield_active = False
                gs.boss_shield_pct = 0.0
                gs.boss_in_overdrive = False
                gs.boss_invincible = False
        except Exception:
            pass
        if self._dps_tracker:
            try:
                if self._dps_tracker.has_active_encounter():
                    self._dps_tracker.reset()
                else:
                    self._dps_tracker.invalidate_snapshot_cache()
            except Exception:
                pass
        self._dps_visible = False
        self._dps_faded = False
        self._dps_mode = 'hidden'
        if self._boss_raid_engine:
            try:
                if getattr(self._boss_raid_engine, '_state', '') != 'running':
                    self._boss_raid_engine.reset()
            except Exception:
                pass
        return True

    def _current_player_uid_int(self) -> int:
        for source in (
            getattr(self, '_game_state', None),
            getattr(getattr(self, '_state_mgr', None), 'state', None),
        ):
            try:
                uid = getattr(source, 'player_id', '') if source is not None else ''
                if str(uid).isdigit():
                    return int(uid)
            except Exception:
                pass
        return 0

    def _normalize_damage_event_for_self(self, event):
        if not isinstance(event, dict) or event.get('attacker_is_self'):
            return event
        self_uid = self._current_player_uid_int()
        if self_uid <= 0:
            return event

        def _event_int(name: str) -> int:
            try:
                return int(event.get(name) or 0)
            except Exception:
                return 0

        candidate_uids = [_event_int('attacker_uid')]
        for key in ('attacker_uuid', 'attacker_uuid_raw', 'top_summoner_id'):
            raw = _event_int(key)
            if raw and (raw & 0xFFFF) == 640:
                candidate_uids.append(raw >> 16)
            elif key == 'top_summoner_id' and raw:
                candidate_uids.append(raw)
        if self_uid not in candidate_uids:
            return event

        fixed = dict(event)
        fixed['attacker_is_self'] = True
        fixed['attacker_uid'] = self_uid
        fixed.setdefault('self_uid', self_uid)
        now = time.time()
        if now - float(getattr(self, '_damage_self_fallback_log_ts', 0.0) or 0.0) > 10.0:
            self._damage_self_fallback_log_ts = now
            print(
                f'[SAO] 修正伤害归属: attacker_uid -> self_uid={self_uid}',
                flush=True,
            )
        return fixed

    def _on_packet_damage(self, event):
        """Damage event callback from packet_parser → boss raid engine + DPS tracker.

        This is the critical path: when the DPS panel shows data, damage events
        ARE flowing. We use this to also ensure the boss bar target is set.
        """
        event = self._normalize_damage_event_for_self(event)
        # Track last self -> non-player combat target damage for boss bar target.
        # BossHP only displays later if packet_parser has usable HP data.
        _is_self_combat_target = bool(
            event.get('attacker_is_self')
            and event.get('target_uuid', 0)
            and (
                event.get('target_is_combat_target', False)
                or event.get('target_is_monster', False)
                or ('target_is_player' in event and not event.get('target_is_player', False))
            )
        )
        self._maybe_apply_pending_combat_reset(event, _is_self_combat_target)
        if _is_self_combat_target:
            target_uuid = event.get('target_uuid', 0)
            if target_uuid:
                # Same-map retry / monster UUID reuse: parser state may still
                # carry the previous unit's dead flag, which would make the
                # boss bar filter drop this live target.
                try:
                    _bridge = getattr(self, '_packet_engine', None)
                    _m = _bridge.get_monster(target_uuid) if _bridge else None
                    if _m is not None and getattr(_m, 'is_dead', False):
                        _m.is_dead = False
                        _m.last_update = time.time()
                except Exception:
                    pass
                self._bb_recent_targets[target_uuid] = time.time()
                self._bb_last_target_uuid = target_uuid
                self._bb_last_damage_ts = time.time()
                # Update DPS tracker boss target for boss-only total filtering
                if self._dps_tracker:
                    try:
                        self._dps_tracker.set_boss_uuid(target_uuid)
                    except Exception:
                        pass
                # Proactively check if the monster has max_hp in parser.
                # If not, estimate from current HP (the server never sends
                # AttrMaxHp for monsters, so packet_parser estimates it from
                # the first HP observation; this is a secondary fallback).
                try:
                    _bridge = getattr(self, '_packet_engine', None)
                    _m = _bridge.get_monster(target_uuid) if _bridge else None
                    if _m and _m.max_hp == 0 and _m.hp > 0:
                        _m.max_hp = _m.hp
                        logger.info(
                            f'[WebView] Estimated max_hp from HP on damage: '
                            f'{_m.hp} uuid={target_uuid}'
                        )
                except Exception:
                    pass
        if self._boss_raid_engine:
            try:
                self._boss_raid_engine.on_damage_event(event)
            except Exception:
                pass
        if self._dps_tracker:
            try:
                self._dps_tracker.on_damage_event(event)
            except Exception:
                pass

    def _on_monster_update(self, monster_data):
        """Monster update callback from packet_parser → boss raid engine + break bar tracking.

        When a boss monster appears in a new scene (after SyncNearEntities),
        pre-set the target UUID so the boss bar can immediately display HP
        when the player starts attacking. Also handles break bar pre-tracking.
        """
        if self._boss_raid_engine:
            try:
                self._boss_raid_engine.on_monster_update(monster_data)
            except Exception:
                pass

        # Pre-track monsters for boss bar:
        # - Any monster with HP (for immediate boss bar when damage starts)
        # - Monsters with break data (for immediate break bar display)
        try:
            _uuid = monster_data.get('uuid', 0)
            _max_ext = int(monster_data.get('max_extinction', 0) or 0)
            _max_hp = int(monster_data.get('max_hp', 0) or 0)
            _hp = int(monster_data.get('hp', 0) or 0)
            _is_dead = monster_data.get('is_dead', False)
            # Accept monster if it has either max_hp or hp (server may not
            # send AttrMaxHp; packet_parser estimates max_hp from HP).
            if _uuid and (_max_hp > 0 or _hp > 0) and (not _is_dead or _hp > 0):
                # Adopt this monster as the target if:
                # 1. No target yet (first monster after scene change)
                # 2. Current target is stale (dead, or no longer in monsters dict)
                _should_adopt = False
                if not self._bb_last_target_uuid:
                    _should_adopt = True
                else:
                    # Check if current target is still valid
                    try:
                        _bridge = getattr(self, '_packet_engine', None)
                        _cur = _bridge.get_monster(self._bb_last_target_uuid) if _bridge else None
                        if not self._boss_monster_usable(_cur):
                            _should_adopt = True
                    except Exception:
                        pass
                if _should_adopt:
                    self._bb_last_target_uuid = _uuid
                    logger.debug(
                        f'[WebView] Pre-tracked monster target uuid={_uuid} '
                        f'max_hp={_max_hp} hp={_hp} max_ext={_max_ext}'
                    )
        except Exception:
            pass

    def _on_boss_event(self, event):
        """Boss buff event callback from packet_parser → boss raid engine + boss bar effects."""
        if self._boss_raid_engine:
            try:
                self._boss_raid_engine.on_boss_event(event)
            except Exception:
                pass

        # Forward break/shield events to boss HP overlay for visual effects
        try:
            evt_type = event.get('event_type', 0)
            host_uuid = event.get('host_uuid', 0)
            # Match against the monster currently shown on the boss bar
            _target = self._bb_last_target_uuid
            # Also check boss_raid_engine's tracked boss
            if not _target and self._boss_raid_engine:
                _target = getattr(self._boss_raid_engine, '_boss_uuid', 0)
            if not _target or host_uuid != _target:
                return
            # Map BuffEventType → JS triggerBreakEffect type
            _EVT_MAP = {58: 'enter_breaking', 47: 'shield_broken', 51: 'super_armor_broken', 88: 'into_fracture_state'}
            js_type = _EVT_MAP.get(evt_type)
            if js_type:
                self._eval_boss_hp(f'triggerBreakEffect("{js_type}")')
        except Exception:
            pass

    def _on_scene_change(self, scene_event=None):
        """场景服务器切换回调 (切换地图/副本时由 packet_parser 触发)。

        清理:
        - Boss HP bar: 立即隐藏 (旧怪物已不在新场景)
        - Boss bar 目标追踪: 清除 uuid + 时间戳
        - DPS tracker: 结束当前遭遇战并重置
        - Boss raid engine: 如果不在 raid 中则重置
        - HP/Level: 强制重推当前值 (确保 webview 在新场景后及时刷新)
        """
        _scene_kind = ''
        _scene_reason = ''
        _preserve_combat = False
        _reset_on_next_damage = False
        if isinstance(scene_event, dict):
            _scene_kind = str(scene_event.get('kind') or '')
            _scene_reason = str(scene_event.get('reason') or '')
            _preserve_combat = bool(scene_event.get('preserve_combat', False))
            _reset_on_next_damage = bool(scene_event.get('reset_on_next_damage', False))
        if _reset_on_next_damage:
            self._arm_pending_combat_reset(scene_event)
            return
        if _preserve_combat:
            # Same-dungeon layer/map transitions can happen mid-fight. Do not
            # wipe the live encounter; just invalidate the next boss-bar push.
            print(
                f'[SAO] ↔ 同副本软切换({ _scene_kind or "transition" }/{_scene_reason}) '
                f'— 保留 boss bar 和 DPS 追踪',
                flush=True,
            )
            self._last_boss_bar_sig = None
            try:
                if self._dps_tracker:
                    self._dps_tracker.invalidate_snapshot_cache()
            except Exception:
                pass
            return

        print('[SAO] ⚡ 场景切换 — 重置 boss bar 和 DPS 追踪', flush=True)

        # 1. Boss HP bar: 强制隐藏, 清除所有 boss 状态
        self._bb_last_target_uuid = 0
        self._bb_last_damage_ts = 0.0
        self._bb_recent_targets = {}
        self._last_boss_bar_sig = None  # 强制下次更新重新推送
        self._pending_combat_reset_after = 0.0
        self._pending_combat_reset_reason = ''
        try:
            self._eval_boss_hp('updateBossBar({active:false})')
        except Exception:
            pass
        # Reset GameState boss fields to defaults (avoid stale data in fallback path)
        try:
            gs = self._game_state
            if gs:
                gs.boss_breaking_stage = -1
                gs.boss_extinction_pct = 0.0
                gs.boss_current_hp = 0
                gs.boss_total_hp = 0
                gs.boss_hp_source = 'none'
                gs.boss_hp_est_pct = 1.0
                gs.boss_shield_active = False
                gs.boss_shield_pct = 0.0
                gs.boss_in_overdrive = False
                gs.boss_invincible = False
        except Exception:
            pass

        # 2. DPS tracker: 结束当前遭遇战
        if self._dps_tracker:
            try:
                self._dps_tracker.reset()
                print('[SAO] DPS tracker reset on scene change', flush=True)
            except Exception:
                pass

        # 3. Boss raid engine: 仅在非活动时重置
        if self._boss_raid_engine:
            try:
                if getattr(self._boss_raid_engine, '_state', '') != 'running':
                    self._boss_raid_engine.reset()
            except Exception:
                pass

        # 4. Force re-push current level + HP to webview so display doesn't go stale
        try:
            gs = self._game_state
            if gs:
                _lv = getattr(gs, 'level_base', 0) or self._level
                _lv_extra = int(getattr(gs, 'level_extra', 0) or 0)
                _lv_str = f'{_lv}(+{_lv_extra})' if _lv_extra > 0 else str(_lv)
                _hp = int(getattr(gs, 'hp_current', 0) or 0)
                _hp_max = int(getattr(gs, 'hp_max', 0) or 0)
                if _hp_max > 0:
                    self._eval_hp(f'updateHP({_hp}, {_hp_max}, "{_lv_str}")')
        except Exception:
            pass

    def _refresh_boss_raid_upload_auth(self, force: bool = False):
        config = self._load_boss_raid_config()
        identity_state = self._boss_raid_identity_state()
        server_url = str(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL)
        if not identity_state.get('ready'):
            missing = ', '.join(identity_state.get('missing') or [])
            return self._set_boss_raid_upload_auth(
                token='',
                expires_at='',
                error=f'Identity is incomplete: {missing}',
                mode='',
                identity=identity_state,
                server_url=server_url,
            )
        if not force and self._boss_raid_upload_auth_valid(identity_state, config):
            return getattr(self, '_boss_raid_upload_auth', None) or default_upload_auth_state()
        try:
            client = BossRaidCloudClient(server_url)
            result = client.issue_upload_token({
                'player_uid': identity_state.get('player_uid', ''),
                'player_name': identity_state.get('player_name', ''),
                'profession_id': int(identity_state.get('profession_id') or 0),
                'profession_name': identity_state.get('profession_name', ''),
            })
            issued_identity = build_identity_state(
                result.get('identity') or identity_state,
                source=identity_state.get('source') or 'packet',
            )
            return self._set_boss_raid_upload_auth(
                token=str(result.get('token') or '').strip(),
                expires_at=str(result.get('expires_at') or '').strip(),
                error='',
                mode=str(result.get('mode') or '').strip(),
                identity=issued_identity,
                server_url=server_url,
            )
        except Exception as e:
            return self._set_boss_raid_upload_auth(
                token='',
                expires_at='',
                error=str(e),
                mode='',
                identity=identity_state,
                server_url=server_url,
            )

    def _toggle_auto_script(self):
        config = self._load_auto_key_config()
        config['enabled'] = not bool(config.get('enabled', False))
        self._save_auto_key_config(config)
        self._sync_auto_key_menu()
        self._eval_menu(f'SAO.showToast("AUTO KEY: {"ON" if config["enabled"] else "OFF"}")')

    def _reset_burst_tracking(self):
        self._last_ready_slots = {}
        self._burst_seen_cooling = set()
        self._last_watched_signature = ()
        self._last_burst_ready = False
        self._last_burst_slot = 0

    def _save_game_cache(self, quiet: bool = False):
        try:
            if hasattr(self, '_state_mgr') and getattr(self, '_state_mgr', None) and \
               hasattr(self, '_cfg_settings_ref') and getattr(self, '_cfg_settings_ref', None):
                self._persist_cached_identity_state(save_now=False)
                self._state_mgr.save_cache(self._cfg_settings_ref)
                if not quiet:
                    gs = self._state_mgr.state
                    print(
                        f"[SAO] 退出前已保存游戏状态缓存: "
                        f"HP={int(getattr(gs, 'hp_current', 0) or 0)}/"
                        f"{int(getattr(gs, 'hp_max', 0) or 0)}, "
                        f"LV={int(getattr(gs, 'level_base', 0) or 0)}"
                    )
        except Exception:
            pass

    def _reset_sta_offline_state(self):
        self._sta_offline_armed = False
        try:
            self._eval_hp('setSTAOffline(false)')
        except Exception:
            pass

    def _should_show_sta_offline(self, gs) -> bool:
        if gs is None:
            return False
        # STA OFFLINE 仅由 vision 驱动。packet 活动不会抹除该状态，
        # 但也不会因为 vision recognition_ok 闪动到 False 而误报。
        # v2.1.3 修复: 当 vision 抓帧失败时 (onedir 高 DPI 下 PrintWindow
        # 偶发失败), recognition.py 不会更新 stamina_offline (保留旧值),
        # 但同时会把 error_msg 设为 "vision capture failed"。这种情况下
        # 我们不能信任 stamina_offline=True (它可能是上次成功识别留下的
        # 陈旧值), 因此抑制 OFFLINE 信号, 保持 HP 面板可见 — 比起把整个
        # HP/STA 面板隐藏掉, 显示一个 stale 的 STA 数值是更好的体验。
        try:
            err = str(getattr(gs, 'error_msg', '') or '')
            if 'vision capture failed' in err:
                return False
        except Exception:
            pass
        return bool(getattr(gs, 'stamina_offline', False))

    def _persist_cached_identity_state(self, save_now: bool = False):
        settings = getattr(self, '_cfg_settings_ref', None)
        if not settings:
            return
        cache = dict(settings.get('game_cache', {}) or {})
        gs = getattr(self, '_game_state', None)
        # v2.1.18: 实例变量 self._username/_profession/_level 只在菜单刷新或
        # recognition_loop 中显式同步, 切换 UI 时往往是 stale 的. 优先用 GameState
        # 上的实时数据 (gs.player_name/profession_name/level_base), 实例变量作为兜底.
        gs_name = str(getattr(gs, 'player_name', '') or '').strip() if gs is not None else ''
        gs_prof = str(getattr(gs, 'profession_name', '') or '').strip() if gs is not None else ''
        gs_level = int(getattr(gs, 'level_base', 0) or 0) if gs is not None else 0
        name = gs_name or str(getattr(self, '_username', '') or '').strip()
        profession = gs_prof or str(getattr(self, '_profession', '') or '').strip()
        level_base = gs_level if gs_level > 0 else int(getattr(self, '_level', 0) or 0)
        if name and name.lower() != 'player':
            cache['player_name'] = name
        if profession:
            cache['profession_name'] = profession
        if level_base > 0:
            cache['level_base'] = level_base
        level_extra = int(getattr(gs, 'level_extra', 0) or 0) if gs is not None else 0
        season_exp = int(getattr(gs, 'season_exp', 0) or 0) if gs is not None else 0
        uid = str(getattr(gs, 'player_id', '') or '').strip() if gs is not None else ''
        if level_extra > 0:
            cache['level_extra'] = level_extra
        if season_exp > 0:
            cache['season_exp'] = season_exp
        if uid:
            cache['player_id'] = uid
        fight_point = int(getattr(gs, 'fight_point', 0) or 0) if gs is not None else 0
        if fight_point > 0:
            cache['fight_point'] = fight_point
        # v2.1.18: HP/MP 也立即写入, 切到 entity 后能直接显示上次的血量条
        if gs is not None:
            for fld in ('hp_current', 'hp_max', 'hp_pct',
                        'stamina_current', 'stamina_max', 'stamina_pct',
                        'profession_id'):
                v = getattr(gs, fld, None)
                if v is not None and (isinstance(v, (int, float)) and v > 0 or
                                       (isinstance(v, str) and v)):
                    cache[fld] = v
        settings.set('game_cache', cache)
        if save_now:
            try:
                settings.save()
            except Exception:
                pass

    def _sync_identity_alert(self, gs):
        if gs is None:
            return
        try:
            alert_serial = int(getattr(gs, 'identity_alert_serial', 0) or 0)
        except Exception:
            alert_serial = 0
        alert_title = str(getattr(gs, 'identity_alert_title', '') or '')
        alert_message = str(getattr(gs, 'identity_alert_message', '') or '')

        # v2.2.0: 不要让普通 identity 通知打断 hide&seek 持续 alert.
        # 之前 packet_parser 推一次 identity_alert (例如切场景/上线广播)
        # 就会用 kind='identity' 覆盖 hide_seek alert 的 nonce, 9s 后
        # auto-dismiss 把 hide_seek alert 一起拉黑 → 表现为 "过一会就消失".
        # 现在 hide_seek active 时直接吞掉 identity 推送.
        if alert_serial > 0 and alert_serial != getattr(self, '_last_identity_alert_serial', 0):
            self._last_identity_alert_serial = alert_serial
            if getattr(self, '_hide_seek_alert_active', False):
                # 仅记账, 不弹窗, 避免覆盖 hide_seek alert
                pass
            else:
                self._show_identity_alert_window(alert_title, alert_message, 9000, alert_kind='identity')
            return

        # Don't auto-dismiss when Hide & Seek persistent alert is active —
        # the H&S engine manages its own alert lifecycle via the 8s timer.
        # Use the synchronous flag (not engine.running) to avoid race conditions.
        if getattr(self, '_hide_seek_alert_active', False):
            return

        has_identity = bool(
            str(getattr(gs, 'player_name', '') or '').strip()
            and int(getattr(gs, 'level_base', 0) or 0) > 0
        )
        if (has_identity
            and getattr(self, '_identity_alert_visible', False)
            and str(getattr(self, '_identity_alert_kind', '') or '') == 'identity'):
            self._hide_identity_alert_window()

    def _is_dead_state(self, gs) -> bool:
        if gs is None:
            return False
        try:
            hp_max = int(getattr(gs, 'hp_max', 0) or 0)
            hp_current = int(getattr(gs, 'hp_current', 0) or 0)
            hp_pct = float(getattr(gs, 'hp_pct', 1.0) or 0.0)
        except Exception:
            return False
        return hp_max > 0 and hp_current <= 0 and hp_pct <= 0.001

    def _clear_skillfx_state_for_death(self):
        self._last_skillfx_sig = None
        self._reset_burst_tracking()
        try:
            self._state_mgr.update(burst_ready=False, skill_slots=[])
        except Exception:
            pass
        payload = {
            'slots': [],
            'watched_slots': self._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]),
            'burst_enabled': bool(self._get_setting('burst_enabled', True)),
            'burst_slot': 0,
            'burst_ready': False,
            'enabled': bool(self._get_setting('burst_enabled', True)),
        }
        layout = self._get_skillfx_layout(getattr(self, '_game_state', None))
        if layout:
            self._skillfx_layout = layout
            payload['viewport'] = self._viewport_to_css(layout['viewport'])
        try:
            self._eval_skillfx(f'SkillFX.update({json.dumps(payload, ensure_ascii=False)})')
            self._eval_skillfx('SkillFX.hideBurstReady()')
        except Exception:
            pass
        self._last_burst_ready = False

    def _pause_vision_for_death(self):
        engine = getattr(self, '_vision_engine', None)
        if engine is None or getattr(self, '_vision_paused_for_death', False):
            return
        try:
            engine.stop()
        except Exception:
            pass
        self._vision_engine = None
        self._recognition_engines = [
            item for item in (getattr(self, '_recognition_engines', []) or [])
            if item is not engine
        ]
        self._vision_paused_for_death = True
        self._clear_skillfx_state_for_death()
        print('[SAO] Vision engine paused (death)')

    def _resume_vision_after_revive(self):
        if not getattr(self, '_vision_paused_for_death', False):
            return
        if getattr(self, '_vision_engine', None) is not None:
            self._vision_paused_for_death = False
            self._bump_boss_hp_target_hold('revive')
            return
        if not getattr(self, '_cfg_settings_ref', None) or not getattr(self, '_state_mgr', None):
            return
        from recognition import RecognitionEngine
        vision_engine = RecognitionEngine(self._state_mgr, self._cfg_settings_ref)
        vision_engine.start()
        self._vision_engine = vision_engine
        self._recognition_engines.append(vision_engine)
        self._vision_paused_for_death = False
        self._bump_boss_hp_target_hold('revive')
        print('[SAO] Vision engine resumed (revive)')

    def _bump_boss_hp_target_hold(self, reason: str = ''):
        target_uuid = int(getattr(self, '_bb_last_target_uuid', 0) or 0)
        if not target_uuid:
            return
        bridge = getattr(self, '_packet_engine', None)
        if not bridge:
            return
        try:
            monster = bridge.get_monster(target_uuid)
            if not monster:
                return
            if not self._boss_monster_usable(monster):
                return
            now = time.time()
            self._bb_recent_targets[target_uuid] = now
            self._bb_last_damage_ts = now
            self._last_boss_bar_sig = None
        except Exception:
            pass

    def _boss_monster_usable(self, monster) -> bool:
        if not monster:
            return False
        try:
            hp = int(getattr(monster, 'hp', 0) or 0)
            max_hp = int(getattr(monster, 'max_hp', 0) or 0)
            is_dead = bool(getattr(monster, 'is_dead', False))
            if is_dead and hp > 0:
                monster.is_dead = False
                monster.last_update = time.time()
                is_dead = False
            return (not is_dead) and (max_hp > 0 or hp > 0)
        except Exception:
            return False

    def _sync_vision_lifecycle(self, gs):
        dead_now = self._is_dead_state(gs)
        dead_prev = bool(getattr(self, '_last_dead_state', False))
        if dead_now and not dead_prev:
            self._pause_vision_for_death()
        elif (not dead_now) and dead_prev:
            self._resume_vision_after_revive()
            self._bump_boss_hp_target_hold('revive')
        self._last_dead_state = dead_now

    def _pick_burst_trigger_slot(self, gs):
        """Pick the slot index to anchor the Burst Ready visual.

        Stable selection: prefer the slot picked last time as long as it is
        still usable (state in ready/active, or cooldown_pct ≤ 0.02).
        A new ``ready_edge`` always wins — that's the slot whose CD just
        expired, and is the most relevant for the alert.
        """
        watched = self._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]) or []
        try:
            watched = [int(x) for x in watched if int(x) > 0]
        except Exception:
            watched = []
        if not watched:
            watched = [1]

        slots = getattr(gs, 'skill_slots', []) or []
        edge_slot = 0
        first_ready = 0
        first_active = 0
        first_low_cd = 0
        prev_slot = getattr(self, '_last_burst_slot', 0)
        prev_still_ok = False

        for slot in slots:
            if not isinstance(slot, dict):
                continue
            try:
                idx = int(slot.get('index', 0) or 0)
            except Exception:
                continue
            if idx not in watched:
                continue
            state = str(slot.get('state', '') or '').strip().lower()
            try:
                cd = float(slot.get('cooldown_pct', 1.0) or 1.0)
            except Exception:
                cd = 1.0
            is_ready = state in ('ready', 'active') or cd <= 0.02
            if bool(slot.get('ready_edge')) and not edge_slot:
                edge_slot = idx
            if state == 'ready' and not first_ready:
                first_ready = idx
            if state == 'active' and not first_active:
                first_active = idx
            if cd <= 0.02 and not first_low_cd:
                first_low_cd = idx
            if idx == prev_slot and is_ready:
                prev_still_ok = True

        # Priority: ready_edge > sticky previous > first ready > first active > low cd
        if edge_slot:
            chosen = edge_slot
        elif prev_still_ok and prev_slot:
            chosen = prev_slot
        elif first_ready:
            chosen = first_ready
        elif first_active:
            chosen = first_active
        elif first_low_cd:
            chosen = first_low_cd
        else:
            chosen = 0

        self._last_burst_slot = chosen
        return chosen

    def _stop_recognition_engines(self, preserve_packet: bool = False):
        if getattr(self, '_auto_key_engine', None):
            try:
                self._auto_key_engine.stop()
            except Exception:
                pass
            self._auto_key_engine = None
        if getattr(self, '_boss_raid_engine', None):
            try:
                self._boss_raid_engine.stop()
            except Exception:
                pass
            self._boss_raid_engine = None
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
        self._reset_sta_offline_state()
        # Flush DPS player cache to disk before teardown
        if self._dps_tracker:
            try:
                self._dps_tracker.save_player_cache()
            except Exception:
                pass
        self._vision_paused_for_death = False
        self._last_dead_state = False
        if not preserve_packet:
            self._recognition_active = False

    def _reconfigure_data_engines(self, restart_packet: bool = True):
        """Restart packet/vision engines to match the current per-component source map."""
        if not getattr(self, '_cfg_settings_ref', None) or not getattr(self, '_state_mgr', None):
            return

        self._stop_recognition_engines(preserve_packet=not restart_packet)
        try:
            self._state_mgr.update(burst_ready=False)
        except Exception:
            pass
        self._reset_burst_tracking()
        with self._state_mgr._lock:
            self._state_mgr._state.stamina_current = 0
            self._state_mgr._state.stamina_max = 0
            self._state_mgr._state.stamina_pct = 0.0
        self._state_mgr._prev_stamina_current = 0
        self._sta_pixel_detector_enabled = False

        engines = list(getattr(self, '_recognition_engines', []) or [])
        if restart_packet or getattr(self, '_packet_engine', None) is None:
            try:
                from packet_bridge import PacketBridge
                packet_engine = PacketBridge(self._state_mgr, self._cfg_settings_ref,
                                             on_damage=self._on_packet_damage,
                                             on_monster_update=self._on_monster_update,
                                             on_boss_event=self._on_boss_event,
                                             on_scene_change=self._on_scene_change)
                packet_engine.start()
                self._packet_engine = packet_engine
                engines = [engine for engine in engines if engine is not packet_engine]
                engines.insert(0, packet_engine)
                print('[SAO] Packet bridge started (network capture)')
            except Exception as e:
                import traceback
                print(f'[SAO] Packet bridge FAILED to start: {e}', flush=True)
                traceback.print_exc()
                self._packet_engine = None
        elif self._packet_engine not in engines:
            engines.insert(0, self._packet_engine)

        # DPS Tracker
        try:
            self._dps_tracker = DpsTracker()
            # Load skill name mapping
            _skill_json = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                       'assets', 'skill_names.json')
            if os.path.isfile(_skill_json):
                try:
                    with open(_skill_json, 'r', encoding='utf-8') as _sf:
                        _raw = json.load(_sf)
                    if isinstance(_raw, dict):
                        self._dps_tracker.set_skill_names(
                            {int(k): v for k, v in _raw.items() if str(k).isdigit()}
                        )
                        print(f'[SAO] Loaded {len(_raw)} skill names')
                except Exception as _se:
                    print(f'[SAO] Failed to load skill_names.json: {_se}')
            print('[SAO] DPS tracker initialized')
        except Exception as e:
            print(f'[SAO] DPS tracker init failed: {e}')
            self._dps_tracker = None

        try:
            from recognition import RecognitionEngine
            vision_engine = RecognitionEngine(self._state_mgr, self._cfg_settings_ref)
            vision_engine.start()
            engines.append(vision_engine)
            self._vision_engine = vision_engine
            self._vision_paused_for_death = False
            self._last_dead_state = False
            print('[SAO] Recognition engine started (window vision / printwindow)')
        except Exception as e:
            import traceback
            print(f'[SAO] Recognition engine FAILED to start: {e}', flush=True)
            traceback.print_exc()
            self._vision_engine = None

        self._recognition_engines = engines
        self._recognition_engine = getattr(self, '_packet_engine', None) or (engines[0] if engines else None)
        self._recognition_active = bool(getattr(self, '_packet_engine', None) or getattr(self, '_vision_engine', None))

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
        skillfx_url = _web_file_uri('skillfx.html')
        alert_url = _web_file_uri('alert.html')

        # HP 固定位置: 跟随游戏窗口所在显示器, 避免 webview 在高 DPI /
        # 多显示器环境下按系统 DPI 定位而产生几何漂移。
        game_hwnd, game_rect = self._get_game_window_context()
        monitor_left, monitor_top, monitor_right, monitor_bottom = self._get_monitor_rect_for_target(
            hwnd=game_hwnd, rect=game_rect
        )
        self._hud_monitor_rect = (monitor_left, monitor_top, monitor_right, monitor_bottom)
        _sw = max(1, monitor_right - monitor_left)
        _sh = max(1, monitor_bottom - monitor_top)
        _dpi_scale = self._refresh_webview_dpi_scale(hwnd=game_hwnd, rect=game_rect)

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

        self.skillfx_win = webview.create_window(
            'SAO SkillFX', skillfx_url,
            width=self._to_webview_px(max(320, int(_sw * 0.42))),
            height=self._to_webview_px(max(140, int(_sh * 0.20))),
            x=self._to_webview_px(monitor_left + max(0, int(_sw * 0.29))),
            y=self._to_webview_px(monitor_top + max(0, int(_sh * 0.74))),
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
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

        # Boss HP overlay — covers native boss bar
        # Reference 1080p: bar 466×61 at (740, 15)-(1206, 76), anchor (1206, 76)
        boss_hp_url = _web_file_uri('boss_hp.html')
        _bhp_geom = self._calc_boss_hp_geometry()
        self._boss_hp_geometry = dict(_bhp_geom)
        self.boss_hp_win = webview.create_window(
            'SAO-BossHP', boss_hp_url,
            width=int(_bhp_geom['width']), height=int(_bhp_geom['height']),
            x=int(_bhp_geom['x']), y=int(_bhp_geom['y']),
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._api,
        )

        # DPS meter — right side, vertically centered
        dps_url = _web_file_uri('dps.html')
        _dps_w = max(320, int(min(_sw, 1920) * 0.19))
        _dps_h = max(420, int(min(_sh, 1080) * 0.48))
        self._dps_base_w = int(_dps_w)
        self._dps_base_h = int(_dps_h)
        _detail_w_default = max(760, int(min(_sw, 1920) * 0.40))
        _detail_h_default = max(560, int(min(_sh, 1080) * 0.56))
        try:
            self._dps_detail_w = int(self._get_setting('dps_detail_w', _detail_w_default))
            self._dps_detail_h = int(self._get_setting('dps_detail_h', _detail_h_default))
        except Exception:
            self._dps_detail_w = _detail_w_default
            self._dps_detail_h = _detail_h_default
        self._dps_detail_w = max(520, min(1180, int(self._dps_detail_w)))
        self._dps_detail_h = max(420, min(900, int(self._dps_detail_h)))
        _dps_x = max(0, _sw - _dps_w - max(16, int(_sw * 0.012)))
        _dps_y = max(0, int(_sh * 0.18))
        self._dps_api = DpsWindowAPI(self)
        self.dps_win = webview.create_window(
            'SAO-DPS', dps_url,
            width=_dps_w, height=_dps_h,
            x=_dps_x, y=_dps_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._dps_api,
        )

        # Raid Editor overlay — left side, same height as DPS
        raid_editor_url = _web_file_uri('raid_editor.html')
        _re_w = max(360, int(min(_sw, 1920) * 0.22))
        _re_h = max(460, int(min(_sh, 1080) * 0.52))
        _re_x = max(16, int(_sw * 0.012))
        _re_y = max(0, int(_sh * 0.18))
        self._raid_editor_api = RaidEditorAPI(self)
        self.raid_editor_win = webview.create_window(
            'SAO-RaidEditor', raid_editor_url,
            width=_re_w, height=_re_h,
            x=_re_x, y=_re_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._raid_editor_api,
        )

        # AutoKey Editor overlay — left side, below raid editor
        autokey_editor_url = _web_file_uri('autokey_editor.html')
        _ak_w = max(340, int(min(_sw, 1920) * 0.20))
        _ak_h = max(400, int(min(_sh, 1080) * 0.44))
        _ak_x = max(16, int(_sw * 0.012))
        _ak_y = _re_y + _re_h + 12
        self._autokey_editor_api = AutoKeyEditorAPI(self)
        self.autokey_editor_win = webview.create_window(
            'SAO-AutoKeyEditor', autokey_editor_url,
            width=_ak_w, height=_ak_h,
            x=_ak_x, y=_ak_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._autokey_editor_api,
        )

        # Commander panel — center-left, above raid editor
        commander_url = _web_file_uri('commander.html')
        _cmd_w = max(300, int(min(_sw, 1920) * 0.18))
        _cmd_h = max(380, int(min(_sh, 1080) * 0.42))
        _cmd_x = max(16, int(_sw * 0.25))
        _cmd_y = max(0, int(_sh * 0.15))
        self._commander_api = CommanderAPI(self)
        self.commander_win = webview.create_window(
            'SAO-Commander', commander_url,
            width=_cmd_w, height=_cmd_h,
            x=_cmd_x, y=_cmd_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=self._commander_api,
        )

        webview.start(self._on_webview_started, debug=False)

        # ── Phase 3: 热切换 ──
        if self._pending_switch:
            self._do_hot_switch(self._pending_switch)

    # ─── HUD 位置自动检测 ───
    def _get_game_window_context(self):
        rect = None
        try:
            gs = self._state_mgr.state if getattr(self, '_state_mgr', None) is not None else None
            window_rect = getattr(gs, 'window_rect', None) if gs else None
            if isinstance(window_rect, (list, tuple)) and len(window_rect) == 4:
                rect = tuple(int(v) for v in window_rect)
        except Exception:
            rect = None

        try:
            from window_locator import WindowLocator
            locator = getattr(self, '_locator', None)
            if locator is None:
                locator = WindowLocator()
                self._locator = locator
            result = locator.find_game_window()
            if result:
                hwnd, _title, found_rect = result
                return int(hwnd or 0), tuple(int(v) for v in found_rect)
        except Exception:
            pass

        return 0, rect

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

            ls_root = tk.Tk()
            ls_root.withdraw()

            done = threading.Event()

            try:
                from gpu_overlay_window import (
                    suspend_gpu_overlay_creation as _suspend_gpu_overlays,
                    resume_gpu_overlay_creation as _resume_gpu_overlays,
                )
            except Exception:
                _suspend_gpu_overlays = None  # type: ignore[assignment]
                _resume_gpu_overlays = None  # type: ignore[assignment]
            _gpu_overlays_suspended = False
            if _suspend_gpu_overlays is not None:
                try:
                    _suspend_gpu_overlays()
                    _gpu_overlays_suspended = True
                except Exception:
                    _gpu_overlays_suspended = False

            def _resume_overlay_creation():
                nonlocal _gpu_overlays_suspended
                if not _gpu_overlays_suspended or _resume_gpu_overlays is None:
                    return
                _gpu_overlays_suspended = False
                try:
                    _resume_gpu_overlays()
                except Exception:
                    pass

            def on_done():
                done.set()
                _resume_overlay_creation()
                try:
                    ls_root.after(50, ls_root.destroy)
                except Exception:
                    pass

            try:
                ls = SAOLinkStart(ls_root, on_done=on_done)
                ls.play()
            except Exception:
                _resume_overlay_creation()
                raise
            ls_root.mainloop()
        except Exception as e:
            print(f"[SAO] LinkStart skipped: {e}")

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
        _apply_for('SAO SkillFX', self.skillfx_win)
        _apply_for('SAO Alert', self.alert_win)
        _apply_for('SAO-BossHP', self.boss_hp_win)
        # DPS 窗口只做 Win32 色键, 不设 .NET TransparencyKey
        # (TransparencyKey 会令 HTML 透明区域变成鼠标穿透, 导致按钮/行无法点击)
        try:
            dps_hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-DPS')
            if dps_hwnd:
                _make_transparent_ctypes(dps_hwnd)
        except Exception:
            pass
        # 菜单窗口只做 Win32 色键, 不设 .NET TransparencyKey
        # (TransparencyKey 会令菜单 HTML 透明区域变成鼠标穿透, 导致按钮无法点击)
        try:
            menu_hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO Menu')
            if menu_hwnd:
                _make_transparent_ctypes(menu_hwnd)
        except Exception:
            pass
        # Commander: only Win32 (needs to be clickable when visible; .NET TransparencyKey interferes)
        try:
            cmd_hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-Commander')
            if cmd_hwnd:
                _make_transparent_ctypes(cmd_hwnd)
        except Exception:
            pass

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
                    try:
                        self._ensure_dps_clickable()
                    except Exception:
                        pass
                    # 确保隐藏面板保持 WS_EX_TRANSPARENT (防止隐藏窗口意外拦截点击)
                    try:
                        self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                    # 确保纯覆盖层窗口始终保持 WS_EX_TRANSPARENT
                    # (show() 或其他 WinForms 操作可能重置 exstyle)
                    try:
                        self._setup_skillfx_click_through()
                    except Exception:
                        pass
                    try:
                        self._setup_boss_hp_click_through(_wait_retries=0)
                    except Exception:
                        pass
                    try:
                        # Alert 只在未显示时设穿透, 避免干扰活动弹窗的按钮点击
                        if not getattr(self, '_identity_alert_visible', False):
                            self._setup_alert_click_through()
                    except Exception:
                        pass

        threading.Thread(target=_loop, daemon=True, name='hp_position_guard').start()

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

        threading.Thread(target=_loop, daemon=True, name='hp_click_bootstrap').start()

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

    def _ensure_dps_clickable(self):
        """安全检查: 确保 DPS 窗口未被意外设为鼠标穿透.

        与 _ensure_hp_clickable 相同逻辑: 移除 WS_EX_TRANSPARENT, 保留 WS_EX_LAYERED.
        DPS 面板需要接收点击 (行点击 / 拖拽 / Reset 按钮), 鼠标穿透由 LWA_COLORKEY 处理.
        """
        try:
            dps_hwnd = getattr(self, '_dps_hwnd', 0)
            if not dps_hwnd:
                dps_hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-DPS')
                if dps_hwnd:
                    self._dps_hwnd = dps_hwnd
            if not dps_hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(dps_hwnd, _GWL_EXSTYLE)
            if ex & _WS_EX_TRANSPARENT:
                user32.SetWindowLongW(
                    dps_hwnd, _GWL_EXSTYLE,
                    (ex & ~_WS_EX_TRANSPARENT) | _WS_EX_LAYERED)
        except Exception:
            pass

    def _make_dps_unclickable(self):
        """隐藏 DPS 面板时设为鼠标穿透, 防止隐藏状态下的误操作.

        设置 WS_EX_TRANSPARENT | WS_EX_LAYERED, 与 _setup_boss_hp_click_through 逻辑相同.
        重新显示时由 _ensure_dps_clickable 还原.
        """
        try:
            dps_hwnd = getattr(self, '_dps_hwnd', 0)
            if not dps_hwnd:
                dps_hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-DPS')
                if dps_hwnd:
                    self._dps_hwnd = dps_hwnd
            if not dps_hwnd:
                return
            user32 = ctypes.windll.user32
            ex = user32.GetWindowLongW(dps_hwnd, _GWL_EXSTYLE)
            user32.SetWindowLongW(
                dps_hwnd, _GWL_EXSTYLE,
                ex | _WS_EX_TRANSPARENT | _WS_EX_LAYERED)
        except Exception:
            pass

    def _ensure_hidden_panels_passthrough(self):
        """确保所有当前隐藏的面板窗口保持 WS_EX_TRANSPARENT, 防止意外拦截点击.

        在 position guard 中周期性调用.
        """
        _panels = [
            ('SAO-DPS', '_dps_visible', '_dps_hwnd'),
            ('SAO-RaidEditor', '_raid_editor_visible', None),
            ('SAO-AutoKeyEditor', '_autokey_editor_visible', None),
            ('SAO-Commander', '_commander_visible', None),
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
            # Cache hwnd if it's a known window
            if title == 'SAO-BossHP':
                self._boss_hp_hwnd = hwnd
            elif title == 'SAO-DPS':
                self._dps_hwnd = hwnd
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

        threading.Thread(target=_loop, daemon=True, name='hp_mouse_passthrough').start()

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
        self._ensure_skillfx_on_top()

    def _force_hp_to_bottom(self, force: bool = False, quiet: bool = False):
        """用 GetWindowRect + SetWindowPos 强制 HP 窗口贴屏幕底部 (物理像素)。"""
        if not self._hp_hwnd:
            return
        if not force and self._is_hp_position_locked():
            return
        try:
            user32 = ctypes.windll.user32
            game_hwnd, game_rect = self._get_game_window_context()
            target_hwnd = self._hp_hwnd or game_hwnd
            monitor_left, monitor_top, monitor_right, monitor_bottom = self._get_monitor_rect_for_target(
                hwnd=target_hwnd, rect=game_rect
            )
            self._hud_monitor_rect = (monitor_left, monitor_top, monitor_right, monitor_bottom)
            self._refresh_webview_dpi_scale(hwnd=target_hwnd, rect=game_rect)
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
            self._set_window_alpha('SAO SkillFX', 0.0)
            try:
                if self.hp_win and not self._hp_visible:
                    self.hp_win.show()
                    self._hp_visible = True
            except Exception:
                pass
            try:
                if self.skillfx_win and not self._skillfx_visible:
                    self.skillfx_win.show()
                    self._skillfx_visible = True
            except Exception:
                pass
            # SkillFX 是纯覆盖层, 必须立即设为鼠标穿透 (WS_EX_TRANSPARENT)
            # 否则 show() 到 _update_skillfx_layout() 之间窗口会拦截点击
            self._setup_skillfx_click_through()
            threading.Timer(0.5, self._setup_skillfx_click_through).start()
            time.sleep(0.18)
            self._eval_hp(f'setUsername("{self._safe_js(self._username)}")')
            # 初始显示等级 (来自 profile 缓存, 等待抓包数据覆盖)
            self._eval_hp(f'updateHP(0, 1, {self._level})')
            self._sync_menu_info()
            # 设置 click-through (延迟确保窗口已完全创建)
            time.sleep(0.3)
            self._setup_click_through()
            self._update_skillfx_layout()
            self._request_hp_hit_regions()
            self._start_hp_position_guard()
            self._start_hp_click_bootstrap(8.0)
            # WebView2 透明背景 — 持续重试
            self._apply_webview2_transparency()
            # 标记 reveal 未完成, 阻止 _reassert_hp_transparency 修改 alpha
            self._hp_reveal_pending = True
            self._reassert_hp_transparency(1.0, retries=15, delay=0.35)
            # HP/SkillFX 启动时 alpha=0, 0.8s 后淡入 (等待透明应用后再变可见)
            def _reveal_windows():
                self._hp_reveal_pending = False
                self._set_window_alpha('SAO-HP', 1.0)
                self._set_window_alpha('SAO SkillFX', 1.0)
                self._setup_skillfx_click_through()
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
                    # SkillFX 也需要重新设置穿透 (冷启动可能延迟)
                    self._setup_skillfx_click_through()
                except Exception:
                    pass
            threading.Timer(12.0, _late_hp_recovery).start()
            threading.Timer(20.0, _late_hp_recovery).start()
            # v2.1.17: onedir 冷启动 BossHP / DPS 面板恢复. 第一次 PyInstaller
            # 解包 + WebView2 初始化可能比 _init 内部的同步 show() 慢, 导致
            # boss_hp_win.show() / dps_win 准备工作生效前窗口仍处于不可见状态.
            # 在 4s/10s/16s 处补做一遍, 不会重复 show 已可见窗口.
            def _late_panel_recovery():
                try:
                    if self.boss_hp_win and not self._boss_hp_visible:
                        self._setup_boss_hp_click_through()
                        self._set_window_alpha('SAO-BossHP', 1.0)
                        try:
                            self.boss_hp_win.show()
                            self._boss_hp_visible = True
                            self._setup_boss_hp_click_through()
                        except Exception:
                            pass
                    if self.dps_win:
                        self._wait_and_apply_click_through('SAO-DPS', timeout=0.5)
                        self._make_dps_unclickable()
                except Exception:
                    pass
            threading.Timer(4.0, _late_panel_recovery).start()
            threading.Timer(10.0, _late_panel_recovery).start()
            threading.Timer(16.0, _late_panel_recovery).start()
            # 任务栏图标
            self._set_window_icon('SAO-HP')
            self._set_window_icon('SAO Menu')
            self._set_window_icon('SAO SkillFX')
            self._set_window_icon('SAO Alert')
            self._set_window_icon('SAO-BossHP')
            self._set_window_icon('SAO-DPS')
            self._set_window_icon('SAO-RaidEditor')
            self._set_window_icon('SAO-AutoKeyEditor')
            self._set_window_icon('SAO-Commander')
            # 菜单窗口在启动阶段保持完全透明, 避免偶发白色方框闪现
            self._set_window_alpha('SAO Menu', 0.0)
            self._set_window_alpha('SAO Alert', 1.0)
            # Alert window: default click-through so the hidden window never captures clicks
            self._setup_alert_click_through()
            # Boss HP overlay: transparency + click-through + show (hidden by default)
            # Ensure click-through is applied BEFORE the window becomes visible.
            self._setup_boss_hp_click_through()
            self._wait_and_apply_click_through('SAO-BossHP', timeout=2.0)
            self._set_window_alpha('SAO-BossHP', 1.0)
            try:
                if self.boss_hp_win:
                    self.boss_hp_win.show()
                    self._boss_hp_visible = True
                    # show() 后再确认一次穿透 (防止 show 重置 exstyle)
                    self._setup_boss_hp_click_through()
                    # Sync FX overflow margins so CSS padding matches the enlarged window
                    _g = self._boss_hp_geometry
                    _fx_lr = _g.get('fx_lr', 200)
                    _fx_top = _g.get('fx_top', 120)
                    _fx_bot = _g.get('fx_bot', 160)
                    multi_h = _g.get('multi_extra_h', 240)
                    self._eval_boss_hp(
                        f'if(window.BossHP)BossHP.setFxMargins({_fx_top},{_fx_lr},{_fx_bot},{_fx_lr},{multi_h})'
                    )
            except Exception:
                pass
            # DPS meter: hidden until combat starts or a report is opened
            try:
                self._dps_visible = False
                self._dps_mode = 'hidden'
                if self.dps_win:
                    self._apply_webview2_transparency()
                    self._set_window_alpha('SAO-DPS', 1.0)
                # 启动时 DPS 面板处于隐藏状态, 设为鼠标穿透避免点击穿透到游戏窗口
                # 确保 hwnd 已就绪再设穿透, 避免首次 FindWindow 失败
                # (DPS 本身是隐藏的, 用短超时避免阻塞初始化)
                self._wait_and_apply_click_through('SAO-DPS', timeout=0.5)
                self._make_dps_unclickable()
                self._sync_dps_report_availability()
            except Exception:
                pass
            # Raid Editor / AutoKey Editor / Commander: 初始隐藏, 设为鼠标穿透
            try:
                self._wait_and_apply_click_through('SAO-RaidEditor', timeout=0.5)
                self._wait_and_apply_click_through('SAO-AutoKeyEditor', timeout=0.5)
                self._wait_and_apply_click_through('SAO-Commander', timeout=0.5)
            except Exception:
                pass
            # Commander panel must start hidden (explicitly enforce after webview init)
            try:
                self._commander_visible = False
                if self.commander_win:
                    self._set_window_alpha('SAO-Commander', 0.0)
                    self.commander_win.hide()
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
            try:
                self._update_skillfx_layout()
            except Exception:
                pass

            # Restore sound settings
            try:
                from sao_sound import set_sound_enabled, set_sound_volume
                _snd_on = cfg_settings.get('sound_enabled', True)
                _snd_vol = cfg_settings.get('sound_volume', 70)
                set_sound_enabled(bool(_snd_on) if _snd_on is not None else True)
                set_sound_volume(int(_snd_vol) if _snd_vol is not None else 70)
            except Exception:
                pass

            # 用缓存名替换默认 "Player"
            cached_name = self._state_mgr.state.player_name
            if cached_name:
                self._username = cached_name
                self._eval_hp(f'setUsername("{self._safe_js(cached_name)}")')
                print(f'[SAO] 从缓存加载角色名: {cached_name}')
            cached_lv = self._state_mgr.state.level_base
            cached_lv_extra = self._state_mgr.state.level_extra
            if cached_lv > 0:
                self._level = cached_lv
                cached_hp = max(0, int(self._state_mgr.state.hp_current or 0))
                cached_hp_max = max(1, int(self._state_mgr.state.hp_max or 1))
                if cached_lv_extra > 0:
                    cached_level_str = f'{cached_lv}(+{cached_lv_extra})'
                else:
                    cached_level_str = str(cached_lv)
                self._eval_hp(f'updateHP({cached_hp}, {cached_hp_max}, "{cached_level_str}")')
            # 同步缓存的职业/UID到 id-plate
            cached_prof = self._state_mgr.state.profession_name
            cached_uid = self._state_mgr.state.player_id
            if cached_prof or cached_uid:
                import json as _j
                info = {}
                if cached_prof:
                    info['profession'] = cached_prof
                if cached_uid:
                    info['uid'] = cached_uid
                self._eval_hp(f'setPlayerInfo({_j.dumps(info, ensure_ascii=False)})')
                print(f'[SAO] 从缓存加载: 职业={cached_prof}, UID={cached_uid}')

            self._reconfigure_data_engines(restart_packet=not bool(getattr(self, '_packet_engine', None)))
            self._auto_key_engine = AutoKeyEngine(
                self._state_mgr,
                self._cfg_settings_ref,
                extra_gate=lambda: bool(getattr(self, '_recognition_active', False)),
            )
            self._auto_key_engine.start()
            self._sync_auto_key_menu()

            # Boss Raid Engine + Autokey Linkage
            self._boss_autokey_linkage = BossAutoKeyLinkage(
                self._cfg_settings_ref,
                send_key=self._send_linked_key,
                on_log=lambda msg: print(msg),
            )

            def _on_boss_alert_with_linkage(title, message):
                # v2.1.17: BossRaidEngine separately calls on_sound("boss_alert")
                # which maps to the same Popup.SAO.Alert.mp3 file as 'alert'.
                # Suppress the alert window's built-in sound to avoid playing
                # the same clip twice.
                self._show_identity_alert_window(
                    title, message, play_sound=False, alert_kind='boss_raid')
                if self._boss_autokey_linkage:
                    try:
                        self._boss_autokey_linkage.on_boss_raid_alert(title, message)
                    except Exception:
                        pass

            self._boss_raid_engine = BossRaidEngine(
                self._state_mgr,
                self._cfg_settings_ref,
                on_alert=_on_boss_alert_with_linkage,
                on_sound=self._play_sound,
                on_entity_update=self._on_raid_entity_update,
            )
            self._sync_boss_raid_menu()

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
                self._reset_sta_offline_state()
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
            'toggle_auto_script': self._toggle_auto_script,
            'toggle_topmost': lambda: None,
            'hide_panels': lambda: None,
            'boss_raid_start': self._toggle_boss_raid,
            'boss_raid_next_phase': self._boss_raid_next_phase,
            'toggle_hide_seek': self._toggle_hide_seek,
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
            print('[SAO WebView] Hotkeys (pynput): F5=toggle_recognition, F6=toggle_auto_script')
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

    def _hk_check(self):
        # Merge saved hotkeys with defaults so new keys (like toggle_hide_seek)
        # are always available even if settings.json doesn't contain them.
        saved = getattr(self, '_cfg_settings_ref', None)
        user_hotkeys = {} if saved is None else (saved.get('hotkeys') or {})
        hotkeys = {**DEFAULT_HOTKEYS, **user_hotkeys}
        for action, info in hotkeys.items():
            vk = None
            if isinstance(info, dict):
                vk = info.get('vk')
            elif isinstance(info, str) and info:
                vk = self._FKEY_VK.get(info.upper())
            if vk and vk in self._hk_pressed:
                cb = self._hk_actions.get(action)
                if cb:
                    threading.Thread(target=cb, daemon=True).start()
                    self._hk_pressed.clear()
                    return

    def _hk_poll_tick(self):
        """Poll GetAsyncKeyState for F-key presses (called from recognition loop).

        This is a fallback for when pynput's keyboard hook fails to receive events
        (common with DirectInput games). Detects rising edges only.
        """
        if not getattr(self, '_hk_poll_ok', False):
            return
        try:
            # Merge saved hotkeys with defaults
            saved = getattr(self, '_cfg_settings_ref', None)
            user_hotkeys = {} if saved is None else (saved.get('hotkeys') or {})
            hotkeys = {**DEFAULT_HOTKEYS, **user_hotkeys}
            for action, info in hotkeys.items():
                vk = None
                if isinstance(info, dict):
                    vk = info.get('vk')
                elif isinstance(info, str) and info:
                    vk = self._FKEY_VK.get(info.upper())
                if not vk:
                    continue
                # GetAsyncKeyState returns short; bit 15 = currently pressed
                state = self._hk_GetAsyncKeyState(vk)
                is_pressed = bool(state & 0x8000)
                was_pressed = self._hk_poll_prev.get(vk, False)
                self._hk_poll_prev[vk] = is_pressed
                if is_pressed and not was_pressed:
                    cb = self._hk_actions.get(action)
                    if cb:
                        threading.Thread(target=cb, daemon=True).start()
                        return
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

    def _eval_skillfx(self, js):
        try:
            if self.skillfx_win:
                self.skillfx_win.evaluate_js(js)
        except Exception:
            pass

    def _eval_alert(self, js):
        try:
            if self.alert_win:
                self.alert_win.evaluate_js(js)
        except Exception:
            pass

    def _eval_boss_hp(self, js):
        try:
            if self.boss_hp_win:
                self.boss_hp_win.evaluate_js(js)
        except Exception:
            pass

    def _calc_boss_hp_geometry(self):
        try:
            sw = ctypes.windll.user32.GetSystemMetrics(0)
            sh = ctypes.windll.user32.GetSystemMetrics(1)
        except Exception:
            sw, sh = 1920, 1080
        pad = max(8, int(min(sw, sh) * 0.008))
        bar_w = max(556, int(sw * 0.28960))
        bar_h = max(88, int(sh * 0.08150))
        right = int(sw * 0.62813)
        # Extra margin so CSS VFX (shards, bloom, burst) aren't clipped.
        # Break shards can fly ~150 px in any direction; add generous room.
        fx_lr = max(160, int(min(sw, 1920) * 0.088))
        fx_top = max(100, int(min(sh, 1080) * 0.096))
        fx_bot = max(160, int(min(sh, 1080) * 0.150))
        # Bar's intended screen position (must NOT change when FX margins grow).
        # At 1080p the bar renders at x=642, y=24 — derived from the original
        # small-margin code where max(0, 4-24)+24 = 24.
        bar_screen_x = right - bar_w - pad
        bar_screen_y = max(0, int(sh * 0.0046) - 24) + 24  # preserve legacy offset
        # Extra height for multi-unit support (additional attacked units below main bar)
        multi_extra_h = 240  # room for ~3 additional small bars
        return {
            'width':  bar_w + pad * 2 + 20 + fx_lr * 2,
            'height': bar_h + pad * 2 + fx_top + fx_bot + multi_extra_h,
            'x': bar_screen_x - fx_lr,
            'y': bar_screen_y - fx_top,
            'fx_lr':  fx_lr,
            'fx_top': fx_top,
            'fx_bot': fx_bot,
            'multi_extra_h': multi_extra_h,
        }

    def _refresh_boss_hp_geometry(self, force: bool = False):
        geom = self._calc_boss_hp_geometry()
        prev = getattr(self, '_boss_hp_geometry', None)
        if (not force) and prev == geom:
            return
        self._boss_hp_geometry = geom
        try:
            if self.boss_hp_win:
                self.boss_hp_win.resize(int(geom['width']), int(geom['height']))
                self.boss_hp_win.move(int(geom['x']), int(geom['y']))
                _fx_lr = geom.get('fx_lr', 200)
                _fx_top = geom.get('fx_top', 120)
                _fx_bot = geom.get('fx_bot', 160)
                multi_h = geom.get('multi_extra_h', 240)
                self._eval_boss_hp(
                    f'if(window.BossHP)BossHP.setFxMargins({_fx_top},{_fx_lr},{_fx_bot},{_fx_lr},{multi_h})'
                )
                # Re-apply click-through after resize/move (container flags can reset)
                self._setup_boss_hp_click_through()
        except Exception:
            pass

    def _eval_dps(self, js):
        try:
            if self.dps_win:
                self.dps_win.evaluate_js(js)
        except Exception:
            pass

    def _resize_dps_window(self, width: int, height: int,
                           persist: bool = False):
        try:
            width = max(520, min(1180, int(width)))
            height = max(420, min(900, int(height)))
        except Exception:
            return
        self._dps_detail_w = width
        self._dps_detail_h = height
        try:
            if self.dps_win:
                self.dps_win.resize(width, height)
        except Exception:
            pass
        if persist:
            try:
                self._set_setting('dps_detail_w', width)
                self._set_setting('dps_detail_h', height)
            except Exception:
                pass

    def _set_dps_detail_mode(self, active: bool):
        self._dps_detail_mode = bool(active)
        try:
            if self._dps_detail_mode:
                self._resize_dps_window(
                    int(getattr(self, '_dps_detail_w', 760) or 760),
                    int(getattr(self, '_dps_detail_h', 560) or 560),
                    persist=False,
                )
            else:
                w = int(getattr(self, '_dps_base_w', 0) or 360)
                h = int(getattr(self, '_dps_base_h', 0) or 520)
                if self.dps_win:
                    self.dps_win.resize(w, h)
        except Exception:
            pass
        try:
            if self._dps_visible:
                self._ensure_dps_clickable()
        except Exception:
            pass

    def _combat_damage_timeout_s(self) -> float:
        # User-configurable fade timeout (seconds). Default large enough that
        # normal combat lulls (cast animations, mechanic phases, target swaps)
        # don't cause the DPS panel to disappear every few seconds.
        try:
            raw = self._get_setting('dps_fade_timeout_s', 60)
            v = float(raw if raw is not None else 60)
        except Exception:
            v = 60.0
        if v <= 0:
            return 86400.0
        return float(max(60.0, v))

    def _boss_hp_hold_timeout_s(self) -> float:
        """BossHP gets a longer leash than DPS so revive/mechanics don't hide it."""
        try:
            raw = self._get_setting('boss_hp_hold_timeout_s', 300)
            v = float(raw if raw is not None else 300)
        except Exception:
            v = 300.0
        if v <= 0:
            return 86400.0
        return float(max(180.0, v, self._combat_damage_timeout_s()))

    def _get_dps_last_report_available(self) -> bool:
        tracker = getattr(self, '_dps_tracker', None)
        if not tracker:
            return False
        try:
            return bool(tracker.has_last_report())
        except Exception:
            return False

    def _sync_dps_report_availability(self):
        available = self._get_dps_last_report_available()
        if available == getattr(self, '_dps_last_report_available', False):
            return
        self._dps_last_report_available = available
        self._sync_menu_settings()

    def _show_dps_window(self):
        try:
            if self.dps_win and not self._dps_visible:
                self._dps_fade_seq += 1
                self._apply_webview2_transparency()
                self._set_window_alpha('SAO-DPS', 0.0)
                self.dps_win.show()
                self._eval_dps('if (window.DpsMeter && DpsMeter.fadeIn) DpsMeter.fadeIn()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-DPS', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._dps_visible = True
                self._ensure_dps_clickable()
                threading.Timer(0.5, self._ensure_dps_clickable).start()
                threading.Timer(1.5, self._ensure_dps_clickable).start()
        except Exception:
            pass

    def _hide_dps_window(self):
        self._dps_fade_seq += 1
        _fade_seq = self._dps_fade_seq
        # 立即设为鼠标穿透, 防止面板淡出期间及隐藏状态下接收误操作
        self._make_dps_unclickable()
        self._dps_detail_mode = False
        try:
            if self.dps_win and self._dps_visible:
                self._eval_dps('if (window.DpsMeter && DpsMeter.setDetailMode) DpsMeter.setDetailMode(false)')
                try:
                    self.dps_win.resize(
                        int(getattr(self, '_dps_base_w', 360) or 360),
                        int(getattr(self, '_dps_base_h', 520) or 520),
                    )
                except Exception:
                    pass
                self._eval_dps('if (window.DpsMeter && DpsMeter.fadeOut) DpsMeter.fadeOut()')
                self._set_window_alpha('SAO-DPS', 1.0)
                threading.Timer(
                    0.26,
                    lambda: self._finish_hide_dps_window(_fade_seq),
                ).start()
        except Exception:
            pass
        self._dps_visible = False
        self._dps_faded = False
        self._dps_mode = 'hidden'

    def _finish_hide_dps_window(self, fade_seq: int):
        try:
            if fade_seq != self._dps_fade_seq or self._dps_visible:
                return
            if self.dps_win:
                self.dps_win.hide()
        except Exception:
            pass

    def _show_dps_live_snapshot(self, snapshot=None):
        tracker = getattr(self, '_dps_tracker', None)
        if snapshot is None and tracker:
            try:
                snapshot = tracker.get_snapshot()
            except Exception:
                snapshot = None
        if snapshot is None:
            snapshot = {
                'encounter_active': False,
                'elapsed_s': 0.0,
                'total_damage': 0,
                'total_heal': 0,
                'total_dps': 0,
                'total_hps': 0,
                'entities': [],
            }
        self._show_dps_window()
        self._dps_mode = 'live'
        self._eval_dps(f'DpsMeter.showLive({json.dumps(snapshot, ensure_ascii=False)})')

    def _show_dps_last_report(self, report=None) -> bool:
        tracker = getattr(self, '_dps_tracker', None)
        if report is None and tracker:
            try:
                report = tracker.get_last_report()
            except Exception:
                report = None
        if not report:
            return False
        self._show_dps_window()
        self._dps_mode = 'report'
        self._eval_dps(f'DpsMeter.showLastReport({json.dumps(report, ensure_ascii=False)})')
        return True

    @staticmethod
    def _safe_js(s: str) -> str:
        if not s:
            return ''
        return s.replace('\\', '\\\\').replace('"', '\\"').replace("'", "\\'").replace('\n', '\\n')

    # ── Raid Editor overlay ──

    def _eval_raid_editor(self, js):
        try:
            if self.raid_editor_win:
                self.raid_editor_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_raid_editor_clickable(self):
        """Remove WS_EX_TRANSPARENT so overlay receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-RaidEditor')
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

    def _show_raid_editor(self):
        try:
            if self.raid_editor_win and not self._raid_editor_visible:
                self._apply_webview2_transparency()
                self._set_window_alpha('SAO-RaidEditor', 0.0)
                self.raid_editor_win.show()
                self._eval_raid_editor('if(window.RaidEditor&&RaidEditor.fadeIn)RaidEditor.fadeIn()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-RaidEditor', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._raid_editor_visible = True
                self._ensure_raid_editor_clickable()
                threading.Timer(0.5, self._ensure_raid_editor_clickable).start()
                self._push_raid_editor_full()
        except Exception:
            pass

    def _hide_raid_editor(self):
        try:
            if self.raid_editor_win and self._raid_editor_visible:
                self._eval_raid_editor('if(window.RaidEditor&&RaidEditor.fadeOut)RaidEditor.fadeOut()')
                def _finish():
                    try:
                        if self.raid_editor_win and not self._raid_editor_visible:
                            self.raid_editor_win.hide()
                    except Exception:
                        pass
                threading.Timer(0.3, _finish).start()
        except Exception:
            pass
        self._raid_editor_visible = False

    def _push_raid_editor_entities(self, entities=None):
        """Push entity list to the raid editor overlay."""
        if not self._raid_editor_visible:
            return
        try:
            if entities is None:
                engine = getattr(self, '_boss_raid_engine', None)
                if engine:
                    entities = engine.get_entities()
                else:
                    entities = []
            self._eval_raid_editor(
                f'RaidEditor.updateEntities({json.dumps(entities, ensure_ascii=False)})')
        except Exception:
            pass

    def _push_raid_editor_status(self):
        """Push engine status to the raid editor overlay."""
        if not self._raid_editor_visible:
            return
        try:
            engine = getattr(self, '_boss_raid_engine', None)
            if engine:
                status = engine.get_status()
                self._eval_raid_editor(
                    f'RaidEditor.updateStatus({json.dumps(status, ensure_ascii=False)})')
        except Exception:
            pass

    def _push_raid_editor_full(self):
        """Push full state (entities + status) to the raid editor."""
        if not self._raid_editor_visible:
            return
        try:
            engine = getattr(self, '_boss_raid_engine', None)
            if engine:
                status = engine.get_status()
                entities = engine.get_entities()
                payload = {**status, 'entities': entities}
                self._eval_raid_editor(
                    f'RaidEditor.updateFull({json.dumps(payload, ensure_ascii=False)})')
        except Exception:
            pass

    def _on_raid_entity_update(self, entities):
        """Callback from BossRaidEngine when entity list changes."""
        if self._raid_editor_visible:
            self._push_raid_editor_entities(entities)

    # ── Commander panel ──

    def _eval_commander(self, js):
        try:
            if self.commander_win:
                self.commander_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_commander_clickable(self):
        """Remove WS_EX_TRANSPARENT so commander receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-Commander')
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

    def _show_commander(self):
        try:
            if self.commander_win and not self._commander_visible:
                self._apply_webview2_transparency()
                self._set_window_alpha('SAO-Commander', 0.0)
                self.commander_win.show()
                self._eval_commander('if(window.Commander&&Commander.fadeIn)Commander.fadeIn()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-Commander', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._commander_visible = True
                self._ensure_commander_clickable()
                threading.Timer(0.5, self._ensure_commander_clickable).start()
                self._push_commander_data()
        except Exception:
            pass

    def _hide_commander(self):
        try:
            if self.commander_win and self._commander_visible:
                self._eval_commander('if(window.Commander&&Commander.fadeOut)Commander.fadeOut()')
                def _finish():
                    try:
                        if self.commander_win:
                            self.commander_win.hide()
                            # Ensure click-through when hidden
                            self._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass
                threading.Timer(0.3, _finish).start()
        except Exception:
            pass
        self._commander_visible = False

    def _push_commander_data(self):
        """Push team + CD data to the commander panel."""
        if not self._commander_visible:
            return
        try:
            bridge = getattr(self, '_bridge', None)
            if bridge:
                data = bridge.get_commander_data()
            else:
                data = {'members': [], 'team_id': 0, 'leader_uid': 0, 'dungeon_id': 0}
            self._eval_commander(
                f'Commander.update({json.dumps(data, ensure_ascii=False)})')
        except Exception:
            pass

    # ── AutoKey Editor overlay ──

    def _eval_autokey_editor(self, js):
        try:
            if self.autokey_editor_win:
                self.autokey_editor_win.evaluate_js(js)
        except Exception:
            pass

    def _ensure_autokey_editor_clickable(self):
        """Remove WS_EX_TRANSPARENT so overlay receives clicks."""
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-AutoKeyEditor')
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

    def _show_autokey_editor(self):
        try:
            if self.autokey_editor_win and not self._autokey_editor_visible:
                self._apply_webview2_transparency()
                self._set_window_alpha('SAO-AutoKeyEditor', 0.0)
                self.autokey_editor_win.show()
                self._eval_autokey_editor('if(window.AutoKeyEditor&&AutoKeyEditor.fadeIn)AutoKeyEditor.fadeIn()')
                threading.Timer(
                    0.03,
                    lambda: self._animate_window_alpha('SAO-AutoKeyEditor', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                self._autokey_editor_visible = True
                self._ensure_autokey_editor_clickable()
                threading.Timer(0.5, self._ensure_autokey_editor_clickable).start()
                self._push_autokey_editor_state()
                # Load saved actions
                actions = self._get_setting('autokey_burst_actions', [])
                self._eval_autokey_editor(
                    f'AutoKeyEditor.loadActions({json.dumps(actions, ensure_ascii=False)})')
        except Exception:
            pass

    def _hide_autokey_editor(self):
        try:
            if self.autokey_editor_win and self._autokey_editor_visible:
                self._eval_autokey_editor('if(window.AutoKeyEditor&&AutoKeyEditor.fadeOut)AutoKeyEditor.fadeOut()')
                def _finish():
                    try:
                        if self.autokey_editor_win and not self._autokey_editor_visible:
                            self.autokey_editor_win.hide()
                    except Exception:
                        pass
                threading.Timer(0.3, _finish).start()
        except Exception:
            pass
        self._autokey_editor_visible = False

    def _push_autokey_editor_slots(self, skill_slots=None):
        """Push current skill slot states to the autokey editor."""
        if not self._autokey_editor_visible:
            return
        try:
            if skill_slots is None:
                gs = getattr(self, '_game_state', None)
                if gs:
                    skill_slots = getattr(gs, 'skill_slots', [])
                else:
                    skill_slots = []
            slots_data = []
            for s in skill_slots:
                if isinstance(s, dict):
                    slots_data.append(s)
                else:
                    slots_data.append({
                        'slot_index': getattr(s, 'slot_index', 0),
                        'skill_id': getattr(s, 'skill_id', 0),
                        'skill_name': getattr(s, 'skill_name', ''),
                        'state': getattr(s, 'state', 'unknown'),
                        'cooldown_pct': getattr(s, 'cooldown_pct', 0),
                        'remaining_ms': getattr(s, 'remaining_ms', 0),
                        'total_cd_ms': getattr(s, 'total_cd_ms', 0),
                        'charge_count': getattr(s, 'charge_count', 0),
                        'max_charges': getattr(s, 'max_charges', 1),
                    })
            self._eval_autokey_editor(
                f'AutoKeyEditor.updateSlots({json.dumps(slots_data, ensure_ascii=False)})')
        except Exception:
            pass

    def _push_autokey_editor_state(self):
        """Push burst ready state and profession to autokey editor."""
        if not self._autokey_editor_visible:
            return
        try:
            gs = getattr(self, '_game_state', None)
            burst_ready = False
            profession = self._profession or ''
            if gs:
                burst_ready = getattr(gs, 'burst_ready', False)
            state = {
                'burst_ready': burst_ready,
                'profession': profession,
            }
            self._eval_autokey_editor(
                f'AutoKeyEditor.updateState({json.dumps(state, ensure_ascii=False)})')
        except Exception:
            pass

    def _ensure_skillfx_on_top(self):
        try:
            if not self._skillfx_hwnd:
                self._skillfx_hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO SkillFX')
            if not self._skillfx_hwnd:
                return
            HWND_TOPMOST = ctypes.c_void_p(-1)
            SWP_NOMOVE = 0x0002
            SWP_NOSIZE = 0x0001
            SWP_NOACTIVATE = 0x0010
            ctypes.windll.user32.SetWindowPos(
                self._skillfx_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        except Exception:
            pass

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
        # v2.1.2-m: 防"alert 一直弹很多次"问题. 调用方很多 (boss_raid /
        # identity / hide&seek / update_popup / start_recognition 等),
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

    def _hide_identity_alert_window(self, expected_nonce: int = None):
        if not self.alert_win:
            return
        current_nonce = int(getattr(self, '_identity_alert_nonce', 0) or 0)
        if expected_nonce is not None and expected_nonce != current_nonce:
            return

        # v2.2.0: Hide & Seek 持续 alert 期间, 任何外部/计时器请求关闭都先放行给
        # _hide_hide_seek_persistent_alert 走 (它会清掉 _hide_seek_alert_active
        # 再调本函数). 否则就是误关 — 直接拒绝, 让 50s 刷新器接住.
        if (getattr(self, '_hide_seek_alert_active', False)
                and str(getattr(self, '_identity_alert_kind', '') or '') == 'hide_seek'):
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

    def _setup_skillfx_click_through(self):
        try:
            user32 = ctypes.windll.user32
            hwnd = user32.FindWindowW(None, 'SAO SkillFX')
            if not hwnd:
                return
            self._skillfx_hwnd = hwnd
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            ex |= (_WS_EX_TRANSPARENT | _WS_EX_LAYERED)
            user32.SetWindowLongW(hwnd, _GWL_EXSTYLE, ex)
            self._ensure_skillfx_on_top()
        except Exception:
            pass

    def _setup_boss_hp_click_through(self, _wait_retries: int = 20):
        """Make Boss HP overlay fully click-through (like SkillFX).

        Waits for the hwnd to become findable (up to ~2s) before applying,
        so the window is always transparent before it becomes visible.
        """
        try:
            user32 = ctypes.windll.user32
            hwnd = user32.FindWindowW(None, 'SAO-BossHP')
            if not hwnd and _wait_retries > 0:
                threading.Timer(0.1, lambda: self._setup_boss_hp_click_through(_wait_retries - 1)).start()
                return
            if not hwnd:
                return
            self._boss_hp_hwnd = hwnd
            ex = user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            ex |= (_WS_EX_TRANSPARENT | _WS_EX_LAYERED)
            user32.SetWindowLongW(hwnd, _GWL_EXSTYLE, ex)
            self._ensure_boss_hp_on_top()
        except Exception:
            pass

    def _ensure_boss_hp_on_top(self):
        try:
            if not self._boss_hp_hwnd:
                self._boss_hp_hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO-BossHP')
            if not self._boss_hp_hwnd:
                return
            HWND_TOPMOST = ctypes.c_void_p(-1)
            SWP_NOMOVE = 0x0002
            SWP_NOSIZE = 0x0001
            SWP_NOACTIVATE = 0x0010
            ctypes.windll.user32.SetWindowPos(
                self._boss_hp_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        except Exception:
            pass

    def _get_skillfx_layout(self, gs=None):
        if gs is None and hasattr(self, '_state_mgr'):
            gs = self._state_mgr.state

        client_rect = getattr(gs, 'window_rect', None) if gs else None
        if not client_rect:
            try:
                from window_locator import WindowLocator
                client_rect = WindowLocator().get_rect()
            except Exception:
                client_rect = None
        if not client_rect:
            return None

        client_left, client_top, client_right, client_bottom = client_rect
        client_w = max(1, int(client_right - client_left))
        client_h = max(1, int(client_bottom - client_top))

        slots = []
        for slot in list(getattr(gs, 'skill_slots', []) or []) if gs else []:
            if not isinstance(slot, dict):
                continue
            rect = slot.get('rect') or {}
            try:
                sx = int(rect.get('x', 0))
                sy = int(rect.get('y', 0))
                sw = int(rect.get('w', 0))
                sh = int(rect.get('h', 0))
                idx = int(slot.get('index', 0) or 0)
            except Exception:
                continue
            if idx <= 0 or sw <= 0 or sh <= 0:
                continue
            slots.append({
                'index': idx,
                'screen_rect': {'x': client_left + sx, 'y': client_top + sy, 'w': sw, 'h': sh},
                'client_rect': {'x': sx, 'y': sy, 'w': sw, 'h': sh},
            })

        if not slots:
            for item in get_skill_slot_rects(client_rect):
                left, top, right, bottom = item['bbox']
                slots.append({
                    'index': int(item['index']),
                    'screen_rect': {'x': left, 'y': top, 'w': right - left, 'h': bottom - top},
                    'client_rect': {'x': left - client_left, 'y': top - client_top, 'w': right - left, 'h': bottom - top},
                })

        if not slots:
            return None

        min_x = min(item['screen_rect']['x'] for item in slots)
        max_x = max(item['screen_rect']['x'] + item['screen_rect']['w'] for item in slots)
        max_y = max(item['screen_rect']['y'] + item['screen_rect']['h'] for item in slots)
        pad_x = max(18, int(round(client_w * 0.012)))
        pad_y = max(18, int(round(client_h * 0.016)))
        pad_left = max(96, int(round(client_w * 0.055)))
        pad_right = max(84, int(round(client_w * 0.044)))
        win_x = max(0, min_x - pad_left)
        win_y = max(0, client_top)
        width = max(420, int((client_right - win_x) + pad_right))
        height = max(220, int((max_y - win_y) + pad_y))
        callout_w = max(440, int(round(client_w * 0.29)))
        callout_h = max(128, int(round(client_h * 0.115)))
        callout_margin_x = max(28, int(round(client_w * 0.022)))
        callout_margin_y = max(24, int(round(client_h * 0.040)))
        callout_x = max(callout_margin_x, width - callout_w - callout_margin_x)
        callout_y = callout_margin_y

        payload_slots = []
        for item in slots:
            rect = item['screen_rect']
            payload_slots.append({
                'index': item['index'],
                'rect': {'x': rect['x'] - win_x, 'y': rect['y'] - win_y, 'w': rect['w'], 'h': rect['h']},
                'client_rect': dict(item['client_rect']),
            })
        payload_slots.sort(key=lambda item: item['index'])

        return {
            'window': {'x': int(win_x), 'y': int(win_y), 'w': int(width), 'h': int(height)},
            'viewport': {
                'width': int(width),
                'height': int(height),
                'padding_x': int(max(pad_x, pad_left, pad_right)),
                'padding_y': int(pad_y),
                'callout': {
                    'x': int(callout_x),
                    'y': int(callout_y),
                    'w': int(callout_w),
                    'h': int(callout_h),
                },
            },
            'slots': payload_slots,
            'client_rect': tuple(client_rect),
        }

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

    def _build_skillfx_payload(self, gs):
        layout = self._get_skillfx_layout(gs)
        if not layout:
            return None
        self._skillfx_layout = layout
        slot_map = {}
        for slot in getattr(gs, 'skill_slots', []) or []:
            if isinstance(slot, dict):
                try:
                    slot_map[int(slot.get('index', 0) or 0)] = slot
                except Exception:
                    pass
        payload_slots = []
        for slot_layout in layout['slots']:
            slot = slot_map.get(slot_layout['index'], {})
            payload_slots.append({
                'index': slot_layout['index'],
                'rect': self._rect_to_css(slot_layout['rect']),
                'state': str(slot.get('state', 'unknown') or 'unknown'),
                'cooldown_ratio': float(slot.get('cooldown_ratio', slot.get('cooldown_pct', 0.0)) or 0.0),
                'insufficient_energy': bool(slot.get('insufficient_energy')),
                'ready_edge': bool(slot.get('ready_edge')),
                'active': bool(slot.get('active')),
            })
        return {
            'viewport': self._viewport_to_css(layout['viewport']),
            'slots': payload_slots,
            'watched_slots': self._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]),
            'burst_enabled': bool(self._get_setting('burst_enabled', True)),
        }

    def _update_skillfx_layout(self):
        if not self.skillfx_win:
            return
        layout = self._get_skillfx_layout(getattr(self, '_game_state', None))
        if not layout:
            return
        self._skillfx_layout = layout
        window = layout['window']
        webview_w = max(1, self._to_webview_px(window['w']))
        webview_h = max(1, self._to_webview_px(window['h']))
        webview_x = self._to_webview_px(window['x'])
        webview_y = self._to_webview_px(window['y'])
        try:
            self.skillfx_win.resize(int(webview_w), int(webview_h))
        except Exception:
            pass
        try:
            self.skillfx_win.move(int(webview_x), int(webview_y))
        except Exception:
            pass
        css_vp = self._viewport_to_css(layout['viewport'])
        self._eval_skillfx(f'SkillFX.setViewport({json.dumps(css_vp, ensure_ascii=False)})')
        self._setup_skillfx_click_through()

    # ════════════════════════════════════════
    #  菜单
    # ════════════════════════════════════════
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
            time.sleep(0.04)
            self._sync_session_players_menu(force=True)
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
        # 2) save_now=True 让 player_name / level / profession / fight_point 立刻
        #    持久化, 切换到 entity 后能直接看到角色信息而不用再等一次抓包.
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

        # 退出前保存缓存
        self._save_game_cache(quiet=False)

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
            self._native_fade_window('SAO SkillFX', duration_ms=180, steps=10)
        except Exception:
            pass
        try:
            self._native_fade_window('SAO Alert', duration_ms=180, steps=10)
        except Exception:
            pass
        try:
            self._native_fade_window('SAO-BossHP', duration_ms=180, steps=10)
        except Exception:
            pass
        try:
            self._native_fade_window('SAO-RaidEditor', duration_ms=140, steps=8)
        except Exception:
            pass
        try:
            self._native_fade_window('SAO-AutoKeyEditor', duration_ms=140, steps=8)
        except Exception:
            pass
        try:
            self._native_fade_window('SAO-Commander', duration_ms=140, steps=8)
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
            if self.skillfx_win:
                self.skillfx_win.destroy()
        except Exception:
            pass
        try:
            if self.boss_hp_win:
                self.boss_hp_win.destroy()
        except Exception:
            pass
        try:
            if self.dps_win:
                self.dps_win.destroy()
        except Exception:
            pass
        try:
            if self.raid_editor_win:
                self.raid_editor_win.destroy()
        except Exception:
            pass
        try:
            if self.autokey_editor_win:
                self.autokey_editor_win.destroy()
        except Exception:
            pass
        try:
            if self.commander_win:
                self.commander_win.destroy()
        except Exception:
            pass

        # 强制退出进程 — webview/.NET 内部线程无法自行终止
        # 热切换时不强杀: _do_hot_switch 需要在 webview.start() 返回后运行
        if not self._pending_switch:
            # Final synchronous settings save before os._exit — ensure
            # all settings (including those written during panel destroy
            # callbacks) are persisted before the process dies.
            try:
                settings_ref = getattr(self, '_cfg_settings_ref', None)
                if settings_ref is not None:
                    settings_ref.save()
                elif hasattr(self, 'settings') and self.settings is not None:
                    self.settings.save()
            except Exception:
                pass

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
                _hwnd, game_rect = self._get_game_window_context()
                hwnd = int(_hwnd or 0)
                if game_rect and len(game_rect) == 4:
                    gl, gt, gr, gb = [int(v) for v in game_rect]
                    hx = (gl + gr) // 2
                    hy = (gt + gb) // 2
            except Exception:
                pass
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
        gs = self._game_state
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
    @staticmethod
    def _session_int(value, default: int = 0) -> int:
        try:
            if value is None:
                return default
            return int(value)
        except Exception:
            try:
                text = str(value).strip()
                return int(text) if text.isdigit() else default
            except Exception:
                return default

    def _session_self_uid(self) -> int:
        for source in (
                getattr(self, '_game_state', None),
                getattr(getattr(self, '_state_mgr', None), 'state', None)):
            uid = self._session_int(getattr(source, 'player_id', 0), 0)
            if uid > 0:
                return uid
        return 0

    def _merge_session_player(self, uid, name='', fight_point=0, is_self=False):
        uid = self._session_int(uid, 0)
        if uid <= 0:
            return
        now = time.time()
        changed = False
        entry = self._session_players.get(uid)
        if not entry:
            entry = {
                'uid': uid,
                'name': '',
                'fight_point': 0,
                'first_seen': now,
                'updated_at': now,
                'is_self': False,
            }
            self._session_players[uid] = entry
            changed = True
        name = str(name or '').strip()
        fight_point = self._session_int(fight_point, 0)
        if name and entry.get('name') != name:
            entry['name'] = name
            changed = True
        if fight_point > 0 and entry.get('fight_point') != fight_point:
            entry['fight_point'] = fight_point
            changed = True
        next_is_self = bool(entry.get('is_self') or is_self)
        if bool(entry.get('is_self')) != next_is_self:
            entry['is_self'] = next_is_self
            changed = True
        entry['updated_at'] = now
        if changed:
            self._session_players_version += 1

    def _sync_session_players_cache(self, gs=None):
        self_uid = self._session_self_uid()
        if self_uid > 0 and self_uid != self._session_players_self_uid:
            if self._session_players_self_uid:
                self._session_players.clear()
                self._session_players_version += 1
            self._session_players_self_uid = self_uid

        if gs is None:
            gs = getattr(self, '_game_state', None)
        if gs is not None:
            gs_uid = self._session_int(getattr(gs, 'player_id', 0), 0)
            self._merge_session_player(
                gs_uid,
                getattr(gs, 'player_name', '') or self._username or '',
                getattr(gs, 'fight_point', 0) or 0,
                is_self=bool(gs_uid and gs_uid == self_uid),
            )

        bridge = getattr(self, '_packet_engine', None)
        if not bridge:
            return
        try:
            players = bridge.get_players() or {}
        except Exception:
            players = {}
        for raw_uid, pdata in players.items():
            uid = self._session_int(raw_uid, 0) or self._session_int(getattr(pdata, 'uid', 0), 0)
            self._merge_session_player(
                uid,
                getattr(pdata, 'name', '') or '',
                getattr(pdata, 'fight_point', 0) or 0,
                is_self=bool(uid and uid == self_uid),
            )

    @staticmethod
    def _format_session_power(value) -> str:
        value = SAOWebViewGUI._session_int(value, 0)
        return f'{value:,}' if value > 0 else '--'

    def _get_session_player_rows(self, sync: bool = True) -> List[Dict[str, Any]]:
        if sync:
            self._sync_session_players_cache(getattr(self, '_game_state', None))
        self_uid = self._session_self_uid()
        rows = []
        for uid, entry in self._session_players.items():
            fp = self._session_int(entry.get('fight_point'), 0)
            is_self = bool(uid and uid == self_uid) or bool(entry.get('is_self'))
            rows.append({
                'uid': str(uid) if uid else '--',
                'name': str(entry.get('name') or ''),
                'fight_power': self._format_session_power(fp),
                'fight_power_value': fp,
                'is_self': is_self,
                'first_seen': float(entry.get('first_seen') or 0.0),
            })
        rows.sort(key=lambda r: (
            0 if r.get('is_self') else 1,
            -int(r.get('fight_power_value') or 0),
            str(r.get('name') or ''),
            str(r.get('uid') or ''),
        ))
        return rows

    def _build_session_players_payload(self, sync: bool = True):
        rows = self._get_session_player_rows(sync=sync)
        return {
            'ok': True,
            'count': len(rows),
            'self_uid': str(self._session_self_uid() or ''),
            'players': rows,
            'generated_at': int(time.time()),
        }

    def _sync_session_players_menu(self, force: bool = False):
        if not self.menu_win:
            return
        self._sync_session_players_cache(getattr(self, '_game_state', None))
        sig = (
            len(self._session_players),
            self._session_players_version,
            str(self._session_self_uid() or ''),
        )
        now = time.time()
        if not force and sig == self._session_players_last_sig:
            return
        if not force and now - self._session_players_last_push_ts < 0.5:
            return
        payload = self._build_session_players_payload(sync=False)
        self._session_players_last_sig = sig
        self._session_players_last_push_ts = now
        method = 'showSessionPlayers' if force else 'setSessionPlayersPayload'
        self._eval_menu(
            f'if(window.SAO&&SAO.{method})SAO.{method}({json.dumps(payload, ensure_ascii=False)})'
        )

    def _toggle_session_players_menu(self):
        if not self.menu_win:
            return
        self._sync_session_players_cache(getattr(self, '_game_state', None))
        payload = self._build_session_players_payload(sync=False)
        self._session_players_last_sig = (
            len(self._session_players),
            self._session_players_version,
            str(self._session_self_uid() or ''),
        )
        self._session_players_last_push_ts = time.time()
        self._eval_menu(
            f'if(window.SAO&&SAO.toggleSessionPlayers)SAO.toggleSessionPlayers({json.dumps(payload, ensure_ascii=False)})'
        )

    def _sync_menu_info(self):
        gs = self._game_state
        gs_desc = 'VISION ACTIVE' if self._recognition_active else 'SAO Auto Idle'
        hp_str = '--'
        sta_str = '--'
        if gs and hasattr(gs, 'hp_current'):
            if gs.hp_max > 0:
                hp_str = f'{gs.hp_current}/{gs.hp_max}'
            if getattr(gs, 'stamina_offline', False):
                sta_str = 'OFFLINE'
            else:
                sta_pct = int(round(max(0.0, min(1.0, float(gs.stamina_pct or 0.0))) * 100.0))
                sta_str = f'{sta_pct}%'
            gs_desc = f'HP: {hp_str}  STA: {sta_str}'
        # 使用 GameState 中最新的等级数据 (来自 packet bridge)
        _menu_level = self._level
        _menu_level_str = str(_menu_level)
        if gs and hasattr(gs, 'level_base') and gs.level_base > 0:
            _menu_level = gs.level_base
            self._level = _menu_level  # 同步到 instance 变量
            _menu_level_extra = int(getattr(gs, 'level_extra', 0) or 0)
            if _menu_level_extra > 0:
                _menu_level_str = f'{_menu_level}(+{_menu_level_extra})'
            else:
                _menu_level_str = str(_menu_level)
        # 使用 GameState 中最新的职业名 (来自 packet bridge)
        _menu_prof = self._profession
        if gs and hasattr(gs, 'profession_name') and gs.profession_name:
            _menu_prof = gs.profession_name
            self._profession = _menu_prof
        info = {
            'username': self._username, 'level': _menu_level_str,
            'profession': _menu_prof,
            'hp': hp_str, 'sta': sta_str,
            'des': gs_desc,
            'file': '',
        }
        self._eval_menu(f'SAO.updateInfo({json.dumps(info, ensure_ascii=False)})')
        self._sync_session_players_menu(force=False)
        # Sync menu settings (watched slots, sound, mode, etc.)
        self._sync_menu_settings()
        self._sync_all_panels()
        self._ensure_updater_listener()

    def _push_update_state(self, snapshot=None):
        """将 UpdateManager 快照推送到 menu (SAO.updateUpdaterState)。"""
        try:
            if snapshot is None:
                from sao_updater import get_manager
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
                from sao_updater import get_manager
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
        error = str(getattr(snapshot, 'error', '') or '').strip() if snapshot else ''
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
            return {
                'key': f'error:{latest_version}:{error}',
                'title': 'UPDATE ERROR',
                'message': error or '更新服务暂不可用，请稍后重试。',
                'duration_ms': 5200,
            }
        return None

    def _maybe_show_update_popup(self, snapshot=None):
        # v2.1.2-m: 防 sao_alert 反复弹窗:
        #   1) downloading 状态完全静音 (进度由 SAO 菜单/状态面板显示)
        #   2) 同一个 popup_key 不重复弹 (依旧依赖 _last_update_popup_key)
        #   3) 当前 alert 还在显示且不是 error → 跳过 (避免无意义重叠)
        payload = self._build_update_popup_payload(snapshot)
        if not payload:
            return
        popup_key = str(payload.get('key') or '')
        if not popup_key or popup_key == getattr(self, '_last_update_popup_key', ''):
            return
        if popup_key.startswith('downloading:'):
            self._last_update_popup_key = popup_key
            return
        if (getattr(self, '_identity_alert_visible', False)
                and not popup_key.startswith('error:')):
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
            from sao_updater import get_manager
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
            from sao_sound import get_sound_enabled, get_sound_volume
            cfg = {
                'watched_slots': self._get_setting('watched_skill_slots', [1,2,3,4,5,6,7,8,9]),
                'burst_enabled': self._get_setting('burst_enabled', True),
                'sound_enabled': get_sound_enabled(),
                'sound_volume': get_sound_volume(),
            }
            cfg['auto_key'] = self._get_auto_key_menu_state()
            cfg['boss_bar_mode'] = self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
            cfg['dps_enabled'] = bool(self._get_setting('dps_enabled', True))
            cfg['dps_fade_timeout_s'] = int(self._get_setting('dps_fade_timeout_s', 60))
            cfg['dps_last_report_available'] = self._get_dps_last_report_available()
            cfg['raid_editor_visible'] = bool(self._raid_editor_visible)
            cfg['autokey_editor_visible'] = bool(self._autokey_editor_visible)
            cfg['commander_visible'] = bool(self._commander_visible)
            _hs_engine = getattr(self, '_hide_seek_engine', None)
            cfg['hide_seek_active'] = bool(_hs_engine and _hs_engine.running)
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
            'toggle_auto_script': self._toggle_auto_script,
            'toggle_hide_seek': self._toggle_hide_seek,
            'toggle_raid_editor': lambda: (self._show_raid_editor() if not self._raid_editor_visible else self._hide_raid_editor()),
            'toggle_autokey_editor': lambda: (self._show_autokey_editor() if not self._autokey_editor_visible else self._hide_autokey_editor()),
            'toggle_commander': lambda: (self._show_commander() if not self._commander_visible else self._hide_commander()),
            'toggle_session_players': self._toggle_session_players_menu,
            'switch_to_entity': lambda: self._transition_with_animation('entity'),
            'exit': self._exit_with_animation,
        }
        fn = _map.get(action)
        if fn:
            threading.Thread(target=fn, daemon=True).start()

    # ════════════════════════════════════════
    #  识别状态循环
    # ════════════════════════════════════════
    def _recognition_loop(self):
        """后台识别循环 — 从 GameStateManager 读取状态, 推送到 HP 条 + 体力覆盖板"""
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
                    self._sync_vision_lifecycle(gs)
                except Exception as e:
                    print(f'[SAO-WV] vision lifecycle sync error: {e}')
                    gs = None
            # v2.1.2-l: 即使 _recognition_active=False (例如 onedir 第一次启动
            # PacketBridge / RecognitionEngine 还没起来或起失败) 也要把缓存里的
            # HP/等级推到 HP panel, 否则面板显示空白被用户误判为"HP 不显示"。
            # 只有 HP 数据推送是无条件的; STA / Boss / SkillFX 这些视觉相关的
            # 仍然依赖 _recognition_active.
            if (not self._recognition_active) and gs is not None:
                try:
                    if gs.hp_max > 0:
                        _hp, _hp_max = int(gs.hp_current or 0), int(gs.hp_max)
                    elif gs.hp_pct > 0:
                        _hp, _hp_max = int(gs.hp_pct * 100), 100
                    else:
                        _hp, _hp_max = 0, 1
                    _lv_base = gs.level_base if gs.level_base else self._level
                    if gs.level_extra > 0 and _lv_base > 0:
                        _lv_str = f'{_lv_base}(+{gs.level_extra})'
                    elif _lv_base > 0:
                        _lv_str = str(_lv_base)
                    else:
                        _lv_str = str(self._level)
                    _sig = (_hp, _hp_max, _lv_str)
                    if getattr(self, '_idle_hp_sig', None) != _sig:
                        self._idle_hp_sig = _sig
                        self._eval_hp(f'updateHP({_hp}, {_hp_max}, "{_lv_str}")')
                        self._eval_hp('setPlayState("idle")')
                except Exception:
                    pass
            if self._recognition_active and gs is not None:
                try:
                    self._sync_identity_alert(gs)
                    # ── HP / Level / STA display ──
                    # HP and Level come from packets and are always available.
                    # STA comes from vision and only updates when recognition_ok.
                    if gs.hp_max > 0:
                        hp, hp_max = gs.hp_current, gs.hp_max
                    elif gs.hp_pct > 0:
                        hp, hp_max = int(gs.hp_pct * 100), 100
                    elif gs.level_base > 0:
                        hp, hp_max = 0, 1
                    else:
                        hp, hp_max = 0, 1
                    level_base = gs.level_base if gs.level_base else self._level
                    if gs.level_extra > 0 and level_base > 0:
                        level_str = f'{level_base}(+{gs.level_extra})'
                    elif level_base > 0:
                        level_str = str(level_base)
                    else:
                        level_str = str(self._level)
                    # ── 等级升级检测 ──
                    if level_base > 0 and self._last_displayed_level_base > 0 \
                            and level_base > self._last_displayed_level_base:
                        self._eval_hp(f'showLevelUp({self._last_displayed_level_base}, {level_base})')
                    if level_base > 0:
                        self._last_displayed_level_base = level_base
                    # Use packet HP data if available
                    self._eval_hp(f'updateHP({hp}, {hp_max}, "{level_str}")')
                    sta_offline = self._should_show_sta_offline(gs)
                    self._eval_hp(f'setSTAOffline({str(bool(sta_offline)).lower()})')
                    if not sta_offline:
                        sta = int(round(max(0.0, min(1.0, float(gs.stamina_pct or 0.0))) * 100.0))
                        self._eval_hp(f'updateSTA({sta}, 100)')
                    if gs.recognition_ok or getattr(gs, 'packet_active', False):
                        self._eval_hp('setPlayState("playing")')
                    else:
                        self._eval_hp('setPlayState("idle")')
                    # ── Boss Timer push (packet-driven, always runs) ──
                    _boss_text = getattr(gs, 'boss_timer_text', '') or ''
                    _boss_active = getattr(gs, 'boss_raid_active', False)
                    _boss_enrage = float(getattr(gs, 'boss_enrage_remaining', 0) or 0)
                    if _boss_active and _boss_text:
                        _boss_urgency = 'urgent' if 0 < _boss_enrage < 60 else 'normal'
                    else:
                        _boss_text = ''
                        _boss_urgency = ''
                    if _boss_text != getattr(self, '_last_boss_timer_text', '') or \
                       _boss_urgency != getattr(self, '_last_boss_timer_urgency', ''):
                        self._last_boss_timer_text = _boss_text
                        self._last_boss_timer_urgency = _boss_urgency
                        if _boss_text:
                            self._eval_hp(f'setBossTimer("{self._safe_js(_boss_text)}", "{_boss_urgency}")')
                        else:
                            self._eval_hp('setBossTimer("", "")')
                    # ── Boss Bar (packet-driven, always runs) ──
                    _bb_mode = self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
                    _bb_raid_active = getattr(gs, 'boss_raid_active', False)
                    _bb_src = getattr(gs, 'boss_hp_source', 'none') or 'none'

                    # ── Target-based boss bar: show HP of the monster we're attacking ──
                    # Multi-unit support: highest-HP unit = main panel; others as .additional panels (sorted by HP desc, attack time)
                    _bb_direct_hp = 0
                    _bb_direct_max = 0
                    _bb_direct_data = None
                    _bb_additional = []
                    _now = time.time()
                    _bb_timeout = self._boss_hp_hold_timeout_s()
                    _has_recent_self_damage = (_now - self._bb_last_damage_ts) < _bb_timeout

                    # Cleanup stale recent targets (prevent accumulation)
                    for uuid in list(self._bb_recent_targets.keys()):
                        if _now - self._bb_recent_targets.get(uuid, 0) > _bb_timeout:
                            self._bb_recent_targets.pop(uuid, None)

                    if not _bb_raid_active:
                        _bridge = getattr(self, '_packet_engine', None)
                        if _bridge and _has_recent_self_damage and self._bb_recent_targets:
                            # Collect all recently damaged monsters
                            _recent_monsters = []
                            for uuid, dmg_ts in list(self._bb_recent_targets.items()):
                                if _now - dmg_ts < _bb_timeout:
                                    m = _bridge.get_monster(uuid)
                                    if self._boss_monster_usable(m):
                                        _recent_monsters.append(m)
                            if _recent_monsters:
                                # Sort: HP% desc (highest = primary/boss unit), then recent attack time desc
                                def _sort_key(m):
                                    hp = getattr(m, 'hp', 0) or 0
                                    maxhp = getattr(m, 'max_hp', 0) or hp or 1
                                    hp_pct = hp / maxhp if maxhp > 0 else 0
                                    last_ts = self._bb_recent_targets.get(getattr(m, 'uuid', 0), 0)
                                    return (-hp_pct, -last_ts)
                                _recent_monsters.sort(key=_sort_key)
                                main_m = _recent_monsters[0]
                                self._bb_last_target_uuid = getattr(main_m, 'uuid', 0)  # update target to highest-HP
                                _bb_direct_max = int(getattr(main_m, 'max_hp', 0)) or int(getattr(main_m, 'hp', 0))
                                _bb_direct_hp = max(0, int(getattr(main_m, 'hp', 0)))
                                _bb_direct_data = main_m.to_dict() if hasattr(main_m, 'to_dict') else {}
                                _bb_src = 'packet'
                                # Additional units for secondary panels
                                for m in _recent_monsters[1:]:
                                    if len(_bb_additional) >= 4: break
                                    d = m.to_dict() if hasattr(m, 'to_dict') else {}
                                    _bb_additional.append({
                                        'name': str(d.get('name', 'Unit'))[:20],
                                        'hp_pct': round(float(d.get('hp_pct', 0.0)), 3),
                                        'extinction_pct': round(float(d.get('extinction_pct', 0.0)), 3),
                                        'has_break_data': bool(d.get('has_break_data', False)),
                                        'breaking_stage': int(d.get('breaking_stage', -1)),
                                        'shield_active': bool(d.get('shield_active', False)),
                                        'shield_pct': round(float(d.get('shield_pct', 0.0)), 3)
                                    })
                        elif self._bb_last_target_uuid and not _has_recent_self_damage:
                            # Pre-tracked boss: still fetch data for when damage arrives (single)
                            try:
                                _m = _bridge.get_monster(self._bb_last_target_uuid) if _bridge else None
                                if self._boss_monster_usable(_m):
                                    _bb_direct_max = int(getattr(_m, 'max_hp', 0)) or int(getattr(_m, 'hp', 0))
                                    _bb_direct_hp = max(0, int(getattr(_m, 'hp', 0)))
                                    _bb_direct_data = _m.to_dict() if hasattr(_m, 'to_dict') else {}
                                    _bb_src = 'packet'
                            except Exception:
                                pass

                    # Determine if bar should be visible:
                    # - 'off' mode: never show
                    # - 'boss_raid' mode: show when raid engine active OR we have recent self-damage
                    # - 'always' mode: show when we have recent self-damage to any monster
                    if _bb_mode == 'off':
                        _bb_show = False
                    elif _bb_raid_active:
                        _bb_show = True
                    else:
                        # Show only when we have dealt damage recently
                        _bb_show = _has_recent_self_damage and (_bb_src != 'none' or _bb_direct_data is not None)

                    # Build data: prefer direct monster data; fall back to GameState
                    if _bb_direct_data and not _bb_raid_active:
                        _bb_hp_pct = _bb_direct_hp / _bb_direct_max if _bb_direct_max > 0 else 1.0
                        _bb_cur_hp = _bb_direct_hp
                        _bb_total_hp = _bb_direct_max
                        _bb_shield_active = bool(_bb_direct_data.get('shield_active'))
                        _bb_shield_pct = float(_bb_direct_data.get('shield_pct') or 0.0)
                        _bb_breaking = int(_bb_direct_data.get('breaking_stage') or 0)
                        _bb_has_break = bool(_bb_direct_data.get('has_break_data'))
                        _bb_extinction = float(_bb_direct_data.get('extinction_pct') or 0.0)
                        _bb_extinction_raw = int(_bb_direct_data.get('extinction') or 0)
                        _bb_max_extinction = int(_bb_direct_data.get('max_extinction') or 0)
                        _bb_stop_ticking = bool(_bb_direct_data.get('stop_breaking_ticking'))
                        _bb_overdrive = bool(_bb_direct_data.get('in_overdrive'))
                        _bb_invincible = False
                    else:
                        _bb_hp_pct = round(getattr(gs, 'boss_hp_est_pct', 1.0), 3)
                        _bb_cur_hp = getattr(gs, 'boss_current_hp', 0)
                        _bb_total_hp = getattr(gs, 'boss_total_hp', 0)
                        _bb_shield_active = getattr(gs, 'boss_shield_active', False)
                        _bb_shield_pct = round(getattr(gs, 'boss_shield_pct', 0.0), 3)
                        _bb_breaking = getattr(gs, 'boss_breaking_stage', -1)
                        _bb_has_break = getattr(gs, 'boss_breaking_stage', -1) != -1
                        _bb_extinction = round(getattr(gs, 'boss_extinction_pct', 0.0), 3)
                        _bb_extinction_raw = 0
                        _bb_max_extinction = 0
                        _bb_stop_ticking = False
                        _bb_overdrive = getattr(gs, 'boss_in_overdrive', False)
                        _bb_invincible = getattr(gs, 'boss_invincible', False)

                    _bb_sig = (
                        _bb_show,
                        round(float(_bb_hp_pct), 3),
                        _bb_src,
                        int(_bb_cur_hp),
                        int(_bb_total_hp),
                        bool(_bb_shield_active),
                        round(float(_bb_shield_pct), 3),
                        int(_bb_breaking),
                        bool(_bb_has_break),
                        round(float(_bb_extinction), 3),
                        int(_bb_extinction_raw),
                        int(_bb_max_extinction),
                        bool(_bb_stop_ticking),
                        bool(_bb_overdrive),
                        bool(_bb_invincible),
                    )
                    if _bb_sig != getattr(self, '_last_boss_bar_sig', None):
                        self._last_boss_bar_sig = _bb_sig
                        _bb_data = {
                            'active': _bb_show,
                            'hp_pct': _bb_sig[1],
                            'hp_source': _bb_src,
                            'current_hp': _bb_sig[3],
                            'total_hp': _bb_sig[4],
                            'shield_active': _bb_sig[5],
                            'shield_pct': _bb_sig[6],
                            'breaking_stage': _bb_sig[7],
                            'has_break_data': _bb_sig[8],
                            'extinction_pct': _bb_sig[9],
                            'extinction': _bb_sig[10],
                            'max_extinction': _bb_sig[11],
                            'stop_breaking_ticking': _bb_sig[12],
                            'in_overdrive': _bb_sig[13],
                            'invincible': _bb_sig[14],
                            'boss_name': (_bb_direct_data or {}).get('name', '') or '',
                            'additional': _bb_additional,
                        }
                        self._eval_boss_hp(f'updateBossBar({json.dumps(_bb_data)})')
                    # ── DPS Meter push (packet-driven, always runs) ──
                    if self._dps_tracker:
                        try:
                            if gs.player_id:
                                _p_uid = int(gs.player_id) if str(gs.player_id).isdigit() else 0
                                if _p_uid:
                                    self._dps_tracker.set_self_uid(_p_uid)
                                    _self_fp = 0
                                    _bridge = getattr(self, '_packet_engine', None)
                                    if _bridge:
                                        _all_p = _bridge.get_players()
                                        _sp = _all_p.get(_p_uid)
                                        if _sp:
                                            _self_fp = getattr(_sp, 'fight_point', 0) or 0
                                    self._dps_tracker.update_player_info(
                                        _p_uid,
                                        gs.player_name or '',
                                        gs.profession_name or '',
                                        _self_fp,
                                        int(gs.level_base or 0),
                                    )

                            # ── Sync ALL players' info (name, profession, fight_point) ──
                            _bridge = getattr(self, '_packet_engine', None)
                            if _bridge:
                                try:
                                    for _pu, _pd in _bridge.get_players().items():
                                        if _pu and _pd.name:
                                            self._dps_tracker.update_player_info(
                                                _pu,
                                                _pd.name or '',
                                                _pd.profession or '',
                                                getattr(_pd, 'fight_point', 0) or 0,
                                                getattr(_pd, 'level', 0) or 0,
                                            )
                                except Exception:
                                    pass

                            _dps_enabled = bool(self._get_setting('dps_enabled', True))
                            _dps_idle_timeout = self._combat_damage_timeout_s()
                            if self._dps_tracker.finalize_if_idle(_dps_idle_timeout, 'idle_timeout'):
                                self._sync_dps_report_availability()
                                if self._dps_mode == 'live':
                                    self._hide_dps_window()

                            if self._dps_tracker.is_dirty():
                                _dps_snap = self._dps_tracker.get_snapshot()
                                _dps_has_live = bool(
                                    int(_dps_snap.get('total_damage') or 0) > 0
                                    and self._dps_tracker.has_recent_damage(_dps_idle_timeout)
                                )
                                if _dps_enabled and _dps_has_live:
                                    self._show_dps_live_snapshot(_dps_snap)
                                elif self._dps_visible and self._dps_mode == 'live':
                                    self._eval_dps(
                                        f'DpsMeter.updateDps({json.dumps(_dps_snap, ensure_ascii=False)})'
                                    )
                            # ── DPS fade-out on idle ──
                            if self._dps_visible and self._dps_mode == 'live':
                                try:
                                    if not self._dps_tracker.has_recent_damage(_dps_idle_timeout):
                                        self._hide_dps_window()
                                except Exception:
                                    pass
                            self._sync_dps_report_availability()
                        except Exception:
                            pass
                    # ── Burst Mode Ready 检测 (packet-driven, always runs) ──
                    _burst_enabled = self._get_setting('burst_enabled', True)
                    _burst_now = getattr(gs, 'burst_ready', False)
                    _burst_prev = getattr(self, '_last_burst_ready', False)
                    _burst_slot = self._pick_burst_trigger_slot(gs) if _burst_enabled else 0
                    _prev_layout = getattr(self, '_skillfx_layout', None)
                    _skillfx_payload = self._build_skillfx_payload(gs) or {}
                    _next_layout = getattr(self, '_skillfx_layout', None)
                    if _prev_layout != _next_layout:
                        self._update_skillfx_layout()
                    _skillfx_payload['burst_slot'] = int(_burst_slot or 0)
                    _skillfx_payload['burst_ready'] = bool(_burst_now)
                    _skillfx_payload['enabled'] = bool(_burst_enabled)
                    _skillfx_sig = (
                        int(_skillfx_payload.get('burst_slot', 0) or 0),
                        bool(_skillfx_payload.get('burst_ready')),
                        tuple(
                            (
                                int(item.get('index', 0) or 0),
                                str(item.get('state', 'unknown') or 'unknown')
                            )
                            for item in (_skillfx_payload.get('slots', []) or [])
                            if isinstance(item, dict)
                        )
                    )
                    if getattr(self, '_last_skillfx_sig', None) != _skillfx_sig:
                        self._last_skillfx_sig = _skillfx_sig
                        _watched_dbg = self._get_setting('watched_skill_slots', [1,2,3,4,5,6,7,8,9]) or []
                        _state_dbg = []
                        for item in (_skillfx_payload.get('slots', []) or []):
                            if not isinstance(item, dict):
                                continue
                            try:
                                _idx_dbg = int(item.get('index', 0) or 0)
                            except Exception:
                                _idx_dbg = 0
                            _state_dbg.append(f"{_idx_dbg}:{str(item.get('state', 'unknown') or 'unknown')}")
                        print(
                            f"[SAO-WV] SkillFX sync: watched={list(_watched_dbg)} "
                            f"burst_slot={_skillfx_payload['burst_slot']} "
                            f"burst_ready={_skillfx_payload['burst_ready']} "
                            f"states={_state_dbg}"
                        )
                    self._eval_skillfx(f'SkillFX.update({json.dumps(_skillfx_payload, ensure_ascii=False)})')
                    if (not _burst_now) and _burst_prev:
                        self._eval_skillfx('SkillFX.hideBurstReady()')
                    self._last_burst_ready = _burst_now
                    # ── Push to AutoKey Editor overlay ──
                    if self._autokey_editor_visible:
                        try:
                            self._push_autokey_editor_slots()
                            if _burst_now != _burst_prev:
                                self._push_autokey_editor_state()
                        except Exception:
                            pass
                    # ── Push to Raid Editor overlay (status tick) ──
                    if self._raid_editor_visible:
                        try:
                            self._push_raid_editor_status()
                        except Exception:
                            pass
                    # 缓存到 _game_state 供菜单使用
                    self._game_state = gs
                    # ── 同步玩家名到 WebView ──
                    if gs.player_name and gs.player_name != getattr(self, '_last_gs_name', ''):
                        self._last_gs_name = gs.player_name
                        self._username = gs.player_name
                        self._eval_hp(f'setUsername("{self._safe_js(gs.player_name)}")')
                    # ── 同步职业/UID 到 id-plate ──
                    _prof = gs.profession_name or ''
                    _uid = gs.player_id or ''
                    if (_prof and _prof != getattr(self, '_last_gs_prof', '')) or \
                       (_uid and _uid != getattr(self, '_last_gs_uid', '')):
                        self._last_gs_prof = _prof
                        self._last_gs_uid = _uid
                        import json as _json2
                        info = {}
                        if _prof:
                            info['profession'] = _prof
                        if _uid:
                            info['uid'] = _uid
                        self._eval_hp(f'setPlayerInfo({_json2.dumps(info, ensure_ascii=False)})')
                    # ── 首次获取完整角色数据时自动保存 ──
                    if not getattr(self, '_profile_auto_saved', False) and gs.player_name:
                        self._profile_auto_saved = True
                        try:
                            from character_profile import save_profile
                            lv = gs.level_base if gs.level_base > 0 else 1
                            save_profile(
                                username=gs.player_name,
                                profession=gs.profession_name or '',
                                level=lv,
                                uid=gs.player_id or '',
                            )
                            print(f'[SAO-WV] 自动保存角色: {gs.player_name}, '
                                  f'职业={gs.profession_name}, LV={lv}, UID={gs.player_id}')
                        except Exception:
                            pass
                except Exception as e:
                    print(f'[SAO-WV] recognition loop error: {e}')
            else:
                # 识别未激活时, 仍保留最后已知数据 (如有)
                if gs is not None:
                    try:
                        self._sync_identity_alert(gs)
                    except Exception:
                        pass
                    if gs.hp_max > 0 or gs.level_base > 0:
                        hp = gs.hp_current if gs.hp_max > 0 else 0
                        hp_max = gs.hp_max if gs.hp_max > 0 else 1
                        level_base = gs.level_base if gs.level_base else self._level
                        if gs.level_extra > 0 and level_base > 0:
                            level_str = f'{level_base}(+{gs.level_extra})'
                        elif level_base > 0:
                            level_str = str(level_base)
                        else:
                            level_str = str(self._level)
                        # ── 等级升级检测 (idle path) ──
                        if level_base > 0 and self._last_displayed_level_base > 0 \
                                and level_base > self._last_displayed_level_base:
                            self._eval_hp(f'showLevelUp({self._last_displayed_level_base}, {level_base})')
                        if level_base > 0:
                            self._last_displayed_level_base = level_base
                        self._eval_hp(f'updateHP({hp}, {hp_max}, "{level_str}")')
                    self._eval_hp('setSTAOffline(false)')
                    self._eval_hp('setPlayState("idle")')
                else:
                    self._eval_hp('setSTAOffline(false)')
                    self._eval_hp('setPlayState("idle")')

            _panel_tick += 1
            if _panel_tick % 5 == 0:
                self._refresh_boss_hp_geometry()
                self._sync_auto_key_menu()
                self._sync_boss_raid_menu()
                if self._menu_visible:
                    self._sync_session_players_menu()
                # Push commander data every ~0.5s when visible
                if self._commander_visible:
                    try:
                        self._push_commander_data()
                    except Exception:
                        pass
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
        # 退出前保存缓存
        try:
            self._persist_cached_identity_state(save_now=False)
        except Exception:
            pass
        self._save_game_cache(quiet=False)
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
        try:
            if self.skillfx_win:
                self.skillfx_win.destroy()
        except Exception:
            pass
