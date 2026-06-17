# -*- coding: utf-8 -*-
"""Star Resonance player-panel PIL renderer."""

from __future__ import annotations

from typing import Dict, Optional, Tuple

from PIL import Image, ImageDraw, ImageFont

from gui_modules.sao_menu_hud import _FONT_CJK, _FONT_SAO, _PIL_DRAW_LOCK
from utils.perf_probe import probe as _probe


class PlayerPanelRenderer:
    """PIL renderer for ``sao_gui.SAOPlayerPanel`` used by the GPU
    painter ``sao_player_panel_gpu.PlayerPanelGpuPainter``.

    Mirrors the Tk Canvas paint code in ``SAOPlayerPanel._redraw_top``
    and ``_redraw_bottom`` so the GPU compose looks identical to the
    legacy CPU path. Thread-safe; never touches Tk.
    """

    _GOLD = (243, 175, 18, 255)
    _GOLD_HI = (245, 198, 68, 255)
    _CYAN = (134, 223, 255, 255)
    _DIM = (200, 200, 200, 255)
    _LABEL = (170, 170, 170, 255)
    _TITLE_FG = (100, 99, 100, 255)
    _TOP_BG = (255, 255, 255, 255)
    _BOTTOM_BG = (229, 227, 227, 255)
    _SEP = (170, 170, 170, 255)
    _XP_TRACK = (232, 232, 232, 255)
    _XP_BORDER = (216, 216, 216, 255)
    _SCAN_GRAY = (232, 232, 232, 255)
    _VAL_GRAY = (119, 119, 119, 255)
    _VAL_GRAY_LIGHT = (153, 153, 153, 255)
    _STATUS_GRAY = (176, 176, 176, 255)
    _SYS_GRAY = (200, 200, 200, 255)

    def __init__(self) -> None:
        self._font_cache: Dict[Tuple[str, int, bool], ImageFont.FreeTypeFont] = {}
        self._top_body_cache: Dict[Tuple, Image.Image] = {}
        self._bottom_body_cache: Dict[Tuple, Image.Image] = {}
        self._top_sig: Optional[Tuple] = None
        self._top_frame_cache: Optional[Image.Image] = None

    def reset(self) -> None:
        self._top_body_cache.clear()
        self._bottom_body_cache.clear()
        self._top_sig = None
        self._top_frame_cache = None

    # ── top plate ────────────────────────────────────────────────
    @_probe.decorate('ui.menu.player_panel_top')
    def render_top_pil(self, username: str, level: int, level_extra: int,
                       season_exp: int,
                       hp: Tuple[int, int], sta: Tuple[int, int],
                       width: int, height: int,
                       scan_phase: float = 0.0) -> Image.Image:
        with _PIL_DRAW_LOCK:
            return self._render_top_locked(
                username, level, level_extra, season_exp,
                hp, sta, width, height, scan_phase,
            )

    def _render_top_locked(self, username, level, level_extra, season_exp,
                           hp, sta, width, height, scan_phase):
        width = max(1, int(width))
        height = max(1, int(height))
        body = self._get_top_body(
            username, int(level), int(level_extra), int(season_exp),
            (int(hp[0]), int(hp[1])), (int(sta[0]), int(sta[1])),
            width, height,
        )
        if height <= 185:
            return body
        scan_q = round(max(0.0, min(1.0, float(scan_phase))) * 32.0) / 32.0
        sig = (
            username, int(level), int(level_extra), int(season_exp),
            (int(hp[0]), int(hp[1])), (int(sta[0]), int(sta[1])),
            width, height, scan_q,
        )
        if sig == self._top_sig and self._top_frame_cache is not None:
            return self._top_frame_cache
        # Animated bottom scan dot overlaid on cached body.
        image = body.copy()
        draw = ImageDraw.Draw(image)
        scan_y = height - 16
        scan_x = 10 + int((width - 20) * scan_q)
        draw.rectangle((scan_x - 12, scan_y - 1, scan_x + 12, scan_y + 1),
                       fill=self._CYAN)
        self._top_sig = sig
        self._top_frame_cache = image
        return image

    def _get_top_body(self, username, level, level_extra, season_exp,
                      hp, sta, width, height):
        key = (username, level, level_extra, season_exp, hp, sta, width, height)
        cached = self._top_body_cache.get(key)
        if cached is not None:
            return cached
        image = Image.new('RGBA', (width, height), (0, 0, 0, 0))
        if width >= 40 and height >= 40:
            self._compose_top(image, username, level, level_extra,
                              season_exp, hp, sta, width, height)
        if len(self._top_body_cache) > 96:
            self._top_body_cache.clear()
        self._top_body_cache[key] = image
        return image

    def _compose_top(self, image, username, level, level_extra,
                     season_exp, hp, sta, w, h):
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, w - 1, h - 1), fill=self._TOP_BG)

        # HUD brackets (4 corners)
        bk = 14
        draw.line((2, 2, 2 + bk, 2), fill=self._CYAN, width=1)
        draw.line((2, 2, 2, 2 + bk), fill=self._CYAN, width=1)
        draw.line((w - 2 - bk, 2, w - 2, 2), fill=self._GOLD, width=1)
        draw.line((w - 2, 2, w - 2, 2 + bk), fill=self._GOLD, width=1)
        draw.line((2, h - 2, 2 + bk, h - 2), fill=self._CYAN, width=1)
        draw.line((2, h - 2 - bk, 2, h - 2), fill=self._CYAN, width=1)
        draw.line((w - 2 - bk, h - 2, w - 2, h - 2), fill=self._GOLD, width=1)
        draw.line((w - 2, h - 2 - bk, w - 2, h - 2), fill=self._GOLD, width=1)

        # SYS:PLAYER (Tk anchor='e' at (w-8, 12))
        font_tiny = self._font(_FONT_SAO, 6)
        bbox = self._text_bbox(draw, 'SYS:PLAYER', font_tiny)
        tw = bbox[2] - bbox[0]; th = bbox[3] - bbox[1]
        draw.text((w - 8 - tw, 12 - th // 2), 'SYS:PLAYER',
                  font=font_tiny, fill=self._DIM)

        # Username (Tk anchor='center' at (w/2, 26))
        title_y = 26
        display_name = username or ''
        if len(display_name) > 18:
            display_name = display_name[:16] + '…'
        font_name = self._font(_FONT_CJK, 13, bold=True)
        bbox = self._text_bbox(draw, display_name, font_name)
        tw = bbox[2] - bbox[0]; th = bbox[3] - bbox[1]
        draw.text(((w - tw) // 2, title_y - th // 2), display_name,
                  font=font_name, fill=self._TITLE_FG)

        # Separator + scan dots
        sep_y = 44
        draw.line((10, sep_y, w - 10, sep_y), fill=self._SEP, width=2)
        for i in range(5):
            dot_x = 14 + i * 8
            draw.rectangle((dot_x, sep_y - 1, dot_x + 3, sep_y),
                           fill=self._CYAN)

        if h < 60:
            return

        # LEVEL label (Tk anchor='w' at (20, 62))
        font_label = self._font(_FONT_SAO, 7)
        bbox = self._text_bbox(draw, 'LEVEL', font_label)
        th = bbox[3] - bbox[1]
        draw.text((20, 62 - th // 2), 'LEVEL', font=font_label, fill=self._LABEL)

        # Level value (Tk anchor='center' at (w/2, 84))
        level_text = f'Lv. {level}'
        level_font_size = 20
        if level_extra > 0:
            level_text = f'Lv. {level}(+{level_extra})'
            level_font_size = 16 if level_extra < 100 else 14
        font_lvl = self._font(_FONT_SAO, level_font_size, bold=True)
        bbox = self._text_bbox(draw, level_text, font_lvl)
        tw = bbox[2] - bbox[0]; th = bbox[3] - bbox[1]
        draw.text(((w - tw) // 2, 84 - th // 2), level_text,
                  font=font_lvl, fill=self._GOLD)

        if h < 120:
            return

        # EXP label (Tk anchor='w' at (20, 108))
        bbox = self._text_bbox(draw, 'EXP', font_label)
        th = bbox[3] - bbox[1]
        draw.text((20, 108 - th // 2), 'EXP', font=font_label, fill=self._LABEL)

        # EXP value (Tk anchor='e' at (w-20, 108))
        exp_text = f'{season_exp:,}' if season_exp > 0 else '—'
        font_exp = self._font(_FONT_SAO, 8)
        bbox = self._text_bbox(draw, exp_text, font_exp)
        tw_e = bbox[2] - bbox[0]; th_e = bbox[3] - bbox[1]
        draw.text((w - 20 - tw_e, 108 - th_e // 2), exp_text,
                  font=font_exp, fill=self._VAL_GRAY_LIGHT)

        # EXP bar
        if h > 124:
            xp_y = 122; xp_x = 20; xp_w = w - 40; xp_h = 6
            draw.rectangle((xp_x, xp_y, xp_x + xp_w, xp_y + xp_h),
                           fill=self._XP_TRACK, outline=self._XP_BORDER)
            if season_exp > 0:
                draw.rectangle((xp_x + 1, xp_y + 1,
                                xp_x + 10, xp_y + xp_h - 1),
                               fill=self._GOLD)
                draw.rectangle((xp_x + 1, xp_y + 1,
                                xp_x + 10, xp_y + 3),
                               fill=self._GOLD_HI)

        # HP row
        if h > 150:
            info_y = 140
            font_lab_b = self._font(_FONT_SAO, 7, bold=True)
            font_val = self._font(_FONT_SAO, 8)
            hp_c, hp_m = hp
            bbox = self._text_bbox(draw, 'HP', font_lab_b)
            th = bbox[3] - bbox[1]
            draw.text((20, info_y - th // 2), 'HP',
                      font=font_lab_b, fill=self._CYAN)
            hp_text = f'{hp_c}/{hp_m}' if hp_m > 0 else '—'
            bbox = self._text_bbox(draw, hp_text, font_val)
            tw_h = bbox[2] - bbox[0]; th_h = bbox[3] - bbox[1]
            draw.text((w - 20 - tw_h, info_y - th_h // 2), hp_text,
                      font=font_val, fill=self._VAL_GRAY)

        # STA row
        if h > 170:
            info_y2 = 158
            font_lab_b = self._font(_FONT_SAO, 7, bold=True)
            font_val = self._font(_FONT_SAO, 8)
            sta_c, sta_m = sta
            bbox = self._text_bbox(draw, 'STA', font_lab_b)
            th = bbox[3] - bbox[1]
            draw.text((20, info_y2 - th // 2), 'STA',
                      font=font_lab_b, fill=self._GOLD)
            sta_text = f'{sta_c}/{sta_m}' if sta_m > 0 else '—'
            bbox = self._text_bbox(draw, sta_text, font_val)
            tw_s = bbox[2] - bbox[0]; th_s = bbox[3] - bbox[1]
            draw.text((w - 20 - tw_s, info_y2 - th_s // 2), sta_text,
                      font=font_val, fill=self._VAL_GRAY)

        # Bottom scan rail (static; the moving cyan dot is overlaid in
        # _render_top_locked because it depends on time)
        if h > 185:
            scan_y = h - 16
            draw.line((10, scan_y, w - 10, scan_y),
                      fill=self._SCAN_GRAY, width=1)

    # ── bottom plate ─────────────────────────────────────────────
    @_probe.decorate('ui.menu.player_panel_bottom')
    def render_bottom_pil(self, shift_mode: str,
                          width: int, height: int) -> Image.Image:
        with _PIL_DRAW_LOCK:
            return self._render_bottom_locked(shift_mode, width, height)

    def _render_bottom_locked(self, shift_mode, width, height):
        width = max(1, int(width)); height = max(1, int(height))
        key = (shift_mode or '', width, height)
        cached = self._bottom_body_cache.get(key)
        if cached is not None:
            return cached
        image = Image.new('RGBA', (width, height), (0, 0, 0, 0))
        if width >= 40 and height >= 15:
            self._compose_bottom(image, shift_mode or '', width, height)
        if len(self._bottom_body_cache) > 32:
            self._bottom_body_cache.clear()
        self._bottom_body_cache[key] = image
        return image

    def _compose_bottom(self, image, shift_mode, w, h):
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, w - 1, h - 1), fill=self._BOTTOM_BG)

        # top gradient lines
        for i in range(3):
            av = int(220 + i * 8)
            draw.line((0, i, w, i), fill=(av, av, av, 255), width=1)

        # corner brackets
        draw.line((3, 3, 12, 3), fill=self._CYAN, width=1)
        draw.line((3, 3, 3, 12), fill=self._CYAN, width=1)
        draw.line((w - 12, h - 3, w - 3, h - 3), fill=self._GOLD, width=1)
        draw.line((w - 3, h - 12, w - 3, h - 3), fill=self._GOLD, width=1)

        # STATUS label (Tk anchor='w' at (12, 12))
        font_tiny = self._font(_FONT_SAO, 6)
        bbox = self._text_bbox(draw, 'STATUS', font_tiny)
        th = bbox[3] - bbox[1]
        draw.text((12, 12 - th // 2), 'STATUS',
                  font=font_tiny, fill=self._STATUS_GRAY)

        # shift_mode (Tk anchor='w' at (15, h//2 - 2))
        sm = shift_mode if shift_mode else '普通模式'
        if sm == '普通模式':
            sm_color = (33, 150, 243, 255)
        elif 'CTRL' in sm:
            sm_color = (230, 81, 0, 255)
        else:
            sm_color = (21, 101, 192, 255)
        font_sm = self._font(_FONT_CJK, 9, bold=True)
        bbox = self._text_bbox(draw, sm, font_sm)
        th = bbox[3] - bbox[1]
        draw.text((15, h // 2 - 2 - th // 2), sm,
                  font=font_sm, fill=sm_color)

        # SAO://SYSTEM (Tk anchor='e' at (w-8, h-10))
        if h > 50:
            font_sys = self._font(_FONT_SAO, 5)
            bbox = self._text_bbox(draw, 'SAO://SYSTEM', font_sys)
            tw = bbox[2] - bbox[0]; th = bbox[3] - bbox[1]
            draw.text((w - 8 - tw, h - 10 - th // 2), 'SAO://SYSTEM',
                      font=font_sys, fill=self._SYS_GRAY)

    # ── helpers ──────────────────────────────────────────────────
    def _font(self, font_path: str, size: int, bold: bool = False):
        # ZhuZiAYuanJWD/SAOUI ship single-weight TTFs; the bold flag is
        # accepted for caller symmetry but resolves to the same file.
        key = (font_path, size, bool(bold))
        cached = self._font_cache.get(key)
        if cached is not None:
            return cached
        try:
            font = ImageFont.truetype(font_path, size=size)
        except Exception:
            font = ImageFont.load_default()
        self._font_cache[key] = font
        return font

    @staticmethod
    def _text_bbox(draw, text, font):
        if hasattr(draw, 'textbbox'):
            return draw.textbbox((0, 0), text, font=font)
        width, height = draw.textsize(text, font=font)
        return (0, 0, width, height)
