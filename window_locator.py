# -*- coding: utf-8 -*-
"""
SAO Auto — 游戏窗口定位

查找游戏窗口句柄、获取矩形，输出归一化坐标。
"""

import ctypes
import ctypes.wintypes
from typing import Optional, Tuple, List

from config import GAME_WINDOW_KEYWORDS, GAME_PROCESS_NAMES


# ═══════════════════════════════════════════════
#  Win32 API
# ═══════════════════════════════════════════════
user32 = ctypes.windll.user32


def _get_client_rect_screen(hwnd: int) -> Optional[Tuple[int, int, int, int]]:
    """获取窗口客户区在屏幕上的绝对坐标 (left, top, right, bottom)。
    使用 GetClientRect + ClientToScreen 避免 GetWindowRect 包含标题栏/边框。"""
    try:
        cr = ctypes.wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(cr))
        cw, ch = cr.right, cr.bottom
        if cw < 100 or ch < 100:
            return None
        pt = ctypes.wintypes.POINT(0, 0)
        user32.ClientToScreen(hwnd, ctypes.byref(pt))
        return (pt.x, pt.y, pt.x + cw, pt.y + ch)
    except Exception:
        return None


def _enum_windows() -> List[Tuple[int, str, tuple]]:
    """枚举所有可见窗口: [(hwnd, title, (l,t,r,b)), ...]
    返回的 rect 是 **客户区** 的屏幕坐标 (不含标题栏/边框)。"""
    results = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int))
    def _cb(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, buf, 256)
            title = buf.value
            if title:
                cr = _get_client_rect_screen(hwnd)
                if cr is not None:
                    w = cr[2] - cr[0]
                    h = cr[3] - cr[1]
                    if w > 200 and h > 200:
                        results.append((hwnd, title, cr))
        return True

    user32.EnumWindows(_cb, 0)
    return results


# ═══════════════════════════════════════════════
#  进程名辅助
# ═══════════════════════════════════════════════
def _get_process_name(hwnd: int) -> str:
    """通过 hwnd → pid → OpenProcess → QueryFullProcessImageNameW 获取 exe 名。"""
    try:
        pid = ctypes.wintypes.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        h = ctypes.windll.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
        if not h:
            return ''
        try:
            buf = ctypes.create_unicode_buffer(512)
            size = ctypes.wintypes.DWORD(512)
            ok = ctypes.windll.kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size))
            if ok:
                import os as _os
                return _os.path.basename(buf.value).lower()
        finally:
            ctypes.windll.kernel32.CloseHandle(h)
    except Exception:
        pass
    return ''


class WindowLocator:
    """游戏窗口定位器"""

    def __init__(self, keywords: Optional[List[str]] = None,
                 process_names: Optional[List[str]] = None):
        self._keywords = keywords or GAME_WINDOW_KEYWORDS
        self._process_names = [p.lower() for p in (process_names or GAME_PROCESS_NAMES)]
        self._cached_hwnd: int = 0
        self._cached_rect: Optional[tuple] = None
        self._log_once = True

    def _match(self, hwnd: int, title: str) -> bool:
        """标题关键词 **或** 进程名匹配 → True"""
        title_lower = title.lower()
        for kw in self._keywords:
            if kw.lower() in title_lower:
                return True
        if self._process_names:
            exe = _get_process_name(hwnd)
            if exe and exe in self._process_names:
                return True
        return False

    def find_game_window(self) -> Optional[Tuple[int, str, tuple]]:
        """
        查找游戏窗口。返回客户区坐标。

        Returns:
            (hwnd, title, (left, top, right, bottom)) 或 None
        """
        # 优先检查缓存句柄是否仍有效
        if self._cached_hwnd:
            if user32.IsWindow(self._cached_hwnd) and user32.IsWindowVisible(self._cached_hwnd):
                cr = _get_client_rect_screen(self._cached_hwnd)
                if cr is not None:
                    w, h = cr[2] - cr[0], cr[3] - cr[1]
                    if w > 200 and h > 200:
                        self._cached_rect = cr
                        buf = ctypes.create_unicode_buffer(256)
                        user32.GetWindowTextW(self._cached_hwnd, buf, 256)
                        return (self._cached_hwnd, buf.value, cr)

        # 全量枚举 — 标题关键词 + 进程名双重匹配
        windows = _enum_windows()
        for hwnd, title, rect in windows:
            if self._match(hwnd, title):
                self._cached_hwnd = hwnd
                self._cached_rect = rect
                if self._log_once:
                    self._log_once = False
                    w, h = rect[2] - rect[0], rect[3] - rect[1]
                    exe = _get_process_name(hwnd)
                    print(f'[识别] 找到游戏窗口: "{title}" [{exe}] '
                          f'client={rect[0]},{rect[1]}→{rect[2]},{rect[3]} '
                          f'({w}x{h})')
                return (hwnd, title, rect)

        self._cached_hwnd = 0
        self._cached_rect = None
        return None

    def get_rect(self) -> Optional[Tuple[int, int, int, int]]:
        """获取窗口客户区矩形 (left, top, right, bottom)"""
        result = self.find_game_window()
        return result[2] if result else None

    def get_size(self) -> Optional[Tuple[int, int]]:
        """获取窗口客户区宽高"""
        rect = self.get_rect()
        if rect:
            return (rect[2] - rect[0], rect[3] - rect[1])
        return None

    def roi_to_pixels(self, roi: dict, rect: Optional[tuple] = None) -> Optional[Tuple[int, int, int, int]]:
        """
        将百分比 ROI 转换为像素坐标 bbox。

        Args:
            roi: {'x': float, 'y': float, 'w': float, 'h': float}  (0.0~1.0)
            rect: 窗口客户区矩形，None 则自动获取

        Returns:
            (left, top, right, bottom) 绝对屏幕坐标，或 None
        """
        if rect is None:
            rect = self.get_rect()
        if rect is None:
            return None

        left, top, right, bottom = rect
        ww = right - left
        wh = bottom - top

        rx = left + int(ww * roi['x'])
        ry = top  + int(wh * roi['y'])
        rr = rx   + int(ww * roi['w'])
        rb = ry   + int(wh * roi['h'])

        return (rx, ry, rr, rb)

    @staticmethod
    def get_screen_size() -> Tuple[int, int]:
        """主屏幕分辨率"""
        return (user32.GetSystemMetrics(0), user32.GetSystemMetrics(1))
