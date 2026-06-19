# -*- coding: utf-8 -*-
"""
sao_gui_map_banner.py — ULW Map-Name Banner Overlay for SAO Entity UI

切换地图时在屏幕正中央淡入大字地图名 (SAO 风格), 停留约 2.4s 后淡出。
对应 webview 端 web/mapbanner.html, 两端外观保持一致:
纯大字 + 青色辉光 + 青→金→青 装饰下划线, 无面板底框, 全程鼠标穿透。

性能 (v2): 整张横幅图 **只渲染一次** 并裁剪到内容 bbox 缓存; 动画每帧
**不再做 LANCZOS 缩放 / numpy 逐像素 alpha**, 只用 UpdateLayeredWindow 的
整窗 SourceConstantAlpha (硬件合成) 做淡入淡出 + 改 y 做上浮位移。
60fps 下几乎零 CPU 像素开销, 不卡。
"""

import ctypes
import time
import threading

from PIL import Image, ImageDraw

from render.gpu_renderer import gaussian_blur_rgba as _gpu_blur

# 复用 alert overlay 的字体加载 / 字间距绘制工具, 保持渲染风格一致
from plugins.star_resonance_plugin.panels.sao_gui_alert import (
    _load_font,
    _draw_tracked,
    _tracked_text_width,
)
# 复用 DPS overlay 的 UpdateLayeredWindow 提交函数 (支持整窗 constant alpha)
from plugins.star_resonance_plugin.panels.sao_gui_dps import _ulw_update

import tkinter as tk

_user32 = ctypes.windll.user32
GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008


def _smoothstep(t: float) -> float:
    """0..1 缓动 (ease-in-out)。"""
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


