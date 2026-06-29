# -*- coding: utf-8 -*-
"""Process Selector Panel — platform-level process attach + module filter."""
from __future__ import annotations

import os
import threading
import tkinter as tk
from typing import Any, List, Optional

from utils.sao_sound import get_sao_font as _sao_font, get_cjk_font as _cjk_font, play_sound
from gui_modules.sao_panel_ui import (
    _sao_panel_header, _sao_panel_body, _bind_panel_drag,
    _apply_panel_style,
    _SAO_PANEL_BODY_BG, _SAO_PANEL_BORDER, _SAO_PANEL_ACCENT,
    _SAO_PANEL_LABEL_FG, _SAO_PANEL_VALUE_FG,
    _SAO_PANEL_SEP, _theme_color,
)
from gui_modules.sao_panel_components import action_button

def _list_processes_primary() -> Optional[List[dict]]:
    try:
        from mem_probe._pm._core import PageResolver
        entries = PageResolver().enumerate_processes()
        if not entries:
            return None
        seen: dict[int, dict] = {}
        for img, pid in entries:
            if pid > 0 and img and pid not in seen:
                seen[pid] = {"name": img, "pid": pid}
        return sorted(seen.values(), key=lambda x: x["name"].lower())
    except Exception:
        return None


def _list_processes_fallback() -> List[dict]:
    from mem_probe.process import _iter_process_entries_wide
    seen: dict[int, dict] = {}
    for exe_name, pid in _iter_process_entries_wide():
        name = os.path.basename(str(exe_name or ""))
        if not name or pid <= 0:
            continue
        if pid not in seen:
            seen[pid] = {"name": name, "pid": pid}
    return sorted(seen.values(), key=lambda x: x["name"].lower())


def list_processes() -> List[dict]:
    return _list_processes_primary() or _list_processes_fallback()


MODULE_NONE = "none"
MODULE_PRIMARY = "primary"
MODULE_SELECTED = "selected"
MODULE_ALL = "all"

_sp_ref = None
_gp_ref = None


def _cache_result(sp, gp):
    global _sp_ref, _gp_ref
    _sp_ref, _gp_ref = sp, gp


def get_cached_gp():
    return _gp_ref


# ── Palette helpers (theme-aware) ──
def _tc(key: str, fallback: str = '') -> str:
    return _theme_color(key, fallback) or fallback


