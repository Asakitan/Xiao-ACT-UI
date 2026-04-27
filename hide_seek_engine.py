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
import sys
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
MOUSEEVENTF_VIRTUALDESK = 0x4000
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
KEYEVENTF_KEYUP = 0x0002
VK_MENU = 0x12  # Alt key
SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79


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
_LIGHT_GRAY = ((200, 200, 200), 55)  # widened tolerance 35→55

# Debug: save ROI/filtered/template images to temp/debug_hs/
# Automatically disabled in packaged (frozen) builds.
_DEBUG_SAVE = not getattr(sys, 'frozen', False)
_DEBUG_DIR = os.path.join(os.path.dirname(__file__), 'temp', 'debug_hs')
_DEBUG_INTERVAL_S = 1.0  # minimum seconds between saves per step

STEPS: List[Dict] = [
    {  # Step 0: accept invitation
        'image': '1.png',
        'roi': _br_to_rel(1446, 650, 35, 100),
        'colors': [_WHITE],
        'alt_click': True,
        'sqdiff': False,
        'name': 'Accept',
    },
    {  # Step 1: confirm dialog 1  ("匹配进入" — must NOT match "取消匹配")
        'image': '2.png',
        'roi': _br_to_rel(1893, 1020, 260, 75),
        'colors': [_DARK_GRAY, _LIGHT_GRAY],
        'alt_click': False,
        'sqdiff': False,
        'name': 'Confirm-1',
    },
    {  # Step 2: confirm dialog 2  ("接受" — low-contrast, needs SQDIFF)
        'image': '3.png',
        'roi': _br_to_rel(1869, 956, 310, 75),
        'colors': [_DARK_GRAY, _LIGHT_GRAY],
        'alt_click': False,
        'sqdiff': True,
        'name': 'Confirm-2',
    },
    {  # Step 3: confirm dialog 3
        'image': '4.png',
        'roi': _br_to_rel(1115, 1005, 310, 75),
        'colors': [_DARK_GRAY, _LIGHT_GRAY],
        'alt_click': False,
        'sqdiff': True,
        'name': 'Confirm-3',
    },
    {  # Step 4: confirm dialog 4 → loop back
        'image': '5.png',
        'roi': _br_to_rel(1115, 1005, 310, 75),
        'colors': [_DARK_GRAY, _LIGHT_GRAY],
        'alt_click': False,
        'sqdiff': True,
        'name': 'Confirm-4',
    },
]