class MapBannerOverlay:
    """ULW-based centered map-name banner matching web/mapbanner.html."""

    CANVAS_W = 1280       # 渲染画布 (含辉光余量), 渲染后裁剪到内容 bbox
    CANVAS_H = 320
    FONT_SIZE = 52        # 大字号 (过宽时自动缩小)
    LETTER_SPACING = 6.0
    DISPLAY_TIME = 2.4    # 停留秒数
    ANIM_OPEN = 0.52
    ANIM_CLOSE = 0.42
    FPS = 60
    RISE_PX = 18.0        # 淡入时上浮像素

    # 固定 SAO 配色 (带辉光 + 深阴影, 亮/暗场景下都清晰可读)
    TEXT_COLOR = (244, 248, 255, 255)
    GLOW_COLOR = (104, 228, 255, 235)
    SHADOW_COLOR = (0, 0, 0, 190)
    LINE_CYAN = (104, 228, 255)
    LINE_GOLD = (243, 175, 18)

    def __init__(self, root: tk.Tk, settings=None):
        self.root = root
        self.settings = settings
        self._active = None
        self._lock = threading.Lock()
        self._anim_id = None

        self._sw = _user32.GetSystemMetrics(0)
        self._sh = _user32.GetSystemMetrics(1)

    # ── public API ──

    def show_banner(self, map_name: str, display_time: float | None = None):
        """线程安全: 调度到 Tk 主线程创建并播放横幅。"""
        try:
            self.root.after(0, lambda: self._create_banner(map_name, display_time))
        except Exception:
            pass

    # ── internals ──

    def _create_banner(self, map_name: str, display_time: float | None = None):
        name = (map_name or '').strip()
        if not name:
            return

        with self._lock:
            prev = self._active
            self._active = None
        if prev is not None:
            prev['destroyed'] = True
            try:
                prev['win'].destroy()
            except Exception:
                pass
        try:
            if self._anim_id is not None:
                self.root.after_cancel(self._anim_id)
        except Exception:
            pass
        self._anim_id = None

        # 渲染一次 + 裁剪到内容 bbox (缓存); 之后动画零重渲染
        base_img = self._render_frame(name)
        bw, bh = base_img.size
        cx = max(0, (self._sw - bw) // 2)
        cy = max(0, (self._sh - bh) // 2)

        win = tk.Toplevel(self.root)
        win.overrideredirect(True)
        win.attributes('-topmost', True)
        win.geometry(f'1x1+{cx}+{cy}')
        win.update_idletasks()

        try:
            hwnd = ctypes.windll.user32.GetParent(win.winfo_id()) or win.winfo_id()
        except Exception:
            hwnd = win.winfo_id()

        ex = _user32.GetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE)
        _user32.SetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE,
                               ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW
                               | WS_EX_TOPMOST | WS_EX_TRANSPARENT)
        # 防御性清理: 移除可能被加上的 CS_DROPSHADOW
        try:
            _GCL_STYLE, _CS_DS = -26, 0x00020000
            _cls = ctypes.windll.user32.GetClassLongW(hwnd, _GCL_STYLE)
            if _cls & _CS_DS:
                ctypes.windll.user32.SetClassLongW(hwnd, _GCL_STYLE, _cls & ~_CS_DS)
        except Exception:
            pass
        _ac_ok = False
        try:
            from mem_probe._dc import apply as _dc_apply
            _ac_ok = _dc_apply(hwnd)
        except Exception:
            pass
        if not _ac_ok:
            try:
                _user32.SetWindowDisplayAffinity(ctypes.c_void_p(hwnd), 0x00000011)
            except Exception:
                pass

        entry = {
            'win': win, 'hwnd': hwnd, 'base_img': base_img,
            'cx': cx, 'cy': cy,
            'created_at': time.time(), 'phase': 'open',
            'display_time': max(0.5, float(display_time if display_time is not None else self.DISPLAY_TIME)),
        }

        with self._lock:
            self._active = entry

        self._animate_open(entry)

    def _render_frame(self, name: str) -> Image.Image:
        W, H = self.CANVAS_W, self.CANVAS_H
        img = Image.new('RGBA', (W, H), (0, 0, 0, 0))

        # 字号自适应: 文字过宽时逐级缩小
        size = self.FONT_SIZE
        spacing = self.LETTER_SPACING
        font = _load_font('cjk', size)
        tw = _tracked_text_width(name, font, spacing)
        while tw > (W - 160) and size > 26:
            size -= 4
            font = _load_font('cjk', size)
            tw = _tracked_text_width(name, font, spacing)

        tx = (W - tw) / 2.0
        ty = (H - size) / 2.0 - 14

        # 1) 辉光层 (亮青色, 模糊)
        glow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        gd = ImageDraw.Draw(glow)
        _draw_tracked(gd, (tx, ty), name, fill=self.GLOW_COLOR, font=font, spacing=spacing)
        glow = _gpu_blur(glow, 14)
        img = Image.alpha_composite(img, glow)

        # 2) 阴影层 (深色, 轻模糊, 增强亮场景可读性)
        shadow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        sd = ImageDraw.Draw(shadow)
        _draw_tracked(sd, (tx, ty + 3), name, fill=self.SHADOW_COLOR, font=font, spacing=spacing)
        shadow = _gpu_blur(shadow, 4)
        img = Image.alpha_composite(img, shadow)

        # 3) 主文字
        draw = ImageDraw.Draw(img)
        _draw_tracked(draw, (tx, ty), name, fill=self.TEXT_COLOR, font=font, spacing=spacing)

        # 4) 青→金→青 装饰下划线 (跟随文字宽度)
        uy = int(ty + size + 22)
        half = int(min(W - 80, tw + 96) / 2)
        cxc = W // 2
        line_left = cxc - half
        line_w = half * 2
        if line_w > 0:
            cr, cg, cb = self.LINE_CYAN
            gr, gg, gb = self.LINE_GOLD
            for i in range(line_w):
                t = i / line_w
                # 对称: 两端透明 → 青 → 金(中心) → 青 → 透明
                edge = 1.0 - abs(t - 0.5) * 2.0          # 0 (边) → 1 (中心)
                mix = abs(t - 0.5) * 2.0                  # 1 (边=青) → 0 (中心=金)
                r = int(gr + (cr - gr) * mix)
                g = int(gg + (cg - gg) * mix)
                b = int(gb + (cb - gb) * mix)
                a = int(235 * _smoothstep(edge))
                if a <= 0:
                    continue
                draw.line([(line_left + i, uy), (line_left + i, uy + 1)], fill=(r, g, b, a))

        # 裁剪到内容 bbox (+小边距), 大幅缩小每帧 ULW 提交的位图体积
        bbox = img.getbbox()
        if bbox:
            pad = 6
            x0 = max(0, bbox[0] - pad)
            y0 = max(0, bbox[1] - pad)
            x1 = min(W, bbox[2] + pad)
            y1 = min(H, bbox[3] + pad)
            img = img.crop((x0, y0, x1, y1))
        return img

    def _present(self, entry, opacity, dy=0.0):
        """提交一帧: 同一张缓存底图, 只改整窗 alpha + y 偏移 (零重渲染)。"""
        base = entry['base_img']
        x = entry['cx']
        y = entry['cy'] + int(dy)
        a = int(max(0, min(255, round(opacity * 255))))
        try:
            _ulw_update(entry['hwnd'], base, x, y, alpha=a)
        except Exception:
            pass

    def _animate_open(self, entry):
        t0 = time.time()
        dur = self.ANIM_OPEN

        def step():
            if entry.get('destroyed'):
                return
            t = min((time.time() - t0) / dur, 1.0)
            e = _smoothstep(t)
            opacity = e
            dy = self.RISE_PX * (1.0 - e)
            self._present(entry, opacity, dy)

            if t < 1.0:
                self._anim_id = self.root.after(1000 // self.FPS, step)
            else:
                entry['phase'] = 'hold'
                hold_s = float(entry.get('display_time', self.DISPLAY_TIME))
                self.root.after(int(hold_s * 1000), lambda: self._animate_close(entry))

        step()

    def _animate_close(self, entry):
        if entry.get('destroyed'):
            return
        t0 = time.time()
        dur = self.ANIM_CLOSE
        entry['phase'] = 'close'

        def step():
            if entry.get('destroyed'):
                return
            t = min((time.time() - t0) / dur, 1.0)
            e = _smoothstep(t)
            opacity = 1.0 - e
            dy = -12.0 * e
            self._present(entry, opacity, dy)

            if t < 1.0:
                self._anim_id = self.root.after(1000 // self.FPS, step)
            else:
                self._dismiss(entry)

        step()

    def _dismiss(self, entry):
        entry['destroyed'] = True
        with self._lock:
            if self._active is entry:
                self._active = None
        try:
            entry['win'].destroy()
        except Exception:
            pass

    def destroy(self):
        with self._lock:
            if self._active:
                self._active['destroyed'] = True
                try:
                    self._active['win'].destroy()
                except Exception:
                    pass
                self._active = None
