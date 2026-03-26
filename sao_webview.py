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
import ctypes
import numpy as np
from typing import Optional

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
                regions = []
        if not isinstance(regions, list):
            regions = []
        self._g._hp_hit_regions = regions
        self._g._set_hp_region(expanded=self._g._ctx_menu_active, menu_bounds=self._g._ctx_menu_bounds)

    def get_state(self):
        """供 JS 查询当前识别状态 (JSON 格式)"""
        gs = self._g._game_state
        return json.dumps({
            'recognition_active': self._g._recognition_active,
            'hp': gs.hp_current if gs and hasattr(gs, 'hp_current') else 0,
            'hp_max': gs.hp_max if gs and hasattr(gs, 'hp_max') else 0,
            'stamina': gs.stamina_current if gs and hasattr(gs, 'stamina_current') else 0,
            'stamina_max': gs.stamina_max if gs and hasattr(gs, 'stamina_max') else 0,
            'level': gs.level_base if gs and hasattr(gs, 'level_base') else 0,
        })

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
        self._recog_lock = threading.Lock()  # 保护 _recognition_active 切换

        # 菜单
        self._sta_detector_started = False
        self._menu_visible = False

        # 窗口
        self.hp_win = None
        self.menu_win = None

        # 热切换目标
        self._pending_switch: Optional[str] = None

        # JS API
        self._api = SAOWebAPI(self)

        # 窗口 click-through
        self._hp_hwnd = 0
        self._ctx_menu_active = False
        self._ctx_menu_bounds = None
        self._hp_hit_regions = []
        self._fisheye_active = False
        self._fisheye_gen = 0
        self._fisheye_prev_frame = None
        self._panel_wins = {}
        self._panel_float_active = False
        self._panel_origins = {}
        self._hp_visible = False
        self._exit_animating = False

        # 识别相关引用
        self._cfg_settings_ref = None

    # ─── 音效 ───
    def _play_sound(self, name: str):
        if self._sound_ok and self._sao_sound:
            try:
                self._sao_sound.play_sound(name)
            except Exception:
                pass

    # ════════════════════════════════════════
    #  入口
    # ════════════════════════════════════════
    def run(self):
        # ── Phase 1: LinkStart (tkinter, 阻塞) ──
        self._run_tkinter_link_start()

        # ── Phase 2: pywebview ──
        web_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'web')
        hp_url = os.path.join(web_dir, 'hp.html')
        menu_url = os.path.join(web_dir, 'menu.html')

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

        # HP 悬浮窗 — 初始在屏幕中央, 动画滑到固定位置
        cx, cy = (_sw - hud_w) // 2, (_sh - 500) // 2

        self.hp_win = webview.create_window(
            'SAO-HP', hp_url,
            width=hud_w, height=500,
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
            transparent=True,
            hidden=True,
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
                ' if (window.startHitRegionBootRetry) { startHitRegionBootRetry(); }'
            )
        except Exception:
            pass

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
                return
            self._hp_hwnd = hwnd
            self._set_hp_region(False)
        except Exception as e:
            print(f"[SAO] click-through setup failed: {e}")

    def _set_hp_region(self, expanded=False, menu_bounds=None):
        if not self._hp_hwnd:
            return
        win_h = getattr(self, '_win_h_phys', 500)
        win_w = getattr(self, '_win_w_phys', getattr(self, '_hud_w', 540))
        dpi_s = getattr(self, '_dpi_scale', 1.0)
        try:
            gdi32 = ctypes.windll.gdi32
            user32 = ctypes.windll.user32
            hit_rects = []
            for rect in getattr(self, '_hp_hit_regions', []) or []:
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
                right = min(win_w, left + width)
                bottom = min(win_h, top + height)
                left = max(0, left)
                top = max(0, top)
                if right - left >= 2 and bottom - top >= 2:
                    hit_rects.append((left, top, right, bottom))

            if menu_bounds and isinstance(menu_bounds, dict):
                try:
                    left = int(float(menu_bounds.get('left', 0)) * dpi_s)
                    top = int(float(menu_bounds.get('top', 0)) * dpi_s)
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

    def _force_hp_to_bottom(self):
        """用 GetWindowRect + SetWindowPos 强制 HP 窗口贴屏幕底部 (物理像素)。"""
        if not self._hp_hwnd:
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

            # 保存实测物理尺寸 (供 _set_hp_region 使用)
            self._win_w_phys = win_w
            self._win_h_phys = win_h

            # 目标: 与 Entity 模式对齐 (x=4%屏宽, 底边贴屏幕底)
            target_x, _ = self._calc_hud_target(sw, sh)
            target_y = sh - win_h

            HWND_TOPMOST = ctypes.c_void_p(-1)
            SWP_NOACTIVATE = 0x0010
            user32.SetWindowPos(
                self._hp_hwnd, HWND_TOPMOST,
                target_x, target_y, win_w, win_h,
                SWP_NOACTIVATE)

            # 用实测尺寸重新设置裁剪区域
            self._set_hp_region(False)
            print(f'[SAO] force position: screen_h={sh}, win={win_w}x{win_h}, y={target_y}')
        except Exception as e:
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
            if not self._hp_hwnd:
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
                    self._force_hp_to_bottom()
                    return

                HWND_TOPMOST = ctypes.c_void_p(-1)
                SWP_NOACTIVATE = 0x0010
                SWP_NOSIZE = 0x0001

                # 起点: 强制移到屏幕中央 (无论当前在哪)
                sx = (sw - win_w) // 2
                sy = (sh - win_h) // 2
                user32.SetWindowPos(
                    self._hp_hwnd, HWND_TOPMOST,
                    sx, sy, 0, 0,
                    SWP_NOACTIVATE | SWP_NOSIZE)
                time.sleep(0.02)

                # 终点: 窗口底边贴屏幕底边
                tx, ty = 0, sh - win_h

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
                        user32.SetWindowPos(
                            self._hp_hwnd, HWND_TOPMOST,
                            nx, ny, 0, 0,
                            SWP_NOACTIVATE | SWP_NOSIZE)
                    except Exception:
                        break
                    time.sleep(dt)

                # 最终精确定位
                self._force_hp_to_bottom()
            except Exception:
                self._force_hp_to_bottom()
        threading.Thread(target=_slide, daemon=True).start()

    # ─── WebView 就绪 ───
    def _on_webview_started(self):
        def _init():
            from character_profile import calc_level
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
            time.sleep(0.18)
            self._eval_hp(f'setUsername("{self._safe_js(self._username)}")')
            lv, cur_xp, need_xp = calc_level(self._xp)
            self._eval_hp(f'updateHP({cur_xp}, {need_xp}, {lv})')
            self._sync_menu_info()
            # 设置 click-through (延迟确保窗口已完全创建)
            time.sleep(0.3)
            self._setup_click_through()
            self._force_hp_to_bottom()
            self._request_hp_hit_regions()
            # WebView2 透明背景 — 持续重试
            self._apply_webview2_transparency()
            self._reassert_hp_transparency(1.0, retries=15, delay=0.35)
            # 任务栏图标
            self._set_window_icon('SAO-HP')
            self._set_window_icon('SAO Menu')
            # 菜单窗口在启动阶段保持完全透明, 避免偶发白色方框闪现
            self._set_window_alpha('SAO Menu', 0.0)
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
        """启动游戏数据引擎 (OCR 或 抓包)"""
        try:
            from game_state import GameStateManager
            from config import SettingsManager as CfgSettings

            self._state_mgr = GameStateManager()
            cfg_settings = CfgSettings()

            # 加载上次缓存的游戏状态 (立即显示)
            self._state_mgr.load_cache(cfg_settings)
            self._cfg_settings_ref = cfg_settings  # 保留引用用于定时保存
            data_source = cfg_settings.get('data_source', 'ocr')
            if data_source == 'packet':
                # Packet mode should not render stale STA before the first
                # valid packet arrives, but keeping the cached extra level
                # avoids a long blank "(+XX)" period after login.
                with self._state_mgr._lock:
                    self._state_mgr._state.stamina_current = 0
                    self._state_mgr._state.stamina_max = 0
                    self._state_mgr._state.stamina_pct = 0.0
                self._state_mgr._prev_stamina_current = 0

            # 用缓存名替换默认 "Player"
            cached_name = self._state_mgr.state.player_name
            if cached_name:
                self._username = cached_name
                self._eval_hp(f'setUsername("{self._safe_js(cached_name)}")')
                print(f'[SAO] 从缓存加载角色名: {cached_name}')
            cached_lv = self._state_mgr.state.level_base
            if cached_lv > 0:
                self._level = cached_lv
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

            if data_source == 'packet':
                from packet_bridge import PacketBridge
                self._recognition_engine = PacketBridge(self._state_mgr, cfg_settings)
                self._recognition_engine.start()
                self._recognition_active = True
                print('[SAO] Packet bridge started (网络抓包模式)')
            else:
                from recognition import RecognitionEngine
                from config import GAME_WINDOW_KEYWORDS, GAME_PROCESS_NAMES
                self._recognition_engine = RecognitionEngine(self._state_mgr, cfg_settings)
                self._recognition_engine.start()
                self._recognition_active = True
                print(f"[SAO] Recognition engine started (keywords={GAME_WINDOW_KEYWORDS}, exe={GAME_PROCESS_NAMES})")

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
        """启动 STA OCR+色彩梯度检测线程.

        流程:
          1. 扫描游戏窗口底部 18% 区域, 找金色条带 → bar_bbox + num_bbox
          2. OCR num_bbox → 解析 cur/max 整数
          3. 分析 bar_bbox 亮度/饱和度梯度 → 填充百分比
          4. EMA 平滑后推送 GameState
        """
        if self._sta_detector_started:
            return
        self._sta_detector_started = True
        packet_mode = (cfg_settings.get('data_source', 'ocr') == 'packet')

        def _find_sta_bar(img_bottom, wl, wt_offset):
            """在底部截图中定位金色 STA 条.

            返回 (bar_bbox, num_bbox) 绝对像素坐标, 未找到返回 (None, None).
            金色: HSV H∈[20,45], S>60, V>80.
            """
            import cv2, numpy as _np
            hsv = cv2.cvtColor(img_bottom, cv2.COLOR_BGR2HSV)
            mask = cv2.inRange(hsv,
                               _np.array([20, 60, 80], dtype=_np.uint8),
                               _np.array([45, 255, 255], dtype=_np.uint8))
            kernel = _np.ones((3, 15), _np.uint8)
            mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel)
            contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
            if not contours:
                return None, None
            c = max(contours, key=cv2.contourArea)
            x, y, w, h = cv2.boundingRect(c)
            if w < 40 or h < 4:
                return None, None
            bar_bbox = (wl + x, wt_offset + y, wl + x + w, wt_offset + y + h)
            num_w = max(60, w // 4)
            num_h = max(18, h * 3)
            num_x = x
            num_y = max(0, y - h)
            num_bbox = (wl + num_x, wt_offset + num_y,
                        wl + num_x + num_w, wt_offset + num_y + num_h)
            return bar_bbox, num_bbox

        def _analyze_bar_fill(img):
            """通过亮度*饱和度梯度计算条带填充比例 [0, 1].

            金色区列得分高, 暗/灰区得分低.
            红色闪烁 (危险) 检测 → 返回近零值.
            """
            import cv2, numpy as _np
            if img is None or img.size == 0:
                return 0.0
            hsv = cv2.cvtColor(img, cv2.COLOR_BGR2HSV).astype(_np.float32)
            h_ch = hsv[:, :, 0]
            s_ch = hsv[:, :, 1] / 255.0
            v_ch = hsv[:, :, 2] / 255.0
            red_mask = (((h_ch < 12) | (h_ch > 168)) & (s_ch > 0.8) & (v_ch > 0.8))
            if red_mask.mean() > 0.25:
                return 0.02
            col_score = (s_ch * 0.65 + v_ch * 0.35).mean(axis=0)
            kernel_size = max(3, len(col_score) // 20) | 1
            col_smooth = _np.convolve(col_score,
                                      _np.ones(kernel_size) / kernel_size, mode='same')
            norm = col_smooth / (col_smooth.max() + 1e-6)
            filled_cols = _np.where(norm > 0.38)[0]
            if len(filled_cols) == 0:
                return 0.0
            return float(filled_cols[-1] + 1) / len(norm)

        def _sta_loop():
            try:
                from window_locator import WindowLocator
                from recognition import (
                    _grab_region, _ocr_numbers, _ocr_image, _detect_bar_pct,
                    _parse_stamina_text, _parse_level_text, _init_ocr
                )
                from config import BAR_COLORS
                import cv2
            except ImportError as e:
                self._sta_detector_started = False
                print(f'[SAO] STA 检测依赖缺失: {e}')
                return
            _init_ocr()
            locator = WindowLocator()
            ema_pct = None
            ema_alpha = 0.30
            loop_count = 0
            max_cached = 0
            level_ocr_every = 12
            print('[SAO] STA OCR+梯度检测线程已启动')

            while self._recognition_active:
                time.sleep(0.1)
                loop_count += 1
                try:
                    result = locator.find_game_window()
                    if result is None:
                        continue
                    hwnd, title, rect = result
                    wl, wt, wr, wb = rect
                    ww, wh = wr - wl, wb - wt
                    if ww < 100 or wh < 100:
                        continue

                    # ── 固定 ROI: 体力数字 cur/max ──
                    ocr_cur, ocr_max = 0, 0
                    st_txt_roi = cfg_settings.get_roi('stamina_text')
                    st_txt_bbox = None
                    if st_txt_roi:
                        st_txt_bbox = (
                            wl + int(st_txt_roi.get('x', 0.0) * ww),
                            wt + int(st_txt_roi.get('y', 0.0) * wh),
                            wl + int((st_txt_roi.get('x', 0.0) + st_txt_roi.get('w', 0.0)) * ww),
                            wt + int((st_txt_roi.get('y', 0.0) + st_txt_roi.get('h', 0.0)) * wh),
                        )
                    if st_txt_bbox is not None:
                        img_num = _grab_region(st_txt_bbox)
                        if img_num is not None and img_num.size > 0:
                            raw_text = _ocr_numbers(img_num)
                            ocr_cur, ocr_max = _parse_stamina_text(raw_text)
                            if ocr_max > 0:
                                max_cached = ocr_max

                    # ── 固定 ROI: 体力条像素百分比 ──
                    pixel_pct = 0.0
                    st_bar_roi = cfg_settings.get_roi('stamina_bar')
                    if st_bar_roi:
                        st_bar_bbox = (
                            wl + int(st_bar_roi.get('x', 0.0) * ww),
                            wt + int(st_bar_roi.get('y', 0.0) * wh),
                            wl + int((st_bar_roi.get('x', 0.0) + st_bar_roi.get('w', 0.0)) * ww),
                            wt + int((st_bar_roi.get('y', 0.0) + st_bar_roi.get('h', 0.0)) * wh),
                        )
                        img_bar = _grab_region(st_bar_bbox)
                        if img_bar is not None and img_bar.size > 0:
                            try:
                                pixel_pct = _detect_bar_pct(img_bar, BAR_COLORS['stamina'])
                            except Exception:
                                pixel_pct = _analyze_bar_fill(img_bar)

                    # ── 融合: OCR 优先 ──
                    if ocr_max > 0 and ocr_cur >= 0:
                        raw_pct = ocr_cur / ocr_max
                    elif max_cached > 0:
                        raw_pct = pixel_pct
                    else:
                        raw_pct = pixel_pct
                    raw_pct = max(0.0, min(1.0, raw_pct))

                    if ema_pct is None:
                        ema_pct = raw_pct
                    else:
                        ema_pct = ema_alpha * raw_pct + (1 - ema_alpha) * ema_pct
                    pct = max(0.0, min(1.0, ema_pct))

                    # ── 推送 GameState ──
                    upd: dict = {'stamina_pct': pct}
                    if max_cached > 0:
                        upd['stamina_current'] = max(0, int(max_cached * pct))
                        upd['stamina_max'] = max_cached
                    if packet_mode:
                        gs = self._state_mgr.state
                        if 'stamina_current' not in upd and gs.stamina_max > 0 and pct > 0.05:
                            upd['stamina_max'] = gs.stamina_max
                            upd['stamina_current'] = max(1, int(gs.stamina_max * pct))

                        if loop_count % level_ocr_every == 0:
                            try:
                                lv_roi = cfg_settings.get_roi('level')
                                if lv_roi:
                                    lv_bbox = (
                                        wl + int(lv_roi.get('x', 0.0) * ww),
                                        wt + int(lv_roi.get('y', 0.0) * wh),
                                        wl + int((lv_roi.get('x', 0.0) + lv_roi.get('w', 0.0)) * ww),
                                        wt + int((lv_roi.get('y', 0.0) + lv_roi.get('h', 0.0)) * wh),
                                    )
                                    lv_img = _grab_region(lv_bbox)
                                    if lv_img is not None and lv_img.size > 0:
                                        try:
                                            h_lv, w_lv = lv_img.shape[:2]
                                            lv_big = cv2.resize(
                                                lv_img,
                                                (max(1, w_lv * 3), max(1, h_lv * 3)),
                                                interpolation=cv2.INTER_CUBIC
                                            )
                                            gray = cv2.cvtColor(lv_big, cv2.COLOR_BGR2GRAY)
                                            thresh = cv2.adaptiveThreshold(
                                                gray, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
                                                cv2.THRESH_BINARY, 11, 2
                                            )
                                            lv_proc = cv2.cvtColor(thresh, cv2.COLOR_GRAY2BGR)
                                        except Exception:
                                            lv_proc = lv_img
                                        lv_txt = _ocr_image(lv_proc)
                                        lv_base, lv_extra = _parse_level_text(lv_txt)
                                        if lv_base > 0 and (gs.level_base <= 0 or lv_base == gs.level_base):
                                            upd['level_base'] = lv_base
                                            if lv_extra > gs.level_extra:
                                                upd['level_extra'] = lv_extra
                            except Exception:
                                pass
                        packet_invalid = (
                            gs.stamina_max <= 0 or
                            (gs.stamina_max >= 100 and gs.stamina_current <= 1)
                        )
                        detector_has_value = (
                            ('stamina_current' in upd and upd['stamina_current'] > 1) or
                            pct > 0.05
                        )
                        stamina_changed = (
                            'stamina_current' in upd and
                            (
                                gs.stamina_max <= 0 or
                                upd.get('stamina_max', gs.stamina_max) != gs.stamina_max or
                                abs(upd['stamina_current'] - gs.stamina_current) >= max(8, int(max(1, gs.stamina_max) * 0.02))
                            )
                        )
                        level_has_value = (
                            ('level_extra' in upd and upd['level_extra'] > gs.level_extra) or
                            ('level_base' in upd and gs.level_base <= 0 and upd['level_base'] > 0)
                        )
                        if (detector_has_value and (packet_invalid or stamina_changed)) or level_has_value:
                            self._state_mgr.update(**upd)
                    else:
                        self._state_mgr.update(**upd)

                except Exception as e:
                    print(f'[SAO] STA 检测异常: {e}')

            self._sta_detector_started = False
            print('[SAO] STA OCR+梯度检测线程已退出')

        threading.Thread(target=_sta_loop, daemon=True, name='sta_pixel').start()

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
            print('[SAO WebView] Hotkeys (pynput): F5=toggle_recognition')
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
        # F5 = toggle recognition
        if 116 in self._hk_pressed:
            threading.Thread(target=self._toggle_recognition, daemon=True).start()
            self._hk_pressed.clear()

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

    @staticmethod
    def _safe_js(s: str) -> str:
        if not s:
            return ''
        return s.replace('\\', '\\\\').replace('"', '\\"').replace("'", "\\'").replace('\n', '\\n')

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
        if self._recognition_engine:
            try:
                self._recognition_engine.stop()
            except Exception:
                pass

        # 退出前保存缓存
        try:
            if hasattr(self, '_state_mgr') and hasattr(self, '_cfg_settings_ref'):
                self._state_mgr.save_cache(self._cfg_settings_ref)
                print('[SAO] 退出前已保存游戏状态缓存')
        except Exception:
            pass

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
        web_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'web')
        url = os.path.join(web_dir, 'panel.html')

        sizes = {
            'control': (280, 240),
            'piano': (700, 120),
            'status': (220, 240),
            'viz': (240, 400),
        }
        w, h = sizes.get(panel_type, (280, 200))

        try:
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
            time.sleep(0.5)
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
        gs_desc = '识别运行中' if self._recognition_active else 'SAO Auto — 待机'
        hp_str = '—'
        sta_str = '—'
        if gs and self._recognition_active and hasattr(gs, 'hp_current'):
            gs_desc = f'HP: {gs.hp_current}/{gs.hp_max}  STA: {gs.stamina_current}/{gs.stamina_max}'
            if gs.hp_max > 0:
                hp_str = f'{gs.hp_current}/{gs.hp_max}'
            if gs.stamina_max > 0:
                sta_str = f'{gs.stamina_current}/{gs.stamina_max}'
        info = {
            'username': self._username, 'level': self._level,
            'xp_pct': round(self._xp_pct, 1), 'profession': self._profession,
            'hp': hp_str, 'sta': sta_str,
            'des': gs_desc,
            'file': '',
        }
        self._eval_menu(f'SAO.updateInfo({json.dumps(info, ensure_ascii=False)})')
        self._sync_all_panels()

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
            time.sleep(0.5)
            try:
                if self.hp_win is None:
                    return
                _ = self.hp_win.x
            except Exception:
                return
            if self._recognition_active and hasattr(self, '_state_mgr'):
                try:
                    gs = self._state_mgr.state
                    if gs.recognition_ok:
                        # OCR 优先, 像素条检测 (hp_pct) 兜底
                        if gs.hp_max > 0:
                            hp, hp_max = gs.hp_current, gs.hp_max
                        elif gs.hp_pct > 0:
                            hp, hp_max = int(gs.hp_pct * 100), 100
                        else:
                            hp, hp_max = 0, 1
                        if gs.stamina_max > 0:
                            sta, sta_max = gs.stamina_current, gs.stamina_max
                        elif gs.stamina_pct > 0:
                            sta, sta_max = int(gs.stamina_pct * 100), 100
                        else:
                            sta, sta_max = -1, -1  # 没数据时不推送
                        level_base = gs.level_base if gs.level_base else self._level
                        if gs.level_extra > 0 and level_base > 0:
                            level_str = f'{level_base}(+{gs.level_extra})'
                        elif level_base > 0:
                            level_str = str(level_base)
                        else:
                            level_str = str(self._level)
                        self._eval_hp(f'updateHP({hp}, {hp_max}, "{level_str}")')
                        self._eval_hp('setPlayState("playing")')
                        # 更新体力条 (已合并到 HP 窗口) — 仅在有数据时推送
                        if sta >= 0 and sta_max > 0:
                            self._eval_hp(f'updateSTA({sta}, {sta_max})')
                        # 更新技能栏
                        if gs.skill_slots:
                            import json as _json
                            self._eval_hp(f'updateSkillBar({_json.dumps(gs.skill_slots)})')
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
                if hasattr(self, '_state_mgr'):
                    gs = self._state_mgr.state
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
            if _panel_tick >= 6 and self._panel_wins:
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
        if self._recognition_engine:
            try:
                self._recognition_engine.stop()
            except Exception:
                pass
        # 退出前保存缓存
        try:
            if hasattr(self, '_state_mgr') and hasattr(self, '_cfg_settings_ref'):
                self._state_mgr.save_cache(self._cfg_settings_ref)
        except Exception:
            pass
        self._destroy_all_panels()
        try:
            self.hp_win.destroy()
        except Exception:
            pass
        try:
            self.menu_win.destroy()
        except Exception:
            pass
