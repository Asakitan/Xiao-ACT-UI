# -*- coding: utf-8 -*-
"""
SAO Auto — 截图与识别层

功能:
  - 截取游戏窗口 / 指定 ROI 区域
  - HP / 体力条像素长度识别
  - OCR 识别名字 / 等级 / ID / 数字
"""

import re
import time
import threading
import numpy as np
from typing import Optional, Tuple

from config import DEFAULT_ROI, BAR_COLORS, CAPTURE_FPS
from window_locator import WindowLocator
from game_state import GameStateManager

# ═══════════════════════════════════════════════
#  截图 — 线程安全: 每个线程拥有独立的 mss 实例
# ═══════════════════════════════════════════════
_mss_local = threading.local()

def _grab_region(bbox: Tuple[int, int, int, int]) -> Optional[np.ndarray]:
    """截取屏幕区域，返回 BGR numpy 数组。优先 mss，回退 PIL。"""
    try:
        import mss
        if not hasattr(_mss_local, 'sct') or _mss_local.sct is None:
            _mss_local.sct = mss.mss()
        mon = {'left': bbox[0], 'top': bbox[1],
               'width': bbox[2] - bbox[0], 'height': bbox[3] - bbox[1]}
        raw = _mss_local.sct.grab(mon)
        # mss 返回 BGRA
        arr = np.frombuffer(raw.rgb, dtype=np.uint8).reshape(raw.height, raw.width, 3)
        # RGB → BGR for cv2 compatibility
        return arr[:, :, ::-1].copy()
    except Exception as _e:
        # 实例可能损坏 — 丢弃让下次重建
        _mss_local.sct = None
        print(f'[识别] mss 截图失败: {_e}')
    try:
        from PIL import ImageGrab
        img = ImageGrab.grab(bbox=bbox, all_screens=True)
        arr = np.array(img)
        # RGB → BGR
        return arr[:, :, ::-1].copy()
    except Exception as _e:
        print(f'[识别] PIL 截图失败: {_e}')
    return None


def _grab_full_window(rect: Tuple[int, int, int, int]) -> Optional[np.ndarray]:
    """截取整个游戏窗口"""
    return _grab_region(rect)


# ═══════════════════════════════════════════════
#  条形值识别 (HP / 体力)
# ═══════════════════════════════════════════════
def _detect_bar_pct(img: np.ndarray, color_cfg: dict) -> float:
    """
    检测条形进度百分比。

    通过颜色过滤找到条形区域，计算填充宽度占总宽度的比例。

    Args:
        img: BGR 图像 (截取的条形区域)
        color_cfg: {'h_min', 'h_max', 's_min', 's_max', 'v_min', 'v_max'}

    Returns:
        0.0 ~ 1.0 百分比
    """
    try:
        import cv2
        # 预处理: 高斯模糊减少噪点
        img = cv2.GaussianBlur(img, (3, 3), 0)
        hsv = cv2.cvtColor(img, cv2.COLOR_BGR2HSV)
        lower = np.array([color_cfg['h_min'], color_cfg['s_min'], color_cfg['v_min']])
        upper = np.array([color_cfg['h_max'], color_cfg['s_max'], color_cfg['v_max']])
        mask = cv2.inRange(hsv, lower, upper)

        # 后处理: 形态学闭操作填充条形小空隙
        kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (5, 1))
        mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)

        h, w = mask.shape
        if w < 2 or h < 2:
            return 0.0

        # 按列统计: 每列有多少像素命中颜色
        col_sum = mask.mean(axis=0)  # shape: (w,)
        threshold = 128  # 超过半数像素命中视为"填充"

        filled = col_sum >= threshold
        if not np.any(filled):
            return 0.0

        # 找最大连续填充区间 (避免右侧 tooltip/光标干扰)
        best_start, best_len = 0, 0
        cur_start, cur_len = 0, 0
        for i in range(w):
            if filled[i]:
                if cur_len == 0:
                    cur_start = i
                cur_len += 1
            else:
                if cur_len > best_len:
                    best_start, best_len = cur_start, cur_len
                cur_len = 0
        if cur_len > best_len:
            best_start, best_len = cur_start, cur_len

        if best_len == 0:
            return 0.0

        last_filled = best_start + best_len - 1
        pct = (last_filled + 1) / w
        return min(1.0, max(0.0, pct))
    except ImportError:
        # 无 cv2: 用纯 numpy 亮度估计
        return _detect_bar_pct_simple(img)


