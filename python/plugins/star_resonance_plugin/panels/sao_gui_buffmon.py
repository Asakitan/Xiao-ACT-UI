# -*- coding: utf-8 -*-
# sao_gui_buffmon.py — SAO 主面板风格 Buff 监视器 (ULW + PIL)
#
# 提供两个 overlay:
# - SelfBuffOverlay : 自身 "奥义/幻想 buff" 监视器, 锚定在 HpOverlay (ID 面板) 上方
# - BossBuffOverlay : 当前锁定 boss 的 buff 监视器, 锚定在 BossHpOverlay 右侧
#
# 风格沿用 DpsOverlay 的奶油渐变 shell + 青/金角标 + 微扫描线 + 浅胶囊行。
# 有 buff → 滑入 (淡入 + 12 px 横滑), 没有 buff → 滑出 (淡出) → ShowWindow(SW_HIDE).
# 鼠标穿透, 不抢焦点。
from __future__ import annotations

import time
import math
import ctypes
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import tkinter as tk
from PIL import Image, ImageDraw, ImageFilter, ImageFont

from plugins.star_resonance_plugin.panels.sao_gui_dps import (
    _ulw_update, _user32, _load_font, _pick_font, _text_width,
    GWL_EXSTYLE, WS_EX_LAYERED, WS_EX_TOOLWINDOW, WS_EX_TOPMOST,
    WS_EX_TRANSPARENT,
)
from gui_modules.entity_gpu_policy import require_entity_gpu

try:
    from render.gpu_renderer import gaussian_blur_rgba as _gpu_blur
except Exception:  # pragma: no cover
    def _gpu_blur(img, radius):
        return img.filter(ImageFilter.GaussianBlur(radius))

# GPU presenter pipeline (sao_left_info_gpu / sao_gui_skillfx 同款)
try:
    from render import gpu_overlay_window as _gow
except Exception:  # pragma: no cover
    _gow = None  # type: ignore[assignment]
try:
    from render.overlay_render_worker import AsyncFrameWorker, FrameBuffer
except Exception:  # pragma: no cover
    AsyncFrameWorker = None  # type: ignore[assignment]
    FrameBuffer = None  # type: ignore[assignment]


def _gpu_buffmon_enabled() -> bool:
    # BuffMon requires the shared Entity GPU backend.
    try:
        from sr_config import USE_GPU_BUFFMON as _flag
    except Exception:
        _flag = True
    if not _flag:
        raise RuntimeError('BuffMon requires USE_GPU_BUFFMON=True; no ULW fallback is available')
    if _gow is None or AsyncFrameWorker is None:
        raise RuntimeError('BuffMon requires GPU overlay and AsyncFrameWorker; no ULW fallback is available')
    return require_entity_gpu('BuffMon', _gow)


# ─────────────────────────────────────────
#  Debug logger (config.BUFFMON_DEBUG)
# ─────────────────────────────────────────
def _dbg(msg: str):
    try:
        from sr_config import BUFFMON_DEBUG as _flag
    except Exception:
        _flag = False
    if _flag:
        print(f'[BuffMon] {msg}', flush=True)


def _default_filter_mode() -> str:
    try:
        from sr_config import BUFFMON_SELF_FILTER as _mode
        return str(_mode or 'ultimate').lower()
    except Exception:
        return 'ultimate'


def _finite_float(value: Any, default: float = 0.0, *, lo: float | None = None, hi: float | None = None) -> float:
    try:
        number = float(default if value is None or value == '' else value)
    except Exception:
        number = float(default or 0.0)
    if not math.isfinite(number):
        number = float(default or 0.0)
    if lo is not None:
        number = max(float(lo), number)
    if hi is not None:
        number = min(float(hi), number)
    return number


def _finite_int(value: Any, default: int = 0, *, lo: int | None = None, hi: int | None = None) -> int:
    number = int(_finite_float(value, float(default), lo=lo, hi=hi))
    if lo is not None:
        number = max(int(lo), number)
    if hi is not None:
        number = min(int(hi), number)
    return number


# ─────────────────────────────────────────
#  奥义 / 幻想 buff 过滤器
# ─────────────────────────────────────────
# 「幻想技能」(绝技/奥义/职业大招) buff 判定关键词。完整集 = 分类器
# _ULTIMATE_RE 的通用词 (奥义/幻想/终极/绝技/大招/ULT) ∪ 11 职业大招主名。
ULTIMATE_KEYWORDS: Tuple[str, ...] = (
    '奥义', '幻想', '终极', '绝技', '大招', 'ULT', 'ult',
    # 11 个职业的大招主名 (PROFESSION_ULTIMATE 派生)
    '极诣', '极寒', '炎魔', '风神', '繁盛', '雷爆溟灭',
    '岩御', '神灵凭依', '锐眼·光能', '凛威', '升格',
)

_ULT_ID_SET = None


def _ultimate_skill_id_set():
    # 游戏真实「幻想技能」大招技能ID集 (ACT 分类器单一源头, 懒加载缓存)。
    #
    # = name_table_classifier.ultimate_skill_ids() (aoyi_skill_names +
    # PROFESSION_ULTIMATE)，让面板按游戏真实分类而非纯关键词猜测。
    global _ULT_ID_SET
    if _ULT_ID_SET is None:
        try:
            from tools.tablekit.name_table_classifier import ultimate_skill_ids
            _ULT_ID_SET = frozenset(ultimate_skill_ids())
        except Exception:
            _ULT_ID_SET = frozenset()
    return _ULT_ID_SET


def is_ultimate_buff(name: str, buff_id: int = 0) -> bool:
    # True 当 buff 属于「幻想技能」(绝技/奥义/职业大招)。
    #
    # 判定优先级: ① buff_id 命中游戏真实大招技能ID集 (权威, 名字缺失也能识别)
    # → ② buff 名命中完整关键词集。
    buff_id = _finite_int(buff_id, 0, lo=0)
    if buff_id and buff_id in _ultimate_skill_id_set():
        return True
    if not name:
        return False
    for kw in ULTIMATE_KEYWORDS:
        if kw in name:
            return True
    return False


