# -*- coding: utf-8 -*-
"""
SAOMenuLeftStack — Star Resonance left-column stack containing
SAOPlayerPanel and SAOSessionPlayersPanel, with show/hide animation for
the session list.

Moved out of gui_modules so game-specific Entity/Tk menu UI stays inside
the Star Resonance plugin.
"""

from __future__ import annotations

import tkinter as tk

from .sao_player_panel import SAOPlayerPanel
from .sao_session_players_panel import SAOSessionPlayersPanel


class SAOMenuLeftStack(tk.Frame):
    """左侧区域: 玩家信息面板 + 本次登录玩家列表同列排列。"""

    def __init__(self, parent, username='Player', profession='',
                 rows_provider=None, **kw):
        super().__init__(parent, bg='#010101', highlightthickness=0, **kw)
        self._rows_provider = rows_provider
        self.player_panel = SAOPlayerPanel(
            self, username=username, profession=profession,
            panel_width=SAOSessionPlayersPanel.PANEL_W,
        )
        try:
            target_w = max(
                int(getattr(self.player_panel, '_target_w', 0) or 0),
                SAOSessionPlayersPanel.PANEL_W,
            )
            self.player_panel._target_w = target_w
            self.player_panel.configure(width=target_w)
            if getattr(self.player_panel, '_gpu_managed', False):
                self.player_panel._top.configure(width=target_w)
                self.player_panel._bottom.configure(width=target_w)
        except Exception:
            pass
        self.session_panel = SAOSessionPlayersPanel(
            self, rows_provider=self._rows_provider)
        self._session_visible = False
        self._active = False
        self._top = getattr(self.player_panel, '_top', None)
        player_w = int(getattr(self.player_panel, '_target_w', 0) or 0)
        player_h = int(getattr(self.player_panel, '_top_h', 0) or 0) + \
            int(getattr(self.player_panel, '_bottom_h', 0) or 0)
        self._stack_gap = 14
        self._target_w = max(player_w, SAOSessionPlayersPanel.PANEL_W)
        # Reserve transparent shell space so this on-demand panel can open
        # without resizing/repositioning the GPU popup.
        self._top_h = player_h + self._stack_gap + SAOSessionPlayersPanel.PANEL_H
        self._bottom_h = 0
        self.player_panel.pack(side=tk.TOP, anchor='nw')
        try:
            self.session_panel.update_rows(force=True)
        except Exception:
            pass

    def _ensure_session_panel(self):
        if self.session_panel is None:
            self.session_panel = SAOSessionPlayersPanel(
                self, rows_provider=self._rows_provider)
        return self.session_panel

    def is_session_players_visible(self) -> bool:
        return bool(self._session_visible and self.session_panel is not None)

    def show_session_players(self, rows=None, force: bool = True):
        panel = self._ensure_session_panel()
        was_visible = self.is_session_players_visible()
        if not was_visible:
            try:
                if hasattr(panel, 'prepare_open_animation'):
                    panel.prepare_open_animation()
                panel.pack(side=tk.TOP, anchor='nw', pady=(self._stack_gap, 0))
            except Exception:
                pass
            self._session_visible = True
        try:
            panel.update_rows(rows, force=force)
            if not was_visible:
                panel.play_open_animation()
        except Exception:
            pass
        return panel

    def hide_session_players(self):
        self._session_visible = False
        panel = self.session_panel
        if panel is not None:
            painter = getattr(panel, '_gpu_painter', None)
            if painter is not None:
                try:
                    painter.hide()
                except Exception:
                    pass
            try:
                panel.pack_forget()
            except Exception:
                pass

    def toggle_session_players(self, rows=None, force: bool = True):
        if self.is_session_players_visible():
            self.hide_session_players()
            return None
        return self.show_session_players(rows=rows, force=force)

    def set_active(self, active: bool):
        self._active = bool(active)
        self.player_panel.set_active(active)
        try:
            if active:
                self.show_session_players(rows=None, force=False)
                panel = getattr(self, 'session_panel', None)
                if panel is not None and hasattr(panel, 'bind_global_wheel_fallback'):
                    panel.bind_global_wheel_fallback()
            elif self.is_session_players_visible():
                self.hide_session_players()
        except Exception:
            pass

    def sync_pulse(self):
        if self.is_session_players_visible():
            try:
                self.session_panel.sync_pulse()
            except Exception:
                pass
        if hasattr(self.player_panel, 'sync_pulse'):
            try:
                self.player_panel.sync_pulse()
            except Exception:
                pass
