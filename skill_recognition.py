# -*- coding: utf-8 -*-
"""Fixed-ROI visual skill recognition for 16:9 game client windows."""

from __future__ import annotations

import os
from typing import Any, Dict, List, Optional, Tuple

import cv2
import numpy as np

from config import (
    BAR_COLORS,
    SKILL_BASELINE_DIR,
    get_skill_slot_client_rects,
    get_skill_slot_rects,
)
from vision_accel import cvt_color, gaussian_blur

_DEFAULT_BASELINE = {
    "inner_v_mean": 150.0,
    "inner_s_mean": 150.0,
    "ring_ratio": 0.16,
    "bright_ratio": 0.20,
    "icon_v_mean": 165.0,
    "icon_s_mean": 95.0,
}


def _clamp01(value: float) -> float:
    return max(0.0, min(1.0, float(value)))


def _slot_masks(h: int, w: int) -> Optional[Tuple[np.ndarray, np.ndarray, np.ndarray]]:
    if h < 8 or w < 8:
        return None
    cx = w / 2.0
    cy = h * 0.46
    radius_outer = max(6.0, min(w, h) * 0.47)
    radius_inner = max(4.0, min(w, h) * 0.30)
    yy, xx = np.ogrid[:h, :w]
    dist2 = (xx - cx) ** 2 + (yy - cy) ** 2
    icon_mask = dist2 <= radius_outer * radius_outer
    inner_mask = dist2 <= radius_inner * radius_inner
    ring_mask = icon_mask & (~inner_mask)
    if not np.any(icon_mask) or not np.any(inner_mask) or not np.any(ring_mask):
        return None
    return icon_mask, inner_mask, ring_mask


def _prepare_hsv(img_bgr: Optional[np.ndarray]) -> Optional[np.ndarray]:
    if img_bgr is None or img_bgr.size == 0:
        return None
    return cvt_color(gaussian_blur(img_bgr, (3, 3), 0), cv2.COLOR_BGR2HSV)


def _measure_slot(img_bgr: Optional[np.ndarray]) -> Optional[Dict[str, float]]:
    hsv = _prepare_hsv(img_bgr)
    if hsv is None:
        return None
    h, w = hsv.shape[:2]
    masks = _slot_masks(h, w)
    if masks is None:
        return None
    icon_mask, inner_mask, ring_mask = masks

    hue = hsv[:, :, 0]
    sat = hsv[:, :, 1]
    val = hsv[:, :, 2]

    icon_total = max(1, int(np.count_nonzero(icon_mask)))
    inner_total = max(1, int(np.count_nonzero(inner_mask)))
    ring_total = max(1, int(np.count_nonzero(ring_mask)))

    warm_mask = (hue >= 5) & (hue <= 35) & (sat >= 55) & (val >= 30)
    cyan_ring_mask = (hue >= 72) & (hue <= 126) & (sat >= 30) & (val >= 100)
    dark_mask = val <= BAR_COLORS["skill_cooldown"]["v_max_dark"]
    gray_dark_mask = dark_mask & (sat <= BAR_COLORS["skill_cooldown"]["s_max_gray"])
    dim_mask = val <= 120
    bright_mask = val >= 175

    inner_v_mean = float(val[inner_mask].mean())
    inner_s_mean = float(sat[inner_mask].mean())
    icon_v_mean = float(val[icon_mask].mean())
    icon_s_mean = float(sat[icon_mask].mean())
    ring_ratio = float(np.count_nonzero(cyan_ring_mask & ring_mask)) / ring_total
    bright_ratio = float(np.count_nonzero(bright_mask & inner_mask)) / inner_total
    dark_ratio = float(np.count_nonzero(dark_mask & inner_mask)) / inner_total
    gray_dark_ratio = float(np.count_nonzero(gray_dark_mask & inner_mask)) / inner_total
    dim_ratio = float(np.count_nonzero(dim_mask & inner_mask)) / inner_total
    warm_ratio = float(np.count_nonzero(warm_mask & inner_mask)) / inner_total
    shadow_ratio = float(np.count_nonzero(dark_mask & icon_mask)) / icon_total
    icon_score = (0.74 * (icon_v_mean / 255.0)) + (0.26 * (icon_s_mean / 255.0))

    ready_score = (
        (inner_v_mean / 255.0) * 0.38
        + ring_ratio * 0.28
        + bright_ratio * 0.16
        + (icon_v_mean / 255.0) * 0.12
        + max(0.0, 1.0 - dark_ratio) * 0.06
    )

    return {
        "inner_v_mean": inner_v_mean,
        "inner_s_mean": inner_s_mean,
        "icon_v_mean": icon_v_mean,
        "icon_s_mean": icon_s_mean,
        "ring_ratio": ring_ratio,
        "bright_ratio": bright_ratio,
        "dark_ratio": dark_ratio,
        "gray_dark_ratio": gray_dark_ratio,
        "dim_ratio": dim_ratio,
        "warm_ratio": warm_ratio,
        "shadow_ratio": shadow_ratio,
        "icon_score": icon_score,
        "ready_score": ready_score,
    }