def _detect_bar_pct_simple(img: np.ndarray) -> float:
    """无 cv2 的简易条形检测: 按行平均亮度寻找断点"""
    h, w, _ = img.shape
    if w < 2:
        return 0.0
    # 取中间行的亮度
    mid_rows = img[h // 3: 2 * h // 3, :, :]
    brightness = mid_rows.mean(axis=(0, 2))  # shape: (w,)
    avg = brightness.mean()
    threshold = avg * 0.6

    filled_cols = np.where(brightness >= threshold)[0]
    if len(filled_cols) == 0:
        return 0.0
    last = filled_cols[-1]
    return min(1.0, (last + 1) / w)


# ═══════════════════════════════════════════════
#  OCR 文本识别
# ═══════════════════════════════════════════════
_OCR_ENGINE = None
_OCR_INIT_TRIED = False
_OCR_INIT_FAILURES = 0
_OCR_LAST_RETRY_T = 0.0
_OCR_MAX_RETRIES = 3
_OCR_RETRY_INTERVAL = 30.0
_MIN_OCR_CONFIDENCE = 0.5


def _init_ocr():
    """懒初始化 OCR 引擎。失败后允许以 30 秒间隔重试，最多 3 次。"""
    global _OCR_ENGINE, _OCR_INIT_TRIED, _OCR_INIT_FAILURES, _OCR_LAST_RETRY_T
    if _OCR_ENGINE is not None:
        return _OCR_ENGINE
    if _OCR_INIT_TRIED:
        if _OCR_INIT_FAILURES >= _OCR_MAX_RETRIES:
            return None
        if time.time() - _OCR_LAST_RETRY_T < _OCR_RETRY_INTERVAL:
            return None
    _OCR_INIT_TRIED = True
    _OCR_LAST_RETRY_T = time.time()

    # 尝试 PaddleOCR
    try:
        from paddleocr import PaddleOCR
        _OCR_ENGINE = PaddleOCR(use_angle_cls=False, lang='ch', show_log=False)
        print('[识别] OCR 引擎: PaddleOCR')
        return _OCR_ENGINE
    except Exception:
        pass

    # 尝试 EasyOCR
    try:
        import easyocr
        _OCR_ENGINE = easyocr.Reader(['ch_sim', 'en'], gpu=False, verbose=False)
        print('[识别] OCR 引擎: EasyOCR')
        return _OCR_ENGINE
    except Exception:
        pass

    # 尝试 Tesseract
    try:
        import pytesseract
        _OCR_ENGINE = 'tesseract'
        print('[识别] OCR 引擎: Tesseract')
        return _OCR_ENGINE
    except Exception:
        pass

    _OCR_INIT_FAILURES += 1
    remaining = _OCR_MAX_RETRIES - _OCR_INIT_FAILURES
    print(f'[识别] 警告: 无可用 OCR 引擎 (剩余重试 {remaining} 次)')
    return None


def _ocr_image(img: np.ndarray) -> str:
    """对图像执行 OCR，返回识别文本 (低置信度结果已过滤)"""
    engine = _init_ocr()
    if engine is None:
        return ''

    try:
        # PaddleOCR
        if hasattr(engine, 'ocr'):
            rgb = img[:, :, ::-1]
            results = engine.ocr(rgb, cls=False)
            if results and results[0]:
                texts = []
                for line in results[0]:
                    if line[1]:
                        text, conf = line[1]
                        if conf >= _MIN_OCR_CONFIDENCE:
                            texts.append(text)
                return ' '.join(texts)
            return ''

        # EasyOCR
        if hasattr(engine, 'readtext'):
            results = engine.readtext(img)
            texts = [r[1] for r in results if r[1] and r[2] >= _MIN_OCR_CONFIDENCE]
            return ' '.join(texts)

        # Tesseract (无置信度)
        if engine == 'tesseract':
            import pytesseract
            from PIL import Image
            pil_img = Image.fromarray(img[:, :, ::-1])
            text = pytesseract.image_to_string(pil_img, lang='chi_sim+eng')
            return text.strip()
    except Exception as e:
        print(f'[识别] OCR 错误: {e}')

    return ''


def _ocr_numbers(img: np.ndarray) -> str:
    """专门识别数字文本 (HP/体力值)"""
    engine = _init_ocr()
    if engine is None:
        return ''

    try:
        if engine == 'tesseract':
            import pytesseract
            from PIL import Image
            pil_img = Image.fromarray(img[:, :, ::-1])
            text = pytesseract.image_to_string(
                pil_img, config='--psm 7 -c tessedit_char_whitelist=0123456789/')
            return text.strip()

        # 通用 OCR 回退
        return _ocr_image(img)
    except Exception:
        return ''


# ═══════════════════════════════════════════════
#  数值解析
# ═══════════════════════════════════════════════
def _parse_hp_text(text: str) -> Tuple[int, int]:
    """解析 HP 文本 '406569/406569' → (current, max)"""
    text = text.replace(' ', '').replace(',', '')
    m = re.search(r'(\d+)\s*/\s*(\d+)', text)
    if m:
        return (int(m.group(1)), int(m.group(2)))
    # 尝试匹配单个数字
    nums = re.findall(r'\d+', text)
    if len(nums) >= 2:
        return (int(nums[0]), int(nums[1]))
    return (0, 0)


def _parse_level_text(text: str) -> Tuple[int, int]:
    """
    解析等级文本 '等级60(+12)' → (60, 12)
    支持格式: '60(+12)', 'Lv60(+12)', '等级60(+12)', '60（+12）',
             'LV6', 'LV.6', 'Lv 6', 'L6', 'V6', 'lv6'
    """
    text = text.replace('（', '(').replace('）', ')').replace(' ', '')
    text = text.replace('，', '').replace('.', '').replace('。', '')
    # 尝试 xx(+yy) 格式
    m = re.search(r'(\d+)\s*\(\s*\+?\s*(\d+)\s*\)', text)
    if m:
        return (int(m.group(1)), int(m.group(2)))
    # LV/Lv/lv + 数字 (包括单位数)
    m = re.search(r'[Ll][Vv]\s*\.?\s*(\d+)', text)
    if m:
        return (int(m.group(1)), 0)
    # OCR 误读: L 或 V + 数字 (常见 OCR 将 Lv 拆分)
    m = re.search(r'[LV](\d+)', text, re.IGNORECASE)
    if m:
        val = int(m.group(1))
        if 1 <= val <= 999:
            return (val, 0)
    # 等级 + 数字
    m = re.search(r'等级\s*(\d+)', text)
    if m:
        return (int(m.group(1)), 0)
    # 纯数字 (最后手段)
    m = re.search(r'(\d+)', text)
    if m:
        val = int(m.group(1))
        if 1 <= val <= 999:
            return (val, 0)
    return (0, 0)


def _parse_stamina_text(text: str) -> Tuple[int, int]:
    """解析体力文本 '1200/1200' → (current, max)"""
    return _parse_hp_text(text)  # 格式相同


# ═══════════════════════════════════════════════
#  采集循环
# ═══════════════════════════════════════════════
class RecognitionEngine:
    """持续采集 + 识别 → 更新 GameState"""

    def __init__(self, state_mgr: GameStateManager, settings=None):
        self._state_mgr = state_mgr
        self._locator = WindowLocator()
        self._settings = settings
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._fps = CAPTURE_FPS
        self._debug_callback = None    # 调试回调: fn(roi_name, img)
        self._ocr_enabled = True
        self._bar_detect_enabled = True
        self._last_ocr_t = 0.0        # 初始化 OCR 时间戳
        self._diag_logged = False      # 首次成功截图诊断
        self._no_window_logged = False # 首次找不到窗口诊断

        # ── 动态锚定 ROI ──
        self._anchored_rois: Optional[dict] = None
        self._anchor_window_rect: Optional[tuple] = None
        self._last_anchor_t = 0.0
        self._anchor_interval = 15.0
        self._anchor_logged = False

        # ── EMA 平滑 ──
        self._ema_hp_pct: Optional[float] = None
        self._ema_sta_pct: Optional[float] = None
        self._ema_alpha = 0.35

        # ── 异常值拦截 (连续确认机制) ──
        self._prev_hp_cur = 0
        self._prev_hp_max = 0
        self._prev_sta_cur = 0
        self._prev_sta_max = 0
        self._pending_hp: Optional[Tuple[int, int]] = None
        self._pending_sta: Optional[Tuple[int, int]] = None
        self._hp_confirm_count = 0
        self._sta_confirm_count = 0
        self._outlier_threshold = 0.5
        self._confirm_needed = 2

        # ── 技能栏跟踪 ──
        try:
            from skill_recognition import SkillBarTracker
            self._skill_tracker = SkillBarTracker(state_mgr, settings)
        except Exception:
            self._skill_tracker = None

    def start(self):
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        print('[识别] 采集引擎已启动')

    def stop(self):
        self._running = False
        print('[识别] 采集引擎已停止')

    def set_debug_callback(self, cb):
        self._debug_callback = cb

    def _validate_hp_value(self, cur: int, mx: int) -> Tuple[int, int]:
        """若新 HP 与前值差距 >50%，需连续 2 帧确认后才接受。"""
        if self._prev_hp_max == 0:
            self._prev_hp_cur, self._prev_hp_max = cur, mx
            return cur, mx
        prev_pct = self._prev_hp_cur / self._prev_hp_max
        new_pct = cur / mx if mx > 0 else 0
        if abs(new_pct - prev_pct) > self._outlier_threshold:
            if self._pending_hp == (cur, mx):
                self._hp_confirm_count += 1
            else:
                self._pending_hp = (cur, mx)
                self._hp_confirm_count = 1
            if self._hp_confirm_count >= self._confirm_needed:
                self._prev_hp_cur, self._prev_hp_max = cur, mx
                self._pending_hp = None
                self._hp_confirm_count = 0
                return cur, mx
            return self._prev_hp_cur, self._prev_hp_max
        self._prev_hp_cur, self._prev_hp_max = cur, mx
        self._pending_hp = None
        self._hp_confirm_count = 0
        return cur, mx

    def _validate_sta_value(self, cur: int, mx: int) -> Tuple[int, int]:
        """若新体力与前值差距 >50%，需连续 2 帧确认后才接受。"""
        if self._prev_sta_max == 0:
            self._prev_sta_cur, self._prev_sta_max = cur, mx
            return cur, mx
        prev_pct = self._prev_sta_cur / self._prev_sta_max
        new_pct = cur / mx if mx > 0 else 0
        if abs(new_pct - prev_pct) > self._outlier_threshold:
            if self._pending_sta == (cur, mx):
                self._sta_confirm_count += 1
            else:
                self._pending_sta = (cur, mx)
                self._sta_confirm_count = 1
            if self._sta_confirm_count >= self._confirm_needed:
                self._prev_sta_cur, self._prev_sta_max = cur, mx
                self._pending_sta = None
                self._sta_confirm_count = 0
                return cur, mx
            return self._prev_sta_cur, self._prev_sta_max
        self._prev_sta_cur, self._prev_sta_max = cur, mx
        self._pending_sta = None
        self._sta_confirm_count = 0
        return cur, mx

    @property
    def locator(self) -> WindowLocator:
        return self._locator

    def _get_roi(self, name: str) -> dict:
        if self._settings:
            return self._settings.get_roi(name)
        return DEFAULT_ROI.get(name, {})

    def _loop(self):
        """主采集循环"""
        _fail_count = 0
        while self._running:
            t0 = time.time()
            try:
                self._tick()
                _fail_count = 0
            except Exception as e:
                _fail_count += 1
                if _fail_count <= 3:
                    print(f'[识别] 采集异常: {e}')
                self._state_mgr.update(recognition_ok=False, error_msg=str(e))
            elapsed = time.time() - t0
            sleep_t = max(0.01, (1.0 / self._fps) - elapsed)
            time.sleep(sleep_t)

    # ── 动态锚定: OCR 扫描底部找 "生命"/"体力" 关键词 ──
    def _find_anchors(self, rect):
        """扫描游戏窗口底部 25% 寻找 '生命' 和 '体力' 文本锚点.

        找到后计算 hp_bar / hp_text / stamina_bar / stamina_text 的绝对像素 bbox,
        缓存到 self._anchored_rois 供后续 _tick 使用.
        """
        wl, wt, wr, wb = rect
        ww, wh = wr - wl, wb - wt
        if ww < 100 or wh < 100:
            return

        # 截取底部 25%
        scan_top = wt + int(wh * 0.75)
        scan_bbox = (wl, scan_top, wr, wb)
        scan_img = _grab_region(scan_bbox)
        if scan_img is None or scan_img.size == 0:
            return

        engine = _init_ocr()
        if engine is None:
            return

        # 获取带 bbox 的 OCR 结果
        results = []
        try:
            if hasattr(engine, 'readtext'):
                # EasyOCR: [(bbox, text, conf), ...]
                results = engine.readtext(scan_img)
            elif hasattr(engine, 'ocr'):
                # PaddleOCR
                raw = engine.ocr(scan_img[:, :, ::-1], cls=False)
                if raw and raw[0]:
                    for line in raw[0]:
                        bbox_pts, (text, conf) = line
                        # 转换为 (tl, tr, br, bl) → 取 x_min, y_min, x_max, y_max
                        xs = [p[0] for p in bbox_pts]
                        ys = [p[1] for p in bbox_pts]
                        results.append(([min(xs), min(ys), max(xs), max(ys)], text, conf))
        except Exception as e:
            print(f'[识别] 锚定 OCR 失败: {e}')
            return

        hp_anchor = None
        sta_anchor = None
        for item in results:
            if hasattr(engine, 'readtext'):
                bbox, text, conf = item
                # EasyOCR bbox: [[x1,y1],[x2,y2],[x3,y3],[x4,y4]]
                xs = [p[0] for p in bbox]
                ys = [p[1] for p in bbox]
                cx = (min(xs) + max(xs)) / 2
                cy = (min(ys) + max(ys)) / 2
                bw = max(xs) - min(xs)
                bh = max(ys) - min(ys)
            else:
                bbox, text, conf = item
                cx = (bbox[0] + bbox[2]) / 2
                cy = (bbox[1] + bbox[3]) / 2
                bw = bbox[2] - bbox[0]
                bh = bbox[3] - bbox[1]

            text_clean = text.replace(' ', '')
            if '生命' in text_clean and hp_anchor is None:
                hp_anchor = (int(cx), int(cy), int(bw), int(bh))
            elif '体力' in text_clean and sta_anchor is None:
                sta_anchor = (int(cx), int(cy), int(bw), int(bh))

        if hp_anchor is None and sta_anchor is None:
            if not self._anchor_logged:
                print(f'[识别] 锚定: 未找到 "生命"/"体力" (共 {len(results)} 文本)')
            return

        anchored = {}
        scan_h = scan_img.shape[0]
        scan_w = scan_img.shape[1]

        if hp_anchor:
            ax, ay, aw, ah = hp_anchor
            # "生命" 文字中心 → HP 条在其右侧, 向下偏移一点
            # 条形: 从文字右边缘开始, 到窗口右侧 65%
            bar_left = wl + int(ax + aw * 0.6)   # 文字右侧
            bar_top = scan_top + int(ay - ah * 0.5)     # 和文字同行
            bar_right = min(wr, bar_left + int(ww * 0.30))
            bar_bottom = bar_top + max(8, int(ah * 1.5))
            anchored['hp_bar'] = (bar_left, bar_top, bar_right, bar_bottom)

            # HP 数字文本: 条形内或右侧
            txt_left = bar_left + int((bar_right - bar_left) * 0.3)
            txt_top = bar_top
            txt_right = bar_right
            txt_bottom = bar_bottom
            anchored['hp_text'] = (txt_left, txt_top, txt_right, txt_bottom)

        if sta_anchor:
            ax, ay, aw, ah = sta_anchor
            bar_left = wl + int(ax + aw * 0.6)
            bar_top = scan_top + int(ay - ah * 0.5)
            bar_right = min(wr, bar_left + int(ww * 0.30))
            bar_bottom = bar_top + max(8, int(ah * 1.5))
            anchored['stamina_bar'] = (bar_left, bar_top, bar_right, bar_bottom)

            txt_left = bar_left + int((bar_right - bar_left) * 0.3)
            txt_top = bar_top
            txt_right = bar_right
            txt_bottom = bar_bottom
            anchored['stamina_text'] = (txt_left, txt_top, txt_right, txt_bottom)

        self._anchored_rois = anchored
        self._anchor_window_rect = rect
        self._last_anchor_t = time.time()
        found = ', '.join(anchored.keys())
        print(f'[识别] 锚定成功: {found}')
        for k, v in anchored.items():
            print(f'  {k}: {v}')

    def _should_reanchor(self, rect):
        """是否需要重新锚定 (窗口变动 / 超时 / 首次)"""
        if self._anchored_rois is None:
            return True
        if time.time() - self._last_anchor_t > self._anchor_interval:
            return True
        if self._anchor_window_rect != rect:
            return True
        return False

    def _get_anchored_bbox(self, name, rect):
        """优先返回动态锚定的绝对 bbox, 不存在则回退到静态 ROI."""
        if self._anchored_rois and name in self._anchored_rois:
            return self._anchored_rois[name]
        roi = self._get_roi(name)
        return self._locator.roi_to_pixels(roi, rect)

    def _tick(self):
        """单次采集周期"""
        result = self._locator.find_game_window()
        if result is None:
            if not self._no_window_logged:
                self._no_window_logged = True
                print('[识别] 未找到游戏窗口 (关键词: ' + str(self._locator._keywords) + ')')
            self._state_mgr.update(
                recognition_ok=False,
                error_msg='未找到游戏窗口',
                window_rect=None,
            )
            return
        self._no_window_logged = False

        hwnd, title, rect = result
        ww = rect[2] - rect[0]
        wh = rect[3] - rect[1]

        updates = {
            'window_rect': rect,
            'window_width': ww,
            'window_height': wh,
            'recognition_ok': True,
            'error_msg': '',
        }

        # ── 动态锚定 (低频自动执行) ──
        if self._should_reanchor(rect):
            try:
                self._find_anchors(rect)
            except Exception as e:
                print(f'[识别] 锚定异常: {e}')

        # ── HP 条像素识别 + EMA 平滑 ──
        if self._bar_detect_enabled:
            hp_bbox = self._get_anchored_bbox('hp_bar', rect)
            if hp_bbox:
                hp_img = _grab_region(hp_bbox)
                if hp_img is not None and hp_img.size > 0:
                    raw_pct = _detect_bar_pct(hp_img, BAR_COLORS['hp'])
                    if self._ema_hp_pct is None:
                        self._ema_hp_pct = raw_pct
                    else:
                        self._ema_hp_pct = self._ema_alpha * raw_pct + (1 - self._ema_alpha) * self._ema_hp_pct
                    hp_pct = max(0.0, min(1.0, self._ema_hp_pct))
                    updates['hp_pct'] = hp_pct
                    if not self._diag_logged:
                        self._diag_logged = True
                        anchored = '(anchored)' if self._anchored_rois and 'hp_bar' in self._anchored_rois else '(static)'
                        print(f'[识别] HP ROI bbox={hp_bbox}, img={hp_img.shape}, hp_pct={hp_pct:.2f} {anchored}')
                    if self._debug_callback:
                        self._debug_callback('hp_bar', hp_img)

            # ── 体力条像素识别 + EMA 平滑 ──
            st_bbox = self._get_anchored_bbox('stamina_bar', rect)
            if st_bbox:
                st_img = _grab_region(st_bbox)
                if st_img is not None and st_img.size > 0:
                    raw_pct = _detect_bar_pct(st_img, BAR_COLORS['stamina'])
                    if self._ema_sta_pct is None:
                        self._ema_sta_pct = raw_pct
                    else:
                        self._ema_sta_pct = self._ema_alpha * raw_pct + (1 - self._ema_alpha) * self._ema_sta_pct
                    st_pct = max(0.0, min(1.0, self._ema_sta_pct))
                    updates['stamina_pct'] = st_pct
                    if self._debug_callback:
                        self._debug_callback('stamina_bar', st_img)

        # ── OCR 识别 (低频: 每 2 秒一次) ──
        if self._ocr_enabled and (time.time() - self._last_ocr_t < 2.0):
            self._state_mgr.update(**updates)
            return
        self._last_ocr_t = time.time()

        if self._ocr_enabled:
            # HP 数字 (含异常值拦截)
            hp_txt_bbox = self._get_anchored_bbox('hp_text', rect)
            if hp_txt_bbox:
                hp_txt_img = _grab_region(hp_txt_bbox)
                if hp_txt_img is not None and hp_txt_img.size > 0:
                    txt = _ocr_numbers(hp_txt_img)
                    cur, mx = _parse_hp_text(txt)
                    if mx > 0:
                        cur, mx = self._validate_hp_value(cur, mx)
                        if mx > 0:
                            updates['hp_current'] = cur
                            updates['hp_max'] = mx
                            updates['hp_pct'] = cur / mx

            # 体力数字 (含异常值拦截)
            st_txt_bbox = self._get_anchored_bbox('stamina_text', rect)
            if st_txt_bbox:
                st_txt_img = _grab_region(st_txt_bbox)
                if st_txt_img is not None and st_txt_img.size > 0:
                    txt = _ocr_numbers(st_txt_img)
                    cur, mx = _parse_stamina_text(txt)
                    if mx > 0:
                        cur, mx = self._validate_sta_value(cur, mx)
                        if mx > 0:
                            updates['stamina_current'] = cur
                            updates['stamina_max'] = mx
                            updates['stamina_pct'] = cur / mx

            # 等级 — 只在识别成功 (base>0) 时更新，保留上次有效值
            lv_roi = self._get_roi('level')
            lv_bbox = self._locator.roi_to_pixels(lv_roi, rect)
            if lv_bbox:
                lv_img = _grab_region(lv_bbox)
                if lv_img is not None and lv_img.size > 0:
                    # 预处理: 放大2× + 自适应阈值增强小文字可读性
                    try:
                        import cv2
                        h_lv, w_lv = lv_img.shape[:2]
                        lv_big = cv2.resize(lv_img, (w_lv * 2, h_lv * 2),
                                            interpolation=cv2.INTER_CUBIC)
                        gray = cv2.cvtColor(lv_big, cv2.COLOR_BGR2GRAY)
                        thresh = cv2.adaptiveThreshold(
                            gray, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
                            cv2.THRESH_BINARY, 11, 2)
                        lv_proc = cv2.cvtColor(thresh, cv2.COLOR_GRAY2BGR)
                    except Exception:
                        lv_proc = lv_img
                    txt = _ocr_image(lv_proc)
                    base, extra = _parse_level_text(txt)
                    if base > 0:
                        updates['level_base'] = base
                        updates['level_extra'] = extra

            # 名字 — 需含字母/汉字，保留上次有效值
            nm_roi = self._get_roi('name')
            nm_bbox = self._locator.roi_to_pixels(nm_roi, rect)
            if nm_bbox:
                nm_img = _grab_region(nm_bbox)
                if nm_img is not None and nm_img.size > 0:
                    txt = _ocr_image(nm_img)
                    cleaned = txt.strip()
                    if cleaned and len(cleaned) <= 20 and re.search(r'[\w\u4e00-\u9fff]', cleaned):
                        updates['player_name'] = cleaned

            # 玩家 ID — 至少 3 位数字，保留上次有效值
            id_roi = self._get_roi('player_id')
            id_bbox = self._locator.roi_to_pixels(id_roi, rect)
            if id_bbox:
                id_img = _grab_region(id_bbox)
                if id_img is not None and id_img.size > 0:
                    txt = _ocr_numbers(id_img)
                    nums = re.findall(r'\d+', txt)
                    if nums and len(nums[0]) >= 3:
                        updates['player_id'] = nums[0]

        # ── 技能栏冷却检测 ──
        if self._skill_tracker is not None:
            try:
                sk_bbox = self._get_anchored_bbox('skill_bar', rect)
                if sk_bbox is None:
                    sk_roi = self._get_roi('skill_bar')
                    sk_bbox = self._locator.roi_to_pixels(sk_roi, rect)
                if sk_bbox:
                    sk_img = _grab_region(sk_bbox)
                    if sk_img is not None and sk_img.size > 0:
                        self._skill_tracker.tick(sk_img)
            except Exception:
                pass

        self._state_mgr.update(**updates)

    def single_capture(self) -> Optional[dict]:
        """手动触发一次采集，返回结果 dict"""
        self._tick()
        return self._state_mgr.state.to_dict()
