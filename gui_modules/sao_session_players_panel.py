# -*- coding: utf-8 -*-
"""
SAOSessionPlayersPanel — left-stack panel listing all players seen during
the current login session (in-game encountered roster).

Extracted from sao_gui.py (round 26 of the split refactor). The panel
is GPU-painted when ``sao_left_info_gpu`` reports availability and
silently no-ops otherwise (the original ``ENTITY_GPU_ONLY`` invariant
is preserved).

Public surface (unchanged from the original):
  * ``SAOSessionPlayersPanel(parent, rows_provider=None, **kw)``
  * ``.update_rows(rows=None, force=False)``
  * ``.prepare_open_animation()`` / ``.play_open_animation()`` / ``.sync_pulse()``
  * ``.bind_global_wheel_fallback()`` / ``.unbind_global_wheel_fallback()``
"""

from __future__ import annotations

import time
import tkinter as tk
from typing import Any, Dict

import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]
from utils.sao_sound import get_sao_font, get_cjk_font


# ── Global wheel-routing registry ──────────────────────────────────────
# Moved here from sao_gui.py module level (rounds 25-26 refactor). The
# registry is keyed by ``id(root)`` so multiple SAO root windows in the
# same process — unlikely but supported — don't clash. The class adds /
# removes itself from the per-root list in `bind_global_wheel_fallback`.
_SESSION_WHEEL_ROOTS: Dict[int, Dict[str, Any]] = {}


def _dispatch_session_wheel(root, event):
    entry = _SESSION_WHEEL_ROOTS.get(id(root))
    if not entry:
        return None
    panels = list(entry.get('panels') or ())
    for panel in reversed(panels):
        try:
            if not panel.winfo_exists() or not panel.winfo_ismapped():
                continue
            if not panel._cursor_inside_panel(event):
                continue
            return panel._on_mousewheel(event)
        except Exception:
            continue
    return None


