# -*- coding: utf-8 -*-
"""Pure-vision recognition pipeline for stamina."""

from __future__ import annotations

import ctypes
import threading
import time
from typing import Callable, Optional, Tuple

import cv2
import numpy as np

from config import (
    BAR_COLORS,
    CAPTURE_FPS_FAST,
    get_visual_rect_bbox,
)
from vision_accel import cvt_color, gaussian_blur
from window_locator import WindowLocator

_mss_local = threading.local()
_capture_local = threading.local()


def _set_capture_target(hwnd: int, rect: Optional[Tuple[int, int, int, int]]):
    _capture_local.hwnd = int(hwnd or 0)
    _capture_local.rect = tuple(rect) if rect else None


def _bitmap_to_bgr(hdc_mem, hbmp, width: int, height: int) -> Optional[np.ndarray]:
    if width <= 0 or height <= 0:
        return None
    gdi32 = ctypes.windll.gdi32
    user32 = ctypes.windll.user32
    BI_RGB = 0
    DIB_RGB_COLORS = 0

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [
            ("biSize", ctypes.c_uint32),
            ("biWidth", ctypes.c_long),
            ("biHeight", ctypes.c_long),
            ("biPlanes", ctypes.c_ushort),
            ("biBitCount", ctypes.c_ushort),
            ("biCompression", ctypes.c_uint32),
            ("biSizeImage", ctypes.c_uint32),
            ("biXPelsPerMeter", ctypes.c_long),
            ("biYPelsPerMeter", ctypes.c_long),
            ("biClrUsed", ctypes.c_uint32),
            ("biClrImportant", ctypes.c_uint32),
        ]

    class BITMAPINFO(ctypes.Structure):
        _fields_ = [
            ("bmiHeader", BITMAPINFOHEADER),
            ("bmiColors", ctypes.c_uint32 * 3),
        ]

    try:
        bmi = BITMAPINFO()
        bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bmi.bmiHeader.biWidth = width
        bmi.bmiHeader.biHeight = -height
        bmi.bmiHeader.biPlanes = 1
        bmi.bmiHeader.biBitCount = 32
        bmi.bmiHeader.biCompression = BI_RGB
        buffer = ctypes.create_string_buffer(width * height * 4)
        rows = gdi32.GetDIBits(
            hdc_mem,
            hbmp,
            0,
            height,
            buffer,
            ctypes.byref(bmi),
            DIB_RGB_COLORS,
        )
        if rows != height:
            return None
        arr = np.frombuffer(buffer, dtype=np.uint8).reshape((height, width, 4))
        return arr[:, :, :3].copy()
    except Exception:
        return None


def _capture_looks_blank(img: Optional[np.ndarray]) -> bool:
    if img is None or img.size == 0:
        return True
    try:
        return bool(np.max(img) <= 2 and float(np.std(img)) <= 1.0)
    except Exception:
        return False


def _capture_hwnd_client_bitblt(
    hwnd: int,
    client_rect: Tuple[int, int, int, int],
) -> Optional[np.ndarray]:
    if hwnd <= 0 or not client_rect:
        return None
    cl, ct, cr, cb = client_rect
    width = max(1, int(cr - cl))
    height = max(1, int(cb - ct))
    if width <= 1 or height <= 1:
        return None

    user32 = ctypes.windll.user32
    gdi32 = ctypes.windll.gdi32
    SRCCOPY = 0x00CC0020
    hdc_window = user32.GetDC(hwnd)
    if not hdc_window:
        return None
    hdc_mem = gdi32.CreateCompatibleDC(hdc_window)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_window, width, height)
    old_obj = None
    try:
        if not hdc_mem or not hbmp:
            return None
        old_obj = gdi32.SelectObject(hdc_mem, hbmp)
        if not gdi32.BitBlt(hdc_mem, 0, 0, width, height, hdc_window, 0, 0, SRCCOPY):
            return None
        return _bitmap_to_bgr(hdc_mem, hbmp, width, height)
    except Exception:
        return None
    finally:
        try:
            if old_obj:
                gdi32.SelectObject(hdc_mem, old_obj)
        except Exception:
            pass
        try:
            if hbmp:
                gdi32.DeleteObject(hbmp)
        except Exception:
            pass
        try:
            if hdc_mem:
                gdi32.DeleteDC(hdc_mem)
        except Exception:
            pass
        try:
            user32.ReleaseDC(hwnd, hdc_window)
        except Exception:
            pass