# ─────────────────────────────────────────
#  通用 base class
# ─────────────────────────────────────────
class _BuffPanelBase:
    # 主面板风格 buff 倒计时面板的基础实现。
    #
    # 子类需要重写 `_resolve_anchor_xy()` 决定屏幕位置。

    # ── 几何 ──
    WIDTH = 220
    ROW_H = 24
    ROW_GAP = 4
    PAD_X = 12
    PAD_TOP = 30          # 留出 header 空间 + 上 padding
    PAD_BOT = 10
    HEADER_H = 22
    PILL_BEVEL = 8        # web/dps.html .entity-row 风格切角
    SHADOW_PAD = 8
    CORNER_SIZE = 12
    SLIDE_OFFSET = 12     # 滑入 / 滑出 的水平像素
    SHELL_CUT = 14        # shell 切角 (右上 + 左下) — 比 DPS 主面板小一点更精致

    # ── 调色 — light/dark 双主题, 与 web/buff_coverage.html 的 CSS 变量 1:1 ──
    # light = cream hi-tech 主面板系 (复刻 DpsOverlay v2.2.0);
    # dark  = 深蓝黑系 (对齐 buff_coverage.html 暗色变量 + panel 家族 dark 调色)。
    # 主题键 = settings panel_themes['buffmon'], 经 _apply_theme() 切换。
    _PALETTES = {
        'light': {
            'PANEL_BG_A': (250, 252, 253, 245),
            'PANEL_BG_B': (220, 224, 229, 245),
            'PANEL_EDGE': (128, 190, 220, 255),
            'INNER_HIGHLIGHT': (255, 255, 255, 255),
            'PANEL_LINE': (255, 255, 255, 255),
            'SHELL_SHEEN_CYAN': (104, 228, 255, 28),
            'SHELL_SHEEN_SHADOW': (42, 52, 64, 22),
            'SCAN_LINE': (104, 228, 255, 18),
            'HEADER_TEXT': (104, 138, 162, 255),
            'HEADER_TICK': (104, 228, 255, 90),
            'HEADER_SEP': (255, 255, 255, 200),
            'TEXT_MAIN': (90, 92, 100, 255),
            'TEXT_MUTED': (140, 135, 138, 255),
            'GOLD': (222, 166, 32, 255),
            'CYAN_DEEP': (50, 130, 170, 255),
            'ROW_BG': (248, 247, 244, 200),
            'ROW_BORDER': (156, 178, 194, 200),
            'ROW_BG_URGENT': (244, 168, 152, 230),
            'ROW_BG_CRITICAL': (240, 110, 90, 240),
            'ROW_TEXT_URGENT': (140, 30, 20, 255),
            'ROW_TOP_SHEEN': (255, 255, 255, 90),
            'BADGE_FILL': (255, 255, 255, 80),
            'UPTIME_TRACK': (255, 255, 255, 34),
        },
        'dark': {
            'PANEL_BG_A': (24, 32, 42, 245),
            'PANEL_BG_B': (12, 16, 22, 245),
            'PANEL_EDGE': (70, 160, 200, 255),
            'INNER_HIGHLIGHT': (255, 255, 255, 36),
            'PANEL_LINE': (255, 255, 255, 40),
            'SHELL_SHEEN_CYAN': (104, 228, 255, 22),
            'SHELL_SHEEN_SHADOW': (0, 0, 0, 30),
            'SCAN_LINE': (104, 228, 255, 12),
            'HEADER_TEXT': (140, 180, 205, 255),
            'HEADER_TICK': (104, 228, 255, 90),
            'HEADER_SEP': (255, 255, 255, 50),
            'TEXT_MAIN': (232, 240, 244, 255),     # web --ink
            'TEXT_MUTED': (159, 179, 191, 255),    # web --ink-dim
            'GOLD': (255, 207, 90, 255),           # web --gold
            'CYAN_DEEP': (110, 200, 230, 255),
            'ROW_BG': (34, 46, 56, 200),           # web --row
            'ROW_BORDER': (70, 224, 216, 48),
            'ROW_BG_URGENT': (120, 54, 40, 230),   # web --row-urgent
            'ROW_BG_CRITICAL': (150, 44, 30, 240),
            'ROW_TEXT_URGENT': (255, 138, 106, 255),  # web .urgent .sec
            'ROW_TOP_SHEEN': (255, 255, 255, 28),
            'BADGE_FILL': (255, 255, 255, 18),
            'UPTIME_TRACK': (255, 255, 255, 26),
        },
    }
    # 角标亮点两主题共用 (青/金 accent 在深浅底上都成立)
    CORNER_CYAN = (104, 228, 255, 255)
    CORNER_GOLD = (222, 190, 80, 255)
    CORNER_CYAN_ACCENT = (104, 228, 255, 120)
    CORNER_GOLD_ACCENT = (222, 190, 80, 120)

    # 类级默认 = light (实例化后由 _apply_theme 按 settings 覆盖为实际主题)
    PANEL_BG_A = _PALETTES['light']['PANEL_BG_A']
    PANEL_BG_B = _PALETTES['light']['PANEL_BG_B']
    PANEL_EDGE = _PALETTES['light']['PANEL_EDGE']
    INNER_HIGHLIGHT = _PALETTES['light']['INNER_HIGHLIGHT']
    PANEL_LINE = _PALETTES['light']['PANEL_LINE']
    SHELL_SHEEN_CYAN = _PALETTES['light']['SHELL_SHEEN_CYAN']
    SHELL_SHEEN_SHADOW = _PALETTES['light']['SHELL_SHEEN_SHADOW']
    SCAN_LINE = _PALETTES['light']['SCAN_LINE']
    HEADER_TEXT = _PALETTES['light']['HEADER_TEXT']
    HEADER_TICK = _PALETTES['light']['HEADER_TICK']
    HEADER_SEP = _PALETTES['light']['HEADER_SEP']
    TEXT_MAIN = _PALETTES['light']['TEXT_MAIN']
    TEXT_MUTED = _PALETTES['light']['TEXT_MUTED']
    GOLD = _PALETTES['light']['GOLD']
    CYAN_DEEP = _PALETTES['light']['CYAN_DEEP']
    ROW_BG = _PALETTES['light']['ROW_BG']
    ROW_BORDER = _PALETTES['light']['ROW_BORDER']
    ROW_BG_URGENT = _PALETTES['light']['ROW_BG_URGENT']
    ROW_BG_CRITICAL = _PALETTES['light']['ROW_BG_CRITICAL']
    ROW_TEXT_URGENT = _PALETTES['light']['ROW_TEXT_URGENT']
    ROW_TOP_SHEEN = _PALETTES['light']['ROW_TOP_SHEEN']
    BADGE_FILL = _PALETTES['light']['BADGE_FILL']
    UPTIME_TRACK = _PALETTES['light']['UPTIME_TRACK']

    # ── 阈值 ──
    URGENT_LT = 3.0
    CRITICAL_LT = 1.0

    # ── 动画 ──
    FADE_IN_S = 0.30
    FADE_OUT_S = 0.24
    TICK_MS = 80          # 12.5 FPS 足够秒级倒计时

    # 子类可覆盖
    HEADER_LABEL = ''           # 'BUFFS' / 'TARGET BUFFS' / ''
    BADGE_LABEL = ''            # 'AURA' / 'TARGET'
    BADGE_COLOR = (104, 228, 255, 255)
    HEADER_KICKER = ''          # 'SYSTEM CALL' 风格小标签

    def __init__(self, root: tk.Tk, settings: Any = None,
                 anchor: Any = None, name: str = 'buff'):
        self.root = root
        self.settings = settings
        self._anchor = anchor
        self._name = name

        # 共用状态
        self._enabled = True
        self._destroyed = False
        self._tick_id = None

        # 动画 state
        self._vis_target = 0.0
        self._vis_value = 0.0
        self._anim_start_t = 0.0
        self._anim_from = 0.0
        self._anim_dur = self.FADE_IN_S

        # 缓存 base_img (不含动画) — 仅在 row 内容/秒数变化时重画
        self._cached_base: Optional[Image.Image] = None
        # shell 部件单槽缓存 — 键 (sw, sh, theme), 见 _shell_parts
        self._shell_parts_cache = None
        self._cached_total_h = 0
        self._cached_sig: Tuple = ()
        # base_img 重建代数; compose 签名用它而不是 id(base_img) —
        # 释放后的地址复用会让 id() 撞车, 把真实新帧当成旧帧跳过。
        self._cached_seq = 0

        # ── GPU presenter 路径 (优先) ──
        self._gpu_enabled = _gpu_buffmon_enabled()
        self._gpu_window: Optional[Any] = None
        self._gpu_presenter: Optional[Any] = None
        self._gpu_worker: Optional[Any] = None
        self._gpu_visible: bool = False  # GpuOverlayWindow show 状态
        self._gpu_last_geom: Optional[Tuple[int, int, int, int]] = None
        self._gpu_last_compose_sig: Tuple = ()

        # ── Legacy ULW fields (inactive; kept only until dead-code removal) ──
        self._win: Optional[tk.Toplevel] = None
        self._hwnd: int = 0
        self._ulw_created = False
        self._visible_hwnd = False

        try:
            self._gpu_worker = AsyncFrameWorker(prefer_isolation=False)
            _dbg(f'[{self._name}] GPU worker created')
        except Exception as e:
            self._gpu_enabled = False
            self._gpu_worker = None
            raise RuntimeError(
                f'BuffMon {self._name} requires GPU worker support; no ULW fallback is available'
            ) from e

        # 主题: settings panel_themes['buffmon'] (默认 dark, 与 Web 端/面板家族一致)
        self._theme = 'dark'
        self._apply_theme(self._initial_theme())

        self._schedule_tick()

    # ─────────────────────────────────────
    #  Theme (light/dark, 与 web/buff_coverage.html 1:1)
    # ─────────────────────────────────────
    def _initial_theme(self) -> str:
        try:
            cfg = self.settings
            themes = (cfg.get('panel_themes', {}) if cfg is not None else {}) or {}
            return 'light' if str(themes.get('buffmon', 'dark')).lower() == 'light' else 'dark'
        except Exception:
            return 'dark'

    def _apply_theme(self, theme: str):
        # 切换 light/dark 调色板并失效所有渲染缓存 (菜单皮肤切换入口)。
        theme = 'light' if str(theme or '').lower() == 'light' else 'dark'
        palette = self._PALETTES.get(theme) or self._PALETTES['dark']
        for attr, value in palette.items():
            setattr(self, attr, value)
        self._theme = theme
        self._cached_base = None
        self._cached_sig = ()
        self._cached_seq += 1
        self._gpu_last_compose_sig = ()

    # ─────────────────────────────────────
    #  Window — legacy ULW (inactive; scheduled for removal)
    # ─────────────────────────────────────
    def _init_ulw_window(self):
        raise RuntimeError(
            f'BuffMon {self._name} legacy ULW window path is disabled; GPU presentation is required'
        )

    # ─────────────────────────────────────
    #  Window — GPU (优先路径)
    # ─────────────────────────────────────
    def _ensure_gpu_window(self, x: int, y: int, w: int, h: int) -> bool:
        # 惰性创建 GpuOverlayWindow + BgraPresenter。
        if self._gpu_window is not None:
            return True
        require_entity_gpu(f'BuffMon {self._name}', _gow)
        try:
            pump = _gow.get_glfw_pump(self.root)
            self._gpu_presenter = _gow.BgraPresenter()
            self._gpu_window = _gow.GpuOverlayWindow(
                pump,
                w=max(1, int(w)), h=max(1, int(h)),
                x=int(x), y=int(y),
                render_fn=self._gpu_presenter.render,
                click_through=True,
                title=f'sao_buffmon_gpu_{self._name}',
            )
            # 起始隐藏 (没数据时不显示)
            self._gpu_visible = False
            _dbg(f'[{self._name}] GPU window created at xy=({x},{y}) wh=({w},{h})')
            return True
        except Exception as e:
            self._gpu_presenter = None
            self._gpu_window = None
            self._gpu_enabled = False
            raise RuntimeError(
                f'BuffMon {self._name} requires GPU window support; no ULW fallback is available'
            ) from e

    # ─────────────────────────────────────
    #  Public API
    # ─────────────────────────────────────
    def set_enabled(self, enabled: bool):
        self._enabled = bool(enabled)
        if not self._enabled:
            self._vis_target = 0.0
            self._vis_value = 0.0
            self._hide_window()

    def set_anchor(self, anchor):
        self._anchor = anchor

    def hide(self):
        self._vis_target = 0.0
        self._vis_value = 0.0
        self._hide_window()

    def destroy(self):
        self._destroyed = True
        if self._tick_id is not None:
            try:
                self.root.after_cancel(self._tick_id)
            except Exception:
                pass
            self._tick_id = None
        # GPU 路径
        if self._gpu_worker is not None:
            try:
                self._gpu_worker.stop()
            except Exception:
                pass
            self._gpu_worker = None
        if self._gpu_presenter is not None:
            try:
                self._gpu_presenter.release()
            except Exception:
                pass
            self._gpu_presenter = None
        if self._gpu_window is not None:
            try:
                self._gpu_window.destroy()
            except Exception:
                pass
            self._gpu_window = None
        # ULW 路径
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._hwnd = 0

    # 子类负责喂 buff 数据
    def get_rows(self) -> List[dict]:
        return []

    # 子类决定屏幕坐标 (左上角)
    def _resolve_anchor_xy(self, total_h: int) -> Optional[Tuple[int, int]]:
        return None

    # ─────────────────────────────────────
    #  Tick
    # ─────────────────────────────────────
    def _schedule_tick(self):
        if self._destroyed:
            return
        try:
            self._tick_id = self.root.after(self.TICK_MS, self._tick)
        except Exception:
            self._tick_id = None

    def _tick(self):
        self._tick_id = None
        if self._destroyed:
            return
        try:
            self._update_anim()
            self._render_and_present()
        except Exception as e:
            print(f'[BuffMon:{self._name}] tick error: {e}')
        finally:
            self._schedule_tick()

    # ─────────────────────────────────────
    #  Animation
    # ─────────────────────────────────────
    def _ease_out_cubic(self, t: float) -> float:
        t = max(0.0, min(1.0, t))
        return 1.0 - (1.0 - t) ** 3

    def _set_vis_target(self, target: float):
        if abs(target - self._vis_target) < 1e-3:
            return
        self._anim_from = self._vis_value
        self._anim_start_t = time.time()
        self._anim_dur = self.FADE_IN_S if target > self._vis_value else self.FADE_OUT_S
        self._vis_target = target

    def _update_anim(self):
        if abs(self._vis_value - self._vis_target) < 1e-3:
            self._vis_value = self._vis_target
            return
        elapsed = time.time() - self._anim_start_t
        t = elapsed / max(0.01, self._anim_dur)
        eased = self._ease_out_cubic(t)
        self._vis_value = self._anim_from + (self._vis_target - self._anim_from) * eased
        if t >= 1.0:
            self._vis_value = self._vis_target

    # ─────────────────────────────────────
    #  Row formatting helpers
    # ─────────────────────────────────────
    def _fmt_seconds(self, rem_s: float) -> str:
        rem_s = _finite_float(rem_s, -1.0)
        if rem_s < 0:
            return '∞'
        if rem_s >= 60:
            return f'{int(rem_s // 60)}:{int(rem_s % 60):02d}'
        return f'{rem_s:.1f}'

    # ─────────────────────────────────────
    #  Layout
    # ─────────────────────────────────────
    def _compute_total_h(self, rows: List[dict]) -> int:
        if not rows:
            return 0
        h = self.PAD_TOP + self.PAD_BOT
        n = len(rows)
        h += n * self.ROW_H + (n - 1) * self.ROW_GAP
        return h

    # ─────────────────────────────────────
    #  Render
    # ─────────────────────────────────────
    def _shell_polygon(self, sx: int, sy: int, sw: int, sh: int, inset: int = 0):
        # web/dps.html clip-path 风格: 右上 + 左下 各切 14px 对角。
        c = max(2, self.SHELL_CUT - inset)
        x0, y0 = sx + inset, sy + inset
        x1, y1 = sx + sw - 1 - inset, sy + sh - 1 - inset
        return [
            (x0, y0),
            (x1 - c, y0),
            (x1, y0 + c),
            (x1, y1),
            (x0 + c, y1),
            (x0, y1 - c),
        ]

    def _shell_parts(self, sw: int, sh: int) -> dict:
        # 渐变/mask/sheen/scan/底线是 (尺寸, 主题) 的纯函数 — 单槽缓存。
        # base 重建在倒计时期间高达 10Hz (签名含 0.1s 秒数桶), 高斯模糊 +
        # numpy 渐变 + 逐像素底线循环不该每次重算。部件用 tile 本地坐标,
        # 贴回时带 (sx, sy) 偏移, 输出逐像素等价。
        key = (sw, sh, self._theme)
        cached = getattr(self, '_shell_parts_cache', None)
        if cached is not None and cached[0] == key:
            return cached[1]
        # 1) Vertical gradient
        grad = np.zeros((sh, 1, 4), dtype=np.uint8)
        ys = np.linspace(0, 1, sh)
        for i in range(4):
            grad[:, 0, i] = (
                self.PANEL_BG_A[i] + (self.PANEL_BG_B[i] - self.PANEL_BG_A[i]) * ys
            )
        grad_img = Image.fromarray(grad, 'RGBA').resize((sw, sh))

        # 2) Polygon mask — webview cut-corner shape
        local_poly = self._shell_polygon(0, 0, sw, sh)
        mask = Image.new('L', (sw, sh), 0)
        ImageDraw.Draw(mask).polygon(local_poly, fill=255)

        # 3) 顶部青光 sheen (clipped by polygon)
        sheen = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        sd = ImageDraw.Draw(sheen, 'RGBA')
        sd.rectangle(
            (1, 1, sw - 2, max(14, int(sh * 0.32))),
            fill=self.SHELL_SHEEN_CYAN,
        )
        sheen = _gpu_blur(sheen, 3)
        sheen_masked = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        sheen_masked.paste(sheen, (0, 0), mask)

        # 4) 微扫描线 (4px 间隔, web 风格)
        scan = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        sd_scan = ImageDraw.Draw(scan)
        for y in range(0, sh, 4):
            sd_scan.line((0, y, sw, y), fill=self.SCAN_LINE)
        scan_masked = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        scan_masked.paste(scan, (0, 0), mask)

        # 6) 底部 cyan→gold 渐变线 — SAO 招牌
        line_x0 = self.SHELL_CUT + 2
        line_x1 = sw - 3
        line_w = max(0, line_x1 - line_x0)
        grad_line = None
        if line_w > 4:
            arr = np.zeros((1, line_w, 4), dtype=np.uint8)
            for i in range(line_w):
                t = i / max(1, line_w - 1)
                if t < 0.30:
                    a = int(220 * (t / 0.30))
                    arr[0, i] = (104, 228, 255, a)
                elif t < 0.70:
                    u = (t - 0.30) / 0.40
                    r = int(104 + (243 - 104) * u)
                    g = int(228 + (175 - 228) * u)
                    b = int(255 + (18 - 255) * u)
                    a = int(220 + (200 - 220) * u)
                    arr[0, i] = (r, g, b, a)
                else:
                    a = int(200 * (1.0 - (t - 0.70) / 0.30))
                    arr[0, i] = (243, 175, 18, max(0, a))
            grad_line = Image.fromarray(arr, 'RGBA')
        parts = {'grad': grad_img, 'mask': mask, 'sheen': sheen_masked,
                 'scan': scan_masked, 'grad_line': grad_line}
        self._shell_parts_cache = (key, parts)
        return parts

    def _draw_shell(self, img: Image.Image, sx: int, sy: int, sw: int, sh: int):
        parts = self._shell_parts(sw, sh)
        img.paste(parts['grad'], (sx, sy), parts['mask'])
        img.alpha_composite(parts['sheen'], (sx, sy))
        img.alpha_composite(parts['scan'], (sx, sy))

        # 5) 边框 — cyan polygon + 内侧 1px 白色 highlight (廉价线段, 现画)
        draw = ImageDraw.Draw(img, 'RGBA')
        outer_poly = self._shell_polygon(sx, sy, sw, sh)
        draw.line(outer_poly + [outer_poly[0]],
                  fill=self.PANEL_EDGE, width=1)
        inner_poly = self._shell_polygon(sx, sy, sw, sh, inset=1)
        draw.line(inner_poly + [inner_poly[0]],
                  fill=self.INNER_HIGHLIGHT, width=1)

        if parts['grad_line'] is not None:
            img.alpha_composite(parts['grad_line'],
                                (sx + self.SHELL_CUT + 2, sy + sh - 2))

    def _draw_corners(self, draw: ImageDraw.ImageDraw,
                       sx: int, sy: int, sw: int, sh: int):
        # 高亮 cut-corner 转折点 (top-left full + 两对角切口 + bottom-right full)。
        c = self.SHELL_CUT
        cs = self.CORNER_SIZE
        # Top-left full corner — bright cyan
        draw.line((sx + 2, sy + 2, sx + 2 + cs, sy + 2),
                  fill=self.CORNER_CYAN, width=2)
        draw.line((sx + 2, sy + 2, sx + 2, sy + 2 + cs),
                  fill=self.CORNER_CYAN, width=2)
        # Top-right diagonal cut — 沿对角线 2px 青线
        draw.line(
            (sx + sw - 1 - c, sy + 1, sx + sw - 1, sy + 1 + c),
            fill=self.CORNER_CYAN_ACCENT, width=2,
        )
        # Bottom-left diagonal cut — 沿对角线 2px 金线
        draw.line(
            (sx + 1, sy + sh - 1 - c, sx + 1 + c, sy + sh - 1),
            fill=self.CORNER_GOLD_ACCENT, width=2,
        )
        # Bottom-right full corner — gold accent
        bx, by = sx + sw - 2, sy + sh - 2
        draw.line((bx - cs, by, bx, by), fill=self.CORNER_GOLD, width=2)
        draw.line((bx, by - cs, bx, by), fill=self.CORNER_GOLD, width=2)

    def _draw_header(self, draw: ImageDraw.ImageDraw, img: Image.Image,
                      sx: int, sy: int, sw: int):
        x_left = sx + self.PAD_X
        x_right = sx + sw - self.PAD_X
        y = sy + 7

        # Kicker (CJK 副标题, 走 _pick_font 自动 CJK 字体)
        if self.HEADER_KICKER:
            font_kick = _pick_font(self.HEADER_KICKER, 10)
            draw.text((x_left, y), self.HEADER_KICKER,
                      fill=self.TEXT_MUTED, font=font_kick)
            y += 11

        # 主 label — CJK-aware
        if self.HEADER_LABEL:
            font_label = _pick_font(self.HEADER_LABEL, 13)
            draw.text((x_left, y - 1), self.HEADER_LABEL,
                      fill=self.TEXT_MAIN, font=font_label)

        # 右侧 badge (ASCII)
        if self.BADGE_LABEL:
            font_badge = _load_font('sao', 9)
            bw = _text_width(draw, self.BADGE_LABEL, font_badge) + 12
            bh = 14
            bx0 = x_right - bw
            by0 = sy + 9
            draw.rounded_rectangle(
                (bx0, by0, bx0 + bw, by0 + bh - 1),
                radius=2, fill=self.BADGE_FILL, outline=self.BADGE_COLOR,
            )
            draw.text((bx0 + 6, by0 + 1), self.BADGE_LABEL,
                      fill=self.BADGE_COLOR, font=font_badge)

        # 分隔线 — 双层 (浅白 + 青调)
        line_y = sy + self.PAD_TOP - 6
        draw.line(
            (sx + self.PAD_X - 2, line_y, sx + sw - self.PAD_X + 2, line_y),
            fill=self.HEADER_SEP, width=1,
        )
        draw.line(
            (sx + self.PAD_X - 2, line_y + 1, sx + sw - self.PAD_X + 2, line_y + 1),
            fill=self.HEADER_TICK, width=1,
        )

    def _row_polygon(self, x0: int, y0: int, x1: int, y1: int):
        # web/dps.html .entity-row 风格切角: 左上 + 右下各切。
        b = self.PILL_BEVEL
        return [
            (x0 + b, y0),
            (x1, y0),
            (x1, y1 - b),
            (x1 - b, y1),
            (x0, y1),
            (x0, y0 + b),
        ]

    def _draw_row(self, draw: ImageDraw.ImageDraw, img: Image.Image,
                   row: dict, sx: int, ry: int, sw: int):
        rem_s = _finite_float(row.get('rem_s'), -1.0)
        name = str(row.get('name', '') or '')
        layer = _finite_int(row.get('layer'), 0, lo=0)
        count = _finite_int(row.get('count'), 0, lo=0)

        rx0 = sx + self.PAD_X
        rx1 = sx + sw - self.PAD_X
        ry1 = ry + self.ROW_H - 1

        # 背景胶囊 — 紧迫时插值红
        if 0 <= rem_s <= self.CRITICAL_LT:
            pulse = 0.5 + 0.5 * math.sin(time.time() * 9.0)
            bg = self._lerp_color(self.ROW_BG_URGENT, self.ROW_BG_CRITICAL, pulse)
            border = self.ROW_BG_CRITICAL
            text_color = self.ROW_TEXT_URGENT
            secs_color = self.ROW_TEXT_URGENT
        elif 0 <= rem_s <= self.URGENT_LT:
            t = 1.0 - (rem_s - self.CRITICAL_LT) / (self.URGENT_LT - self.CRITICAL_LT)
            bg = self._lerp_color(self.ROW_BG, self.ROW_BG_URGENT, t)
            border = self._lerp_color(self.ROW_BORDER, self.ROW_BG_URGENT, t)
            text_color = self._lerp_color(self.TEXT_MAIN, self.ROW_TEXT_URGENT, t)
            secs_color = self.ROW_TEXT_URGENT
        else:
            bg = self.ROW_BG
            border = self.ROW_BORDER
            text_color = self.TEXT_MAIN
            secs_color = self.GOLD if rem_s >= 0 else self.CYAN_DEEP

        # Cut-corner pill (web/dps.html clip-path 风格)
        poly = self._row_polygon(rx0, ry, rx1, ry1)
        # 填充背景 — overlay 只开行 bbox 大小, 平移多边形后贴回行位
        overlay = Image.new('RGBA', (rx1 - rx0 + 1, ry1 - ry + 1), (0, 0, 0, 0))
        local_poly = [(px - rx0, py - ry) for px, py in poly]
        ImageDraw.Draw(overlay, 'RGBA').polygon(local_poly, fill=bg)
        img.alpha_composite(overlay, (rx0, ry))
        # 描边
        draw.line(poly + [poly[0]], fill=border, width=1)
        # 顶部细青色 highlight (web .entity-row::after top sheen)
        draw.line(
            (rx0 + self.PILL_BEVEL + 1, ry + 1, rx1 - 1, ry + 1),
            fill=self.ROW_TOP_SHEEN, width=1,
        )

        # 名字 (左) — ×层数 · 触发次数
        applies = _finite_int(row.get('apply_count'), 0, lo=0)
        if layer > 1:
            name = f'{name} ×{layer}'
        elif count > 1:
            name = f'{name} ×{count}'
        if applies > 1:
            name = f'{name}·{applies}'

        font_name = _pick_font(name, 12)
        # 给秒数 60 px
        max_name_w = (rx1 - rx0) - 16 - 56
        while name and _text_width(draw, name, font_name) > max_name_w:
            name = name[:-1]
            if len(name) <= 2:
                break
        if name and _text_width(draw, name, font_name) > max_name_w:
            name = name[:-1] + '…'

        tx = rx0 + 8
        ty = ry + (self.ROW_H - 14) // 2
        draw.text((tx, ty), name, fill=text_color, font=font_name)

        # 秒数 (右)
        secs = self._fmt_seconds(rem_s)
        font_secs = _load_font('sao', 13)
        sw_text = _text_width(draw, secs, font_secs)
        sx_text = rx1 - 8 - sw_text
        sy_text = ry + (self.ROW_H - 15) // 2
        draw.text((sx_text, sy_text), secs, fill=secs_color, font=font_secs)
        # 's' 后缀 — 只有有限秒数才有
        if rem_s >= 0:
            font_unit = _load_font('sao', 9)
            unit_x = rx1 - 8 + 1
            # 重新计算: secs 文字右对齐到 rx1-8, 单位放在 secs 之后会越界, 改为 secs 内部
            # 干脆把 's' 包含进 secs 文字
            pass

        # ACT 本场 uptime 覆盖率: 行底边细条 (self buff 才有; -1 = 无数据)
        up = _finite_float(row.get('uptime_pct'), -1.0, lo=-1.0, hi=1.0)
        if up >= 0.0:
            pct = max(0.0, min(1.0, up))
            bar_y = ry1 - 1
            bar_x0 = rx0 + 8
            bar_x1 = rx1 - 8
            full_w = max(1, bar_x1 - bar_x0)
            draw.line((bar_x0, bar_y, bar_x1, bar_y), fill=self.UPTIME_TRACK, width=2)
            fill_w = int(full_w * pct)
            if fill_w > 0:
                draw.line((bar_x0, bar_y, bar_x0 + fill_w, bar_y),
                          fill=self.GOLD if pct >= 0.85 else self.CYAN_DEEP, width=2)

    def _lerp_color(self, ca, cb, t):
        t = max(0.0, min(1.0, t))
        return tuple(int(ca[i] + (cb[i] - ca[i]) * t) for i in range(4))

    def _render_base(self, rows: List[dict], total_h: int) -> Image.Image:
        sw = self.WIDTH
        sh = total_h
        pad = self.SHADOW_PAD
        cw, ch = sw + pad * 2, sh + pad * 2
        img = Image.new('RGBA', (cw, ch), (0, 0, 0, 0))

        # Drop shadow — 沿用 shell 切角 polygon, 然后把切角三角形 + shell 内部抠掉
        # 防止 blur 后阴影从切角漏出
        shadow = Image.new('RGBA', (cw, ch), (0, 0, 0, 0))
        sh_poly = [(p[0] + 1, p[1] + 2) for p in
                   self._shell_polygon(pad, pad, sw, sh)]
        ImageDraw.Draw(shadow, 'RGBA').polygon(sh_poly, fill=(0, 0, 0, 90))
        shadow = _gpu_blur(shadow, 5)

        c = self.SHELL_CUT
        cut_mask = Image.new('L', (cw, ch), 255)
        cd = ImageDraw.Draw(cut_mask)
        # 右上 + 左下 切角三角形
        cd.polygon([
            (pad + sw - c, pad), (pad + sw, pad), (pad + sw, pad + c),
        ], fill=0)
        cd.polygon([
            (pad, pad + sh - c), (pad + c, pad + sh), (pad, pad + sh),
        ], fill=0)
        # shell 主体 (会被覆盖, 提前抠掉避免半透叠加)
        cd.polygon(self._shell_polygon(pad, pad, sw, sh), fill=0)

        from PIL import ImageChops
        a = shadow.getchannel('A')
        shadow.putalpha(ImageChops.multiply(a, cut_mask))
        img = Image.alpha_composite(img, shadow)

        # Shell
        self._draw_shell(img, pad, pad, sw, sh)
        draw = ImageDraw.Draw(img, 'RGBA')
        self._draw_corners(draw, pad, pad, sw, sh)
        self._draw_header(draw, img, pad, pad, sw)

        # Rows
        ry = pad + self.PAD_TOP
        for i, row in enumerate(rows):
            self._draw_row(draw, img, row, pad, ry, sw)
            ry += self.ROW_H + (self.ROW_GAP if i < len(rows) - 1 else 0)

        return img

    # ─────────────────────────────────────
    #  Present
    # ─────────────────────────────────────
    def _header_sig(self) -> Tuple:
        try:
            header_label = self.HEADER_LABEL
        except Exception:
            header_label = ''
        return (
            str(self.HEADER_KICKER or ''),
            str(header_label or ''),
            str(self.BADGE_LABEL or ''),
            tuple(self.BADGE_COLOR or ()),
        )

    def _row_sig(self, rows: List[dict]) -> Tuple:
        # 变化 signature — 只在可见内容/秒数(0.1s 桶)变化时重画 base_img。
        return (
            self._header_sig(),
            tuple(
                (r.get('id'), r.get('uuid'),
                 int(_finite_float(r.get('rem_s'), -1.0) * 10),
                 _finite_int(r.get('layer'), 0, lo=0), _finite_int(r.get('count'), 0, lo=0),
                 _finite_int(r.get('apply_count'), 0, lo=0),
                 round(_finite_float(r.get('uptime_pct'), -1.0, lo=-1.0, hi=1.0), 3),
                 r.get('name'))
                for r in rows
            ),
        )

    def _render_and_present(self):
        if not self._enabled or self._destroyed:
            self._hide_window()
            return
        require_entity_gpu(f'BuffMon {self._name}', _gow)

        rows = self.get_rows() if not self._destroyed else []
        has_rows = bool(rows)

        if has_rows:
            self._set_vis_target(1.0)
        else:
            self._set_vis_target(0.0)

        if self._vis_value <= 0.001 and self._vis_target <= 0.001:
            self._hide_window()
            return

        # 计算 base_img + total_h
        if not rows and self._cached_base is not None:
            base_img = self._cached_base
            total_h = self._cached_total_h
        elif not rows:
            self._hide_window()
            return
        else:
            sig = self._row_sig(rows)
            if sig != self._cached_sig or self._cached_base is None:
                total_h = self._compute_total_h(rows)
                if total_h <= 0:
                    self._hide_window()
                    return
                base_img = self._render_base(rows, total_h)
                self._cached_base = base_img
                self._cached_total_h = total_h
                self._cached_sig = sig
                self._cached_seq += 1
            else:
                base_img = self._cached_base
                total_h = self._cached_total_h

        anchor = self._resolve_anchor_xy(total_h)
        if anchor is None:
            if not getattr(self, '_dbg_anchor_warned', False):
                self._dbg_anchor_warned = True
                _dbg(f'[{self._name}] anchor unresolved (no _hp_overlay/_boss_overlay or '
                     f'missing _x/_y/WIDTH); panel will stay hidden.')
            self._hide_window()
            return

        if not getattr(self, '_dbg_first_anchor', False):
            self._dbg_first_anchor = True
            _dbg(f'[{self._name}] FIRST anchor resolved: xy={anchor} total_h={total_h} '
                 f'mode=GPU')

        ax, ay = anchor

        slide = (1.0 - self._vis_value) * self.SLIDE_OFFSET
        x = int(ax + slide)
        y = int(ay)
        alpha = int(max(0, min(255, self._vis_value * 255)))

        self._present_gpu(base_img, x, y, alpha)

    def _present_ulw(self, base_img: Image.Image, x: int, y: int, alpha: int):
        if not self._visible_hwnd:
            try:
                _user32.ShowWindow(ctypes.c_void_p(self._hwnd), 8)
            except Exception:
                pass
            self._visible_hwnd = True
            _dbg(f'[{self._name}] (ULW) shown xy=({x},{y}) alpha={alpha}')
        try:
            _ulw_update(self._hwnd, base_img, x, y, alpha=alpha)
        except Exception as e:
            print(f'[BuffMon:{self._name}] ULW update error: {e}')

    def _present_gpu(self, base_img: Image.Image, x: int, y: int, alpha: int):
        # GPU 路径: 后台 worker 合成 (alpha pre-multiply 进 PIL), GpuOverlayWindow 显示。
        #
        # compose_fn 在 worker 线程跑, 所以要把所有变量按值捕获。
        w, h = base_img.size
        if w <= 0 or h <= 0:
            self._hide_window()
            return

        # 1) 取上一次 worker 完成的结果 → 上传 + 显示
        if self._gpu_worker is not None and self._gpu_presenter is not None:
            fb = self._gpu_worker.take_result(allow_during_capture=True)
            if fb is not None:
                if not self._ensure_gpu_window(fb.x, fb.y, fb.width, fb.height):
                    return
                geom = (fb.x, fb.y, fb.width, fb.height)
                if self._gpu_window is not None:
                    if geom != self._gpu_last_geom:
                        try:
                            self._gpu_window.set_geometry(*geom)
                            self._gpu_last_geom = geom
                        except Exception:
                            pass
                    try:
                        self._gpu_presenter.set_frame(fb.bgra_bytes, fb.width, fb.height)
                        if not self._gpu_visible:
                            self._gpu_window.show()
                            self._gpu_visible = True
                            _dbg(f'[{self._name}] (GPU) shown xy=({fb.x},{fb.y}) wh=({fb.width},{fb.height})')
                        self._gpu_window.request_redraw()
                    except Exception as e:
                        _dbg(f'[{self._name}] GPU present error: {e}')

        # 2) 提交下一帧 (包含当前 alpha 状态)
        if self._gpu_worker is None:
            return

        # 仅在内容/alpha 显著变化时重新合成 (alpha 量化到 16 级)
        sig = (self._cached_seq, x, y, alpha >> 4)
        if sig == self._gpu_last_compose_sig:
            return
        self._gpu_last_compose_sig = sig

        # capture by value; alpha pre-multiply 在 compose 内部完成
        _img = base_img
        _alpha_f = alpha / 255.0

        def compose(_now: float) -> Image.Image:
            if _alpha_f >= 0.999:
                return _img
            arr = np.asarray(_img, dtype=np.uint8).copy()
            # 仅缩放 alpha 通道 (背景已是 RGBA, RGB 不变)
            arr[:, :, 3] = (arr[:, :, 3].astype(np.uint16) *
                            int(round(_alpha_f * 255)) // 255).astype(np.uint8)
            return Image.fromarray(arr, 'RGBA')

        try:
            self._gpu_worker.submit(compose, time.time(), 0, x, y)
        except Exception as e:
            _dbg(f'[{self._name}] GPU submit error: {e}')

    def _hide_window(self):
        if self._gpu_visible and self._gpu_window is not None:
            try:
                self._gpu_window.hide()
            except Exception:
                pass
            self._gpu_visible = False


# ─────────────────────────────────────────
#  自身 buff overlay
# ─────────────────────────────────────────
class SelfBuffOverlay(_BuffPanelBase):
    # 锚定在 HpOverlay (ID 面板) 上方, 仅显示奥义/幻想 buff。

    WIDTH = 220
    HEADER_LABEL = 'AURA'
    HEADER_KICKER = '幻想之力'
    BADGE_LABEL = 'ULT'
    BADGE_COLOR = (222, 190, 80, 255)

    # 距 ID 面板上沿的缓冲 (留给原生 buff 栏)
    Y_GAP = 10

    def __init__(self, root, settings=None, hp_overlay=None):
        self._hp_overlay = hp_overlay
        # buff_cache 抗 "服务器偶发只发 1 个 buff" 的不完整同步:
        # 我们按 (id, uuid) 缓存; 每次推送 upsert 进来, 仅按 duration 自然过期。
        # key=(buff_id, buff_uuid) → buff dict
        self._buff_cache: Dict[Tuple[int, int], dict] = {}
        self._server_offset_ms: float = 0.0
        self._filter_mode = _default_filter_mode()
        super().__init__(root, settings, anchor=hp_overlay, name='self')

    def set_hp_overlay(self, hp_overlay):
        self._hp_overlay = hp_overlay
        self.set_anchor(hp_overlay)

    def update_buffs(self, raw_buffs: list, server_offset_ms: float = 0.0,
                     uptime=None):
        # Upsert 进 _buff_cache; 旧的 buff 由 duration 自然过期, 不被新推送清空。
        #
        # 这样即使服务器偶发只发部分 buff (例如 AoiSyncDelta 只带 1 个 '箭雨'),
        # 奥义 buff 也不会被误清, 不再闪烁。
        #
        # ``uptime`` = dps_tracker.get_buff_uptime() 快照 (ACT 本场覆盖率),
        # 合并进行渲染: 每个 buff 的 uptime%/触发次数。
        if not isinstance(raw_buffs, list):
            raw_buffs = []
        try:
            self._server_offset_ms = _finite_float(server_offset_ms, 0.0)
        except Exception:
            self._server_offset_ms = 0.0

        # ACT 本场 buff 覆盖率 map: {buff_id: {uptime_pct, apply_count, max_layer}}
        if uptime is not None:
            try:
                uptime_map = {}
                for u in (uptime.get('buffs') or []):
                    if not isinstance(u, dict):
                        continue
                    ubid = _finite_int(u.get('buff_id'), 0, lo=0)
                    if ubid > 0:
                        uptime_map[ubid] = u
                self._uptime_map = uptime_map
            except Exception:
                self._uptime_map = {}

        if raw_buffs and not getattr(self, '_dbg_first_data', False):
            self._dbg_first_data = True
            sample = [b.get('name', f"#{b.get('id', '?')}") for b in raw_buffs[:5] if isinstance(b, dict)]
            _dbg(f'[self] FIRST data: {len(raw_buffs)} buffs, sample={sample}')

        # Upsert (不删除已缓存且未过期的 buff)
        for b in raw_buffs:
            if not isinstance(b, dict):
                continue
            bid = _finite_int(b.get('id') or b.get('buff_id'), 0, lo=0)
            if bid <= 0:
                continue
            uuid = _finite_int(b.get('uuid') or b.get('buff_uuid'), 0, lo=0)
            self._buff_cache[(bid, uuid)] = dict(b)

        # 立即清理已过期的 (按 begin+duration vs server_now)
        now_ms = time.time() * 1000.0 + self._server_offset_ms
        expired_keys = []
        for k, v in self._buff_cache.items():
            begin = _finite_int(v.get('begin_ms') or v.get('begin_time'), 0, lo=0)
            duration = _finite_int(v.get('duration_ms') or v.get('duration'), 0, lo=0)
            if duration > 0 and begin > 0:
                if (begin + duration) <= now_ms:
                    expired_keys.append(k)
        for k in expired_keys:
            del self._buff_cache[k]

    def set_filter_mode(self, mode: str):
        # 'ultimate' (默认, 仅奥义/幻想) 或 'all' (全部 self buff)。
        self._filter_mode = str(mode or 'ultimate').lower()

    def get_rows(self) -> List[dict]:
        mode = getattr(self, '_filter_mode', 'ultimate')
        now_ms = time.time() * 1000.0 + self._server_offset_ms
        rows: List[dict] = []
        rejected_examples: List[str] = []
        # 顺便在 get_rows 里再清一遍过期 (anim tick 比 update_buffs 频繁)
        expired_keys = []
        for key, b in self._buff_cache.items():
            name = str(b.get('name', '') or '')
            bid, uuid = key
            begin = _finite_int(b.get('begin_ms') or b.get('begin_time'), 0, lo=0)
            duration = _finite_int(b.get('duration_ms') or b.get('duration'), 0, lo=0)
            if duration > 0 and begin > 0:
                rem_ms = (begin + duration) - now_ms
                if rem_ms <= 0:
                    expired_keys.append(key)
                    continue
                rem_s = rem_ms / 1000.0
            else:
                rem_s = -1.0
            # 过滤: 「幻想技能」分类 = 名字关键词 ∪ 游戏真实大招技能ID集
            if mode != 'all' and not is_ultimate_buff(name, bid):
                if len(rejected_examples) < 3:
                    rejected_examples.append(name or f"#{bid}")
                continue
            _up = getattr(self, '_uptime_map', None)
            _ur = _up.get(bid) if _up else None
            rows.append({
                'id': bid,
                'uuid': uuid,
                'name': name,
                'rem_s': rem_s,
                'layer': _finite_int(b.get('layer'), 0, lo=0),
                'count': _finite_int(b.get('count'), 0, lo=0),
                'uptime_pct': _finite_float(_ur.get('uptime_pct'), -1.0, lo=-1.0, hi=1.0) if _ur else -1.0,
                'apply_count': _finite_int(_ur.get('apply_count'), 0, lo=0) if _ur else 0,
                'sort_key': rem_s if rem_s >= 0 else 1e9,
            })
        # 过期清理
        for k in expired_keys:
            self._buff_cache.pop(k, None)

        # Debug: 第一次过滤出非空 rows 时打印
        if rows and not getattr(self, '_dbg_first_rows', False):
            self._dbg_first_rows = True
            _dbg(f'[self] FIRST rows after filter ({mode}): {[r["name"] for r in rows]}')
        elif (self._buff_cache and not rows
              and not getattr(self, '_dbg_all_rejected', False)
              and len(self._buff_cache) >= 1):
            self._dbg_all_rejected = True
            _dbg(f'[self] cache has {len(self._buff_cache)} buffs, all rejected by filter ({mode}). '
                 f'Examples: {rejected_examples}. '
                 f'config.BUFFMON_SELF_FILTER="all" 可显示全部。')

        rows.sort(key=lambda r: r['sort_key'])
        return rows[:8]

    def _resolve_anchor_xy(self, total_h: int) -> Optional[Tuple[int, int]]:
        hp = self._hp_overlay
        if hp is None:
            return None
        try:
            hp_x = int(getattr(hp, '_x', 0) or 0)
            hp_y = int(getattr(hp, '_y', 0) or 0)
            hp_w = int(getattr(hp, 'WIDTH', 0) or 0)
        except Exception:
            return None
        if hp_w <= 0:
            return None
        # 右对齐到 ID 面板的右边再左偏 ~80px (避开原生游戏 buff 栏)
        # 然后向上抬 panel_height + Y_GAP
        right_edge = hp_x + hp_w
        x = right_edge - self.WIDTH - 80 - self.SHADOW_PAD
        if x < 0:
            x = 0
        y = hp_y - total_h - self.Y_GAP - self.SHADOW_PAD
        if y < 0:
            y = 0
        return (x, y)


# ─────────────────────────────────────────
#  Boss buff overlay
# ─────────────────────────────────────────
class BossBuffOverlay(_BuffPanelBase):
    # 锚定在 BossHpOverlay 右侧, 显示当前锁定 boss 的所有 buff。

    WIDTH = 240
    HEADER_LABEL = 'TARGET'
    HEADER_KICKER = '目标 BUFF'
    BADGE_LABEL = 'BOSS'
    BADGE_COLOR = (239, 104, 78, 255)

    X_GAP = 8

    def __init__(self, root, settings=None, boss_hp_overlay=None):
        self._boss_overlay = boss_hp_overlay
        self._target_uuid = 0
        self._target_name = ''
        # 同 SelfBuffOverlay: 抗服务器不完整同步, 按 (id,uuid) 缓存 + duration 自然过期
        self._buff_cache: Dict[Tuple[int, int], dict] = {}
        self._server_offset_ms: float = 0.0
        super().__init__(root, settings, anchor=boss_hp_overlay, name='boss')

    def set_boss_overlay(self, boss_hp_overlay):
        self._boss_overlay = boss_hp_overlay
        self.set_anchor(boss_hp_overlay)

    def update_target(self, uuid: int, name: str, raw_buffs: list,
                       server_offset_ms: float = 0.0):
        try:
            uuid = _finite_int(uuid, 0, lo=0)
        except Exception:
            uuid = 0
        # 切目标 → 缓存清零
        if uuid != self._target_uuid:
            self._target_uuid = uuid
            self._buff_cache.clear()
        if name:  # 只在非空时覆盖, 避免被空值清掉
            self._target_name = str(name)
        try:
            self._server_offset_ms = _finite_float(server_offset_ms, 0.0)
        except Exception:
            self._server_offset_ms = 0.0

        if raw_buffs and not getattr(self, '_dbg_first_data', False):
            self._dbg_first_data = True
            sample = [b.get('name', f"#{b.get('id','?')}") for b in raw_buffs[:5] if isinstance(b, dict)]
            _dbg(f'[boss] FIRST data: target={uuid} name={name!r} buffs={sample}')

        # Upsert
        if isinstance(raw_buffs, list):
            for b in raw_buffs:
                if not isinstance(b, dict):
                    continue
                bid = _finite_int(b.get('id') or b.get('buff_id'), 0, lo=0)
                if bid <= 0:
                    continue
                buuid = _finite_int(b.get('uuid') or b.get('buff_uuid'), 0, lo=0)
                self._buff_cache[(bid, buuid)] = dict(b)

        # 过期清理
        now_ms = time.time() * 1000.0 + self._server_offset_ms
        expired = []
        for k, v in self._buff_cache.items():
            begin = _finite_int(v.get('begin_ms') or v.get('begin_time'), 0, lo=0)
            duration = _finite_int(v.get('duration_ms') or v.get('duration'), 0, lo=0)
            if duration > 0 and begin > 0 and (begin + duration) <= now_ms:
                expired.append(k)
        for k in expired:
            del self._buff_cache[k]

    def clear_target(self):
        self._target_uuid = 0
        self._target_name = ''
        self._buff_cache.clear()

    @property
    def HEADER_LABEL(self):
        return (self._target_name or 'TARGET').upper()[:18]

    def get_rows(self) -> List[dict]:
        if not self._target_uuid:
            return []
        now_ms = time.time() * 1000.0 + self._server_offset_ms
        rows: List[dict] = []
        expired = []
        for key, b in self._buff_cache.items():
            bid, buuid = key
            name = str(b.get('name', '') or '')
            if not name:
                name = f'Buff #{bid}'
            begin = _finite_int(b.get('begin_ms') or b.get('begin_time'), 0, lo=0)
            duration = _finite_int(b.get('duration_ms') or b.get('duration'), 0, lo=0)
            if duration > 0 and begin > 0:
                rem_ms = (begin + duration) - now_ms
                if rem_ms <= 0:
                    expired.append(key)
                    continue
                rem_s = rem_ms / 1000.0
            else:
                rem_s = -1.0
            rows.append({
                'id': bid,
                'uuid': buuid,
                'name': name,
                'rem_s': rem_s,
                'layer': _finite_int(b.get('layer'), 0, lo=0),
                'count': _finite_int(b.get('count'), 0, lo=0),
                'sort_key': rem_s if rem_s >= 0 else 1e9,
            })
        for k in expired:
            self._buff_cache.pop(k, None)
        rows.sort(key=lambda r: r['sort_key'])
        return rows[:8]

    def _resolve_anchor_xy(self, total_h: int) -> Optional[Tuple[int, int]]:
        bo = self._boss_overlay
        if bo is None:
            return None
        try:
            bx = int(getattr(bo, '_x', 0) or 0)
            by = int(getattr(bo, '_y', 0) or 0)
            bw = int(getattr(bo, 'WIDTH', 0) or 0)
        except Exception:
            return None
        if bw <= 0:
            return None
        # boss 面板右沿 + 间隔
        x = bx + bw + self.X_GAP - self.SHADOW_PAD
        # 顶部对齐到 boss 面板内容区域 (+12 y偏移让它对齐 BAR_Y)
        y = by + 6 - self.SHADOW_PAD
        if y < 0:
            y = 0
        return (x, y)