class ProcessSelectorPanel:
    MIN_W = 380
    MIN_H = 360
    DEFAULT_W = 480
    DEFAULT_H = 580
    _MAX_VISIBLE_ROWS = 200

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._processes: List[dict] = []
        self._filtered: List[dict] = []
        self._selected_pid: int = 0
        self._module_mode: str = self._load_module_mode()
        self._search_var = tk.StringVar()
        self._filter_after_id: Optional[str] = None
        self._search_var.trace_add("write", lambda *_: self._schedule_filter())
        self._row_frames: List[tk.Frame] = []
        self._scroll_canvas: Optional[tk.Canvas] = None
        self._inner_frame: Optional[tk.Frame] = None
        self._attached_label: Optional[tk.Label] = None
        self._mode_label: Optional[tk.Label] = None
        self._count_label: Optional[tk.Label] = None
        self._attach_mode: str = ""
        self._enum_source: str = ""
        self._resize_state: dict = {}
        self._build()

    def _load_module_mode(self) -> str:
        try:
            m = self.owner.settings.get("process_module_mode", MODULE_PRIMARY)
            return m if m in (MODULE_NONE, MODULE_PRIMARY, MODULE_SELECTED, MODULE_ALL) else MODULE_PRIMARY
        except Exception:
            return MODULE_PRIMARY

    def _exists(self) -> bool:
        try:
            return bool(self._win and self._win.winfo_exists())
        except Exception:
            return False

    # ── Resize support ──
    _EDGE_SIZE = 6

    def _hit_edge(self, x, y, w, h):
        edges = []
        if y < self._EDGE_SIZE:
            edges.append('n')
        elif y > h - self._EDGE_SIZE:
            edges.append('s')
        if x < self._EDGE_SIZE:
            edges.append('w')
        elif x > w - self._EDGE_SIZE:
            edges.append('e')
        return ''.join(edges)

    def _on_resize_motion(self, event):
        if not self._exists():
            return
        w = self._win.winfo_width()
        h = self._win.winfo_height()
        edge = self._hit_edge(event.x, event.y, w, h)
        cursors = {
            'n': 'top_side', 's': 'bottom_side',
            'w': 'left_side', 'e': 'right_side',
            'nw': 'top_left_corner', 'ne': 'top_right_corner',
            'sw': 'bottom_left_corner', 'se': 'bottom_right_corner',
        }
        self._win.configure(cursor=cursors.get(edge, ''))

    def _on_resize_press(self, event):
        if not self._exists():
            return
        w = self._win.winfo_width()
        h = self._win.winfo_height()
        edge = self._hit_edge(event.x, event.y, w, h)
        if not edge:
            return
        self._resize_state = {
            'edge': edge,
            'sx': event.x_root, 'sy': event.y_root,
            'ow': w, 'oh': h,
            'ox': self._win.winfo_x(), 'oy': self._win.winfo_y(),
        }

    def _on_resize_drag(self, event):
        s = self._resize_state
        if not s or not self._exists():
            return
        edge = s['edge']
        dx = event.x_root - s['sx']
        dy = event.y_root - s['sy']
        nw, nh = s['ow'], s['oh']
        nx, ny = s['ox'], s['oy']

        if 'e' in edge:
            nw = max(self.MIN_W, s['ow'] + dx)
        if 's' in edge:
            nh = max(self.MIN_H, s['oh'] + dy)
        if 'w' in edge:
            proposed = s['ow'] - dx
            if proposed >= self.MIN_W:
                nw = proposed
                nx = s['ox'] + dx
        if 'n' in edge:
            proposed = s['oh'] - dy
            if proposed >= self.MIN_H:
                nh = proposed
                ny = s['oy'] + dy

        self._win.geometry(f"{nw}x{nh}+{nx}+{ny}")

    def _on_resize_release(self, _event):
        self._resize_state = {}

    def _build(self):
        from render.tk_mirror import SaoToplevel
        win = SaoToplevel(self.root, mirror_name='process_selector')
        win.withdraw()
        win.overrideredirect(True)
        win.configure(bg=_tc('border', _SAO_PANEL_BORDER))
        self._win = win

        win.bind("<Motion>", self._on_resize_motion)
        win.bind("<Button-1>", self._on_resize_press, add='+')
        win.bind("<B1-Motion>", self._on_resize_drag, add='+')
        win.bind("<ButtonRelease-1>", self._on_resize_release, add='+')

        header = _sao_panel_header(win, '◈', 'PROCESS SELECTOR', on_close=self.hide)
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win)
        body_bg = _tc('body_bg', _SAO_PANEL_BODY_BG)
        body.configure(bg=body_bg)

        # ── Search bar ──
        search_frame = tk.Frame(body, bg=body_bg)
        search_frame.pack(fill=tk.X, padx=12, pady=(10, 4))

        search_icon = tk.Label(search_frame, text="🔍", bg=body_bg,
                               fg=_tc('label_fg', _SAO_PANEL_LABEL_FG),
                               font=_cjk_font(10))
        search_icon.pack(side=tk.LEFT, padx=(0, 6))
        ctrl_bg = _tc('control_bg', '#fafbfb')
        val_fg = _tc('value_fg', _SAO_PANEL_VALUE_FG)
        placeholder_fg = _tc('label_fg', _SAO_PANEL_LABEL_FG)
        search_entry = tk.Entry(
            search_frame, textvariable=self._search_var,
            bg=ctrl_bg, fg=val_fg,
            insertbackground=val_fg,
            relief=tk.FLAT, font=_sao_font(10),
            highlightthickness=1,
            highlightcolor=_tc('accent', _SAO_PANEL_ACCENT),
            highlightbackground=_tc('border', _SAO_PANEL_BORDER),
        )
        search_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, ipady=4)
        self._search_entry = search_entry

        self._placeholder_active = True
        search_entry.insert(0, "Filter by name or PID…")
        search_entry.configure(fg=placeholder_fg)

        def _on_search_focus_in(_e):
            if self._placeholder_active:
                self._placeholder_active = False
                search_entry.delete(0, tk.END)
                search_entry.configure(fg=val_fg)

        def _on_search_focus_out(_e):
            if not self._search_var.get():
                self._placeholder_active = True
                search_entry.insert(0, "Filter by name or PID…")
                search_entry.configure(fg=placeholder_fg)

        search_entry.bind("<FocusIn>", _on_search_focus_in)
        search_entry.bind("<FocusOut>", _on_search_focus_out)

        # ── Module loading filter ──
        mod_frame = tk.Frame(body, bg=body_bg)
        mod_frame.pack(fill=tk.X, padx=12, pady=(4, 6))
        tk.Label(mod_frame, text="Module", bg=body_bg,
                 fg=_tc('label_fg', _SAO_PANEL_LABEL_FG),
                 font=_sao_font(8)).pack(side=tk.LEFT, padx=(0, 8))

        self._mod_buttons = {}
        for mode, label in [(MODULE_NONE, "None"), (MODULE_PRIMARY, "Primary"),
                            (MODULE_SELECTED, "Selected"), (MODULE_ALL, "All")]:
            btn = tk.Label(
                mod_frame, text=label,
                bg=_tc('control_bg', '#fafbfb'),
                fg=_tc('value_fg', _SAO_PANEL_VALUE_FG),
                font=_sao_font(8), padx=10, pady=3, cursor="hand2",
                relief=tk.FLAT, highlightthickness=1,
                highlightbackground=_tc('border', _SAO_PANEL_BORDER),
            )
            btn.pack(side=tk.LEFT, padx=2)
            btn.bind("<Button-1>", lambda e, m=mode: self._set_module_mode(m))
            self._mod_buttons[mode] = btn
        self._update_module_buttons()

        # ── Column headers ──
        sep_bg = _tc('sep', _SAO_PANEL_SEP)
        hdr_frame = tk.Frame(body, bg=sep_bg)
        hdr_frame.pack(fill=tk.X, padx=12, pady=(2, 0))
        tk.Label(hdr_frame, text="Process", bg=sep_bg,
                 fg=_tc('label_fg', _SAO_PANEL_LABEL_FG),
                 font=_sao_font(8), anchor="w").pack(side=tk.LEFT, padx=(8, 0), pady=2)
        tk.Label(hdr_frame, text="PID", bg=sep_bg,
                 fg=_tc('label_fg', _SAO_PANEL_LABEL_FG),
                 font=_sao_font(8), anchor="e").pack(side=tk.RIGHT, padx=(0, 8), pady=2)

        # ── Scrollable process list ──
        list_frame = tk.Frame(body, bg=body_bg)
        list_frame.pack(fill=tk.BOTH, expand=True, padx=12, pady=(0, 4))

        card_bg = _tc('card_bg', '#ffffff')
        canvas = tk.Canvas(list_frame, bg=card_bg, highlightthickness=1,
                           highlightbackground=_tc('border', _SAO_PANEL_BORDER), bd=0)
        scrollbar = tk.Scrollbar(list_frame, orient=tk.VERTICAL, command=canvas.yview)
        inner = tk.Frame(canvas, bg=card_bg)

        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        self._canvas_window_id = canvas.create_window(
            (0, 0), window=inner, anchor="nw", width=self.DEFAULT_W - 52)
        canvas.configure(yscrollcommand=scrollbar.set)

        def _sync_inner_width(event):
            canvas.itemconfigure(self._canvas_window_id, width=max(10, event.width - 2))
        canvas.bind("<Configure>", _sync_inner_width)

        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        self._bind_mousewheel_recursive(canvas)
        self._bind_mousewheel_recursive(inner)

        self._scroll_canvas = canvas
        self._inner_frame = inner
        self._list_frame = list_frame

        # ── Status line ──
        status_frame = tk.Frame(body, bg=body_bg)
        status_frame.pack(fill=tk.X, padx=12, pady=(6, 0))

        self._mode_label = tk.Label(status_frame, text="", bg=body_bg,
                                    fg=_tc('label_fg', _SAO_PANEL_LABEL_FG),
                                    font=_sao_font(7), anchor="w")
        self._mode_label.pack(side=tk.LEFT, padx=(2, 0))
        self._update_mode_display()

        # ── Footer ──
        footer = tk.Frame(body, bg=body_bg)
        footer.pack(fill=tk.X, padx=12, pady=(6, 10))

        self._count_label = tk.Label(footer, text="", bg=body_bg,
                                     fg=_tc('label_fg', _SAO_PANEL_LABEL_FG),
                                     font=_sao_font(8), anchor="w")
        self._count_label.pack(side=tk.LEFT, padx=(2, 0))

        self._attached_label = tk.Label(
            footer, text="Not attached", bg=body_bg,
            fg=_tc('label_fg', _SAO_PANEL_LABEL_FG),
            font=_sao_font(9), anchor="w")
        self._attached_label.pack(side=tk.LEFT, padx=(6, 0), fill=tk.X, expand=True)

        btn_close = action_button(footer, "Close", self.hide)
        btn_close.pack(side=tk.RIGHT, padx=(6, 0))

        btn_attach = action_button(footer, "Attach", self._do_attach, kind="cyan")
        btn_attach.pack(side=tk.RIGHT, padx=(6, 0))

        btn_refresh = action_button(footer, "Refresh", self._do_refresh)
        btn_refresh.pack(side=tk.RIGHT, padx=(6, 0))

        # ── Resize grip (bottom-right corner visual hint) ──
        grip = tk.Label(body, text="◢", bg=body_bg,
                        fg=_tc('border', _SAO_PANEL_BORDER),
                        font=_sao_font(8), cursor="bottom_right_corner")
        grip.place(relx=1.0, rely=1.0, anchor="se", x=-2, y=-2)

        win.bind("<Escape>", lambda _e: self.hide())
        win.bind("<Return>", lambda _e: self._do_attach())
        win.bind("<Up>", lambda _e: self._navigate(-1))
        win.bind("<Down>", lambda _e: self._navigate(1))

        self._restore_last_attach()
        self.root.after(50, self._do_refresh)

    def _bind_mousewheel_recursive(self, widget):
        def _on_mousewheel(event):
            if self._scroll_canvas:
                self._scroll_canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        widget.bind("<MouseWheel>", _on_mousewheel)
        for child in widget.winfo_children():
            self._bind_mousewheel_recursive(child)

    def _update_mode_display(self):
        if not self._mode_label:
            return
        try:
            from mem_probe import rt_io
            parts = []
            if getattr(rt_io, '_r1_ok', None) or getattr(rt_io, '_DRIVER_OK', False):
                parts.append("EngA")
            if getattr(rt_io, 'has_write_engine', lambda: False)():
                parts.append("EngE")
            tier = ""
            try:
                tier = rt_io.memory_tier()
            except Exception:
                pass
            if tier:
                parts.append(f"Tier:{tier}")
            src = self._enum_source
            if src:
                parts.append(f"Enum:{src}")
            if self._attach_mode:
                parts.append(self._attach_mode)
            text = " · ".join(parts) if parts else "No engine"
            fg = _tc('ok', '#3fae5a') if parts else _tc('label_fg', _SAO_PANEL_LABEL_FG)
            self._mode_label.configure(text=text, fg=fg)
        except Exception:
            self._mode_label.configure(text="", fg=_tc('label_fg', _SAO_PANEL_LABEL_FG))

    def _restore_last_attach(self):
        pass

    # ── Module mode ──
    def _set_module_mode(self, mode: str):
        self._module_mode = mode
        self._update_module_buttons()
        try:
            self.owner.settings.set("process_module_mode", mode)
        except Exception:
            pass

    def _update_module_buttons(self):
        for m, btn in self._mod_buttons.items():
            if m == self._module_mode:
                btn.configure(
                    bg=_tc('accent', _SAO_PANEL_ACCENT), fg="#000000",
                    highlightbackground=_tc('accent', _SAO_PANEL_ACCENT))
            else:
                btn.configure(
                    bg=_tc('control_bg', '#fafbfb'),
                    fg=_tc('value_fg', _SAO_PANEL_VALUE_FG),
                    highlightbackground=_tc('border', _SAO_PANEL_BORDER))

    def _schedule_filter(self):
        if self._filter_after_id is not None:
            try:
                self.root.after_cancel(self._filter_after_id)
            except Exception:
                pass
        self._filter_after_id = self.root.after(150, self._apply_filter)

    # ── Process list ──
    def _do_refresh(self):
        def _bg():
            primary = _list_processes_primary()
            if primary is not None:
                procs, src = primary, "A"
            else:
                procs, src = _list_processes_fallback(), "S"
            if self._exists():
                self.root.after(0, lambda: self._set_processes(procs, src))

        threading.Thread(target=_bg, daemon=True).start()

    def _set_processes(self, procs: List[dict], source: str = ""):
        if not self._exists():
            return
        self._processes = procs
        self._enum_source = source
        self._apply_filter()
        self._update_mode_display()

    def _apply_filter(self):
        self._filter_after_id = None
        if not self._exists():
            return
        query = "" if self._placeholder_active else self._search_var.get().strip().lower()
        if query:
            self._filtered = [p for p in self._processes
                              if query in p["name"].lower() or query in str(p["pid"])]
        else:
            self._filtered = list(self._processes)
        self._rebuild_rows()
        if self._count_label:
            total = len(self._processes)
            shown = len(self._filtered)
            self._count_label.configure(
                text=f"{shown}/{total}" if shown != total else str(total))

    def _rebuild_rows(self):
        inner = self._inner_frame
        if inner is None:
            return
        for w in inner.winfo_children():
            w.destroy()
        self._row_frames = []

        card_bg = _tc('card_bg', '#ffffff')
        fg = _tc('value_fg', _SAO_PANEL_VALUE_FG)
        fg_pid = _tc('label_fg', '#8c878a')

        display = self._filtered[:self._MAX_VISIBLE_ROWS]

        if not display:
            empty = tk.Label(inner, text="No processes found" if self._processes else "Loading…",
                             bg=card_bg, fg=fg_pid, font=_cjk_font(9), pady=20)
            empty.pack(fill=tk.X)
            return

        accent = _tc('accent', _SAO_PANEL_ACCENT)
        sel_fg = _tc('active_fg', '#000000')

        for i, proc in enumerate(display):
            is_sel = proc["pid"] == self._selected_pid
            bg = self._row_bg(i, proc["pid"])
            row = tk.Frame(inner, bg=bg, cursor="hand2")
            row.pack(fill=tk.X, pady=0)

            bar = tk.Frame(row, bg=accent if is_sel else bg, width=3)
            bar.pack(side=tk.LEFT, fill=tk.Y)
            bar._is_accent_bar = True

            name_lbl = tk.Label(row, text=proc["name"], bg=bg,
                                fg=sel_fg if is_sel else fg,
                                font=_cjk_font(9), anchor="w")
            name_lbl.pack(side=tk.LEFT, padx=(6, 4), pady=3, fill=tk.X, expand=True)
            name_lbl._default_fg = fg

            pid_lbl = tk.Label(row, text=str(proc["pid"]), bg=bg,
                               fg=sel_fg if is_sel else fg_pid,
                               font=_sao_font(9), anchor="e")
            pid_lbl.pack(side=tk.RIGHT, padx=(4, 8), pady=3)
            pid_lbl._default_fg = fg_pid

            pid = proc["pid"]
            for widget in (row, name_lbl, pid_lbl):
                widget.bind("<Button-1>", lambda e, p=pid: self._select_pid(p))
                widget.bind("<Enter>", lambda e, r=row: self._hover_row(r, True))
                widget.bind("<Leave>", lambda e, r=row, idx=i, p=pid: self._hover_row(r, False, idx, p))
                widget.bind("<MouseWheel>", lambda e: self._scroll_canvas and self._scroll_canvas.yview_scroll(
                    int(-1 * (e.delta / 120)), "units"))
                widget.bind("<Double-Button-1>", lambda e, p=pid: self._do_attach())

            self._row_frames.append(row)

        if len(self._filtered) > self._MAX_VISIBLE_ROWS:
            trunc = tk.Label(inner, text=f"… {len(self._filtered) - self._MAX_VISIBLE_ROWS} more (use search)",
                             bg=card_bg, fg=fg_pid, font=_sao_font(8))
            trunc.pack(fill=tk.X, pady=6)

    def _row_bg(self, idx: int, pid: int) -> str:
        if pid == self._selected_pid:
            return _tc('accent', _SAO_PANEL_ACCENT)
        return _tc('card_bg_alt', '#f4f6f8') if idx % 2 else _tc('card_bg', '#ffffff')

    def _hover_row(self, row, entering, idx=0, pid=0):
        if entering:
            bg = _tc('accent_soft', '#e2f6fd')
        else:
            bg = self._row_bg(idx, pid)
        is_sel = pid == self._selected_pid
        accent = _tc('accent', _SAO_PANEL_ACCENT)
        row.configure(bg=bg)
        for c in row.winfo_children():
            if getattr(c, '_is_accent_bar', False):
                c.configure(bg=accent if is_sel else bg)
            else:
                c.configure(bg=bg)

    def _navigate(self, direction: int):
        display = self._filtered[:self._MAX_VISIBLE_ROWS]
        if not display:
            return
        cur_idx = -1
        for i, p in enumerate(display):
            if p["pid"] == self._selected_pid:
                cur_idx = i
                break
        new_idx = max(0, min(len(display) - 1, cur_idx + direction))
        self._select_pid(display[new_idx]["pid"])
        if self._scroll_canvas and new_idx < len(self._row_frames):
            row = self._row_frames[new_idx]
            try:
                self._scroll_canvas.update_idletasks()
                ry = row.winfo_y()
                rh = row.winfo_reqheight()
                ch = self._scroll_canvas.winfo_height()
                _, _, _, scroll_h = self._scroll_canvas.bbox("all") or (0, 0, 0, ch)
                if scroll_h > ch:
                    top = ry / scroll_h
                    bot = (ry + rh) / scroll_h
                    vis_top = self._scroll_canvas.yview()[0]
                    vis_bot = self._scroll_canvas.yview()[1]
                    if top < vis_top:
                        self._scroll_canvas.yview_moveto(top)
                    elif bot > vis_bot:
                        self._scroll_canvas.yview_moveto(bot - (vis_bot - vis_top))
            except Exception:
                pass

    def _select_pid(self, pid: int):
        old_pid = self._selected_pid
        self._selected_pid = pid
        accent = _tc('accent', _SAO_PANEL_ACCENT)
        sel_fg = _tc('active_fg', '#000000')
        display = self._filtered[:self._MAX_VISIBLE_ROWS]
        for i, row in enumerate(self._row_frames):
            if i < len(display):
                p = display[i]["pid"]
                if p == pid or p == old_pid:
                    is_sel = p == pid
                    bg = self._row_bg(i, p)
                    row.configure(bg=bg)
                    for c in row.winfo_children():
                        if getattr(c, '_is_accent_bar', False):
                            c.configure(bg=accent if is_sel else bg)
                        else:
                            c.configure(bg=bg)
                            dfg = getattr(c, '_default_fg', None)
                            if dfg is not None:
                                c.configure(fg=sel_fg if is_sel else dfg)
        proc = next((p for p in self._filtered if p["pid"] == pid), None)
        if proc and self._attached_label:
            self._attached_label.configure(
                text=f"Selected: {proc['name']} ({pid})",
                fg=_tc('value_fg', _SAO_PANEL_VALUE_FG))

    # ── Attach (non-blocking) ──
    def _do_attach(self):
        if self._selected_pid <= 0:
            return
        proc = next((p for p in self._processes if p["pid"] == self._selected_pid), None)
        if not proc:
            return

        name = proc["name"]
        pid = proc["pid"]

        if self._attached_label:
            self._attached_label.configure(text=f"Attaching {name}…",
                                           fg=_tc('gold', '#dea620'))

        def _bg_attach():
            try:
                import config
                config.GAME_PROCESS_NAMES = [name]
            except Exception:
                pass

            primary_ok = False
            found_pid = pid
            try:
                from mem_probe._pm._core import PageResolver
                sp = PageResolver()
                result = sp.find_process_by_name(name)
                if result:
                    found_pid, cr3 = result
                    gp = sp.as_game_process(found_pid)
                    _cache_result(sp, gp)
                    primary_ok = True
            except Exception:
                pass

            if not primary_ok:
                try:
                    from mem_probe.process import set_game_process_names
                    set_game_process_names([name])
                except Exception:
                    pass

            try:
                self.owner.settings.set("process_module_mode", self._module_mode)
                self.owner.settings.save()
            except Exception:
                pass

            if self._exists():
                self.root.after(0, lambda: self._on_attach_done(name, found_pid, primary_ok))

        threading.Thread(target=_bg_attach, daemon=True).start()

    def _on_attach_done(self, name: str, pid: int, primary: bool):
        self._attach_mode = "A" if primary else "S"
        mode = "A" if primary else "S"
        if self._attached_label:
            self._attached_label.configure(
                text=f"✓ {mode}: {name} ({pid})",
                fg=_tc('ok', '#3fae5a'))
        self._update_mode_display()
        try:
            play_sound("alert_close")
        except Exception:
            pass

    # ── Lifecycle ──
    def show(self):
        if not self._exists():
            self._win = None
            self._build()
        self._win.deiconify()
        self._win.lift()
        try:
            self._win.update_idletasks()
            _apply_panel_style(self._win)
        except Exception:
            pass
        self._update_mode_display()
        if not self._win.geometry().startswith("1x1"):
            self._do_refresh()
            return
        try:
            sw = self.root.winfo_screenwidth()
            sh = self.root.winfo_screenheight()
            x = (sw - self.DEFAULT_W) // 2
            y = (sh - self.DEFAULT_H) // 2
            self._win.geometry(f"{self.DEFAULT_W}x{self.DEFAULT_H}+{x}+{y}")
        except Exception:
            pass
        self._do_refresh()

    def refresh_theme(self):
        if not self._exists():
            return
        geo = self._win.geometry()
        visible = self.is_visible()
        self._win.destroy()
        self._win = None
        self._placeholder_active = True
        self._search_var.set('')
        self._build()
        if visible:
            self._win.geometry(geo)
            self._win.deiconify()
            self._win.lift()
            try:
                self._win.update_idletasks()
                _apply_panel_style(self._win)
            except Exception:
                pass
            self._do_refresh()

    def hide(self):
        if self._win:
            try:
                self._win.withdraw()
            except Exception:
                pass

    def is_visible(self) -> bool:
        if not self._exists():
            return False
        try:
            return self._win.state() != "withdrawn"
        except Exception:
            return False

    def destroy(self):
        if self._win:
            try:
                self._win.destroy()
            except Exception:
                pass
            self._win = None
