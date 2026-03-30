"""
Hide & Seek (躲猫猫) Automation Engine.

Sequential image-detection flow with color-filtered template matching.
All ROI coordinates are relative (0.0-1.0) for resolution independence.
Reference resolution: 1920×1080 — positions converted at design time.

Flow:
  Step 0 → detect 1.png (white area)    → Alt+Click → Step 1
  Step 1 → detect 2.png (gray buttons)  → Click     → Step 2
  Step 2 → detect 3.png (gray buttons)  → Click     → Step 3
  Step 3 → detect 4.png (gray buttons)  → Click     → Step 4
  Step 4 → detect 5.png (gray buttons)  → Click     → Step 0  (loop)
"""

from __future__ import annotations

import ctypes
import os
import threading
import time
from typing import Callable, Dict, List, Optional, Tuple

import cv2
import numpy as np

# ── Win32 input structs (mouse + keyboard) ──
INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
KEYEVENTF_KEYUP = 0x0002
VK_MENU = 0x12  # Alt key


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", ctypes.c_long),
        ("dy", ctypes.c_long),
        ("mouseData", ctypes.c_ulong),
        ("dwFlags", ctypes.c_ulong),
        ("time", ctypes.c_ulong),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", ctypes.c_ushort),
        ("wScan", ctypes.c_ushort),
        ("dwFlags", ctypes.c_ulong),
        ("time", ctypes.c_ulong),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class _INPUT_UNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", ctypes.c_ulong), ("iu", _INPUT_UNION)]


# ── ROI step definitions ──
# Each step: relative ROI (top-left x, y, width, height), color ranges, action
# Coordinates computed from 1920×1080 reference.
# "br" = bottom-right anchor, "sz" = box size → tl = br - sz

def _br_to_rel(br_x, br_y, w, h, ref_w=1920, ref_h=1080):
    """Convert bottom-right anchor + size to relative TL + size."""
    tl_x = (br_x - w) / ref_w
    tl_y = (br_y - h) / ref_h
    return (tl_x, tl_y, w / ref_w, h / ref_h)


# Color ranges: (B, G, R) center, tolerance
_WHITE = ((255, 255, 255), 35)
_DARK_GRAY = ((50, 50, 50), 35)
_LIGHT_GRAY = ((200, 200, 200), 35)

STEPS: List[Dict] = [
    {  # Step 0: accept invitation
        'image': '1.png',
        'roi': _br_to_rel(1446, 650, 35, 100),
        'colors': [_WHITE],
        'alt_click': True,
        'name': 'Accept',
    },
    {  # Step 1: confirm dialog 1
        'image': '2.png',
        'roi': _br_to_rel(1893, 1020, 260, 75),
        'colors': [_DARK_GRAY, _LIGHT_GRAY],
        'alt_click': False,
        'name': 'Confirm-1',
    },
    {  # Step 2: confirm dialog 2
        'image': '3.png',
        'roi': _br_to_rel(1869, 956, 310, 75),
        'colors': [_DARK_GRAY, _LIGHT_GRAY],
        'alt_click': False,
        'name': 'Confirm-2',
    },
    {  # Step 3: confirm dialog 3
        'image': '4.png',
        'roi': _br_to_rel(1115, 1005, 310, 75),
        'colors': [_DARK_GRAY, _LIGHT_GRAY],
        'alt_click': False,
        'name': 'Confirm-3',
    },
    {  # Step 4: confirm dialog 4 → loop back
        'image': '5.png',
        'roi': _br_to_rel(1115, 1005, 310, 75),
        'colors': [_DARK_GRAY, _LIGHT_GRAY],
        'alt_click': False,
        'name': 'Confirm-4',
    },
]

# Template matching threshold
_MATCH_THRESHOLD = 0.70