def _capture_hwnd_client_printwindow(
    hwnd: int,
    client_rect: Tuple[int, int, int, int],
) -> Optional[np.ndarray]:
    if hwnd <= 0 or not client_rect:
        return None
    cl, ct, cr, cb = client_rect
    width = max(1, int(cr - cl))
    height = max(1, int(cb - ct))
    if width <= 1 or height <= 1:
        return None

    user32 = ctypes.windll.user32
    gdi32 = ctypes.windll.gdi32
    PW_CLIENTONLY = 0x00000001
    PW_RENDERFULLCONTENT = 0x00000002

    hdc_window = user32.GetWindowDC(hwnd)
    if not hdc_window:
        return None
    hdc_mem = gdi32.CreateCompatibleDC(hdc_window)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_window, width, height)
    old_obj = None
    try:
        if not hdc_mem or not hbmp:
            return None
        old_obj = gdi32.SelectObject(hdc_mem, hbmp)
        for flags in (PW_CLIENTONLY | PW_RENDERFULLCONTENT, PW_CLIENTONLY, 0):
            try:
                ok = user32.PrintWindow(hwnd, hdc_mem, flags)
            except Exception:
                ok = 0
            if ok:
                img = _bitmap_to_bgr(hdc_mem, hbmp, width, height)
                if not _capture_looks_blank(img):
                    return img
        return None
    except Exception:
        return None
    finally:
        try:
            if old_obj:
                gdi32.SelectObject(hdc_mem, old_obj)
        except Exception:
            pass
        try:
            if hbmp:
                gdi32.DeleteObject(hbmp)
        except Exception:
            pass
        try:
            if hdc_mem:
                gdi32.DeleteDC(hdc_mem)
        except Exception:
            pass
        try:
            user32.ReleaseDC(hwnd, hdc_window)
        except Exception:
            pass


def _capture_hwnd_client(
    hwnd: int,
    client_rect: Tuple[int, int, int, int],
) -> Tuple[Optional[np.ndarray], str]:
    img = _capture_hwnd_client_printwindow(hwnd, client_rect)
    if not _capture_looks_blank(img):
        return img, "printwindow"
    # PrintWindow failed (blank/black frame) — try BitBlt as fallback
    img = _capture_hwnd_client_bitblt(hwnd, client_rect)
    if not _capture_looks_blank(img):
        return img, "bitblt"
    return None, "failed"


def _crop_client_capture(
    client_img: Optional[np.ndarray],
    client_rect: Tuple[int, int, int, int],
    bbox: Tuple[int, int, int, int],
) -> Optional[np.ndarray]:
    if client_img is None or client_img.size == 0 or not client_rect or not bbox:
        return None
    cl, ct, cr, cb = client_rect
    bl, bt, br, bb = bbox
    src_x1 = max(0, int(bl - cl))
    src_y1 = max(0, int(bt - ct))
    src_x2 = min(int(cr - cl), int(br - cl))
    src_y2 = min(int(cb - ct), int(bb - ct))
    if src_x2 <= src_x1 or src_y2 <= src_y1:
        return None
    try:
        return client_img[src_y1:src_y2, src_x1:src_x2].copy()
    except Exception:
        return None


def _grab_hwnd_region(
    hwnd: int,
    client_rect: Tuple[int, int, int, int],
    bbox: Tuple[int, int, int, int],
) -> Optional[np.ndarray]:
    img, _backend = _capture_hwnd_client(hwnd, client_rect)
    return _crop_client_capture(img, client_rect, bbox)


def _grab_region(bbox: Tuple[int, int, int, int]) -> Optional[np.ndarray]:
    try:
        target_hwnd = getattr(_capture_local, "hwnd", 0)
        target_rect = getattr(_capture_local, "rect", None)
        if target_hwnd and target_rect:
            cl, ct, cr, cb = target_rect
            bl, bt, br, bb = bbox
            if bl >= cl and bt >= ct and br <= cr and bb <= cb:
                img = _grab_hwnd_region(int(target_hwnd), target_rect, bbox)
                if img is not None and img.size > 0:
                    return img
                return None
    except Exception:
        return None

    try:
        import mss

        if not hasattr(_mss_local, "sct") or _mss_local.sct is None:
            _mss_local.sct = mss.mss()
        mon = {
            "left": int(bbox[0]),
            "top": int(bbox[1]),
            "width": int(bbox[2] - bbox[0]),
            "height": int(bbox[3] - bbox[1]),
        }
        raw = _mss_local.sct.grab(mon)
        arr = np.frombuffer(raw.rgb, dtype=np.uint8).reshape(raw.height, raw.width, 3)
        return arr[:, :, ::-1].copy()
    except Exception:
        _mss_local.sct = None

    try:
        from PIL import ImageGrab

        img = ImageGrab.grab(bbox=bbox, all_screens=True)
        arr = np.array(img)
        return arr[:, :, ::-1].copy()
    except Exception:
        return None


