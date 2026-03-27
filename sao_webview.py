"""
SAO-UI WebView GUI — sao_auto 移植版 (从 28midi/sao_webview.py 完全复刻)

差异:
  - 移除 MidiPlayer / 播放控制 / 文件选择 / 文件夹循环 / MIDI解析
  - HP 条改为显示游戏识别数据 (HP / Stamina / Level)
  - HP 条固定位置 (不可拖拽)
  - 窗口检测目标: Star.exe
  - 新增: RecognitionEngine / GameState 集成

保留 (与 28midi 完全一致):
  - LinkStart 入场动画 (SAOLinkStart)
  - SAO Menu (鱼眼背景 / 圆形菜单 / 文件选择器 / 排行榜)
  - 面板系统 (control / status / viz / piano)
  - 透明窗口 (Win32 LWA_COLORKEY + .NET WebView2)
  - 音效系统 (pygame)
  - 角色等级系统 (character_profile)
  - 3-UI 热切换
  - 所有动画特效
"""

import os
import sys
import time
import threading
import json
import copy
import ctypes
import numpy as np
from typing import Optional

from auto_key_engine import (
    AutoKeyCloudClient,
    AutoKeyEngine,
    DEFAULT_AUTO_KEY_SERVER_URL,
    build_auto_key_state,
    clone_profile,
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
from config import (
    DEFAULT_HOTKEYS,
    get_skill_slot_rects,
)

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
    if getattr(sys, 'frozen', False):
        base = sys._MEIPASS if hasattr(sys, '_MEIPASS') else os.path.dirname(sys.executable)
    else:
        base = os.path.dirname(os.path.abspath(__file__))
    icon_path = os.path.join(base, 'icon.ico')
    return icon_path if os.path.exists(icon_path) else None


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
        from System.Drawing import Color as DColor
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


def _invoke_dotnet_transparency(win_obj):
    """从后台线程安全地在 GUI 线程设置 Form 色键透明.

    原理:
      HTML transparent 区域穿透到 Form 背景色.
      原本 Form BackColor = 白色 → 白底可见.
      设置 BackColor = TransparencyKey = rgb(1,0,1) 后,
      Form 背景变成 key color, Win32 COLORKEY 再将 key color 穿透到桌面.

    win_obj.native 是 pywebview BrowserForm 实例
    (winforms.py BrowserForm.__init__: self.pywebview_window.native = self).
    通过 form.Invoke 投递到 GUI 线程执行, 避免跨线程 .NET 访问死锁.
    """
    try:
        form = getattr(win_obj, 'native', None)
        if form is None:
            return
        from System import Action
        form.Invoke(Action(lambda: _setup_dotnet_transparency(form)))
    except Exception as e:
        print(f"[SAO] invoke dotnet transparency: {e}")


# ════════════════════════════════════════════════
#  鱼眼特效 (截图 → barrel distortion → base64)
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
    def __init__(self):
        if getattr(sys, 'frozen', False):
            base = os.path.dirname(sys.executable)
        else:
            base = os.path.dirname(os.path.abspath(__file__))
        self._path = os.path.join(base, 'settings.json')
        self._data = {}
        self._load()

    def _load(self):
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
        try:
            with open(self._path, 'w', encoding='utf-8') as f:
                json.dump(self._data, f, indent=2, ensure_ascii=False)
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

        if isinstance(regions, dict):
            display_regions = _sanitize(regions.get('display_regions', []))
            click_regions = _sanitize(regions.get('click_regions', []))
        else:
            display_regions = _sanitize(regions if isinstance(regions, list) else [])
            click_regions = list(display_regions)

        if display_regions:
            self._g._hp_display_regions = display_regions
            self._g._hp_hit_regions = list(display_regions)
        elif not getattr(self._g, '_hp_display_regions', None):
            self._g._hp_display_regions = []

        if click_regions:
            self._g._hp_click_regions = click_regions
        elif not getattr(self._g, '_hp_click_regions', None):
            self._g._hp_click_regions = []

        if display_regions or click_regions:
            self._g._hp_hit_regions_ready = True
            self._g._hp_last_hit_region_ts = time.time()

        if not display_regions and not click_regions and getattr(self._g, '_hp_hit_regions', None):
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
            raise RuntimeError('No file picker action pending')
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def select_folder(self, path):
        return json.dumps({'ok': False, 'message': 'Folder selection not used here'}, ensure_ascii=False)

    def set_auto_key_upload_token(self, token):
        try:
            config = self._g._load_auto_key_config()
            config['upload_token'] = str(token or '').strip()
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def set_auto_key_server_url(self, url):
        try:
            config = self._g._load_auto_key_config()
            config['server_url'] = str(url or '').strip()
            self._g._save_auto_key_config(config)
            self._g._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._g._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

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
            token = str(config.get('upload_token') or '').strip()
            if not token:
                raise RuntimeError('Upload token is empty')
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

        from character_profile import load_profile, calc_level

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

        # 角色
        profile = load_profile()
        self._username = profile.get('username', '') or 'Player'
        self._profession = profile.get('profession', '剑士')
        self._level = profile.get('level', 1)
        self._xp = profile.get('xp', 0)
        self._songs_played = profile.get('songs_played', 0)
        self._play_time = profile.get('play_time', 0)
        lv, cur_xp, need_xp = calc_level(self._xp)
        self._level = lv
        self._xp_pct = (cur_xp / max(1, need_xp)) * 100

        # 识别状态
        self._recognition_active = False
        self._game_state = None  # GameState dataclass
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

        # 窗口
        self.hp_win = None
        self.menu_win = None
        self.skillfx_win = None
        self.alert_win = None

        # 热切换目标
        self._pending_switch: Optional[str] = None

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
        self._auto_key_engine = None
        self._auto_key_picker_purpose = ''
        self._auto_key_last_menu_state = None
        self._last_identity_alert_serial = 0
        self._identity_alert_visible = False
        self._identity_alert_nonce = 0

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
            except Exception:
                pass

    def _get_setting(self, key: str, default=None):
        """Read a setting."""
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            return self._cfg_settings_ref.get(key, default)
        return default

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
        return build_auto_key_state(config, engine_status=status)

    def _sync_auto_key_menu(self):
        try:
            state = self._get_auto_key_menu_state()
            if state != getattr(self, '_auto_key_last_menu_state', None):
                self._auto_key_last_menu_state = copy.deepcopy(state)
                self._eval_menu(f'SAO.syncAutoKeyState({json.dumps(state, ensure_ascii=False)})')
        except Exception:
            pass

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

    def _save_game_cache(self, quiet: bool = False):
        try:
            if hasattr(self, '_state_mgr') and getattr(self, '_state_mgr', None) and \
               hasattr(self, '_cfg_settings_ref') and getattr(self, '_cfg_settings_ref', None):
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

    def _sync_identity_alert(self, gs):
        if gs is None:
            return
        try:
            alert_serial = int(getattr(gs, 'identity_alert_serial', 0) or 0)
        except Exception:
            alert_serial = 0
        alert_title = str(getattr(gs, 'identity_alert_title', '') or '')
        alert_message = str(getattr(gs, 'identity_alert_message', '') or '')

        if alert_serial > 0 and alert_serial != getattr(self, '_last_identity_alert_serial', 0):
            self._last_identity_alert_serial = alert_serial
            self._show_identity_alert_window(alert_title, alert_message, 9000)
            return

        has_identity = bool(
            str(getattr(gs, 'player_name', '') or '').strip()
            and int(getattr(gs, 'level_base', 0) or 0) > 0
        )
        if has_identity and getattr(self, '_identity_alert_visible', False):
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
            payload['viewport'] = dict(layout['viewport'])
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
            return
        if not getattr(self, '_cfg_settings_ref', None) or not getattr(self, '_state_mgr', None):
            return
        from recognition import RecognitionEngine
        vision_engine = RecognitionEngine(self._state_mgr, self._cfg_settings_ref)
        vision_engine.start()
        self._vision_engine = vision_engine
        self._recognition_engines.append(vision_engine)
        self._vision_paused_for_death = False
        print('[SAO] Vision engine resumed (revive)')

    def _sync_vision_lifecycle(self, gs):
        dead_now = self._is_dead_state(gs)
        dead_prev = bool(getattr(self, '_last_dead_state', False))
        if dead_now and not dead_prev:
            self._pause_vision_for_death()
        elif (not dead_now) and dead_prev:
            self._resume_vision_after_revive()
        self._last_dead_state = dead_now

    def _pick_burst_trigger_slot(self, gs):
        watched = self._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]) or []
        try:
            watched = [int(x) for x in watched if int(x) > 0]
        except Exception:
            watched = []
        if not watched:
            watched = [1]
        for slot in getattr(gs, 'skill_slots', []) or []:
            if not isinstance(slot, dict):
                continue
            try:
                idx = int(slot.get('index', 0) or 0)
            except Exception:
                continue
            if idx in watched and bool(slot.get('ready_edge')):
                return idx
        for slot in getattr(gs, 'skill_slots', []) or []:
            if not isinstance(slot, dict):
                continue
            try:
                idx = int(slot.get('index', 0) or 0)
                state = str(slot.get('state', '') or '').strip().lower()
            except Exception:
                continue
            if idx in watched and state == 'ready':
                return idx
        for slot in getattr(gs, 'skill_slots', []) or []:
            if not isinstance(slot, dict):
                continue
            try:
                idx = int(slot.get('index', 0) or 0)
                cd = float(slot.get('cooldown_pct', 1.0) or 1.0)
            except Exception:
                continue
            if idx in watched and cd <= 0.02:
                return idx
        return 0

    def _stop_recognition_engines(self):
        if getattr(self, '_auto_key_engine', None):
            try:
                self._auto_key_engine.stop()
            except Exception:
                pass
            self._auto_key_engine = None
        engines = list(getattr(self, '_recognition_engines', []) or [])
        if not engines and self._recognition_engine:
            engines = [self._recognition_engine]
        for engine in engines:
            try:
                engine.stop()
            except Exception:
                pass
        self._recognition_engines = []
        self._recognition_engine = None
        self._packet_engine = None
        self._vision_engine = None
        self._vision_paused_for_death = False
        self._last_dead_state = False

    def _reconfigure_data_engines(self):
        """Restart packet/vision engines to match the current per-component source map."""
        if not getattr(self, '_cfg_settings_ref', None) or not getattr(self, '_state_mgr', None):
            return

        self._stop_recognition_engines()
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

        engines = []
        try:
            from packet_bridge import PacketBridge
            packet_engine = PacketBridge(self._state_mgr, self._cfg_settings_ref)
            packet_engine.start()
            engines.append(packet_engine)
            self._packet_engine = packet_engine
            print('[SAO] Packet bridge started (network capture)')
        except Exception as e:
            import traceback
            print(f'[SAO] Packet bridge FAILED to start: {e}', flush=True)
            traceback.print_exc()
            self._packet_engine = None

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
        self._recognition_engine = engines[0] if engines else None
        self._recognition_active = bool(engines)

    # ════════════════════════════════════════
    #  入口
    # ════════════════════════════════════════
    def run(self):
        # ── Phase 1: LinkStart (tkinter, 阻塞) ──
        self._run_tkinter_link_start()
        self._lock_hp_position(1.0)

        # ── Phase 2: pywebview ──
        web_dir = os.path.join(getattr(sys, '_MEIPASS', os.path.dirname(os.path.abspath(__file__))), 'web')
        hp_url = os.path.join(web_dir, 'hp.html')
        menu_url = os.path.join(web_dir, 'menu.html')
        skillfx_url = os.path.join(web_dir, 'skillfx.html')
        alert_url = os.path.join(web_dir, 'alert.html')

        # HP 固定位置: 左下角覆盖 等级/UID 区域
        try:
            _sw = ctypes.windll.user32.GetSystemMetrics(0)
            _sh = ctypes.windll.user32.GetSystemMetrics(1)
        except Exception:
            _sw, _sh = 1920, 1080

        try:
            _dpi = ctypes.windll.user32.GetDpiForSystem()
        except Exception:
            _dpi = 96
        _dpi_scale = max(1.0, _dpi / 96.0)
        self._dpi_scale = _dpi_scale

        # 统一 HUD 窗口: 覆盖左下角 + 中底 HP + STA
        hud_w = int(_sw * 0.75)
        self._hud_w = hud_w

        # 裁剪区域参数 (在 _setup_click_through 后由 _force_hp_to_bottom 根据实际窗口尺寸重新计算)
        self._hp_clip_top = max(200, int(500 - 120 * _dpi_scale))

        # 目标位置
        tx0, ty0 = self._calc_hud_target(_sw, _sh)
        self._hp_target_x = tx0
        self._hp_target_y = ty0

        # HP 悬浮窗 — 初始放在动画起点, 避免 show() 时先闪到错误位置
        if self._hp_fullscreen:
            cx, cy = 0, 0
        else:
            cx = max(0, int((_sw - hud_w) / 2))
            cy = max(0, int((_sh - 500) / 2))

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
            width=max(320, int(_sw * 0.42)),
            height=max(140, int(_sh * 0.20)),
            x=max(0, int(_sw * 0.29)),
            y=max(0, int(_sh * 0.74)),
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

        webview.start(self._on_webview_started, debug=False)

        # ── Phase 3: 热切换 ──
        if self._pending_switch:
            self._do_hot_switch(self._pending_switch)

    # ─── HUD 位置自动检测 ───
    def _calc_hud_target(self, sw: int = 0, sh: int = 0) -> tuple:
        """计算 HUD 目标位置 (x, y) — 与 Entity 模式对齐: x=4%屏宽, y=屏幕底部。"""
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
        # 可用户自定义偏移
        offset_pct = 0.04
        if self._cfg_settings_ref:
            offset_pct = self._cfg_settings_ref.get('hud_offset_x', 0.04)
        return int(sw * offset_pct), sh - 500

    # ─── LinkStart (tkinter) ───
    def _run_tkinter_link_start(self):
        try:
            import tkinter as tk
            from sao_theme import SAOLinkStart

            ls_root = tk.Tk()
            ls_root.withdraw()

            done = threading.Event()

            def on_done():
                done.set()
                try:
                    ls_root.after(50, ls_root.destroy)
                except Exception:
                    pass

            ls = SAOLinkStart(ls_root, on_done=on_done)
            ls.play()
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
        # 菜单窗口只做 Win32 色键, 不设 .NET TransparencyKey
        # (TransparencyKey 会令菜单 HTML 透明区域变成鼠标穿透, 导致按钮无法点击)
        try:
            menu_hwnd = ctypes.windll.user32.FindWindowW(None, 'SAO Menu')
            if menu_hwnd:
                _make_transparent_ctypes(menu_hwnd)
        except Exception:
            pass

    def _reassert_hp_transparency(self, alpha: float = 1.0, retries: int = 4, delay: float = 0.18):
        """反复重置 HP 窗口透明状态，修复热切换后偶发白底。"""
        def _apply_once():
            try:
                self._apply_webview2_transparency()
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
            while True:
                time.sleep(0.12)
                try:
                    if self._hp_hwnd and not self._hp_entry_animating and self._is_hp_position_locked():
                        self._force_hp_to_bottom(force=True, quiet=True)
                except Exception:
                    pass

        threading.Thread(target=_loop, daemon=True, name='hp_position_guard').start()

    def _start_hp_click_bootstrap(self, duration: float = 4.5):
        if getattr(self, '_hp_click_bootstrap_started', False):
            return
        self._hp_click_bootstrap_started = True

        def _loop():
            deadline = time.time() + max(1.0, float(duration or 0.0))
            while time.time() < deadline:
                try:
                    self._setup_click_through()
                    self._request_hp_hit_regions()
                    if self._hp_hwnd and getattr(self, '_hp_js_hit_regions_ready', False):
                        break
                except Exception:
                    pass
                time.sleep(0.16)

            try:
                self._setup_click_through()
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
        """Fallback clickable regions when JS hit-regions have not registered yet."""
        try:
            display_regions = self._default_hp_display_regions()
            if not display_regions:
                return []
            return [display_regions[2]]
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
            if getattr(self, '_hp_fullscreen', False):
                self._set_hp_mouse_passthrough(True)
                self._start_hp_mouse_passthrough_poller()
            else:
                self._set_hp_mouse_passthrough(False)
        except Exception as e:
            print(f"[SAO] click-through setup failed: {e}")

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
            regions = getattr(self, '_hp_click_regions', []) or []
            if not regions:
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
            hit_rects = []
            off_x = int(getattr(self, '_hp_viewport_offset_x', 0) or 0)
            off_y = int(getattr(self, '_hp_viewport_offset_y', 0) or 0)
            regions = getattr(self, '_hp_display_regions', []) or []
            if not regions:
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
                if getattr(self, '_hp_fullscreen', False):
                    hrgn = gdi32.CreateRectRgn(0, 0, 0, 0)
                else:
                    visible_phys = int(120 * dpi_s)
                    clip_top = max(0, win_h - visible_phys)
                    hrgn = gdi32.CreateRectRgn(0, clip_top, win_w, win_h)
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
                return

            # 目标: 与 Entity 模式对齐 (x=4%屏宽, 底边贴屏幕底)
            if getattr(self, '_hp_fullscreen', False):
                # WebView/WinForms occasionally leaves a small non-client gutter
                # at the bottom even for frameless transparent windows.
                # Overscan the fullscreen HUD window slightly so the visible
                # display area truly reaches the monitor edge.
                overscan = max(12, int(sh * 0.012))
                target_x = 0
                target_y = -overscan
                win_w = sw
                win_h = sh + overscan
                self._hp_viewport_offset_x = 0
                self._hp_viewport_offset_y = overscan
            else:
                target_x, _ = self._calc_hud_target(sw, sh)
                target_y = sh - win_h
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
            from character_profile import calc_level
            self._lock_hp_position(2.0)
            self._hp_hit_regions_ready = False
            self._hp_js_hit_regions_ready = False
            self._hp_last_hit_region_ts = 0.0
            # ── 先应用透明, 再显示窗口 (防止白底闪现) ──
            self._apply_webview2_transparency()
            time.sleep(0.15)
            self._apply_webview2_transparency()  # 二次确保
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
            time.sleep(0.18)
            self._eval_hp(f'setUsername("{self._safe_js(self._username)}")')
            lv, cur_xp, need_xp = calc_level(self._xp)
            self._eval_hp(f'updateHP({cur_xp}, {need_xp}, {lv})')
            self._sync_menu_info()
            # 设置 click-through (延迟确保窗口已完全创建)
            time.sleep(0.3)
            self._setup_click_through()
            self._update_skillfx_layout()
            self._request_hp_hit_regions()
            self._start_hp_position_guard()
            self._start_hp_click_bootstrap(5.5)
            # WebView2 透明背景 — 持续重试
            self._apply_webview2_transparency()
            self._reassert_hp_transparency(1.0, retries=15, delay=0.35)
            # Safety: re-run _force_hp_to_bottom after a delay in case hwnd
            # was not available during the first attempt.
            def _safety_force():
                if self._hp_hwnd and not getattr(self, '_win_h_phys', 0):
                    self._force_hp_to_bottom()
            threading.Timer(1.5, _safety_force).start()
            threading.Timer(3.0, _safety_force).start()
            # 任务栏图标
            self._set_window_icon('SAO-HP')
            self._set_window_icon('SAO Menu')
            self._set_window_icon('SAO SkillFX')
            self._set_window_icon('SAO Alert')
            # 菜单窗口在启动阶段保持完全透明, 避免偶发白色方框闪现
            self._set_window_alpha('SAO Menu', 0.0)
            self._set_window_alpha('SAO SkillFX', 1.0)
            self._set_window_alpha('SAO Alert', 1.0)
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
            from game_state import GameStateManager
            from config import SettingsManager as CfgSettings

            self._state_mgr = GameStateManager()
            cfg_settings = CfgSettings()

            # 加载上次缓存的游戏状态 (立即显示)
            self._state_mgr.load_cache(cfg_settings)
            self._cfg_settings_ref = cfg_settings  # 保留引用用于定时保存
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

            self._reconfigure_data_engines()
            self._auto_key_engine = AutoKeyEngine(
                self._state_mgr,
                self._cfg_settings_ref,
                extra_gate=lambda: bool(getattr(self, '_recognition_active', False)),
            )
            self._auto_key_engine.start()
            self._sync_auto_key_menu()

            # 启动定时缓存保存 (每30秒)
            import threading as _thr
            def _cache_loop():
                import time as _t
                while True:
                    _t.sleep(30)
                    try:
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
        }
        self._hk_pressed = set()
        self._hk_listener = None
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
            print(f'[SAO WebView] Hotkeys unavailable: {e}')

    def _hk_on_press(self, key):
        try:
            if isinstance(key, self._hk_KeyCode) and key.vk:
                self._hk_pressed.add(key.vk)
            elif isinstance(key, self._hk_Key):
                self._hk_pressed.add(key.value.vk if hasattr(key.value, 'vk') else str(key))
        except Exception:
            pass
        self._hk_check()

    def _hk_on_release(self, key):
        try:
            if isinstance(key, self._hk_KeyCode) and key.vk:
                self._hk_pressed.discard(key.vk)
            elif isinstance(key, self._hk_Key):
                self._hk_pressed.discard(key.value.vk if hasattr(key.value, 'vk') else str(key))
        except Exception:
            pass

    def _hk_check(self):
        saved = getattr(self, '_cfg_settings_ref', None)
        hotkeys = DEFAULT_HOTKEYS if saved is None else saved.get('hotkeys', DEFAULT_HOTKEYS)
        for action, info in (hotkeys or {}).items():
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

    @staticmethod
    def _safe_js(s: str) -> str:
        if not s:
            return ''
        return s.replace('\\', '\\\\').replace('"', '\\"').replace("'", "\\'").replace('\n', '\\n')

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

    def _show_identity_alert_window(self, title: str, message: str, duration_ms: int = 9000):
        if not self.alert_win:
            return
        self._identity_alert_visible = True
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
        self._set_window_alpha('SAO Alert', 1.0)
        self._ensure_alert_on_top()
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
        threading.Timer(0.12, _push).start()
        threading.Timer(0.32, _push).start()
        threading.Timer(0.72, _push).start()
        threading.Timer(stay_ms / 1000.0, lambda: self._hide_identity_alert_window(expected_nonce=nonce)).start()

    def _hide_identity_alert_window(self, expected_nonce: int = None):
        if not self.alert_win:
            return
        current_nonce = int(getattr(self, '_identity_alert_nonce', 0) or 0)
        if expected_nonce is not None and expected_nonce != current_nonce:
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

        threading.Timer(0.52, _finish_hide).start()

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
                'rect': dict(slot_layout['rect']),
                'state': str(slot.get('state', 'unknown') or 'unknown'),
                'cooldown_ratio': float(slot.get('cooldown_ratio', slot.get('cooldown_pct', 0.0)) or 0.0),
                'insufficient_energy': bool(slot.get('insufficient_energy')),
                'ready_edge': bool(slot.get('ready_edge')),
                'active': bool(slot.get('active')),
            })
        return {
            'viewport': dict(layout['viewport']),
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
        try:
            self.skillfx_win.resize(int(window['w']), int(window['h']))
        except Exception:
            pass
        try:
            self.skillfx_win.move(int(window['x']), int(window['y']))
        except Exception:
            pass
        self._eval_skillfx(f'SkillFX.setViewport({json.dumps(layout["viewport"], ensure_ascii=False)})')
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
                if not (_ex2 & WS_EX_LAYERED):
                    ctypes.windll.user32.SetWindowLongW(_hwnd2, GWL_EXSTYLE, _ex2 | WS_EX_LAYERED)
                ctypes.windll.user32.SetLayeredWindowAttributes(_hwnd2, _COLORREF_KEY, 0,
                                                                _LWA_ALPHA | _LWA_COLORKEY)
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
        gen = self._fisheye_gen
        threading.Thread(target=self._fisheye_loop, args=(gen,), daemon=True).start()

        def _init_menu():
            self._reassert_menu_transparency(0.0)
            time.sleep(0.12)
            self._eval_menu('SAO.openMenu(500, 300)')
            time.sleep(0.04)
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

    def _exit_with_animation(self):
        self._transition_with_animation()

    # ─── 鱼眼截屏 ───
    def _capture_current_monitor_b64(self, quality=55):
        """快速截屏 → 低分辨率 JPEG base64，用于 WebGL 鱼眼纹理 (目标 <10ms)"""
        try:
            import base64 as b64mod, io
            from PIL import Image

            hx, hy = 0, 0
            try:
                if self.hp_win and hasattr(self.hp_win, 'x'):
                    hx = self.hp_win.x or 0
                    hy = self.hp_win.y or 0
            except Exception:
                pass

            img = None
            try:
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
            tw, th = 480, int(480 * h / max(1, w))
            if tw < w:
                img = img.resize((tw, th), Image.BILINEAR)

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
        web_dir = os.path.join(getattr(sys, '_MEIPASS', os.path.dirname(os.path.abspath(__file__))), 'web')
        url = os.path.join(web_dir, 'panel.html')

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
    def _sync_menu_info(self):
        gs = self._game_state
        gs_desc = 'VISION ACTIVE' if self._recognition_active else 'SAO Auto Idle'
        hp_str = '--'
        sta_str = '--'
        if gs and hasattr(gs, 'hp_current'):
            if gs.hp_max > 0:
                hp_str = f'{gs.hp_current}/{gs.hp_max}'
            sta_pct = int(round(max(0.0, min(1.0, float(gs.stamina_pct or 0.0))) * 100.0))
            sta_str = f'{sta_pct}%'
            gs_desc = f'HP: {hp_str}  STA: {sta_str}'
        info = {
            'username': self._username, 'level': self._level,
            'xp_pct': round(self._xp_pct, 1), 'profession': self._profession,
            'hp': hp_str, 'sta': sta_str,
            'des': gs_desc,
            'file': '',
        }
        self._eval_menu(f'SAO.updateInfo({json.dumps(info, ensure_ascii=False)})')
        # Sync menu settings (watched slots, sound, mode, etc.)
        self._sync_menu_settings()
        self._sync_all_panels()

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
            'exit': self._exit_with_animation,
        }
        fn = _map.get(action)
        if fn:
            threading.Thread(target=fn, daemon=True).start()

    def _menu_action(self, action: str):
        _map = {
            'toggle_recognition': self._toggle_recognition,
            'toggle_auto_script': self._toggle_auto_script,
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
            time.sleep(0.1)
            try:
                if self.hp_win is None:
                    return
                _ = self.hp_win.x
            except Exception:
                return
            gs = None
            if hasattr(self, '_state_mgr'):
                try:
                    gs = self._state_mgr.state
                    self._sync_vision_lifecycle(gs)
                except Exception as e:
                    print(f'[SAO-WV] vision lifecycle sync error: {e}')
                    gs = None
            if self._recognition_active and gs is not None:
                try:
                    self._sync_identity_alert(gs)
                    if gs.recognition_ok:
                        # HP/等级走共享状态，STA 百分比走纯识图
                        if gs.hp_max > 0:
                            hp, hp_max = gs.hp_current, gs.hp_max
                        elif gs.hp_pct > 0:
                            hp, hp_max = int(gs.hp_pct * 100), 100
                        else:
                            hp, hp_max = 0, 1
                        sta = int(round(max(0.0, min(1.0, float(gs.stamina_pct or 0.0))) * 100.0))
                        sta_max = 100
                        level_base = gs.level_base if gs.level_base else self._level
                        if gs.level_extra > 0 and level_base > 0:
                            level_str = f'{level_base}(+{gs.level_extra})'
                        elif level_base > 0:
                            level_str = str(level_base)
                        else:
                            level_str = str(self._level)
                        self._eval_hp(f'updateHP({hp}, {hp_max}, "{level_str}")')
                        self._eval_hp('setPlayState("playing")')
                        self._eval_hp(f'updateSTA({sta}, {sta_max})')
                        # ── Burst Mode Ready 检测 ──
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
                                lv = gs.level_extra if gs.level_extra > 0 else gs.level_base
                                save_profile(
                                    username=gs.player_name,
                                    profession=gs.profession_name or '',
                                    level=lv if lv > 0 else 1,
                                    uid=gs.player_id or '',
                                )
                                print(f'[SAO-WV] 自动保存角色: {gs.player_name}, '
                                      f'职业={gs.profession_name}, LV={lv}, UID={gs.player_id}')
                            except Exception:
                                pass
                    else:
                        # recognition_ok=False, 但如果 game_state 有缓存数据, 继续显示
                        if gs.hp_max > 0 or gs.level_base > 0:
                            # 有历史数据: 保持显示最后已知状态, 不切换到 calc_level
                            hp = gs.hp_current if gs.hp_max > 0 else 0
                            hp_max = gs.hp_max if gs.hp_max > 0 else 1
                            level_base = gs.level_base if gs.level_base else self._level
                            if gs.level_extra > 0 and level_base > 0:
                                level_str = f'{level_base}(+{gs.level_extra})'
                            elif level_base > 0:
                                level_str = str(level_base)
                            else:
                                level_str = str(self._level)
                            self._eval_hp(f'updateHP({hp}, {hp_max}, "{level_str}")')
                            self._eval_hp('setPlayState("idle")')
                        else:
                            # 从未收到过任何游戏数据, 用 calc_level 显示
                            from character_profile import calc_level
                            lv, cur_xp, need_xp = calc_level(self._xp)
                            self._eval_hp(f'updateHP({cur_xp}, {need_xp}, {lv})')
                            self._eval_hp('setPlayState("idle")')
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
                        self._eval_hp(f'updateHP({hp}, {hp_max}, "{level_str}")')
                        self._eval_hp('setPlayState("idle")')
                    else:
                        from character_profile import calc_level
                        lv, cur_xp, need_xp = calc_level(self._xp)
                        self._eval_hp(f'updateHP({cur_xp}, {need_xp}, {lv})')
                        self._eval_hp('setPlayState("idle")')
                else:
                    from character_profile import calc_level
                    lv, cur_xp, need_xp = calc_level(self._xp)
                    self._eval_hp(f'updateHP({cur_xp}, {need_xp}, {lv})')
                    self._eval_hp('setPlayState("idle")')

            _panel_tick += 1
            if _panel_tick % 5 == 0:
                self._sync_auto_key_menu()
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
        pass  # Entity 模式已移除

    def _exit(self):
        if self._exit_animating:
            return
        self._exit_animating = True
        self._recognition_active = False
        self._stop_recognition_engines()
        # 退出前保存缓存
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