def _compare_to_baseline(
    img_bgr: Optional[np.ndarray],
    baseline_bgr: Optional[np.ndarray],
) -> Optional[Dict[str, float]]:
    if img_bgr is None or baseline_bgr is None or img_bgr.size == 0 or baseline_bgr.size == 0:
        return None

    h, w = img_bgr.shape[:2]
    if baseline_bgr.shape[:2] != (h, w):
        baseline_bgr = cv2.resize(baseline_bgr, (w, h), interpolation=cv2.INTER_AREA)

    hsv = _prepare_hsv(img_bgr)
    base_hsv = _prepare_hsv(baseline_bgr)
    if hsv is None or base_hsv is None:
        return None

    masks = _slot_masks(h, w)
    if masks is None:
        return None
    icon_mask, inner_mask, _ring_mask = masks

    cur_v = hsv[:, :, 2].astype(np.float32)
    cur_s = hsv[:, :, 1].astype(np.float32)
    base_v = base_hsv[:, :, 2].astype(np.float32)
    base_s = base_hsv[:, :, 1].astype(np.float32)

    cur_icon_v = cur_v[icon_mask]
    cur_icon_s = cur_s[icon_mask]
    base_icon_v = base_v[icon_mask]
    base_icon_s = base_s[icon_mask]
    cur_inner_v = cur_v[inner_mask]
    base_inner_v = base_v[inner_mask]

    base_icon_v_mean = max(1.0, float(base_icon_v.mean()))
    base_icon_s_mean = max(1.0, float(base_icon_s.mean()))
    cur_icon_v_mean = float(cur_icon_v.mean())
    cur_icon_s_mean = float(cur_icon_s.mean())
    base_score = (0.74 * (base_icon_v_mean / 255.0)) + (0.26 * (base_icon_s_mean / 255.0))
    cur_score = (0.74 * (cur_icon_v_mean / 255.0)) + (0.26 * (cur_icon_s_mean / 255.0))

    darkened_ratio = float(np.mean(cur_inner_v + 14.0 < base_inner_v))
    restored_ratio = float(np.mean(np.abs(cur_inner_v - base_inner_v) <= 16.0))

    return {
        "icon_v_ratio": cur_icon_v_mean / base_icon_v_mean,
        "icon_s_ratio": cur_icon_s_mean / base_icon_s_mean,
        "score_ratio": cur_score / max(0.05, base_score),
        "darkened_ratio": darkened_ratio,
        "restored_ratio": restored_ratio,
        "avg_delta_v": float(cur_inner_v.mean() - base_inner_v.mean()),
    }


def _guess_baseline_state(metrics: Dict[str, float]) -> str:
    if metrics["warm_ratio"] >= 0.45 and metrics["ring_ratio"] <= 0.08:
        return "insufficient_energy"
    if metrics["icon_v_mean"] <= 125.0 or metrics["shadow_ratio"] >= 0.30:
        return "cooldown"
    if metrics["ready_score"] >= 0.34 and metrics["ring_ratio"] >= 0.08:
        return "ready"
    return "unknown"


