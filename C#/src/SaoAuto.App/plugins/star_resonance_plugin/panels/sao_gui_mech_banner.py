# -*- coding: utf-8 -*-
"""
sao_gui_mech_banner.py — ULW 顶部机制横幅 (SAO Entity UI)

屏幕顶部居中的机制提醒堆叠条: 每行 = 左侧机制色条 + 机制文案 + 右侧剩余秒数
+ 底部倒计时进度条, 进入预警窗口时整行红色脉冲。最多 3 行同显 (第 4 条逐出
最旧), 全程鼠标穿透。对应 webview 端 web/mech_banner.html, 双端外观一致。

性能: 单 Toplevel 复用 (无 destroy/recreate 循环); 行底图 (底框/色条/文案)
按签名渲染一次缓存, 每帧只画进度条矩形 + 秒数文字 + 贴 ≤3 张缓存位图;
倒计时期间 30Hz, 无行时定时器自停。
"""

import ctypes
import time
import threading

from PIL import Image, ImageDraw

from plugins.star_resonance_plugin.panels.sao_gui_alert import (
    _load_font,
    _draw_tracked,
    _tracked_text_width,
)
from plugins.star_resonance_plugin.panels.sao_gui_dps import _ulw_update

import tkinter as tk

_user32 = ctypes.windll.user32
GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008

ROW_W = 560
ROW_H = 64
ROW_GAP = 8
MAX_ROWS = 3
FADE_IN_S = 0.22
FADE_OUT_S = 0.30
STATIC_HOLD_S = 2.6     # countdown=0 的纯提示行停留时长
TICK_MS = 33            # ~30Hz, 仅在有行时运行

PANEL_BG = (10, 14, 22, 168)
PANEL_TOPLINE = (104, 228, 255, 130)
TEXT_MAIN = (244, 248, 255, 255)
TEXT_SECONDS = (243, 175, 18, 255)
TEXT_SECONDS_WARN = (255, 92, 74, 255)
BAR_BG = (255, 255, 255, 42)
BAR_CYAN = (104, 228, 255)
BAR_GOLD = (243, 175, 18)
BAR_WARN = (239, 84, 62)
WARN_TINT = (220, 40, 30, 46)


def _parse_color(value, fallback=(104, 228, 255)):
    try:
        s = str(value or "").strip().lstrip("#")
        if len(s) == 6:
            return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))
    except Exception:
        pass
    return fallback