def _subpixel_threshold_crossing(score: np.ndarray, threshold: float, last_filled_idx: int) -> float:
    """Find sub-pixel boundary where score crosses threshold near last_filled_idx."""
    eff_w = score.shape[0]
    if last_filled_idx >= eff_w - 1:
        return float(last_filled_idx + 1)
    s0 = float(score[last_filled_idx])
    s1 = float(score[min(last_filled_idx + 1, eff_w - 1)])
    if s0 > threshold >= s1 and s0 != s1:
        frac = (s0 - threshold) / (s0 - s1)
        return float(last_filled_idx) + frac
    return float(last_filled_idx + 1)


def _gradient_edge_pct(smooth_score: np.ndarray, eff_w: int, dynamic_range: float) -> Optional[float]:
    """Find the fill→empty boundary via the sharpest negative gradient.

    Returns sub-pixel percentage or None if no clear edge was found.
    """
    if eff_w <= 6:
        return None
    gradient = np.diff(smooth_score)
    if gradient.size < 5:
        return None
    # Smooth the gradient with a 7-wide kernel to suppress small fluctuations
    gk = np.ones((7,), dtype=np.float32) / 7.0
    smooth_grad = np.convolve(gradient, gk, mode="same")
    # Search range: skip the first 3% and last 2% (border artifacts)
    s_start = max(2, int(eff_w * 0.03))
    s_end = max(s_start + 4, eff_w - 1 - max(3, int(eff_w * 0.02)))
    region = smooth_grad[s_start:s_end]
    if region.size == 0:
        return None
    min_idx = int(np.argmin(region)) + s_start
    min_val = float(smooth_grad[min_idx])
    # Require a meaningful gradient (at least 0.8% of dynamic range per pixel)
    grad_threshold = -max(0.008, dynamic_range * 0.08)
    if min_val >= grad_threshold:
        return None
    # Sub-pixel interpolation: the boundary lies near min_idx in smooth_score
    # Find where the score crosses the midpoint between the two sides of the gradient
    mid_score = (float(smooth_score[min_idx]) + float(smooth_score[min(min_idx + 1, eff_w - 1)])) * 0.5
    # Search a narrow window around the gradient minimum
    for j in range(max(0, min_idx - 2), min(eff_w - 1, min_idx + 4)):
        s0 = float(smooth_score[j])
        s1 = float(smooth_score[min(j + 1, eff_w - 1)])
        if s0 >= mid_score > s1 and s0 != s1:
            frac = (s0 - mid_score) / (s0 - s1)
            return max(0.0, min(1.0, (j + frac + 0.5) / float(eff_w)))
    return max(0.0, min(1.0, float(min_idx + 1) / float(eff_w)))


def _row_independent_pct(
    hue: np.ndarray, sat: np.ndarray, val: np.ndarray,
    hue_mask: np.ndarray, fill_hue_ref: float, threshold: float,
) -> Optional[float]:
    """Compute per-row fill percentage and return the median.

    Provides outlier-resistant estimation by treating each row independently.
    Returns None if fewer than 2 usable rows.
    """
    n_rows, eff_w = val.shape
    if n_rows < 2 or eff_w <= 4:
        return None
    row_pcts = []
    for r in range(n_rows):
        rv = val[r, :].astype(np.float32)
        rs = sat[r, :].astype(np.float32)
        rh_cov = hue_mask[r, :].astype(np.float32)
        row_score = 0.78 * (rv / 255.0) + 0.22 * (rs / 255.0)
        row_score = np.where(rh_cov >= 0.5, row_score, row_score * 0.80)
        k = np.ones((3,), dtype=np.float32) / 3.0
        row_score = np.convolve(row_score, k, mode="same")
        filled = row_score >= threshold
        if np.any(filled):
            last_idx = int(np.max(np.where(filled)[0]))
            pct = _subpixel_threshold_crossing(row_score, threshold, last_idx) / float(eff_w)
            row_pcts.append(max(0.0, min(1.0, pct)))
    if len(row_pcts) < 2:
        return None
    return float(np.median(row_pcts))