def _classify_state(
    metrics: Dict[str, float],
    baseline: Dict[str, float],
    baseline_cmp: Optional[Dict[str, float]] = None,
    baseline_state: str = "unknown",
) -> Tuple[str, float]:
    ref_v = max(1.0, float(baseline.get("inner_v_mean", _DEFAULT_BASELINE["inner_v_mean"])))
    ref_ring = max(0.06, float(baseline.get("ring_ratio", _DEFAULT_BASELINE["ring_ratio"])))
    ref_icon_v = max(1.0, float(baseline.get("icon_v_mean", _DEFAULT_BASELINE["icon_v_mean"])))

    v_ratio = min(1.5, metrics["inner_v_mean"] / ref_v)
    ring_rel = min(2.0, metrics["ring_ratio"] / ref_ring)
    icon_v_ratio = min(1.5, metrics["icon_v_mean"] / ref_icon_v)

    insufficient_like = (
        metrics["warm_ratio"] >= 0.42
        and metrics["ring_ratio"] <= 0.09
        and metrics["dim_ratio"] >= 0.38
    )
    ready_absolute = (
        metrics["ring_ratio"] >= 0.10
        and metrics["icon_v_mean"] >= 128.0
        and metrics["gray_dark_ratio"] <= 0.20
        and metrics["shadow_ratio"] <= 0.20
    )

    if insufficient_like:
        return "insufficient_energy", _clamp01(max(0.12, 1.0 - metrics["icon_score"]))

    if baseline_cmp:
        if baseline_state == "cooldown":
            if ready_absolute and baseline_cmp["score_ratio"] >= 1.06:
                return "ready", 0.0
            cooldown_ratio = _clamp01(
                max(
                    0.12,
                    (1.0 - min(1.0, baseline_cmp["score_ratio"])) * 0.50
                    + metrics["shadow_ratio"] * 0.30
                    + metrics["gray_dark_ratio"] * 0.20,
                )
            )
            return "cooldown", cooldown_ratio

        if (
            baseline_cmp["score_ratio"] <= 0.90
            and baseline_cmp["darkened_ratio"] >= 0.14
        ) or (
            baseline_cmp["icon_v_ratio"] <= 0.90
            and baseline_cmp["darkened_ratio"] >= 0.18
        ):
            cooldown_ratio = _clamp01(
                (1.0 - min(1.0, baseline_cmp["score_ratio"])) * 0.56
                + baseline_cmp["darkened_ratio"] * 0.28
                + metrics["shadow_ratio"] * 0.16
            )
            return "cooldown", max(0.05, cooldown_ratio)

        if ready_absolute or (
            baseline_cmp["restored_ratio"] >= 0.48
            and baseline_cmp["score_ratio"] >= 0.93
            and metrics["shadow_ratio"] <= 0.24
        ):
            return "ready", 0.0

    if ready_absolute or (
        metrics["ready_score"] >= 0.34
        and v_ratio >= 0.78
        and ring_rel >= 0.72
        and icon_v_ratio >= 0.78
        and metrics["gray_dark_ratio"] <= 0.28
    ):
        return "ready", 0.0

    cooldown_ratio = _clamp01(
        (1.0 - min(1.0, icon_v_ratio)) * 0.42
        + metrics["shadow_ratio"] * 0.30
        + metrics["gray_dark_ratio"] * 0.18
        + (1.0 - min(1.0, ring_rel)) * 0.10
    )
    if cooldown_ratio >= 0.08:
        return "cooldown", max(0.05, cooldown_ratio)
    return "unknown", cooldown_ratio


