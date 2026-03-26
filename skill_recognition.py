# -*- coding: utf-8 -*-
"""
SAO Auto — 技能栏识别模块

检测游戏界面底部技能栏的冷却状态。
采用 OCR-fallback 方案：抓包只能获取 profession_id，技能冷却来自屏幕采集。

策略:
  1. 首次运行: 在 skill_bar ROI 内自动检测技能格 (等间距矩形图标)
  2. 每帧: 对每个格子检测冷却遮罩 (低亮度 + 低饱和度的半透明覆盖)
  3. 冷却比例 = 暗像素占比 (从底部扫描到顶部)
"""

import cv2
import numpy as np
import time
import logging

log = logging.getLogger('sao_auto.skill')

# ── 技能格检测参数 ──
MIN_SLOT_SIZE = 28          # 最小技能格像素宽度
MAX_SLOT_SIZE = 80          # 最大技能格像素宽度
MIN_SLOTS = 4               # 最少技能格数
MAX_SLOTS = 12              # 最多技能格数
EDGE_THRESH_LOW = 50
EDGE_THRESH_HIGH = 150


def detect_skill_slots(img_bgr):
    """
    在技能栏 ROI 图像中检测等间距的方形技能格。

    Parameters
    ----------
    img_bgr : np.ndarray
        技能栏区域截图 (BGR)

    Returns
    -------
    list[dict]
        每个元素 = {'x': int, 'y': int, 'w': int, 'h': int, 'index': int}
        坐标相对于输入图像左上角
    """
    if img_bgr is None or img_bgr.size == 0:
        return []

    h, w = img_bgr.shape[:2]
    gray = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2GRAY)

    # 使用 Canny 边缘检测找矩形轮廓
    edges = cv2.Canny(gray, EDGE_THRESH_LOW, EDGE_THRESH_HIGH)
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (3, 3))
    edges = cv2.dilate(edges, kernel, iterations=1)

    contours, _ = cv2.findContours(edges, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

    candidates = []
    for cnt in contours:
        x, y, cw, ch = cv2.boundingRect(cnt)
        # 技能格应当接近正方形、大小合理
        aspect = cw / max(1, ch)
        if 0.7 < aspect < 1.4 and MIN_SLOT_SIZE <= cw <= MAX_SLOT_SIZE and MIN_SLOT_SIZE <= ch <= MAX_SLOT_SIZE:
            candidates.append({'x': x, 'y': y, 'w': cw, 'h': ch})

    if len(candidates) < MIN_SLOTS:
        # 退化方案: 假设技能栏均匀分布在 ROI 底部
        return _fallback_uniform_slots(w, h)

    # 按 x 坐标排序, 去除重叠
    candidates.sort(key=lambda s: s['x'])
    merged = _merge_overlapping(candidates)

    if len(merged) < MIN_SLOTS:
        return _fallback_uniform_slots(w, h)

    # 限制数量
    merged = merged[:MAX_SLOTS]
    for i, s in enumerate(merged):
        s['index'] = i

    return merged


def _fallback_uniform_slots(roi_w, roi_h, n=8):
    """退化方案: 平均分割 ROI 为 n 个等宽格子。"""
    slot_w = min(roi_h, roi_w // n)
    slot_h = slot_w
    y = roi_h - slot_h - 2
    gap = (roi_w - slot_w * n) // (n + 1)
    slots = []
    for i in range(n):
        x = gap + i * (slot_w + gap)
        slots.append({'x': x, 'y': y, 'w': slot_w, 'h': slot_h, 'index': i})
    return slots


def _merge_overlapping(slots, iou_thresh=0.3):
    """合并重叠检测框。"""
    if not slots:
        return []
    merged = [slots[0]]
    for s in slots[1:]:
        last = merged[-1]
        overlap_x = max(0, min(last['x'] + last['w'], s['x'] + s['w']) - max(last['x'], s['x']))
        overlap_y = max(0, min(last['y'] + last['h'], s['y'] + s['h']) - max(last['y'], s['y']))
        overlap_area = overlap_x * overlap_y
        area_a = last['w'] * last['h']
        area_b = s['w'] * s['h']
        iou = overlap_area / max(1, area_a + area_b - overlap_area)
        if iou < iou_thresh:
            merged.append(s)
    return merged


def detect_cooldowns(img_bgr, slots, cfg=None):
    """
    检测每个技能格的冷却比例。

    Parameters
    ----------
    img_bgr : np.ndarray
        技能栏 ROI 截图 (BGR)
    slots : list[dict]
        由 detect_skill_slots 返回的格子列表
    cfg : dict, optional
        BAR_COLORS['skill_cooldown'] 配置

    Returns
    -------
    list[dict]
        更新后的 slots，每个增加:
          'cooldown_pct': float (0.0=就绪, ~1.0=完全冷却)
          'active': bool
    """
    if cfg is None:
        cfg = {'v_max_dark': 80, 's_max_gray': 40}

    v_max_dark = cfg.get('v_max_dark', 80)
    s_max_gray = cfg.get('s_max_gray', 40)

    if img_bgr is None or img_bgr.size == 0:
        for s in slots:
            s['cooldown_pct'] = 0.0
            s['active'] = False
        return slots

    hsv = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2HSV)
    h_img, w_img = hsv.shape[:2]

    for s in slots:
        sx, sy, sw, sh = s['x'], s['y'], s['w'], s['h']
        # 裁剪格子区域 (带边界检查)
        x1 = max(0, sx + 2)
        y1 = max(0, sy + 2)
        x2 = min(w_img, sx + sw - 2)
        y2 = min(h_img, sy + sh - 2)
        if x2 <= x1 or y2 <= y1:
            s['cooldown_pct'] = 0.0
            s['active'] = False
            continue

        roi = hsv[y1:y2, x1:x2]
        # 冷却遮罩 = 低亮度 + 低饱和度
        dark_mask = (roi[:, :, 2] < v_max_dark) & (roi[:, :, 1] < s_max_gray)
        total_px = roi.shape[0] * roi.shape[1]
        dark_px = np.count_nonzero(dark_mask)

        # 从下到上扫描: 冷却遮罩通常从底部向上收缩
        cd_pct = dark_px / max(1, total_px)

        # 进一步计算: 分行扫描确定精确冷却高度
        if cd_pct > 0.05:
            cd_pct = _scan_cooldown_height(dark_mask)

        # 判断是否正在释放 (高亮 / 边框发光)
        bright_mask = roi[:, :, 2] > 200
        bright_ratio = np.count_nonzero(bright_mask) / max(1, total_px)
        active = bright_ratio > 0.3

        s['cooldown_pct'] = round(min(1.0, max(0.0, cd_pct)), 3)
        s['active'] = active

    return slots


def _scan_cooldown_height(dark_mask):
    """从底部向上逐行扫描, 找到冷却遮罩的上边界。"""
    h, w = dark_mask.shape
    # 从底部向上扫描
    threshold = 0.5  # 一行中超过 50% 为暗色 → 视为冷却行
    cd_rows = 0
    for row in range(h - 1, -1, -1):
        row_dark = np.count_nonzero(dark_mask[row, :])
        if row_dark / max(1, w) > threshold:
            cd_rows += 1
        else:
            break
    return cd_rows / max(1, h)


class SkillBarTracker:
    """
    技能栏持续跟踪器。

    由 RecognitionEngine 在每帧调用，维护技能格位置缓存。
    """

    def __init__(self, state_mgr, settings):
        self._state = state_mgr
        self._settings = settings
        self._slots = []
        self._last_detect_time = 0
        self._detect_interval = 5.0     # 每 5s 重新检测格子位置
        self._enabled = True

    @property
    def slots(self):
        return self._slots

    def tick(self, img_bgr):
        """
        每帧调用: 更新技能冷却状态并推送到 GameState。

        Parameters
        ----------
        img_bgr : np.ndarray
            技能栏 ROI 截图 (BGR)
        """
        if not self._enabled or img_bgr is None or img_bgr.size == 0:
            return

        now = time.time()

        # 周期性重新检测格子位置
        if not self._slots or now - self._last_detect_time > self._detect_interval:
            self._slots = detect_skill_slots(img_bgr)
            self._last_detect_time = now
            if self._slots:
                log.debug(f'Detected {len(self._slots)} skill slots')

        if not self._slots:
            return

        # 检测冷却
        from config import BAR_COLORS
        cd_cfg = BAR_COLORS.get('skill_cooldown', {})
        detect_cooldowns(img_bgr, self._slots, cd_cfg)

        # 推送到 GameState
        skill_data = []
        for s in self._slots:
            skill_data.append({
                'index': s.get('index', 0),
                'cooldown_pct': s.get('cooldown_pct', 0.0),
                'active': s.get('active', False),
                'name': '',
            })
        self._state.update(skill_slots=skill_data)

    def reset(self):
        """重置格子缓存 (窗口大小变化时调用)。"""
        self._slots = []
        self._last_detect_time = 0