class MechBannerOverlay:
    """顶部居中机制横幅: show_mechanic(entry) 推入一行, 到点自散。"""

    def __init__(self, root: tk.Tk, settings=None):
        self.root = root
        self.settings = settings
        self._rows: list = []          # [{id, name, text, color, ends_at, total_s, ...}]
        # 线程安全靠 root.after 把所有变更编进 Tk 线程 (无显式锁)
        self._anim_id = None
        self._win = None
        self._hwnd = None
        self._visible = False

        self._canvas_w = ROW_W
        self._canvas_h = MAX_ROWS * ROW_H + (MAX_ROWS - 1) * ROW_GAP
        self._resolve_monitor_geometry()

    def _resolve_monitor_geometry(self):
        """把横幅摆到游戏所在显示器顶部居中 (多屏: 跟随游戏窗口, 非恒主屏)。
        机制横幅在战斗中触发, 游戏几乎总是前台窗口 → 用游戏/前台窗口定位,
        失败回退主屏 GetSystemMetrics。"""
        left = top = None
        right = bottom = None
        try:
            hwnd = 0
            try:
                from plugins.star_resonance_plugin.engines.auto_key_engine import GAME_PROCESS_NAMES  # noqa: F401
                found = _user32.GetForegroundWindow()
                if found:
                    hwnd = found
            except Exception:
                hwnd = _user32.GetForegroundWindow()
            if hwnd:
                MONITOR_DEFAULTTONEAREST = 2
                hmon = _user32.MonitorFromWindow(ctypes.c_void_p(hwnd),
                                                 MONITOR_DEFAULTTONEAREST)
                if hmon:
                    class _RECT(ctypes.Structure):
                        _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                                    ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

                    class _MI(ctypes.Structure):
                        _fields_ = [("cbSize", ctypes.c_ulong), ("rcMonitor", _RECT),
                                    ("rcWork", _RECT), ("dwFlags", ctypes.c_ulong)]
                    mi = _MI()
                    mi.cbSize = ctypes.sizeof(_MI)
                    if _user32.GetMonitorInfoW(hmon, ctypes.byref(mi)):
                        left, top = mi.rcMonitor.left, mi.rcMonitor.top
                        right, bottom = mi.rcMonitor.right, mi.rcMonitor.bottom
        except Exception:
            left = None
        if left is None:
            left, top = 0, 0
            right = _user32.GetSystemMetrics(0)
            bottom = _user32.GetSystemMetrics(1)
        sw, sh = max(1, right - left), max(1, bottom - top)
        self._sw, self._sh = sw, sh
        self._x = left + max(0, (sw - self._canvas_w) // 2)
        self._y = top + max(0, int(sh * 0.08))

    # ── public API (线程安全) ──

    def show_mechanic(self, entry: dict):
        try:
            self.root.after(0, lambda: self._push_row(dict(entry or {})))
        except Exception:
            pass

    def dismiss(self, mech_id: str):
        try:
            self.root.after(0, lambda: self._dismiss_row(str(mech_id or "")))
        except Exception:
            pass

    def clear_all(self):
        try:
            self.root.after(0, self._clear_rows)
        except Exception:
            pass

    # ── window ──

    def _ensure_window(self):
        if self._win is not None:
            try:
                if self._win.winfo_exists():
                    return
            except Exception:
                pass
        win = tk.Toplevel(self.root)
        win.overrideredirect(True)
        win.attributes('-topmost', True)
        win.geometry(f'1x1+{self._x}+{self._y}')
        win.update_idletasks()
        try:
            hwnd = _user32.GetParent(win.winfo_id()) or win.winfo_id()
        except Exception:
            hwnd = win.winfo_id()
        ex = _user32.GetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE)
        _user32.SetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE,
                               ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW
                               | WS_EX_TOPMOST | WS_EX_TRANSPARENT)
        try:
            _GCL_STYLE, _CS_DS = -26, 0x00020000
            _cls = _user32.GetClassLongW(hwnd, _GCL_STYLE)
            if _cls & _CS_DS:
                _user32.SetClassLongW(hwnd, _GCL_STYLE, _cls & ~_CS_DS)
        except Exception:
            pass
        try:
            _user32.SetWindowDisplayAffinity(ctypes.c_void_p(hwnd), 0x00000011)
        except Exception:
            pass
        self._win = win
        self._hwnd = hwnd
        self._visible = True

    # ── rows ──

    def _push_row(self, entry: dict):
        now = time.time()
        countdown_s = 0.0
        try:
            if entry.get('countdown_ms') is not None:
                countdown_s = max(0.0, float(entry.get('countdown_ms') or 0) / 1000.0)
            else:
                countdown_s = max(0.0, float(entry.get('countdown_s') or 0))
        except Exception:
            countdown_s = 0.0
        try:
            pre_warn_s = max(0.0, float(entry.get('pre_warn_ms') or 0) / 1000.0) \
                if entry.get('pre_warn_ms') is not None \
                else max(0.0, float(entry.get('pre_warn_s') or 0))
        except Exception:
            pre_warn_s = 0.0
        total = countdown_s if countdown_s > 0 else STATIC_HOLD_S
        # 中途推入(remaining < countdown)时进度条从剩余比例起 — 与 Web 端对齐
        try:
            if entry.get('remaining_ms') is not None:
                remaining = max(0.0, float(entry.get('remaining_ms') or 0) / 1000.0)
            elif entry.get('remaining_s') is not None:
                remaining = max(0.0, float(entry.get('remaining_s') or 0))
            else:
                remaining = total
        except Exception:
            remaining = total
        remaining = min(remaining, total) if total > 0 else total
        row = {
            'id': str(entry.get('id') or entry.get('mechanic_id') or f'r{int(now * 1000)}'),
            'name': str(entry.get('name') or '机制'),
            'text': str(entry.get('text') or entry.get('banner_text') or ''),
            'color': _parse_color(entry.get('color')),
            'started_at': now,
            'ends_at': now + (remaining if countdown_s > 0 else total),
            'total_s': total,
            'show_bar': countdown_s > 0,
            'pre_warn_s': pre_warn_s,
            'chrome': None,
        }
        # 同一机制重复点名: 原行重置而不是再堆一行
        self._rows = [r for r in self._rows if r['id'] != row['id']]
        self._rows.insert(0, row)
        if len(self._rows) > MAX_ROWS:
            # 淡出中的行(已到点)先被逐出, 别挤掉仍在倒计时的活行 — 与 Web 端只数活行对齐
            live = [r for r in self._rows if r['ends_at'] > now]
            fading = [r for r in self._rows if r['ends_at'] <= now]
            self._rows = (live[:MAX_ROWS] if len(live) >= MAX_ROWS
                          else live + fading[:MAX_ROWS - len(live)])
        self._resolve_monitor_geometry()   # 每次新行重定位 — 游戏可能换了显示器
        self._ensure_window()
        if self._anim_id is None:
            self._tick()

    def _dismiss_row(self, mech_id: str):
        now = time.time()
        for r in self._rows:
            if r['id'] == mech_id:
                r['ends_at'] = min(r['ends_at'], now)

    def _clear_rows(self):
        now = time.time()
        for r in self._rows:
            r['ends_at'] = min(r['ends_at'], now)

    # ── render ──

    def _row_chrome(self, row) -> Image.Image:
        """行底图: 底框 + 顶线 + 色条 + 机制名/文案 (渲染一次缓存)。"""
        if row['chrome'] is not None:
            return row['chrome']
        img = Image.new('RGBA', (ROW_W, ROW_H), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rectangle([0, 0, ROW_W - 1, ROW_H - 1], fill=PANEL_BG)
        d.rectangle([0, 0, ROW_W - 1, 1], fill=PANEL_TOPLINE)
        cr, cg, cb = row['color']
        d.rectangle([0, 0, 4, ROW_H - 1], fill=(cr, cg, cb, 235))
        name = row['name']
        text = row['text']
        label = text if (text and text != name) else name
        font = _load_font('cjk', 20)
        spacing = 1.5
        max_w = ROW_W - 130
        size = 20
        while _tracked_text_width(label, font, spacing) > max_w and size > 13:
            size -= 1
            font = _load_font('cjk', size)
        # 缩到下限仍超宽则省略号截断 — 别压到右侧秒数 (与 Web ellipsis 对齐)
        if _tracked_text_width(label, font, spacing) > max_w and len(label) > 1:
            while len(label) > 1 and \
                    _tracked_text_width(label + '…', font, spacing) > max_w:
                label = label[:-1]
            label += '…'
        _draw_tracked(d, (18, (ROW_H - size) / 2 - 7), label,
                      fill=TEXT_MAIN, font=font, spacing=spacing)
        row['chrome'] = img
        return img

    def _compose(self, now: float) -> Image.Image:
        canvas = Image.new('RGBA', (self._canvas_w, self._canvas_h), (0, 0, 0, 0))
        y = 0
        for row in self._rows:
            frame = self._row_chrome(row).copy()
            d = ImageDraw.Draw(frame)
            remaining = max(0.0, row['ends_at'] - now)
            in_warn = (row['show_bar'] and row['pre_warn_s'] > 0
                       and 0 < remaining <= row['pre_warn_s'])
            if in_warn:
                pulse = 0.5 + 0.5 * abs((now * 3.0) % 2.0 - 1.0)
                tr, tg, tb, ta = WARN_TINT
                d.rectangle([0, 0, ROW_W - 1, ROW_H - 1],
                            fill=(tr, tg, tb, int(ta * (0.6 + 0.4 * pulse))))
            if row['show_bar']:
                bx0, bx1 = 16, ROW_W - 16
                by = ROW_H - 12
                d.rectangle([bx0, by, bx1, by + 5], fill=BAR_BG)
                frac = max(0.0, min(1.0, remaining / max(0.001, row['total_s'])))
                fill_w = int((bx1 - bx0) * frac)
                if fill_w > 0:
                    if in_warn:
                        d.rectangle([bx0, by, bx0 + fill_w, by + 5],
                                    fill=BAR_WARN + (235,))
                    else:
                        cr2, cg2, cb2 = BAR_CYAN
                        gr2, gg2, gb2 = BAR_GOLD
                        for i in range(0, fill_w, 4):
                            t = i / max(1, bx1 - bx0)
                            r = int(cr2 + (gr2 - cr2) * t)
                            g = int(cg2 + (gg2 - cg2) * t)
                            b = int(cb2 + (gb2 - cb2) * t)
                            d.rectangle([bx0 + i, by,
                                         min(bx0 + i + 3, bx0 + fill_w), by + 5],
                                        fill=(r, g, b, 235))
                sec_text = f'{remaining:.1f}s'
                sfont = _load_font('cjk', 21)
                sw = _tracked_text_width(sec_text, sfont, 0.5)
                _draw_tracked(d, (ROW_W - 18 - sw, 10), sec_text,
                              fill=(TEXT_SECONDS_WARN if in_warn else TEXT_SECONDS),
                              font=sfont, spacing=0.5)
            # 行级淡入/淡出 (整行 alpha)
            age = now - row['started_at']
            alpha = 1.0
            if age < FADE_IN_S:
                alpha = age / FADE_IN_S
            if remaining <= 0:
                over = now - row['ends_at']
                alpha = min(alpha, max(0.0, 1.0 - over / FADE_OUT_S))
            if alpha < 1.0:
                a = frame.getchannel('A').point(
                    lambda v, m=alpha: int(v * m))
                frame.putalpha(a)
            canvas.paste(frame, (0, y), frame)
            y += ROW_H + ROW_GAP
        return canvas

    def _tick(self):
        self._anim_id = None
        now = time.time()
        self._rows = [r for r in self._rows
                      if now < r['ends_at'] + FADE_OUT_S]
        if not self._rows:
            self._present_empty()
            return
        try:
            img = self._compose(now)
            if self._hwnd:
                _ulw_update(self._hwnd, img, self._x, self._y, alpha=255)
        except Exception:
            pass
        try:
            self._anim_id = self.root.after(TICK_MS, self._tick)
        except Exception:
            self._anim_id = None

    def _present_empty(self):
        if self._hwnd:
            try:
                blank = Image.new('RGBA', (1, 1), (0, 0, 0, 0))
                _ulw_update(self._hwnd, blank, self._x, self._y, alpha=0)
            except Exception:
                pass

    def destroy(self):
        try:
            if self._anim_id is not None:
                self.root.after_cancel(self._anim_id)
        except Exception:
            pass
        self._anim_id = None
        self._rows = []
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
            self._win = None
            self._hwnd = None