class HideSeekEngine:
    """自动躲猫猫引擎 — CV模板匹配 + 颜色过滤的自动化序列.

    Uses recognition.py's capture functions for screenshots and
    WindowLocator for game window discovery.
    """

    def __init__(
        self,
        locator,
        on_status: Optional[Callable[[str, int], None]] = None,
    ):
        """
        Args:
            locator: WindowLocator instance.
            on_status: Callback(message, step_index) for status updates.
        """
        self._locator = locator
        self._on_status = on_status
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._current_step = 0
        self._templates: Dict[str, Optional[np.ndarray]] = {}
        self._lock = threading.Lock()
        self._assets_dir = os.path.join(os.path.dirname(__file__), 'assets')

    # ── Public API ──

    @property
    def running(self) -> bool:
        return self._running

    @property
    def current_step(self) -> int:
        return self._current_step

    def start(self):
        if self._running:
            return
        self._running = True
        self._current_step = 0
        self._load_templates()
        self._thread = threading.Thread(
            target=self._run, daemon=True, name='hide_seek_engine')
        self._thread.start()
        self._fire_status('Started — waiting for Step 0', 0)

    def stop(self):
        self._running = False
        t = self._thread
        if t and t.is_alive():
            t.join(timeout=2.0)
        self._thread = None
        self._fire_status('Stopped', -1)

    # ── Template loading ──

    def _load_templates(self):
        """Pre-load all reference images from assets/."""
        self._templates.clear()
        for step in STEPS:
            img_name = step['image']
            path = os.path.join(self._assets_dir, img_name)
            if os.path.isfile(path):
                tpl = cv2.imread(path, cv2.IMREAD_COLOR)
                if tpl is not None:
                    self._templates[img_name] = tpl
                    continue
            self._templates[img_name] = None
            print(f'[HideSeek] WARNING: template not found: {path}')

    # ── Main loop ──

    def _run(self):
        while self._running:
            time.sleep(1.0)
            if not self._running:
                break
            try:
                self._tick()
            except Exception as e:
                self._fire_status(f'Error: {e}', self._current_step)

    def _tick(self):
        # Find game window
        found = self._locator.find_game_window()
        if not found:
            self._fire_status('Game window not found', self._current_step)
            return

        hwnd, _title, client_rect = found
        cl, ct, cr, cb = client_rect
        win_w = max(1, cr - cl)
        win_h = max(1, cb - ct)

        # Capture full client area
        screenshot = self._capture(hwnd, client_rect)
        if screenshot is None:
            self._fire_status('Screenshot failed', self._current_step)
            return

        step = STEPS[self._current_step]
        tpl = self._templates.get(step['image'])
        if tpl is None:
            self._fire_status(f'Template missing: {step["image"]}', self._current_step)
            return

        # Compute absolute ROI from relative coords
        rx, ry, rw, rh = step['roi']
        roi_x = max(0, int(rx * win_w))
        roi_y = max(0, int(ry * win_h))
        roi_w = max(1, int(rw * win_w))
        roi_h = max(1, int(rh * win_h))
        roi_x2 = min(win_w, roi_x + roi_w)
        roi_y2 = min(win_h, roi_y + roi_h)

        # Extract ROI from screenshot
        roi_img = screenshot[roi_y:roi_y2, roi_x:roi_x2]
        if roi_img.size == 0:
            self._fire_status('ROI empty', self._current_step)
            return

        # Color filter: create combined mask for all expected colors
        colors = step['colors']
        mask = self._build_color_mask(roi_img, colors)

        # Apply mask — keep only matching-color pixels
        filtered = cv2.bitwise_and(roi_img, roi_img, mask=mask)

        # Resize template to fit ROI dimensions if needed
        tpl_resized = self._resize_template(tpl, roi_img.shape[:2])
        if tpl_resized is None:
            self._fire_status(f'Template too large for ROI', self._current_step)
            return

        # Also filter template with same colors
        tpl_mask = self._build_color_mask(tpl_resized, colors)
        tpl_filtered = cv2.bitwise_and(tpl_resized, tpl_resized, mask=tpl_mask)

        # Template match on filtered images
        match_pos, confidence = self._template_match(filtered, tpl_filtered)
        if match_pos is None:
            # Fallback: try raw template match without color filtering
            match_pos, confidence = self._template_match(roi_img, tpl_resized)

        if match_pos is None or confidence < _MATCH_THRESHOLD:
            self._fire_status(
                f'Step {self._current_step} ({step["name"]}): '
                f'not found (conf={confidence:.2f})',
                self._current_step,
            )
            return

        # Found! Compute click position in screen coordinates
        mx, my = match_pos
        th, tw = tpl_resized.shape[:2]
        click_x = cl + roi_x + mx + tw // 2
        click_y = ct + roi_y + my + th // 2

        self._fire_status(
            f'Step {self._current_step} ({step["name"]}): '
            f'MATCH conf={confidence:.2f} → click ({click_x}, {click_y})',
            self._current_step,
        )

        # Perform action
        if step.get('alt_click'):
            self._alt_click(click_x, click_y)
        else:
            self._click(click_x, click_y)

        # Advance to next step (wrap around)
        with self._lock:
            self._current_step = (self._current_step + 1) % len(STEPS)

    # ── Screenshot capture ──

    def _capture(
        self, hwnd: int, client_rect: Tuple[int, int, int, int]
    ) -> Optional[np.ndarray]:
        """Capture client area of game window → BGR numpy array."""
        try:
            from recognition import _capture_hwnd_client
            img, method = _capture_hwnd_client(hwnd, client_rect)
            return img
        except Exception:
            pass
        # Fallback: mss screen grab of the client rect
        try:
            import mss
            cl, ct, cr, cb = client_rect
            with mss.mss() as sct:
                region = {'left': cl, 'top': ct, 'width': cr - cl, 'height': cb - ct}
                raw = sct.grab(region)
                img = np.frombuffer(raw.bgra, dtype=np.uint8).reshape(
                    raw.height, raw.width, 4)
                return img[:, :, :3].copy()  # BGRA → BGR
        except Exception:
            return None

    # ── Color filtering ──

    @staticmethod
    def _build_color_mask(
        img: np.ndarray,
        colors: list,
    ) -> np.ndarray:
        """Build a combined binary mask for pixels matching any of the given colors.

        Each color entry: ((B, G, R), tolerance)
        """
        h, w = img.shape[:2]
        combined = np.zeros((h, w), dtype=np.uint8)
        for (b, g, r), tol in colors:
            lower = np.array([max(0, b - tol), max(0, g - tol), max(0, r - tol)],
                             dtype=np.uint8)
            upper = np.array([min(255, b + tol), min(255, g + tol), min(255, r + tol)],
                             dtype=np.uint8)
            mask = cv2.inRange(img, lower, upper)
            combined = cv2.bitwise_or(combined, mask)
        # Dilate slightly to join nearby matching pixels
        kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (3, 3))
        combined = cv2.dilate(combined, kernel, iterations=1)
        return combined

    # ── Template matching ──

    @staticmethod
    def _resize_template(
        tpl: np.ndarray,
        roi_shape: Tuple[int, int],
    ) -> Optional[np.ndarray]:
        """Resize template if it's larger than the ROI; scale down proportionally."""
        th, tw = tpl.shape[:2]
        rh, rw = roi_shape
        if th > rh or tw > rw:
            scale = min(rh / th, rw / tw) * 0.95
            if scale <= 0.1:
                return None
            new_w = max(1, int(tw * scale))
            new_h = max(1, int(th * scale))
            return cv2.resize(tpl, (new_w, new_h), interpolation=cv2.INTER_AREA)
        return tpl

    @staticmethod
    def _template_match(
        img: np.ndarray,
        tpl: np.ndarray,
    ) -> Tuple[Optional[Tuple[int, int]], float]:
        """Run template matching. Returns ((x, y), confidence) or (None, 0.0)."""
        if img is None or tpl is None:
            return None, 0.0
        ih, iw = img.shape[:2]
        th, tw = tpl.shape[:2]
        if th > ih or tw > iw:
            return None, 0.0
        if th <= 0 or tw <= 0:
            return None, 0.0
        try:
            # Convert to grayscale for matching
            img_gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if len(img.shape) == 3 else img
            tpl_gray = cv2.cvtColor(tpl, cv2.COLOR_BGR2GRAY) if len(tpl.shape) == 3 else tpl
            result = cv2.matchTemplate(img_gray, tpl_gray, cv2.TM_CCOEFF_NORMED)
            _, max_val, _, max_loc = cv2.minMaxLoc(result)
            return max_loc, float(max_val)
        except Exception:
            return None, 0.0

    # ── Input simulation ──

    @staticmethod
    def _send_mouse_click(screen_x: int, screen_y: int):
        """Send a left mouse click at absolute screen coordinates via SendInput."""
        user32 = ctypes.windll.user32
        sm_cx = user32.GetSystemMetrics(0)  # screen width
        sm_cy = user32.GetSystemMetrics(1)  # screen height
        # Normalize to 0-65535 range for MOUSEEVENTF_ABSOLUTE
        abs_x = int(screen_x * 65535 / max(1, sm_cx - 1))
        abs_y = int(screen_y * 65535 / max(1, sm_cy - 1))
        extra = ctypes.c_ulong(0)
        # Move + press
        mi_down = MOUSEINPUT(
            dx=abs_x, dy=abs_y, mouseData=0,
            dwFlags=MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN,
            time=0, dwExtraInfo=ctypes.pointer(extra),
        )
        # Release
        extra2 = ctypes.c_ulong(0)
        mi_up = MOUSEINPUT(
            dx=abs_x, dy=abs_y, mouseData=0,
            dwFlags=MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTUP,
            time=0, dwExtraInfo=ctypes.pointer(extra2),
        )
        evt_down = INPUT(type=INPUT_MOUSE, iu=_INPUT_UNION(mi=mi_down))
        evt_up = INPUT(type=INPUT_MOUSE, iu=_INPUT_UNION(mi=mi_up))
        user32.SendInput(1, ctypes.byref(evt_down), ctypes.sizeof(INPUT))
        time.sleep(0.05)
        user32.SendInput(1, ctypes.byref(evt_up), ctypes.sizeof(INPUT))

    @staticmethod
    def _send_key(vk: int, up: bool = False):
        extra = ctypes.c_ulong(0)
        ki = KEYBDINPUT(
            wVk=vk, wScan=0,
            dwFlags=KEYEVENTF_KEYUP if up else 0,
            time=0, dwExtraInfo=ctypes.pointer(extra),
        )
        evt = INPUT(type=INPUT_KEYBOARD, iu=_INPUT_UNION(ki=ki))
        ctypes.windll.user32.SendInput(1, ctypes.byref(evt), ctypes.sizeof(INPUT))

    @classmethod
    def _click(cls, screen_x: int, screen_y: int):
        """Simple left click at screen coords."""
        cls._send_mouse_click(screen_x, screen_y)

    @classmethod
    def _alt_click(cls, screen_x: int, screen_y: int):
        """Alt + left click at screen coords."""
        cls._send_key(VK_MENU, up=False)
        time.sleep(0.05)
        cls._send_mouse_click(screen_x, screen_y)
        time.sleep(0.05)
        cls._send_key(VK_MENU, up=True)

    # ── Status callback ──

    def _fire_status(self, message: str, step: int):
        print(f'[HideSeek] {message}')
        cb = self._on_status
        if cb:
            try:
                cb(message, step)
            except Exception:
                pass