def _detect_bar_pct(img: np.ndarray, color_cfg: dict) -> Tuple[float, float]:
    """Detect bar fill percentage using gradient edge detection + threshold voting.

    Improvements over simple threshold-only approach:
    1. Bilateral filter preserves sharp edges while reducing noise
    2. Gradient analysis finds the fill→empty boundary with sub-pixel precision
    3. Per-row median voting rejects outlier rows (glows, particles)
    4. Sub-pixel interpolation at threshold crossings
    5. Three estimates are combined: gradient, multi-row median, and threshold
    """
    try:
        # --- Pre-processing: bilateral filter preserves the fill boundary edge ---
        try:
            filtered = cv2.bilateralFilter(img, d=5, sigmaColor=50, sigmaSpace=50)
        except Exception:
            filtered = gaussian_blur(img, (3, 3), 0)
        hsv = cvt_color(filtered, cv2.COLOR_BGR2HSV)
        h, w = hsv.shape[:2]
        if h < 2 or w < 4:
            return 0.0, 0.0

        # Vertical padding: sample the middle band (exclude top/bottom 12%)
        y_pad = max(1, int(round(h * 0.12)))
        sample = hsv[y_pad: max(y_pad + 1, h - y_pad), :, :]
        if sample.size == 0:
            sample = hsv
        if sample.size == 0:
            return 0.0, 0.0

        hue = sample[:, :, 0].astype(np.float32)
        sat = sample[:, :, 1].astype(np.float32)
        val = sample[:, :, 2].astype(np.float32)

        # Horizontal padding
        x_pad = max(2, int(round(w * 0.018)))
        x1 = min(max(0, x_pad), max(0, w - 8))
        x2 = max(x1 + 8, w - x_pad)
        hue = hue[:, x1:x2]
        sat = sat[:, x1:x2]
        val = val[:, x1:x2]
        eff_w = val.shape[1]
        if eff_w <= 4:
            return 0.0, 0.0

        # Hue-aware fill mask
        hue_mask = (
            (hue >= float(color_cfg["h_min"])) &
            (hue <= float(color_cfg["h_max"])) &
            (sat >= max(18.0, float(color_cfg["s_min"]) * 0.42))
        )

        # Column-wise scoring
        mean_hue = hue.mean(axis=0)
        mean_sat = sat.mean(axis=0)
        mean_val = val.mean(axis=0)
        hue_coverage = hue_mask.mean(axis=0)
        left_ref_width = max(6, int(round(eff_w * 0.12)))
        fill_hue_ref = float(np.percentile(mean_hue[:left_ref_width], 55))
        hue_delta = np.abs(mean_hue - fill_hue_ref)
        hue_delta = np.minimum(hue_delta, 180.0 - hue_delta)
        hue_bonus = 1.0 - np.clip(hue_delta / 24.0, 0.0, 1.0)

        col_score = (0.78 * (mean_val / 255.0)) + (0.22 * (mean_sat / 255.0))
        col_score = np.where(hue_coverage >= 0.12, col_score, col_score * (0.72 + 0.18 * hue_bonus))
        smooth_kernel = np.ones((5,), dtype=np.float32) / 5.0
        smooth_score = np.convolve(col_score, smooth_kernel, mode="same")

        # References and dynamic range
        ref_width = max(6, int(round(eff_w * 0.12)))
        left_ref = float(np.percentile(smooth_score[:ref_width], 84))
        right_ref = float(np.percentile(smooth_score[max(0, eff_w - ref_width):], 62))
        dynamic_range = max(0.0, left_ref - right_ref)
        if dynamic_range <= 0.055 and float(smooth_score[-ref_width:].mean()) >= (left_ref * 0.96):
            return 1.0, 1.0

        threshold = max(0.20, right_ref + dynamic_range * 0.54, left_ref * 0.71)

        # --- Method 1: Gradient-based edge detection (sub-pixel) ---
        gradient_pct = _gradient_edge_pct(smooth_score, eff_w, dynamic_range)

        # --- Method 2: Per-row median voting ---
        row_median_pct = _row_independent_pct(
            hue, sat, val, hue_mask, fill_hue_ref, threshold
        )

        # --- Method 3: Threshold-based with sub-pixel interpolation ---
        filled = smooth_score >= threshold
        if filled.size >= 3:
            filled[1:-1] = filled[1:-1] | (filled[:-2] & filled[2:])
        if not np.any(filled):
            threshold_pct = 0.0
        else:
            last_idx = int(np.max(np.where(filled)[0]))
            subpixel = _subpixel_threshold_crossing(smooth_score, threshold, last_idx)
            threshold_pct = max(0.0, min(1.0, subpixel / float(eff_w)))

        # --- Combine estimates ---
        confidence = min(1.0, dynamic_range / 0.18)
        estimates = []
        weights = []

        if gradient_pct is not None:
            estimates.append(gradient_pct)
            weights.append(0.45)  # Highest weight: sub-pixel precision
        if row_median_pct is not None:
            estimates.append(row_median_pct)
            weights.append(0.30)  # Outlier-resistant
        estimates.append(threshold_pct)
        weights.append(0.25 if estimates else 1.0)

        # If all methods agree within 3%, use gradient (most precise)
        if len(estimates) >= 2:
            spread = max(estimates) - min(estimates)
            if spread <= 0.03 and gradient_pct is not None:
                pct = gradient_pct
                confidence = min(1.0, confidence * 1.1)
            else:
                total_w = sum(weights)
                pct = sum(e * w for e, w in zip(estimates, weights)) / total_w
        else:
            pct = estimates[0] if estimates else 0.0

        pct = max(0.0, min(1.0, pct))
        confidence = max(0.0, min(1.0, confidence))
        return pct, confidence
    except Exception:
        return _detect_bar_pct_simple(img), 0.0