class SkillVisualTracker:
    """Track fixed visual skill slots using client-rect anchored ROIs."""

    def __init__(self, confirm_frames: int = 2):
        self._confirm_frames = max(1, int(confirm_frames))
        self._slot_cache: Dict[int, Dict[str, Any]] = {}
        self._baseline_dir = SKILL_BASELINE_DIR
        try:
            os.makedirs(self._baseline_dir, exist_ok=True)
        except Exception:
            pass

    def reset(self):
        self._slot_cache.clear()

    def _ensure_slot_state(self, idx: int) -> Dict[str, Any]:
        if idx not in self._slot_cache:
            self._slot_cache[idx] = {
                "baseline": dict(_DEFAULT_BASELINE),
                "baseline_img": None,
                "baseline_state": "unknown",
                "stable_state": "unknown",
                "pending_state": None,
                "pending_count": 0,
            }
        return self._slot_cache[idx]

    def _save_baseline(self, idx: int, img: np.ndarray):
        try:
            os.makedirs(self._baseline_dir, exist_ok=True)
            cv2.imwrite(os.path.join(self._baseline_dir, f"skill_slot_{idx}.png"), img)
        except Exception:
            pass

    def analyze(self, client_rect, capture_region) -> List[Dict[str, Any]]:
        if not client_rect:
            return []

        client_left, client_top, client_right, client_bottom = client_rect
        client_w = max(1, int(client_right - client_left))
        client_h = max(1, int(client_bottom - client_top))
        client_local_map = {
            item["index"]: item["rect"] for item in get_skill_slot_client_rects(client_w, client_h)
        }

        slots: List[Dict[str, Any]] = []
        for item in get_skill_slot_rects(client_rect):
            idx = int(item["index"])
            bbox = item["bbox"]
            img = capture_region(bbox)
            metrics = _measure_slot(img)
            state_store = self._ensure_slot_state(idx)
            ready_edge = False

            if metrics is not None and state_store.get("baseline_img") is None and img is not None:
                state_store["baseline_img"] = img.copy()
                state_store["baseline"] = dict(metrics)
                state_store["baseline_state"] = _guess_baseline_state(metrics)
                self._save_baseline(idx, img)

            baseline_cmp = None
            if img is not None and state_store.get("baseline_img") is not None:
                baseline_cmp = _compare_to_baseline(img, state_store.get("baseline_img"))

            if metrics is None:
                raw_state = "unknown"
                cooldown_ratio = 0.0
            else:
                raw_state, cooldown_ratio = _classify_state(
                    metrics,
                    state_store["baseline"],
                    baseline_cmp,
                    str(state_store.get("baseline_state", "unknown") or "unknown"),
                )

            if raw_state == state_store.get("pending_state"):
                state_store["pending_count"] += 1
            else:
                state_store["pending_state"] = raw_state
                state_store["pending_count"] = 1

            stable_state = str(state_store.get("stable_state", "unknown") or "unknown")
            if state_store["pending_count"] >= self._confirm_frames:
                previous = stable_state
                stable_state = raw_state
                state_store["stable_state"] = stable_state
                if previous != "ready" and stable_state == "ready":
                    ready_edge = True

            if metrics is not None and stable_state == "ready":
                baseline = state_store["baseline"]
                for key in ("inner_v_mean", "inner_s_mean", "icon_v_mean", "icon_s_mean"):
                    baseline[key] = baseline.get(key, metrics[key]) * 0.88 + metrics[key] * 0.12
                for key in ("ring_ratio", "bright_ratio"):
                    baseline[key] = max(
                        baseline.get(key, metrics[key]) * 0.86 + metrics[key] * 0.14,
                        metrics[key] * 0.94,
                    )

            rect = client_local_map.get(idx)
            if not rect:
                left, top, right, bottom = bbox
                rect = {
                    "x": left - client_left,
                    "y": top - client_top,
                    "w": right - left,
                    "h": bottom - top,
                }

            if stable_state == "ready":
                cooldown_ratio = 0.0
            elif stable_state == "insufficient_energy":
                cooldown_ratio = max(cooldown_ratio, 0.0)

            slots.append(
                {
                    "index": idx,
                    "rect": {
                        "x": int(rect["x"]),
                        "y": int(rect["y"]),
                        "w": int(rect["w"]),
                        "h": int(rect["h"]),
                    },
                    "state": stable_state,
                    "cooldown_ratio": round(_clamp01(cooldown_ratio), 3),
                    "cooldown_pct": round(_clamp01(cooldown_ratio), 3),
                    "insufficient_energy": stable_state == "insufficient_energy",
                    "active": stable_state == "ready",
                    "ready_edge": bool(ready_edge),
                }
            )

        slots.sort(key=lambda item: item.get("index", 0))
        return slots
