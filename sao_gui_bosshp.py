# sao_gui_bosshp.py — ULW Boss HP Bar Overlay for SAO Entity UI

import tkinter as tk
import ctypes
from PIL import Image, ImageDraw, ImageFont

from sao_gui_dps import (
    _ulw_update, _user32,
    GWL_EXSTYLE, WS_EX_LAYERED, WS_EX_TOOLWINDOW, WS_EX_TOPMOST,
)


class BossHpOverlay:
    """ULW-based Boss HP bar overlay with SAO styling.

    Renders a horizontal bar at the top-centre of the screen showing
    boss name, HP bar (with numeric readout), optional shield bar,
    breaking-stage pips, and an extinction bar.
    """

    WIDTH = 450
    HEIGHT = 50

    # ── SAO colour palette (RGBA) ──────────────────────────────────
    BG_COLOR        = (20, 22, 30, 200)
    BORDER_COLOR    = (243, 175, 18, 120)       # gold border
    GOLD            = (243, 175, 18, 255)        # #f3af12
    GOLD_DIM        = (243, 175, 18, 140)
    HP_GREEN        = (76, 217, 100, 220)
    HP_YELLOW       = (243, 200, 40, 220)
    HP_RED          = (230, 60, 60, 220)
    SHIELD_BLUE     = (70, 150, 230, 180)
    EXTINCTION_PURPLE = (170, 70, 210, 200)
    BAR_BG          = (40, 44, 55, 180)
    TEXT_WHITE       = (255, 255, 255, 255)
    TEXT_DIM         = (180, 190, 200, 230)
    OVERDRIVE_GLOW  = (255, 60, 60, 200)
    INVINCIBLE_GREY = (130, 130, 130, 200)
    BREAKING_ACTIVE = (255, 200, 50, 230)
    BREAKING_DONE   = (100, 110, 120, 160)

    def __init__(self, root: tk.Tk, settings=None):
        self.root = root
        self.settings = settings
        self._win: tk.Toplevel | None = None
        self._hwnd: int = 0
        self._visible: bool = False
        self._last_data: dict | None = None

        # Default position: top-centre of primary screen
        sw = _user32.GetSystemMetrics(0)
        self._x = (sw - self.WIDTH) // 2
        self._y = 18

        if settings:
            self._x = int(settings.get('boss_hp_ov_x', self._x))
            self._y = int(settings.get('boss_hp_ov_y', self._y))

    # ── public API ─────────────────────────────────────────────────

    def show(self):
        """Create the layered Toplevel if it doesn't already exist."""
        if self._win is not None:
            return
        self._win = tk.Toplevel(self.root)
        self._win.overrideredirect(True)
        self._win.attributes('-topmost', True)
        self._win.geometry(f'1x1+{self._x}+{self._y}')
        self._win.update_idletasks()

        # Resolve the HWND for UpdateLayeredWindow
        try:
            self._hwnd = ctypes.windll.user32.GetParent(self._win.winfo_id()) or self._win.winfo_id()
        except Exception:
            self._hwnd = self._win.winfo_id()

        # Apply extended styles: layered + tool-window + topmost
        ex = _user32.GetWindowLongW(ctypes.c_void_p(self._hwnd), GWL_EXSTYLE)
        _user32.SetWindowLongW(
            ctypes.c_void_p(self._hwnd), GWL_EXSTYLE,
            ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        )
        self._visible = True

    def hide(self):
        """Destroy the Toplevel and reset state."""
        if self._win:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._hwnd = 0
        self._visible = False

    def update(self, data: dict):
        """Redraw the overlay with fresh boss data.

        *data* dict keys:
            active, hp_pct, hp_source, current_hp, total_hp,
            shield_active, shield_pct, breaking_stage, extinction_pct,
            in_overdrive, invincible, boss_name
        """
        if not data or not data.get('active', False):
            # Boss inactive → hide the bar
            if self._visible:
                self.hide()
            return

        if not self._visible or not self._hwnd:
            self.show()

        self._last_data = data
        self._render(data)

    def destroy(self):
        self.hide()

    # ── internal rendering ─────────────────────────────────────────

    @staticmethod
    def _hp_bar_color(pct: float):
        """Return an RGBA colour that shifts green → yellow → red."""
        if pct > 0.50:
            return BossHpOverlay.HP_GREEN
        elif pct > 0.25:
            return BossHpOverlay.HP_YELLOW
        return BossHpOverlay.HP_RED

    @staticmethod
    def _format_hp(value: float) -> str:
        if value >= 1_000_000_000:
            return f'{value / 1_000_000_000:.2f}B'
        if value >= 1_000_000:
            return f'{value / 1_000_000:.2f}M'
        if value >= 1_000:
            return f'{value / 1_000:.1f}K'
        return f'{int(value)}'

    def _render(self, data: dict):
        if not self._hwnd:
            return

        w, h = self.WIDTH, self.HEIGHT
        img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)

        # ── fonts ──────────────────────────────────────────────────
        try:
            font_name = ImageFont.truetype('consola.ttf', 12)
            font_hp   = ImageFont.truetype('consola.ttf', 11)
            font_sm   = ImageFont.truetype('consola.ttf', 9)
        except Exception:
            font_name = ImageFont.load_default()
            font_hp = font_name
            font_sm = font_name

        # ── background + border ────────────────────────────────────
        draw.rounded_rectangle(
            [(0, 0), (w - 1, h - 1)], radius=6,
            fill=self.BG_COLOR, outline=self.BORDER_COLOR,
        )

        # ── unpack data ────────────────────────────────────────────
        hp_pct        = max(0.0, min(1.0, float(data.get('hp_pct', 0))))
        current_hp    = float(data.get('current_hp', 0))
        total_hp      = float(data.get('total_hp', 0))
        shield_active = bool(data.get('shield_active', False))
        shield_pct    = max(0.0, min(1.0, float(data.get('shield_pct', 0))))
        breaking_stage = int(data.get('breaking_stage', 0))
        extinction_pct = max(0.0, min(1.0, float(data.get('extinction_pct', 0))))
        in_overdrive  = bool(data.get('in_overdrive', False))
        invincible    = bool(data.get('invincible', False))
        boss_name     = str(data.get('boss_name', 'Boss'))

        # ── layout constants ───────────────────────────────────────
        pad      = 8
        bar_x    = pad
        bar_w    = w - pad * 2
        name_y   = 4
        bar_y    = 20
        bar_h    = 14
        ext_y    = bar_y + bar_h + 3     # extinction row
        ext_h    = 5

        # ── boss name (left) ───────────────────────────────────────
        name_display = boss_name if len(boss_name) <= 28 else boss_name[:27] + '…'
        name_color = self.OVERDRIVE_GLOW if in_overdrive else self.GOLD
        draw.text((bar_x, name_y), name_display, fill=name_color, font=font_name)

        # ── HP text (right) ────────────────────────────────────────
        hp_text = f'{self._format_hp(current_hp)}/{self._format_hp(total_hp)}  {hp_pct * 100:.1f}%'
        try:
            tw = draw.textlength(hp_text, font=font_hp)
        except Exception:
            tw = len(hp_text) * 7
        hp_text_color = self.INVINCIBLE_GREY if invincible else self.TEXT_WHITE
        draw.text((bar_x + bar_w - tw, name_y + 1), hp_text, fill=hp_text_color, font=font_hp)

        # ── HP bar background ──────────────────────────────────────
        draw.rounded_rectangle(
            [(bar_x, bar_y), (bar_x + bar_w, bar_y + bar_h)],
            radius=3, fill=self.BAR_BG,
        )

        # ── HP bar fill ────────────────────────────────────────────
        if hp_pct > 0:
            fill_w = max(2, int(bar_w * hp_pct))
            bar_color = self.INVINCIBLE_GREY if invincible else self._hp_bar_color(hp_pct)
            draw.rounded_rectangle(
                [(bar_x, bar_y), (bar_x + fill_w, bar_y + bar_h)],
                radius=3, fill=bar_color,
            )

        # ── shield overlay (semi-transparent blue on top of HP bar) ─
        if shield_active and shield_pct > 0:
            shield_w = max(2, int(bar_w * shield_pct))
            draw.rounded_rectangle(
                [(bar_x, bar_y), (bar_x + shield_w, bar_y + bar_h)],
                radius=3, fill=self.SHIELD_BLUE,
            )
            # "SHIELD" label centred
            try:
                stw = draw.textlength('SHIELD', font=font_sm)
            except Exception:
                stw = 36
            sx = bar_x + (shield_w - stw) // 2
            if sx > bar_x + 2:
                draw.text((sx, bar_y + 2), 'SHIELD', fill=self.TEXT_WHITE, font=font_sm)

        # ── breaking-stage pips (right of HP bar row) ──────────────
        if breaking_stage > 0:
            pip_r = 3
            pip_gap = 10
            total_pips = 3  # assume max 3 stages
            pips_w = total_pips * pip_gap
            pip_start_x = bar_x + bar_w - pips_w - 2
            for i in range(total_pips):
                cx = pip_start_x + i * pip_gap + pip_r
                cy = bar_y + bar_h // 2
                color = self.BREAKING_ACTIVE if i < breaking_stage else self.BREAKING_DONE
                draw.ellipse(
                    [(cx - pip_r, cy - pip_r), (cx + pip_r, cy + pip_r)],
                    fill=color,
                )

        # ── extinction bar (thin row below HP bar) ─────────────────
        if extinction_pct > 0:
            draw.rounded_rectangle(
                [(bar_x, ext_y), (bar_x + bar_w, ext_y + ext_h)],
                radius=2, fill=self.BAR_BG,
            )
            ext_fill_w = max(2, int(bar_w * extinction_pct))
            draw.rounded_rectangle(
                [(bar_x, ext_y), (bar_x + ext_fill_w, ext_y + ext_h)],
                radius=2, fill=self.EXTINCTION_PURPLE,
            )

        # ── overdrive pulsing border ───────────────────────────────
        if in_overdrive:
            draw.rounded_rectangle(
                [(0, 0), (w - 1, h - 1)], radius=6,
                outline=self.OVERDRIVE_GLOW, width=2,
            )

        # ── push to screen ─────────────────────────────────────────
        try:
            _ulw_update(self._hwnd, img, self._x, self._y)
        except Exception as e:
            print(f'[BOSS-HP] render error: {e}')

    # ── position helpers ───────────────────────────────────────────

    def set_position(self, x: int, y: int):
        """Move the overlay and persist to settings."""
        self._x = x
        self._y = y
        if self.settings is not None:
            self.settings['boss_hp_ov_x'] = x
            self.settings['boss_hp_ov_y'] = y
        if self._last_data:
            self._render(self._last_data)

    def get_position(self) -> tuple[int, int]:
        return self._x, self._y