def _detect_bar_pct_simple(img: np.ndarray) -> float:
    h, w, _ = img.shape
    if w < 2 or h < 2:
        return 0.0
    sample = img[max(0, h // 2 - 1): min(h, h // 2 + 2), :, :]
    brightness = sample.mean(axis=(0, 2))
    threshold = max(45.0, float(brightness.mean()) * 0.68)
    filled = np.where(brightness >= threshold)[0]
    if len(filled) == 0:
        return 0.0
    return max(0.0, min(1.0, float(filled[-1] + 1) / float(w)))


class RecognitionEngine:
    def __init__(self, state_mgr, settings=None):
        self._state_mgr = state_mgr
        self._settings = settings
        self._locator = WindowLocator()
        self._fps = CAPTURE_FPS_FAST
        self._running = False
        self._thread = None
        self._no_window_logged = False
        self._debug_callback: Optional[Callable[[str, np.ndarray], None]] = None
        self._sta_filtered_pct: Optional[float] = None
        self._sta_pending_pct: Optional[float] = None
        self._sta_pending_since: float = 0.0
        self._sta_drop_lock_until: float = 0.0
        self._capture_error_logged = False
        self._capture_backend = ""
        self._capture_fail_count = 0
        self._last_sta_logged: Optional[int] = None

    def set_debug_callback(self, cb):
        self._debug_callback = cb

    def _use_vision_source(self, component: str) -> bool:
        if not self._settings or not hasattr(self._settings, "get_component_source"):
            return component == "stamina"
        try:
            return self._settings.get_component_source(component, "vision") == "vision"
        except Exception:
            return component == "stamina"

    def _accept_stamina_pct(self, pct: float, now: float) -> float:
        pct = max(0.0, min(1.0, float(pct)))
        prev = self._sta_filtered_pct
        if prev is not None and pct < (prev - 0.001):
            self._sta_drop_lock_until = max(self._sta_drop_lock_until, now + 0.30)
        self._sta_filtered_pct = pct
        self._sta_pending_pct = None
        self._sta_pending_since = 0.0
        return pct

    def _filter_stamina_pct(self, raw_pct: float, confidence: float) -> float:
        now = time.time()
        raw_pct = max(0.0, min(1.0, float(raw_pct)))
        stable = self._sta_filtered_pct

        if stable is None:
            return self._accept_stamina_pct(raw_pct, now)

        if confidence < 0.20:
            return float(stable)

        # After a real decrease, ignore any short-term recovery readings for 0.3s.
        if raw_pct > stable and now < self._sta_drop_lock_until:
            return float(stable)

        delta = raw_pct - stable
        if abs(delta) > 0.20:
            if self._sta_pending_pct is None:
                self._sta_pending_pct = raw_pct
                self._sta_pending_since = now
                return float(stable)

            pending_delta = raw_pct - self._sta_pending_pct
            if abs(pending_delta) <= 0.08:
                if (now - self._sta_pending_since) >= 0.20:
                    return self._accept_stamina_pct(raw_pct, now)
                return float(stable)

            if abs(raw_pct - stable) <= 0.08:
                self._sta_pending_pct = None
                self._sta_pending_since = 0.0
                return float(stable)

            self._sta_pending_pct = raw_pct
            self._sta_pending_since = now
            return float(stable)

        return self._accept_stamina_pct(raw_pct, now)

    def start(self):
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True, name="recognition_vision")
        self._thread.start()

    def stop(self):
        self._running = False
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1.0)
        self._thread = None
        self._sta_filtered_pct = None
        self._sta_pending_pct = None
        self._sta_pending_since = 0.0
        self._sta_drop_lock_until = 0.0
        self._last_sta_logged = None

    def _run(self):
        while self._running:
            start = time.time()
            try:
                self._tick()
            except Exception as exc:
                self._state_mgr.update(recognition_ok=False, error_msg=str(exc))
            elapsed = time.time() - start
            time.sleep(max(0.01, (1.0 / self._fps) - elapsed))

    def _tick(self):
        result = self._locator.find_game_window()
        if result is None:
            _set_capture_target(0, None)
            if not self._no_window_logged:
                self._no_window_logged = True
                print("[Vision] game window not found")
            self._state_mgr.update(
                recognition_ok=False,
                error_msg="game window not found",
                window_rect=None,
            )
            return

        self._no_window_logged = False
        hwnd, _title, rect = result
        _set_capture_target(hwnd, rect)
        client_w = max(1, int(rect[2] - rect[0]))
        client_h = max(1, int(rect[3] - rect[1]))

        updates = {
            "window_rect": rect,
            "window_width": client_w,
            "window_height": client_h,
            "recognition_ok": True,
            "error_msg": "",
        }

        need_vision = self._use_vision_source("stamina")
        client_frame = None
        if need_vision:
            # Backoff: after 5 consecutive failures, only retry every 20 ticks (~2s)
            if self._capture_fail_count >= 5:
                self._capture_fail_count += 1
                if self._capture_fail_count % 20 != 0:
                    # Skip vision this tick but still push window rect as OK
                    # so packet-driven UI (boss bar, DPS, etc.) continues working
                    self._state_mgr.update(**updates)
                    return
            client_frame, backend = _capture_hwnd_client(hwnd, rect)
            if client_frame is None or client_frame.size == 0:
                _set_capture_target(hwnd, rect)
                self._capture_fail_count += 1
                if not self._capture_error_logged:
                    self._capture_error_logged = True
                    print("[Vision] direct window capture failed (printwindow+bitblt)")
                # Still push window rect + recognition_ok=True so packet-driven
                # features (boss bar, DPS, skills) are not blocked by vision failure.
                # Only stamina (vision-dependent) will be missing.
                updates["error_msg"] = "vision capture failed"
                self._state_mgr.update(**updates)
                return
            self._capture_fail_count = 0
            self._capture_error_logged = False
            if backend != self._capture_backend:
                self._capture_backend = backend
                print(f"[Vision] direct window capture backend: {backend}")

        if self._use_vision_source("stamina"):
            st_bbox = get_visual_rect_bbox("stamina_bar_visual", rect)
            if st_bbox:
                st_img = _crop_client_capture(client_frame, rect, st_bbox)
                if st_img is not None and st_img.size > 0:
                    raw_pct, confidence = _detect_bar_pct(st_img, BAR_COLORS["stamina"])
                    sta_pct = self._filter_stamina_pct(raw_pct, confidence)
                    updates["stamina_pct"] = sta_pct
                    updates["stamina_current"] = 0
                    updates["stamina_max"] = 0
                    sta_pct_int = int(round(sta_pct * 100.0))
                    if self._last_sta_logged is None or abs(sta_pct_int - self._last_sta_logged) >= 3:
                        self._last_sta_logged = sta_pct_int
                        print(f"[Vision] STA visual: {sta_pct_int}%")
                    if self._debug_callback:
                        self._debug_callback("stamina_bar_visual", st_img)

        self._state_mgr.update(**updates)

    def single_capture(self) -> Optional[dict]:
        self._tick()
        return self._state_mgr.state.to_dict()