class SAOSessionPlayersPanel(tk.Frame):
    """SAO 菜单内的本次登录玩家列表。"""

    ENTITY_GPU_ONLY = True
    PANEL_W = 304
    PANEL_H = 392
    INITIAL_RENDER_ROWS = 48
    RENDER_BATCH_ROWS = 64
    LOAD_MORE_THRESHOLD = 0.78

    def __init__(self, parent, rows_provider=None, **kw):
        try:
            from gui_modules.sao_left_info_gpu import (
                SessionPlayersGpuPainter as _SPGP,
                _SessionPlayersSnapshot as _SPSnap,
                gpu_session_players_enabled as _spgen,
            )
        except Exception:
            _SPGP = None
            _SPSnap = None
            def _spgen():  # type: ignore[no-redef]
                return False
        self._SPGP_cls = _SPGP
        self._SPSnap_cls = _SPSnap
        self._gpu_required = bool(self.ENTITY_GPU_ONLY)
        self._gpu_managed = bool(
            _SPGP is not None
            and _spgen()
        )
        self._gpu_chroma = '#010101'
        super().__init__(parent, bg='#010101', highlightthickness=0, **kw)
        self._rows_provider = rows_provider
        self._rows_sig = None
        self._canvas_window = None
        self._rows_data = []
        self._rendered_count = 0
        self._loading_footer = None
        self._open_anim_after_id = None
        self._gpu_painter = None
        self._first_visible_row = 0
        self._visible_row_count = 7
        self._cached_screen_xy = None
        self._open_reveal = 1.0
        self._gpu_paint_after_id = None
        self._gpu_drain_after_id = None
        self._gpu_drain_retries = 0
        self._open_anim_duration = 0.90
        self._rows_self_uid = ''
        self._wheel_fallback_bound = False

        if self._gpu_managed:
            self.configure(width=self.PANEL_W, height=self.PANEL_H)
            self.pack_propagate(False)
            self._hit = tk.Frame(self, bg=self._gpu_chroma,
                                 width=self.PANEL_W, height=self.PANEL_H)
            self._hit.pack(fill=tk.BOTH, expand=True)
            self._setup_gpu_painter()
            if self._gpu_managed and self._gpu_painter is not None:
                self._bind_wheel(self)
                self._bind_wheel(self._hit)
                try:
                    self.after(1, self._warmup_gpu_painter)
                except Exception:
                    pass
                return
            try:
                self._hit.destroy()
            except Exception:
                pass
            self._hit = None
        if self._gpu_required:
            self._gpu_managed = False
            self.configure(width=self.PANEL_W, height=self.PANEL_H)
            self.pack_propagate(False)
            self._bind_wheel(self)
            try:
                print('[SAO Entity] Session Players GPU unavailable; Tk fallback suppressed', flush=True)
            except Exception:
                pass
            return

        self.configure(width=self.PANEL_W, height=self.PANEL_H)
        self.pack_propagate(False)
        self._box = tk.Frame(self, bg='#f7f7f6', width=self.PANEL_W, height=self.PANEL_H,
                             highlightthickness=1, highlightbackground='#d4d0d0')
        self._box.pack_propagate(False)
        self._box.pack(anchor='n')

        self._header = tk.Frame(self._box, bg='#ffffff', height=54)
        self._header.pack(fill=tk.X)
        self._header.pack_propagate(False)
        tk.Label(self._header, text='SESSION PLAYERS',
                 bg='#ffffff', fg='#646364',
                 font=get_sao_font(11, True)).pack(anchor='w', padx=14, pady=(8, 0))
        self._summary = tk.Label(self._header, text='本次登录出现过 0 人',
                                 bg='#ffffff', fg='#aaaaaa',
                                 font=get_cjk_font(8))
        self._summary.pack(anchor='w', padx=14, pady=(1, 0))

        self._head = tk.Frame(self._box, bg='#eceff2', height=28)
        self._head.pack(fill=tk.X)
        self._head.pack_propagate(False)
        for text, width, anchor in (
                ('NAME', 16, 'w'), ('UID', 11, 'center'), ('POWER', 10, 'e')):
            tk.Label(self._head, text=text, bg='#eceff2', fg='#7a8792',
                     font=get_cjk_font(7, True), width=width,
                     anchor=anchor).pack(side=tk.LEFT, padx=(10 if text == 'NAME' else 0, 0))

        self._body = tk.Frame(self._box, bg='#eeeeee')
        self._body.pack(fill=tk.BOTH, expand=True)
        self._canvas = tk.Canvas(self._body, bg='#eeeeee', highlightthickness=0,
                                 bd=0, width=286, height=298)
        self._scrollbar = tk.Scrollbar(self._body, orient=tk.VERTICAL,
                                       command=self._on_scrollbar,
                                       width=8, bg='#eeeeee', troughcolor='#eeeeee',
                                       activebackground='#f3af12')
        self._canvas.configure(yscrollcommand=self._scrollbar.set)
        self._canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self._scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self._rows_host = tk.Frame(self._canvas, bg='#eeeeee')
        self._canvas_window = self._canvas.create_window(
            (0, 0), window=self._rows_host, anchor='nw')
        self._rows_host.bind('<Configure>', self._on_rows_configure)
        self._canvas.bind('<Configure>', self._on_canvas_configure)
        self._bind_wheel(self)

    def _setup_gpu_painter(self):
        if self._SPGP_cls is None:
            self._gpu_managed = False
            return
        try:
            self._gpu_painter = self._SPGP_cls(self.winfo_toplevel())
        except Exception:
            self._gpu_painter = None
            self._gpu_managed = False
            return
        self.bind('<Configure>',
                  lambda e: setattr(self, '_cached_screen_xy', None),
                  add='+')
        self.bind('<Destroy>', lambda e: self._on_gpu_destroy(), add='+')

    def _warmup_gpu_painter(self):
        painter = getattr(self, '_gpu_painter', None)
        warmup = getattr(painter, 'warmup', None)
        if not callable(warmup):
            return
        try:
            sx, sy = self.winfo_rootx(), self.winfo_rooty()
        except Exception:
            sx, sy = 0, 0
        try:
            warmup(self.PANEL_W, self.PANEL_H, sx, sy)
        except Exception:
            pass

    def _on_gpu_destroy(self):
        self.unbind_global_wheel_fallback()
        for attr in ('_gpu_paint_after_id', '_gpu_drain_after_id',
                     '_open_anim_after_id'):
            job = getattr(self, attr, None)
            if job:
                try:
                    self.after_cancel(job)
                except Exception:
                    pass
                setattr(self, attr, None)
        painter = getattr(self, '_gpu_painter', None)
        if painter is not None:
            try:
                painter.destroy()
            except Exception:
                pass
            self._gpu_painter = None

    def _gpu_visible_rows(self):
        self._first_visible_row = _CY_UI.clamp_session_first_index(
            self._first_visible_row, len(self._rows_data), self._visible_row_count)
        return _CY_UI.session_visible_rows(
            self._rows_data, self._first_visible_row, self._visible_row_count)

    def _dispatch_gpu_paint(self):
        painter = getattr(self, '_gpu_painter', None)
        snap_cls = getattr(self, '_SPSnap_cls', None)
        if not self._gpu_managed or painter is None or snap_cls is None:
            return
        try:
            if not self.winfo_exists():
                return
        except Exception:
            return
        try:
            if not self.winfo_ismapped():
                return
        except Exception:
            pass
        # The SAO menu's transparent shell can settle after the GPU popup has
        # already opened. Sample fresh root coords so this GPU panel stays
        # attached to the menu instead of reusing an early off-menu position.
        try:
            sx, sy = self.winfo_rootx(), self.winfo_rooty()
            self._cached_screen_xy = (sx, sy)
        except Exception:
            sx, sy = self._cached_screen_xy or (0, 0)
        try:
            self_uid = str(getattr(self, '_rows_self_uid', '') or '')
            if hasattr(painter, 'show'):
                painter.show()
            snap = snap_cls(
                self._gpu_visible_rows(), len(self._rows_data),
                self_uid, self._first_visible_row, self.PANEL_W,
                self.PANEL_H, float(getattr(self, '_open_reveal', 1.0)))
            painter.tick(sx, sy, snap)
        except Exception:
            pass

    def _queue_gpu_drain(self, retries: int = 3, delay_ms: int = 16):
        if not self._gpu_managed:
            return
        self._gpu_drain_retries = max(
            int(getattr(self, '_gpu_drain_retries', 0) or 0),
            max(1, int(retries or 1)),
        )
        if self._gpu_drain_after_id:
            return

        def _run():
            self._gpu_drain_after_id = None
            left = max(0, int(getattr(self, '_gpu_drain_retries', 0) or 0))
            if left <= 0:
                return
            self._gpu_drain_retries = left - 1
            self._dispatch_gpu_paint()
            if self._gpu_drain_retries > 0:
                try:
                    self._gpu_drain_after_id = self.after(max(1, int(delay_ms)), _run)
                except Exception:
                    self._gpu_drain_after_id = None

        try:
            self._gpu_drain_after_id = self.after(max(1, int(delay_ms)), _run)
        except Exception:
            self._gpu_drain_after_id = None

    def _queue_gpu_paint(self, delay_ms: int = 10):
        if not self._gpu_managed:
            return
        if self._gpu_paint_after_id:
            return

        def _run():
            self._gpu_paint_after_id = None
            self._dispatch_gpu_paint()
            self._queue_gpu_drain()

        try:
            self._gpu_paint_after_id = self.after(max(1, int(delay_ms)), _run)
        except Exception:
            self._gpu_paint_after_id = None
            self._dispatch_gpu_paint()
            self._queue_gpu_drain()

    def _on_rows_configure(self, _event=None):
        if not hasattr(self, '_canvas'):
            return
        try:
            self._canvas.configure(scrollregion=self._canvas.bbox('all'))
        except Exception:
            pass

    def _on_canvas_configure(self, event):
        if not hasattr(self, '_canvas'):
            return
        try:
            self._canvas.itemconfigure(self._canvas_window, width=event.width)
        except Exception:
            pass
        self._maybe_load_more()

    def _on_scrollbar(self, *args):
        try:
            self._canvas.yview(*args)
        except Exception:
            pass
        self._maybe_load_more()

    def _bind_wheel(self, widget):
        for seq in ('<MouseWheel>', '<Button-4>', '<Button-5>'):
            widget.bind(seq, self._on_mousewheel, add='+')

    def _bind_wheel_tree(self, widget):
        self._bind_wheel(widget)
        try:
            for child in widget.winfo_children():
                self._bind_wheel_tree(child)
        except Exception:
            pass

    def bind_global_wheel_fallback(self):
        if self._wheel_fallback_bound:
            return
        root = self.winfo_toplevel()
        entry = _SESSION_WHEEL_ROOTS.get(id(root))
        if entry is None:
            entry = {'root': root, 'panels': []}
            _SESSION_WHEEL_ROOTS[id(root)] = entry
            for seq in ('<MouseWheel>', '<Button-4>', '<Button-5>'):
                try:
                    root.bind_all(seq, lambda e, r=root: _dispatch_session_wheel(r, e), add='+')
                except Exception:
                    pass
        panels = entry.get('panels')
        if panels is not None and self not in panels:
            panels.append(self)
        self._wheel_fallback_bound = True

    def unbind_global_wheel_fallback(self):
        try:
            root = self.winfo_toplevel()
            entry = _SESSION_WHEEL_ROOTS.get(id(root))
            panels = entry.get('panels') if entry else None
            if panels is not None:
                entry['panels'] = [p for p in panels if p is not self]
        except Exception:
            pass
        self._wheel_fallback_bound = False

    def _cursor_inside_panel(self, event) -> bool:
        try:
            x_root = int(getattr(event, 'x_root', 0) or 0)
            y_root = int(getattr(event, 'y_root', 0) or 0)
            x1 = int(self.winfo_rootx())
            y1 = int(self.winfo_rooty())
            width = int(getattr(self, 'PANEL_W', self.winfo_width()) or self.winfo_width())
            height = int(getattr(self, 'PANEL_H', self.winfo_height()) or self.winfo_height())
            return x1 <= x_root < x1 + width and y1 <= y_root < y1 + height
        except Exception:
            return False

    def _on_global_mousewheel(self, event):
        try:
            if not self.winfo_exists() or not self.winfo_ismapped():
                return None
        except Exception:
            return None
        if not self._cursor_inside_panel(event):
            return None
        return self._on_mousewheel(event)

    def _on_mousewheel(self, event):
        try:
            delta = _CY_UI.session_scroll_delta(
                getattr(event, 'num', None), getattr(event, 'delta', 0))
            if self._gpu_managed:
                total = len(self._rows_data)
                old_first = int(self._first_visible_row)
                self._first_visible_row = _CY_UI.session_scroll_first_index(
                    self._first_visible_row, total, self._visible_row_count, delta)
                if self._first_visible_row != old_first:
                    self._queue_gpu_paint(16)
                return 'break'
            if not hasattr(self, '_canvas'):
                return 'break'
            self._canvas.yview_scroll(delta, 'units')
        except Exception:
            pass
        self._maybe_load_more()
        return 'break'

    @staticmethod
    def _short_name(name: str) -> str:
        return _CY_UI.short_session_name(name)

    def _destroy_rows(self):
        self._loading_footer = None
        for child in self._rows_host.winfo_children():
            try:
                child.destroy()
            except Exception:
                pass

    def _clear_loading_footer(self):
        footer = self._loading_footer
        self._loading_footer = None
        if footer is not None:
            try:
                footer.destroy()
            except Exception:
                pass

    def _make_row_widget(self, row, idx: int):
        is_self = bool(row.get('is_self'))
        bg = '#fffaf0' if is_self else ('#f8f8f8' if idx % 2 == 0 else '#f1f3f4')
        fg = '#4f5962'
        item = tk.Frame(self._rows_host, bg=bg, height=38, highlightthickness=0)
        item.pack(fill=tk.X, padx=6, pady=(5 if idx == 0 else 0, 0))
        item.pack_propagate(False)
        tk.Frame(item, bg=('#f3af12' if is_self else '#86dfff'),
                 width=3).pack(side=tk.LEFT, fill=tk.Y)
        name_text = self._short_name(row.get('name') or '')
        if is_self:
            name_text = f'* {name_text}'
        tk.Label(item, text=name_text, bg=bg, fg=fg,
                 font=get_cjk_font(9, is_self), anchor='w',
                 width=14).pack(side=tk.LEFT, padx=(8, 4), fill=tk.Y)
        tk.Label(item, text=str(row.get('uid') or '--'), bg=bg, fg='#8a97a3',
                 font=get_sao_font(7), anchor='center',
                 width=11).pack(side=tk.LEFT, fill=tk.Y)
        tk.Label(item, text=str(row.get('fight_power') or '--'), bg=bg,
                 fg=('#d9980e' if is_self else '#6d7379'),
                 font=get_sao_font(8, is_self), anchor='e',
                 width=10).pack(side=tk.RIGHT, padx=(2, 8), fill=tk.Y)
        self._bind_wheel_tree(item)

    def _update_loading_footer(self):
        self._clear_loading_footer()
        total = len(self._rows_data)
        if self._rendered_count >= total:
            return
        remaining = total - self._rendered_count
        footer = tk.Label(
            self._rows_host,
            text=f'继续滚动加载 · 已载入 {self._rendered_count}/{total} · 剩余 {remaining}',
            bg='#eeeeee', fg='#9a8a68',
            font=get_cjk_font(8),
            pady=10,
        )
        footer.pack(fill=tk.X, padx=6, pady=(5, 4))
        self._loading_footer = footer
        self._bind_wheel_tree(footer)

    def _append_row_batch(self, batch_size: int | None = None):
        total = len(self._rows_data)
        if self._rendered_count >= total:
            self._update_loading_footer()
            return
        self._clear_loading_footer()
        batch = int(batch_size or self.RENDER_BATCH_ROWS)
        start = self._rendered_count
        end = min(total, start + max(1, batch))
        for idx in range(start, end):
            self._make_row_widget(self._rows_data[idx], idx)
        self._rendered_count = end
        self._update_loading_footer()
        self._summary.configure(
            text=f'本次登录出现过 {total} 人 · 已载入 {self._rendered_count}'
            if self._rendered_count < total else f'本次登录出现过 {total} 人'
        )
        self._on_rows_configure()

    def _maybe_load_more(self):
        if not hasattr(self, '_canvas'):
            return
        if self._rendered_count >= len(self._rows_data):
            return
        try:
            first, last = self._canvas.yview()
        except Exception:
            first, last = 0.0, 1.0
        if last >= self.LOAD_MORE_THRESHOLD:
            self._append_row_batch(self.RENDER_BATCH_ROWS)

    def prepare_open_animation(self):
        if self._gpu_managed:
            self._open_reveal = 0.0
            self._cached_screen_xy = None
            return
        if getattr(self, '_gpu_required', False):
            return
        try:
            if self._open_anim_after_id:
                self.after_cancel(self._open_anim_after_id)
        except Exception:
            pass
        self._open_anim_after_id = None
        try:
            self.configure(width=self.PANEL_W, height=1)
            self._box.pack_configure(pady=(64, 0))
            self._box.configure(highlightbackground='#f3af12')
            self._summary.configure(fg='#b89036')
        except Exception:
            pass

    def play_open_animation(self):
        """Tiny SAO-style slide/glint so the list follows menu open."""
        if self._gpu_managed:
            self._cached_screen_xy = None
            try:
                if self._open_anim_after_id:
                    self.after_cancel(self._open_anim_after_id)
            except Exception:
                pass
            self._open_anim_after_id = None
            start = time.perf_counter()
            dur = self._open_anim_duration

            def _step():
                try:
                    elapsed = time.perf_counter() - start
                    self._open_reveal = _CY_UI.cubic_open_reveal(elapsed, dur)
                    self._dispatch_gpu_paint()
                    if self._open_reveal < 1.0:
                        self._open_anim_after_id = self.after(16, _step)
                    else:
                        self._open_reveal = 1.0
                        self._open_anim_after_id = None
                except Exception:
                    self._open_reveal = 1.0
                    self._open_anim_after_id = None

            _step()
            return
        if getattr(self, '_gpu_required', False):
            return
        try:
            if self._open_anim_after_id:
                self.after_cancel(self._open_anim_after_id)
        except Exception:
            pass
        self._open_anim_after_id = None
        start = time.perf_counter()
        dur = self._open_anim_duration

        def _step():
            try:
                t, _ease, height, offset, highlight_on = _CY_UI.session_open_anim_geometry(
                    self.PANEL_H, time.perf_counter() - start, dur)
                self.configure(width=self.PANEL_W, height=height)
                self._box.pack_configure(pady=(offset, 0))
                if highlight_on:
                    self._box.configure(highlightbackground='#f3af12')
                    self._summary.configure(fg='#b89036')
                else:
                    self._box.configure(highlightbackground='#d4d0d0')
                    self._summary.configure(fg='#aaaaaa')
                if t < 1.0:
                    self._open_anim_after_id = self.after(16, _step)
                else:
                    self.configure(width=self.PANEL_W, height=self.PANEL_H)
                    self._box.pack_configure(pady=(0, 0))
                    self._box.configure(highlightbackground='#d4d0d0')
                    self._summary.configure(fg='#aaaaaa')
                    self._open_anim_after_id = None
            except Exception:
                self._open_anim_after_id = None

        _step()

    def update_rows(self, rows=None, force: bool = False):
        if rows is None:
            try:
                rows = self._rows_provider() if self._rows_provider else []
            except Exception:
                rows = []
        rows = list(rows or [])
        sig = _CY_UI.session_rows_signature(rows)
        if sig == self._rows_sig and (not force or self._rows_sig is not None):
            return
        self._rows_sig = sig
        self._rows_data = rows
        self._rendered_count = 0
        self._rows_self_uid = _CY_UI.session_self_uid(rows)
        if self._gpu_managed:
            self._first_visible_row = _CY_UI.clamp_session_first_index(
                self._first_visible_row, len(self._rows_data), self._visible_row_count)
            if force:
                self._first_visible_row = 0
            self._cached_screen_xy = None
            self._dispatch_gpu_paint()
            self._queue_gpu_drain(retries=30, delay_ms=33)
            return
        if getattr(self, '_gpu_required', False) and not hasattr(self, '_rows_host'):
            return
        self._destroy_rows()
        try:
            self._canvas.yview_moveto(0.0)
        except Exception:
            pass
        self._summary.configure(text=f'本次登录出现过 {len(rows)} 人')
        if not rows:
            empty = tk.Label(
                self._rows_host,
                text='等待抓包识别玩家\nNo session players yet',
                bg='#eeeeee', fg='#a0a0a0', justify=tk.CENTER,
                font=get_cjk_font(9), pady=34)
            empty.pack(fill=tk.X)
            self._bind_wheel_tree(empty)
            self._on_rows_configure()
            return

        self._append_row_batch(self.INITIAL_RENDER_ROWS)

    def sync_pulse(self):
        if self._gpu_managed:
            self.update_rows(force=False)
            self._dispatch_gpu_paint()
            return
        self.update_rows(force=False)