# Template matching threshold
_MATCH_THRESHOLD = 0.70
# TM_SQDIFF_NORMED threshold (lower = more similar; catches white-on-white buttons)
_SQDIFF_THRESHOLD = 0.10


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
        # v2.1.20: 在 PyInstaller onedir 打包下, __file__ 位于 runtime/ 子目录,
        # 但 assets/ 被 build_release.bat 移到 exe 根目录, 因此用 module-dir
        # 拼出来的 runtime/assets/ 永远不存在 → 5 个 template 全部加载失败 →
        # 引擎线程能跑但 _match_template 永远没结果 → "启动了不会有效果".
        # 改为优先使用 config.BASE_DIR (= exe 顶层目录), 再 fallback 到模块目
        # 录, 兼容源码运行 / onedir / 旧 onefile 三种布局.
        try:
            from config import BASE_DIR as _BASE_DIR
        except Exception:
            _BASE_DIR = os.path.dirname(os.path.abspath(__file__))
        candidates = [
            os.path.join(_BASE_DIR, 'assets'),
            os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets'),
        ]
        self._assets_dir = next(
            (p for p in candidates if os.path.isdir(p)), candidates[0])
        # Track the step we just clicked + when, so we can skip fallback
        # to that SAME step briefly (its UI may still linger on screen).
        self._last_executed_step: int = -1
        self._last_click_ts: float = 0.0
        # Cooldown (seconds) before we allow re-detecting the SAME step
        # we just clicked.  Other earlier steps are always fair game.
        self.STEP_COOLDOWN_S: float = 5.0
        # Debug save: last save timestamp per step index
        self._debug_last_save: Dict[int, float] = {}
        if _DEBUG_SAVE:
            os.makedirs(_DEBUG_DIR, exist_ok=True)

    # ── Public API ──

    @property
    def running(self) -> bool:
        return self._running and self._thread is not None and self._thread.is_alive()

    @property
    def thread_alive(self) -> bool:
        """True only if the background thread is actually alive."""
        t = self._thread
        return t is not None and t.is_alive()

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

    def resume(self):
        """Re-launch the worker thread WITHOUT resetting the current step.

        Used by the external watchdog when the thread died unexpectedly.
        """
        self._running = False
        t = self._thread
        if t and t.is_alive():
            t.join(timeout=2.0)
        # Keep _current_step as-is
        self._running = True
        if not self._templates:
            self._load_templates()
        self._thread = threading.Thread(
            target=self._run, daemon=True, name='hide_seek_engine')
        self._thread.start()
        self._fire_status(
            f'Resumed at Step {self._current_step}', self._current_step)

    def restart(self):
        """Full restart — reset to step 0."""
        self.stop()
        self.start()

    # ── Main loop ──

    def _run(self):
        self._set_thread_dpi_awareness()
        try:
            while self._running:
                time.sleep(1.0)
                if not self._running:
                    break
                try:
                    self._tick()
                except Exception as e:
                    self._fire_status(f'Error: {e}', self._current_step)
                    import traceback
                    traceback.print_exc()
        except BaseException as e:
            print(f'[HideSeek] Thread fatal error: {e}')
            import traceback
            traceback.print_exc()
        finally:
            self._running = False
            print('[HideSeek] Thread exited')

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

        cur = self._current_step

        # After a click, ALL earlier steps' UI may still linger on screen
        # for a few seconds.  Skip fallback to ANY earlier step during the
        # cooldown period to prevent looping (0→1→2→0→1→2…).
        # The current step itself is always checked — no timeout limit.
        since_click = time.time() - self._last_click_ts
        fallback_allowed = (since_click >= self.STEP_COOLDOWN_S)

        # 1) Always try current step (no cooldown restriction)
        hit = self._try_detect_step(cur, screenshot, win_w, win_h, cl, ct)
        if hit is not None:
            step_idx, click_x, click_y, conf = hit
            self._execute_step(step_idx, click_x, click_y, conf)
            return

        # 2) Check ALL earlier steps (0 .. cur-1) only after cooldown
        if cur > 0 and fallback_allowed:
            for fallback_idx in range(cur):
                hit = self._try_detect_step(
                    fallback_idx, screenshot, win_w, win_h, cl, ct)
                if hit is not None:
                    step_idx, click_x, click_y, conf = hit
                    self._fire_status(
                        f'Fallback: detected Step {step_idx} '
                        f'({STEPS[step_idx]["name"]}) while on Step {cur}',
                        step_idx,
                    )
                    with self._lock:
                        self._current_step = step_idx
                    self._execute_step(step_idx, click_x, click_y, conf)
                    return

        # Nothing matched — keep waiting
        self._fire_status(
            f'Step {cur} ({STEPS[cur]["name"]}): waiting …',
            cur,
        )

    # ── Step detection (pure, no side effects) ──

    def _try_detect_step(
        self,
        step_idx: int,
        screenshot: np.ndarray,
        win_w: int,
        win_h: int,
        cl: int,
        ct: int,
    ) -> Optional[Tuple[int, int, int, float]]:
        """Try to detect a specific step in the screenshot.

        Returns (step_idx, click_x, click_y, confidence) on success, else None.
        """
        step = STEPS[step_idx]
        tpl = self._templates.get(step['image'])
        if tpl is None:
            return None

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
            return None

        # Color filter
        colors = step['colors']
        mask = self._build_color_mask(roi_img, colors)
        filtered = cv2.bitwise_and(roi_img, roi_img, mask=mask)

        # Resize template to fit ROI dimensions if needed
        tpl_resized = self._resize_template(tpl, roi_img.shape[:2])
        if tpl_resized is None:
            return None

        # Color-filtered template matching
        tpl_mask = self._build_color_mask(tpl_resized, colors)
        tpl_filtered = cv2.bitwise_and(tpl_resized, tpl_resized, mask=tpl_mask)
        match_pos, confidence = self._template_match(filtered, tpl_filtered)
        match_method = 'color-filtered'

        if match_pos is None or confidence < _MATCH_THRESHOLD:
            # Fallback 1: raw CCOEFF template match
            match_pos, confidence = self._template_match(roi_img, tpl_resized)
            match_method = 'raw'

        if match_pos is None or confidence < _MATCH_THRESHOLD:
            # Fallback 2: SQDIFF match — only for steps that opt in.
            # Excellent for low-contrast templates (white buttons with dark
            # text) where CCOEFF fails, but CANNOT distinguish buttons with
            # different text of similar length, so only enabled per-step.
            if step.get('sqdiff'):
                sq_pos, sq_val = self._template_match_sqdiff(roi_img, tpl_resized)
                if sq_pos is not None and sq_val <= _SQDIFF_THRESHOLD:
                    match_pos = sq_pos
                    confidence = 1.0 - sq_val  # invert so higher = better
                    match_method = f'sqdiff({sq_val:.4f})'

        if match_pos is None or confidence < _MATCH_THRESHOLD:
            # Fallback 3: multi-scale raw template match
            best_pos, best_conf, best_scale = self._multi_scale_match(
                roi_img, tpl)
            if best_pos is not None and best_conf >= _MATCH_THRESHOLD:
                match_pos = best_pos
                confidence = best_conf
                match_method = f'multi-scale({best_scale:.2f})'
                # Use scaled template size for click offset
                th = max(1, int(tpl.shape[0] * best_scale))
                tw = max(1, int(tpl.shape[1] * best_scale))
                click_x = cl + roi_x + best_pos[0] + tw // 2
                click_y = ct + roi_y + best_pos[1] + th // 2
                # Debug save (success)
                self._debug_save_step(step_idx, roi_img, filtered, tpl_resized,
                                      confidence, match_method, True)
                return (step_idx, click_x, click_y, confidence)

        # Debug save (rate-limited)
        detected = (match_pos is not None and confidence >= _MATCH_THRESHOLD)
        self._debug_save_step(step_idx, roi_img, filtered, tpl_resized,
                              confidence, match_method, detected)

        if not detected:
            return None

        # Compute click position in screen coordinates
        mx, my = match_pos
        th, tw = tpl_resized.shape[:2]
        click_x = cl + roi_x + mx + tw // 2
        click_y = ct + roi_y + my + th // 2
        return (step_idx, click_x, click_y, confidence)

    # ── Debug save helper ──

    def _debug_save_step(
        self,
        step_idx: int,
        roi_img: np.ndarray,
        filtered_img: np.ndarray,
        tpl: np.ndarray,
        confidence: float,
        method: str,
        detected: bool,
    ):
        """Rate-limited save of ROI/filtered/template images for debugging."""
        if not _DEBUG_SAVE:
            return
        now = time.time()
        last = self._debug_last_save.get(step_idx, 0.0)
        # Save immediately on success, otherwise rate-limit
        if not detected and (now - last) < _DEBUG_INTERVAL_S:
            return
        self._debug_last_save[step_idx] = now
        tag = 'HIT' if detected else 'MISS'
        ts = time.strftime('%H%M%S')
        prefix = os.path.join(
            _DEBUG_DIR, f'step{step_idx}_{tag}_{ts}_c{confidence:.2f}')
        try:
            cv2.imwrite(f'{prefix}_roi.png', roi_img)
            cv2.imwrite(f'{prefix}_filtered.png', filtered_img)
            cv2.imwrite(f'{prefix}_tpl.png', tpl)
            print(f'[HideSeek][DEBUG] Saved {prefix}_*.png '
                  f'({method}, conf={confidence:.2f})')
        except Exception as e:
            print(f'[HideSeek][DEBUG] Save error: {e}')

    # ── Multi-scale template matching ──

    @staticmethod
    def _multi_scale_match(
        img: np.ndarray,
        tpl: np.ndarray,
        scales: Optional[list] = None,
    ) -> Tuple[Optional[Tuple[int, int]], float, float]:
        """Try template match at multiple scales.

        Returns (match_pos, confidence, best_scale) or (None, 0.0, 1.0).
        """
        if scales is None:
            scales = [0.75, 0.80, 0.85, 0.90, 0.95, 1.05, 1.10, 1.15, 1.20, 1.25]
        ih, iw = img.shape[:2]
        best_pos = None
        best_conf = 0.0
        best_scale = 1.0
        img_gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if len(img.shape) == 3 else img
        tpl_gray = cv2.cvtColor(tpl, cv2.COLOR_BGR2GRAY) if len(tpl.shape) == 3 else tpl
        for s in scales:
            tw = max(1, int(tpl_gray.shape[1] * s))
            th = max(1, int(tpl_gray.shape[0] * s))
            if tw > iw or th > ih or tw <= 0 or th <= 0:
                continue
            tpl_s = cv2.resize(tpl_gray, (tw, th), interpolation=cv2.INTER_AREA)
            try:
                result = cv2.matchTemplate(img_gray, tpl_s, cv2.TM_CCOEFF_NORMED)
                _, max_val, _, max_loc = cv2.minMaxLoc(result)
                if max_val > best_conf:
                    best_conf = max_val
                    best_pos = max_loc
                    best_scale = s
            except Exception:
                continue
        return best_pos, best_conf, best_scale

    # ── Execute a matched step ──

    def _execute_step(self, step_idx: int, click_x: int, click_y: int, conf: float):
        step = STEPS[step_idx]
        self._fire_status(
            f'Step {step_idx} ({step["name"]}): '
            f'MATCH conf={conf:.2f} → click ({click_x}, {click_y})',
            step_idx,
        )

        # Perform action
        if step.get('alt_click'):
            self._alt_click(click_x, click_y)
        else:
            self._click(click_x, click_y)

        # Record which step we just executed + when
        self._last_executed_step = step_idx
        self._last_click_ts = time.time()

        # Advance to next step (wrap around)
        with self._lock:
            self._current_step = (step_idx + 1) % len(STEPS)

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
        """Run template matching (TM_CCOEFF_NORMED). Returns ((x, y), confidence) or (None, 0.0)."""
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

    @staticmethod
    def _template_match_sqdiff(
        img: np.ndarray,
        tpl: np.ndarray,
    ) -> Tuple[Optional[Tuple[int, int]], float]:
        """Run TM_SQDIFF_NORMED template matching.

        Better than CCOEFF for low-contrast templates (e.g. white button with
        dark text) because it measures pixel-level difference rather than
        variance-normalised correlation.

        Returns ((x, y), sqdiff_value) where LOWER = more similar.
        Returns (None, 1.0) on failure.
        """
        if img is None or tpl is None:
            return None, 1.0
        ih, iw = img.shape[:2]
        th, tw = tpl.shape[:2]
        if th > ih or tw > iw:
            return None, 1.0
        if th <= 0 or tw <= 0:
            return None, 1.0
        try:
            img_gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if len(img.shape) == 3 else img
            tpl_gray = cv2.cvtColor(tpl, cv2.COLOR_BGR2GRAY) if len(tpl.shape) == 3 else tpl
            result = cv2.matchTemplate(img_gray, tpl_gray, cv2.TM_SQDIFF_NORMED)
            min_val, _, min_loc, _ = cv2.minMaxLoc(result)
            return min_loc, float(min_val)
        except Exception:
            return None, 1.0

    # ── Input simulation ──

    @staticmethod
    def _set_thread_dpi_awareness():
        """Match recognition thread DPI behavior in packaged onedir builds."""
        try:
            user32 = ctypes.windll.user32
            user32.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
            user32.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
            prev = user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
            if prev:
                print('[HideSeek] thread DPI context set to PerMonitorV2', flush=True)
        except Exception as e:
            print(f'[HideSeek] SetThreadDpiAwarenessContext failed: {e}', flush=True)

    @staticmethod
    def _send_mouse_click(screen_x: int, screen_y: int):
        """Send a left mouse click at absolute screen coordinates via SendInput."""
        user32 = ctypes.windll.user32
        vx = user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
        vy = user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
        vw = user32.GetSystemMetrics(SM_CXVIRTUALSCREEN)
        vh = user32.GetSystemMetrics(SM_CYVIRTUALSCREEN)
        if vw <= 0 or vh <= 0:
            vx, vy = 0, 0
            vw = user32.GetSystemMetrics(0)
            vh = user32.GetSystemMetrics(1)

        x = max(vx, min(vx + max(1, vw) - 1, int(screen_x)))
        y = max(vy, min(vy + max(1, vh) - 1, int(screen_y)))

        # Move the visible cursor first. Frozen/windowed builds can be ignored
        # by the game when move + down are collapsed into one absolute packet.
        try:
            user32.SetCursorPos(int(x), int(y))
        except Exception:
            pass

        # Normalize to 0-65535 over the virtual desktop.
        abs_x = int((x - vx) * 65535 / max(1, vw - 1))
        abs_y = int((y - vy) * 65535 / max(1, vh - 1))

        flags_base = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK
        extra_move = ctypes.c_ulong(0)
        mi_move = MOUSEINPUT(
            dx=abs_x, dy=abs_y, mouseData=0,
            dwFlags=flags_base | MOUSEEVENTF_MOVE,
            time=0, dwExtraInfo=ctypes.pointer(extra_move),
        )
        evt_move = INPUT(type=INPUT_MOUSE, iu=_INPUT_UNION(mi=mi_move))
        user32.SendInput(1, ctypes.byref(evt_move), ctypes.sizeof(INPUT))
        time.sleep(0.03)

        extra_down = ctypes.c_ulong(0)
        mi_down = MOUSEINPUT(
            dx=0, dy=0, mouseData=0,
            dwFlags=MOUSEEVENTF_LEFTDOWN,
            time=0, dwExtraInfo=ctypes.pointer(extra_down),
        )
        extra_up = ctypes.c_ulong(0)
        mi_up = MOUSEINPUT(
            dx=0, dy=0, mouseData=0,
            dwFlags=MOUSEEVENTF_LEFTUP,
            time=0, dwExtraInfo=ctypes.pointer(extra_up),
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
