# -*- coding: utf-8 -*-
# Process Manager Panel — platform-level process monitor, tree view, attach + inspectors.
# Ported from dioprocess-private (Rust/Dioxus) into SAO-UI (Python/Tk).
from __future__ import annotations

import os
import threading
import time as _time
import tkinter as tk
from collections import defaultdict
from typing import Any, Dict, List, Optional, Set

from utils.sao_sound import get_sao_font as _sao_font, get_cjk_font as _cjk_font, play_sound
from gui_modules.sao_panel_ui import (
    _sao_panel_header, _sao_panel_body, _bind_panel_drag,
    _apply_panel_style,
    _SAO_PANEL_BODY_BG, _SAO_PANEL_BORDER, _SAO_PANEL_ACCENT,
    _SAO_PANEL_LABEL_FG, _SAO_PANEL_VALUE_FG,
    _SAO_PANEL_SEP, _theme_color,
)
from gui_modules.sao_panel_components import action_button, rounded_panel, sao_scrollbar


# ── Process enumeration (unchanged API) ──

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


def _list_processes_ext() -> List[dict]:
    """Extended enumeration with ppid, threads, memory, CPU times."""
    try:
        from mem_probe.process import _iter_process_entries_ext
        seen: dict[int, dict] = {}
        for e in _iter_process_entries_ext():
            name = os.path.basename(str(e.name or ""))
            if not name or e.pid <= 0:
                continue
            if e.pid not in seen:
                seen[e.pid] = {
                    "name": name, "pid": e.pid, "ppid": e.ppid,
                    "threads": e.thread_count,
                    "memory_mb": round(e.working_set / (1024 * 1024), 1) if e.working_set else 0.0,
                    "kernel_time": e.kernel_time, "user_time": e.user_time,
                    "cpu": 0.0, "arch": "",
                }
        return sorted(seen.values(), key=lambda x: x["name"].lower())
    except Exception:
        return _list_processes_fallback()


def list_processes() -> List[dict]:
    return _list_processes_primary() or _list_processes_fallback()


MODULE_NONE = "none"
MODULE_PRIMARY = "primary"
MODULE_SELECTED = "selected"
MODULE_ALL = "all"

_sp_ref = None
_gp_ref = None
_name_ref = ""
_pid_ref = 0


def _cache_result(sp, gp, name: str = "", pid: int = 0):
    global _sp_ref, _gp_ref, _name_ref, _pid_ref
    _sp_ref, _gp_ref = sp, gp
    _name_ref, _pid_ref = str(name or ""), int(pid or 0)


def get_cached_gp():
    return _gp_ref


def get_cached_process_info() -> dict:
    gp = _gp_ref
    tier = ""
    if gp is not None:
        try:
            tier = gp.memory_tier
        except Exception:
            tier = ""
    return {
        "attached": gp is not None,
        "engine": "A" if gp is not None else "",
        "name": _name_ref,
        "pid": _pid_ref,
        "tier": tier,
    }


# ── Palette helpers ──
def _tc(key: str, fallback: str = '') -> str:
    return _theme_color(key, fallback) or fallback


# ── Sort keys ──
_SORT_KEYS = {
    "pid": lambda p: p.get("pid", 0),
    "name": lambda p: p.get("name", "").lower(),
    "arch": lambda p: p.get("arch", ""),
    "cpu": lambda p: p.get("cpu", 0.0),
    "threads": lambda p: p.get("threads", 0),
    "memory": lambda p: p.get("memory_mb", 0.0),
}

_COL_HEADERS = {
    "pid": "PID", "name": "Name", "arch": "Arch",
    "cpu": "CPU", "threads": "Threads", "memory": "Memory",
}


# ── Tree builder (ported from dioprocess process_tab.rs) ──

def _build_tree_rows(procs: List[dict], query: str,
                     sort_col: str, sort_asc: bool,
                     expanded: Set[int]) -> List[dict]:
    pid_set = {p["pid"] for p in procs}

    children: Dict[int, List[dict]] = defaultdict(list)
    roots: List[dict] = []
    for p in procs:
        ppid = p.get("ppid", 0)
        if ppid == 0 or ppid not in pid_set:
            roots.append(p)
        else:
            children[ppid].append(p)

    q = query.lower().strip()
    if q:
        matching = {p["pid"] for p in procs
                    if q in p["name"].lower() or q in str(p["pid"])
                    or q in str(p.get("exe_path", "")).lower()}
        visible = set(matching)
        parent_map = {p["pid"]: p.get("ppid", 0) for p in procs}
        for pid in matching:
            cur = pid
            while True:
                pp = parent_map.get(cur, 0)
                if pp == 0 or pp not in pid_set or pp in visible:
                    break
                visible.add(pp)
                cur = pp
    else:
        visible = pid_set

    key_fn = _SORT_KEYS.get(sort_col, _SORT_KEYS["name"])
    roots.sort(key=key_fn, reverse=not sort_asc)
    for ch_list in children.values():
        ch_list.sort(key=key_fn, reverse=not sort_asc)

    result: List[dict] = []

    def _visit(proc, depth, is_last, ancestor_last):
        if proc["pid"] not in visible:
            return
        ch = [c for c in children.get(proc["pid"], []) if c["pid"] in visible]
        has_ch = bool(ch)
        is_exp = proc["pid"] in expanded

        guides = []
        for al in ancestor_last:
            guides.append("  " if al else "│ ")
        connector = ""
        if depth > 0:
            connector = "└─" if is_last else "├─"

        result.append({
            **proc,
            "_depth": depth,
            "_has_children": has_ch,
            "_is_expanded": is_exp,
            "_guides": guides,
            "_connector": connector,
        })

        if has_ch and (is_exp or (q and has_ch)):
            for i, child in enumerate(ch):
                _visit(child, depth + 1, i == len(ch) - 1,
                       ancestor_last + [is_last])

    for i, root in enumerate(roots):
        _visit(root, 0, i == len(roots) - 1, [])

    return result


def _fmt_mem(mb: float) -> str:
    if mb <= 0:
        return "0"
    if mb >= 1024:
        return f"{mb / 1024:.1f} GB"
    return f"{mb:.1f} MB"


def _fmt_uptime(seconds: int) -> str:
    if seconds <= 0:
        return "0m"
    d = seconds // 86400
    h = (seconds % 86400) // 3600
    m = (seconds % 3600) // 60
    if d > 0:
        return f"{d}d {h}h {m}m"
    if h > 0:
        return f"{h}h {m}m"
    return f"{m}m"


# ── Panel ──

class ProcessManagerPanel:
    MIN_W = 640
    MIN_H = 460
    DEFAULT_W = 820
    DEFAULT_H = 640
    _MAX_VISIBLE_ROWS = 400
    _AUTO_REFRESH_MS = 3000

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._processes: List[dict] = []
        self._display_rows: List[dict] = []
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
        self._stats_labels: Dict[str, tk.Label] = {}
        self._attach_mode: str = ""
        self._enum_source: str = ""
        self._resize_state: dict = {}
        self._tree_mode: bool = False
        self._expanded_pids: Set[int] = set()
        self._sort_col: str = "memory"
        self._sort_asc: bool = False
        self._col_labels: Dict[str, tk.Label] = {}
        self._auto_refresh: bool = True
        self._auto_refresh_id: Optional[str] = None
        self._cpu_tracker = None
        self._system_stats: dict = {}
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

    # ── Resize ──
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
        w, h = self._win.winfo_width(), self._win.winfo_height()
        edge = self._hit_edge(event.x, event.y, w, h)
        cursors = {
            'n': 'top_side', 's': 'bottom_side', 'w': 'left_side', 'e': 'right_side',
            'nw': 'top_left_corner', 'ne': 'top_right_corner',
            'sw': 'bottom_left_corner', 'se': 'bottom_right_corner',
        }
        self._win.configure(cursor=cursors.get(edge, ''))

    def _on_resize_press(self, event):
        if not self._exists():
            return
        w, h = self._win.winfo_width(), self._win.winfo_height()
        edge = self._hit_edge(event.x, event.y, w, h)
        if not edge:
            self._resize_state = {}
            return
        self._resize_state = {
            'edge': edge, 'sx': event.x_root, 'sy': event.y_root,
            'ow': w, 'oh': h, 'ox': self._win.winfo_x(), 'oy': self._win.winfo_y(),
        }

    def _on_resize_drag(self, event):
        s = self._resize_state
        if not s or not self._exists():
            return
        edge = s['edge']
        dx, dy = event.x_root - s['sx'], event.y_root - s['sy']
        nw, nh, nx, ny = s['ow'], s['oh'], s['ox'], s['oy']
        if 'e' in edge: nw = max(self.MIN_W, s['ow'] + dx)
        if 's' in edge: nh = max(self.MIN_H, s['oh'] + dy)
        if 'w' in edge:
            proposed = s['ow'] - dx
            if proposed >= self.MIN_W: nw, nx = proposed, s['ox'] + dx
        if 'n' in edge:
            proposed = s['oh'] - dy
            if proposed >= self.MIN_H: nh, ny = proposed, s['oy'] + dy
        self._win.geometry(f"{nw}x{nh}+{nx}+{ny}")

    def _on_resize_release(self, _event):
        self._resize_state = {}

    # ── Build ──

    def _build(self):
        from render.tk_mirror import SaoToplevel
        win = SaoToplevel(self.root, mirror_name='process_selector')
        win.withdraw()
        win.overrideredirect(True)
        win.configure(bg=_tc('border', _SAO_PANEL_BORDER))
        self._win = win

        # Pre-set geometry via Tk's own path so pack/grid knows the size
        # BEFORE mirror attaches (deiconify reads Win32 rect).
        try:
            sw = self.root.winfo_screenwidth()
            sh = self.root.winfo_screenheight()
            x = (sw - self.DEFAULT_W) // 2
            y = (sh - self.DEFAULT_H) // 2
            tk.Toplevel.geometry(win, f"{self.DEFAULT_W}x{self.DEFAULT_H}+{x}+{y}")
            win.update_idletasks()
        except Exception:
            pass

        win.bind("<Motion>", self._on_resize_motion)
        win.bind("<Button-1>", self._on_resize_press, add='+')
        win.bind("<B1-Motion>", self._on_resize_drag, add='+')
        win.bind("<ButtonRelease-1>", self._on_resize_release, add='+')

        header = _sao_panel_header(win, '◈', 'PROCESS MANAGER', on_close=self.hide)
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win)
        body_bg = _tc('body_bg', _SAO_PANEL_BODY_BG)
        body.configure(bg=body_bg)
        self._body = body

        # ── System stats bar ──
        stats_frame = tk.Frame(body, bg=_tc('sep', _SAO_PANEL_SEP))
        stats_frame.pack(fill=tk.X, padx=12, pady=(8, 4))

        sep_bg = _tc('sep', _SAO_PANEL_SEP)
        stat_font = _sao_font(8)
        stat_fg = _tc('label_fg', _SAO_PANEL_LABEL_FG)
        stat_val_fg = _tc('value_fg', _SAO_PANEL_VALUE_FG)

        for key, label_text in [("sys_cpu", "CPU"), ("sys_mem", "Memory"),
                                ("sys_procs", "Processes"), ("sys_uptime", "Uptime")]:
            f = tk.Frame(stats_frame, bg=sep_bg)
            f.pack(side=tk.LEFT, padx=(8, 12), pady=4)
            tk.Label(f, text=label_text + ":", bg=sep_bg, fg=stat_fg,
                     font=stat_font).pack(side=tk.LEFT, padx=(0, 4))
            lbl = tk.Label(f, text="-", bg=sep_bg, fg=stat_val_fg, font=stat_font)
            lbl.pack(side=tk.LEFT)
            self._stats_labels[key] = lbl

        # ── Toolbar: search + buttons ──
        toolbar = tk.Frame(body, bg=body_bg)
        toolbar.pack(fill=tk.X, padx=12, pady=(6, 4))

        ctrl_bg = _tc('control_bg', '#fafbfb')
        val_fg = _tc('value_fg', _SAO_PANEL_VALUE_FG)
        placeholder_fg = _tc('label_fg', _SAO_PANEL_LABEL_FG)

        search_card, search_inner = rounded_panel(
            toolbar, bg=ctrl_bg, border=_tc('border', _SAO_PANEL_BORDER),
            radius=8, pad=6, height=32,
        )
        search_card.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 6))

        search_icon = tk.Label(search_inner, text="🔍", bg=ctrl_bg,
                               fg=placeholder_fg, font=_cjk_font(10))
        search_icon.pack(side=tk.LEFT, padx=(2, 6))
        search_entry = tk.Entry(
            search_inner, textvariable=self._search_var,
            bg=ctrl_bg, fg=val_fg, insertbackground=val_fg,
            relief=tk.FLAT, font=_sao_font(10),
            highlightthickness=0, bd=0,
        )
        search_entry.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, pady=2)
        self._search_entry = search_entry

        self._placeholder_active = True
        search_entry.insert(0, "Search by name, PID, or path…")
        search_entry.configure(fg=placeholder_fg)

        def _on_focus_in(_e):
            if self._placeholder_active:
                self._placeholder_active = False
                search_entry.delete(0, tk.END)
                search_entry.configure(fg=val_fg)

        def _on_focus_out(_e):
            if not self._search_var.get():
                self._placeholder_active = True
                search_entry.insert(0, "Search by name, PID, or path…")
                search_entry.configure(fg=placeholder_fg)

        search_entry.bind("<FocusIn>", _on_focus_in)
        search_entry.bind("<FocusOut>", _on_focus_out)

        btn_refresh = action_button(toolbar, "Refresh", self._do_refresh, padx=10, pady=4)
        btn_refresh.pack(side=tk.LEFT, padx=(0, 3))

        self._tree_btn = action_button(toolbar, "Tree", self._toggle_tree, padx=10, pady=4)
        self._tree_btn.pack(side=tk.LEFT, padx=(0, 3))

        self._expand_btn = action_button(toolbar, "Expand All", self._expand_all, padx=8, pady=4)
        self._collapse_btn = action_button(toolbar, "Collapse", self._collapse_all, padx=8, pady=4)

        # ── Module filter ──
        mod_frame = tk.Frame(body, bg=body_bg)
        mod_frame.pack(fill=tk.X, padx=12, pady=(2, 4))
        tk.Label(mod_frame, text="Module", bg=body_bg,
                 fg=_tc('label_fg', _SAO_PANEL_LABEL_FG),
                 font=_sao_font(8)).pack(side=tk.LEFT, padx=(0, 8))
        self._mod_buttons = {}
        for mode, label in [(MODULE_NONE, "None"), (MODULE_PRIMARY, "Primary"),
                            (MODULE_SELECTED, "Selected"), (MODULE_ALL, "All")]:
            btn = action_button(
                mod_frame, label, lambda m=mode: self._set_module_mode(m),
                kind='normal', padx=10, pady=3,
                active_fill=_tc('accent', _SAO_PANEL_ACCENT),
                active_border=_tc('accent', _SAO_PANEL_ACCENT),
                active_fg='#000000',
            )
            btn.pack(side=tk.LEFT, padx=2)
            self._mod_buttons[mode] = btn
        self._update_module_buttons()

        # ── Column headers ──
        hdr_bg = _tc('sep', _SAO_PANEL_SEP)
        hdr_frame = tk.Frame(body, bg=hdr_bg)
        hdr_frame.pack(fill=tk.X, padx=12, pady=(2, 0))
        self._col_labels = {}

        hdr_fg = _tc('label_fg', _SAO_PANEL_LABEL_FG)
        hdr_font = _sao_font(8)

        columns = [
            ("pid", "PID", "e", 52, False),
            ("name", "Name", "w", 0, True),
            ("arch", "Arch", "center", 36, False),
            ("cpu", "CPU", "e", 48, False),
            ("threads", "Threads", "e", 52, False),
            ("memory", "Memory", "e", 80, False),
            ("path", "Path", "w", 120, False),
        ]

        for col_key, col_text, anchor, width, expand in columns:
            text = col_text
            if col_key == self._sort_col:
                text += " ▲" if self._sort_asc else " ▼"
            lbl = tk.Label(hdr_frame, text=text, bg=hdr_bg, fg=hdr_fg,
                           font=hdr_font, anchor=anchor, cursor="hand2")
            if expand:
                lbl.pack(side=tk.LEFT, padx=(8, 4), pady=3, fill=tk.X, expand=True)
            else:
                lbl.configure(width=width // 7)
                if col_key == "pid":
                    lbl.pack(side=tk.LEFT, padx=(8, 2), pady=3)
                elif col_key == "path":
                    lbl.pack(side=tk.RIGHT, padx=(2, 8), pady=3)
                else:
                    lbl.pack(side=tk.RIGHT, padx=(2, 2), pady=3)
            if col_key != "path":
                lbl.bind("<Button-1>", lambda e, k=col_key: self._on_sort(k))
            self._col_labels[col_key] = lbl

        # ── Scrollable process list (Canvas direct-draw, no child widgets) ──
        list_frame = tk.Frame(body, bg=body_bg)
        list_frame.pack(fill=tk.BOTH, expand=True, padx=12, pady=(0, 4))

        card_bg = _tc('card_bg', '#ffffff')
        canvas = tk.Canvas(list_frame, bg=card_bg, highlightthickness=1,
                           highlightbackground=_tc('border', _SAO_PANEL_BORDER), bd=0)
        scrollbar = sao_scrollbar(list_frame, canvas.yview)
        canvas.configure(yscrollcommand=scrollbar.set)

        def _on_canvas_resize(event):
            self._rebuild_rows()
        canvas.bind("<Configure>", _on_canvas_resize)

        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        canvas.bind("<MouseWheel>", lambda e: canvas.yview_scroll(
            int(-1 * (e.delta / 120)), "units"))
        canvas.bind("<Button-1>", lambda e: self._canvas_click(e))
        canvas.bind("<Double-Button-1>", lambda e: self._canvas_double_click(e))
        canvas.bind("<Button-3>", lambda e: self._canvas_right_click(e))

        self._scroll_canvas = canvas
        self._inner_frame = None

        # ── Status line ──
        status_frame = tk.Frame(body, bg=body_bg)
        status_frame.pack(fill=tk.X, padx=12, pady=(4, 0))
        self._mode_label = tk.Label(status_frame, text="", bg=body_bg,
                                    fg=_tc('label_fg', _SAO_PANEL_LABEL_FG),
                                    font=_sao_font(7), anchor="w")
        self._mode_label.pack(side=tk.LEFT, padx=(2, 0))
        self._update_mode_display()

        # ── Footer ──
        footer = tk.Frame(body, bg=body_bg)
        footer.pack(fill=tk.X, padx=12, pady=(4, 10))

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

        # ── Resize grip ──
        grip = tk.Label(body, text="◢", bg=body_bg,
                        fg=_tc('border', _SAO_PANEL_BORDER),
                        font=_sao_font(8), cursor="bottom_right_corner")
        grip.place(relx=1.0, rely=1.0, anchor="se", x=-2, y=-2)

        # ── Keybinds ──
        win.bind("<Escape>", lambda _e: self.hide())
        win.bind("<Return>", lambda _e: self._do_attach())
        win.bind("<Up>", lambda _e: self._navigate(-1))
        win.bind("<Down>", lambda _e: self._navigate(1))
        win.bind("<F5>", lambda _e: self._do_refresh())

        self._restore_last_attach()
        self.root.after(50, self._do_refresh)

    # ── Helpers ──

    def _bind_mousewheel_recursive(self, widget):
        def _on_mousewheel(event):
            if self._scroll_canvas:
                self._scroll_canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        widget.bind("<MouseWheel>", _on_mousewheel)
        for child in widget.winfo_children():
            self._bind_mousewheel_recursive(child)

    def _toggle_tree(self):
        self._tree_mode = not self._tree_mode
        if hasattr(self, '_tree_btn'):
            self._tree_btn.set_active(self._tree_mode)
        if self._tree_mode:
            self._expand_btn.pack(side=tk.LEFT, padx=(0, 3))
            self._collapse_btn.pack(side=tk.LEFT, padx=(0, 3))
        else:
            self._expand_btn.pack_forget()
            self._collapse_btn.pack_forget()
        self._apply_filter()

    def _expand_all(self):
        self._expanded_pids = {p["pid"] for p in self._processes}
        self._apply_filter()

    def _collapse_all(self):
        self._expanded_pids.clear()
        self._apply_filter()

    def _on_sort(self, col: str):
        if self._sort_col == col:
            self._sort_asc = not self._sort_asc
        else:
            self._sort_col = col
            self._sort_asc = col == "name"
        for k, lbl in self._col_labels.items():
            base = _COL_HEADERS.get(k, k.capitalize())
            if k == self._sort_col:
                base += " ▲" if self._sort_asc else " ▼"
            lbl.configure(text=base)
        self._apply_filter()

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

    def _update_stats_display(self):
        s = self._system_stats
        if not s:
            return
        labels = self._stats_labels
        accent = _tc('accent', _SAO_PANEL_ACCENT)
        if "sys_cpu" in labels:
            cpu = s.get("cpu", 0.0)
            fg = accent if cpu > 50 else _tc('value_fg', _SAO_PANEL_VALUE_FG)
            labels["sys_cpu"].configure(text=f"{cpu:.1f}%", fg=fg)
        if "sys_mem" in labels:
            used = s.get("mem_used_gb", 0.0)
            total = s.get("mem_total_gb", 0.0)
            pct = s.get("mem_pct", 0.0)
            labels["sys_mem"].configure(
                text=f"{used:.1f}/{total:.1f} GB ({pct:.0f}%)")
        if "sys_procs" in labels:
            labels["sys_procs"].configure(text=str(s.get("process_count", 0)))
        if "sys_uptime" in labels:
            labels["sys_uptime"].configure(text=_fmt_uptime(s.get("uptime", 0)))

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
            btn.set_active(m == self._module_mode)

    def _schedule_filter(self):
        if self._filter_after_id is not None:
            try:
                self.root.after_cancel(self._filter_after_id)
            except Exception:
                pass
        self._filter_after_id = self.root.after(150, self._apply_filter)

    # ── Auto refresh ──
    def _schedule_auto_refresh(self):
        if self._auto_refresh_id is not None:
            try:
                self.root.after_cancel(self._auto_refresh_id)
            except Exception:
                pass
            self._auto_refresh_id = None
        if self._auto_refresh and self.is_visible():
            self._auto_refresh_id = self.root.after(
                self._AUTO_REFRESH_MS, self._auto_refresh_tick)

    def _auto_refresh_tick(self):
        self._auto_refresh_id = None
        if self._auto_refresh and self.is_visible():
            self._do_refresh()

    # ── Process list ──
    def _do_refresh(self):
        def _bg():
            procs = _list_processes_ext()
            src = "S"
            try:
                primary = _list_processes_primary()
                if primary is not None:
                    src = "A"
                    ext_pids = {p["pid"] for p in procs}
                    for p in primary:
                        if p["pid"] not in ext_pids:
                            procs.append({
                                "name": p["name"], "pid": p["pid"],
                                "ppid": 0, "threads": 0, "memory_mb": 0.0,
                                "kernel_time": 0, "user_time": 0,
                                "cpu": 0.0, "arch": "",
                            })
            except Exception:
                pass

            cpu_map: Dict[int, float] = {}
            sys_stats: dict = {}
            try:
                if self._cpu_tracker is None:
                    from mem_probe.cpu_tracker import CpuTracker
                    self._cpu_tracker = CpuTracker()
                from mem_probe.process import _iter_process_entries_ext
                entries = list(_iter_process_entries_ext())
                cpu_map = self._cpu_tracker.update(entries)
                total_mem = sum(e.working_set for e in entries)
                sys_stats = {
                    "cpu": sum(cpu_map.values()),
                    "mem_used_gb": total_mem / (1024 ** 3),
                    "mem_total_gb": 0, "mem_pct": 0,
                    "process_count": len(entries),
                    "uptime": 0,
                }
                try:
                    import ctypes
                    kernel32 = ctypes.windll.kernel32
                    sys_stats["uptime"] = kernel32.GetTickCount64() // 1000
                except Exception:
                    pass
                try:
                    import ctypes
                    class MEMSTATUSEX(ctypes.Structure):
                        _fields_ = [
                            ("dwLength", ctypes.c_ulong),
                            ("dwMemoryLoad", ctypes.c_ulong),
                            ("ullTotalPhys", ctypes.c_ulonglong),
                            ("ullAvailPhys", ctypes.c_ulonglong),
                            ("ullTotalPageFile", ctypes.c_ulonglong),
                            ("ullAvailPageFile", ctypes.c_ulonglong),
                            ("ullTotalVirtual", ctypes.c_ulonglong),
                            ("ullAvailVirtual", ctypes.c_ulonglong),
                            ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
                        ]
                    ms = MEMSTATUSEX()
                    ms.dwLength = ctypes.sizeof(MEMSTATUSEX)
                    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms))
                    sys_stats["mem_total_gb"] = ms.ullTotalPhys / (1024 ** 3)
                    sys_stats["mem_used_gb"] = (ms.ullTotalPhys - ms.ullAvailPhys) / (1024 ** 3)
                    if ms.ullTotalPhys > 0:
                        sys_stats["mem_pct"] = (ms.ullTotalPhys - ms.ullAvailPhys) / ms.ullTotalPhys * 100
                except Exception:
                    pass
            except Exception:
                pass

            for p in procs:
                p["cpu"] = round(cpu_map.get(p["pid"], 0.0), 1)

            if self._exists():
                self.root.after(0, lambda: self._set_processes(procs, src, sys_stats))

        threading.Thread(target=_bg, daemon=True).start()

    def _set_processes(self, procs: List[dict], source: str = "",
                       sys_stats: Optional[dict] = None):
        if not self._exists():
            return
        self._processes = procs
        self._enum_source = source
        if sys_stats:
            self._system_stats = sys_stats
            self._update_stats_display()
        self._apply_filter()
        self._update_mode_display()
        self._schedule_auto_refresh()

    def _apply_filter(self):
        self._filter_after_id = None
        if not self._exists():
            return
        query = "" if self._placeholder_active else self._search_var.get().strip()

        if self._tree_mode:
            self._display_rows = _build_tree_rows(
                self._processes, query,
                self._sort_col, self._sort_asc,
                self._expanded_pids,
            )
        else:
            q = query.lower()
            filtered = self._processes
            if q:
                filtered = [p for p in filtered
                            if q in p["name"].lower() or q in str(p["pid"])]
            key_fn = _SORT_KEYS.get(self._sort_col, _SORT_KEYS["name"])
            self._display_rows = sorted(filtered, key=key_fn, reverse=not self._sort_asc)

        self._rebuild_rows()
        if self._count_label:
            total = len(self._processes)
            shown = len(self._display_rows)
            total_mem = sum(p.get("memory_mb", 0) for p in self._display_rows)
            parts = [f"{shown}/{total}" if shown != total else str(total)]
            if total_mem > 0:
                parts.append(_fmt_mem(total_mem))
            self._count_label.configure(text=" · ".join(parts))

    # ── Canvas-based row rendering (zero Frame/Label per row) ──

    _ROW_H = 22

    def _rebuild_rows(self):
        cv = self._scroll_canvas
        if cv is None:
            return
        cv.delete("row")
        self._row_frames = []

        card_bg = _tc('card_bg', '#ffffff')
        card_alt = _tc('card_bg_alt', '#f4f6f8')
        fg = _tc('value_fg', _SAO_PANEL_VALUE_FG)
        fg_dim = _tc('label_fg', '#8c878a')
        accent = _tc('accent', _SAO_PANEL_ACCENT)
        sel_fg = _tc('active_fg', '#000000')
        ok_color = _tc('ok', '#5cc46a')
        danger_color = _tc('danger', '#ef684e')
        border_fg = _tc('border', _SAO_PANEL_BORDER)

        display = self._display_rows[:self._MAX_VISIBLE_ROWS]
        rh = self._ROW_H

        if not display:
            text = "No processes found" if self._processes else "Loading…"
            cv.create_text(10, 30, text=text, fill=fg_dim, font=_cjk_font(9),
                           anchor="w", tags="row")
            cv.configure(scrollregion=(0, 0, 1, 60))
            return

        try:
            cw = max(100, cv.winfo_width() - 2)
        except Exception:
            cw = self.DEFAULT_W - 54

        max_mem = max((p.get("memory_mb", 0) for p in display), default=1) or 1
        name_font = _cjk_font(9)
        mono_font = _sao_font(8)
        small_font = _sao_font(7)
        total_h = len(display) * rh

        # Column X positions (from left)
        x_pid = 6
        x_name = 60
        # From right edge
        x_mem_r = cw - 8
        x_thr_r = x_mem_r - 75
        x_cpu_r = x_thr_r - 50
        x_arch_r = x_cpu_r - 35

        for i, proc in enumerate(display):
            pid = proc["pid"]
            is_sel = pid == self._selected_pid
            y0 = i * rh
            y1 = y0 + rh
            ym = y0 + rh // 2

            bg = accent if is_sel else (card_alt if i % 2 else card_bg)
            text_fg = sel_fg if is_sel else fg
            dim_fg = sel_fg if is_sel else fg_dim

            # Row background
            cv.create_rectangle(0, y0, cw, y1, fill=bg, outline='', tags="row")

            # Accent bar (3px left)
            if is_sel:
                cv.create_rectangle(0, y0, 3, y1, fill=accent, outline='', tags="row")

            # PID
            cv.create_text(x_pid, ym, text=str(pid), fill=dim_fg,
                           font=mono_font, anchor="w", tags="row")

            # Name (with tree prefix)
            name_x = x_name
            if self._tree_mode:
                guides = proc.get("_guides", [])
                connector = proc.get("_connector", "")
                has_ch = proc.get("_has_children", False)
                is_exp = proc.get("_is_expanded", False)
                prefix = "".join(guides) + connector
                if prefix:
                    cv.create_text(name_x, ym, text=prefix, fill=border_fg,
                                   font=mono_font, anchor="w", tags="row")
                    name_x += len(prefix) * 8
                if has_ch:
                    arrow = "▼" if is_exp else "▶"
                    cv.create_text(name_x, ym, text=arrow, fill=accent,
                                   font=small_font, anchor="w", tags="row")
                    name_x += 14
                else:
                    name_x += 14

            # Clip name to available width
            name_avail = x_arch_r - name_x - 8
            name_text = proc["name"]
            cv.create_text(name_x, ym, text=name_text, fill=text_fg,
                           font=name_font, anchor="w", tags="row",
                           width=max(20, name_avail))

            # Memory bar + text (rightmost)
            mem_mb = proc.get("memory_mb", 0.0)
            mem_bar_x = x_mem_r - 72
            bar_pct = min(1.0, mem_mb / max_mem) if max_mem > 0 else 0
            bar_w = max(0, int(68 * bar_pct))
            cv.create_rectangle(mem_bar_x, y0 + 4, x_mem_r, y1 - 4,
                                fill=card_alt if not is_sel else accent,
                                outline='', tags="row")
            if bar_w > 0:
                bar_color = accent if bar_pct > 0.5 else ok_color
                if is_sel:
                    bar_color = sel_fg
                cv.create_rectangle(mem_bar_x, y0 + 4, mem_bar_x + bar_w, y1 - 4,
                                    fill=bar_color, outline='', tags="row")
            cv.create_text((mem_bar_x + x_mem_r) // 2, ym, text=_fmt_mem(mem_mb),
                           fill=dim_fg, font=small_font, tags="row")

            # Threads
            thr = proc.get("threads", 0)
            cv.create_text(x_thr_r, ym, text=str(thr) if thr else "-",
                           fill=dim_fg, font=mono_font, anchor="e", tags="row")

            # CPU
            cpu_val = proc.get("cpu", 0.0)
            cpu_text = f"{cpu_val:.1f}%" if cpu_val >= 0.1 else "0%"
            if is_sel:
                cpu_fg = sel_fg
            elif cpu_val > 50:
                cpu_fg = danger_color
            elif cpu_val > 10:
                cpu_fg = accent
            else:
                cpu_fg = fg_dim
            cv.create_text(x_cpu_r, ym, text=cpu_text, fill=cpu_fg,
                           font=mono_font, anchor="e", tags="row")

            # Arch
            arch = proc.get("arch", "") or "-"
            cv.create_text(x_arch_r, ym, text=arch, fill=dim_fg,
                           font=small_font, anchor="e", tags="row")

        cv.configure(scrollregion=(0, 0, cw, total_h))

    def _toggle_expand(self, pid: int):
        if pid in self._expanded_pids:
            self._expanded_pids.discard(pid)
        else:
            self._expanded_pids.add(pid)
        self._apply_filter()

    def _row_bg(self, idx: int, pid: int) -> str:
        if pid == self._selected_pid:
            return _tc('accent', _SAO_PANEL_ACCENT)
        return _tc('card_bg_alt', '#f4f6f8') if idx % 2 else _tc('card_bg', '#ffffff')

    def _canvas_click(self, event):
        cv = self._scroll_canvas
        if cv is None:
            return
        cy = cv.canvasy(event.y)
        idx = int(cy // self._ROW_H)
        display = self._display_rows[:self._MAX_VISIBLE_ROWS]
        if 0 <= idx < len(display):
            proc = display[idx]
            pid = proc["pid"]
            if self._tree_mode and proc.get("_has_children"):
                name_x = 60
                guides = proc.get("_guides", [])
                connector = proc.get("_connector", "")
                prefix_w = (len("".join(guides)) + len(connector)) * 8
                arrow_region_start = 60 + prefix_w
                arrow_region_end = arrow_region_start + 14
                cx = cv.canvasx(event.x)
                if arrow_region_start <= cx <= arrow_region_end:
                    self._toggle_expand(pid)
                    return
            self._select_pid(pid)

    def _canvas_double_click(self, event):
        self._do_attach()

    def _canvas_right_click(self, event):
        cv = self._scroll_canvas
        if cv is None:
            return
        cy = cv.canvasy(event.y)
        idx = int(cy // self._ROW_H)
        display = self._display_rows[:self._MAX_VISIBLE_ROWS]
        if 0 <= idx < len(display):
            pid = display[idx]["pid"]
            self._select_pid(pid)
            self._show_context_menu(event, pid)

    def _navigate(self, direction: int):
        display = self._display_rows[:self._MAX_VISIBLE_ROWS]
        if not display:
            return
        cur_idx = -1
        for i, p in enumerate(display):
            if p["pid"] == self._selected_pid:
                cur_idx = i
                break
        new_idx = max(0, min(len(display) - 1, cur_idx + direction))
        self._select_pid(display[new_idx]["pid"])
        cv = self._scroll_canvas
        if cv:
            try:
                rh = self._ROW_H
                row_y = new_idx * rh
                _, _, _, total_h = cv.bbox("all") or (0, 0, 0, 1)
                ch = cv.winfo_height()
                if total_h > ch:
                    top = row_y / total_h
                    bot = (row_y + rh) / total_h
                    vis_top, vis_bot = cv.yview()
                    if top < vis_top:
                        cv.yview_moveto(top)
                    elif bot > vis_bot:
                        cv.yview_moveto(bot - (vis_bot - vis_top))
            except Exception:
                pass

    def _select_pid(self, pid: int):
        self._selected_pid = pid
        self._rebuild_rows()
        proc = next((p for p in self._display_rows if p["pid"] == pid), None)
        if proc and self._attached_label:
            self._attached_label.configure(
                text=f"Selected: {proc['name']} ({pid})",
                fg=_tc('value_fg', _SAO_PANEL_VALUE_FG))

    # ── Context menu ──
    def _show_context_menu(self, event, pid: int):
        self._select_pid(pid)
        proc = next((p for p in self._display_rows if p["pid"] == pid), None)
        if not proc:
            return
        menu = tk.Menu(self._win, tearoff=0,
                       bg=_tc('card_bg', '#ffffff'),
                       fg=_tc('value_fg', _SAO_PANEL_VALUE_FG),
                       activebackground=_tc('accent', _SAO_PANEL_ACCENT),
                       activeforeground=_tc('active_fg', '#000000'),
                       font=_cjk_font(9))
        name = proc["name"]
        menu.add_command(label=f"Attach  {name}", command=self._do_attach)
        menu.add_separator()
        menu.add_command(label="Copy PID",
                         command=lambda: self._copy_to_clipboard(str(pid)))
        menu.add_command(label="Copy Name",
                         command=lambda: self._copy_to_clipboard(name))
        menu.add_separator()
        inspect_menu = tk.Menu(menu, tearoff=0,
                               bg=_tc('card_bg', '#ffffff'),
                               fg=_tc('value_fg', _SAO_PANEL_VALUE_FG),
                               activebackground=_tc('accent', _SAO_PANEL_ACCENT),
                               activeforeground=_tc('active_fg', '#000000'),
                               font=_cjk_font(9))
        inspect_menu.add_command(label="Threads", state=tk.DISABLED)
        inspect_menu.add_command(label="Handles", state=tk.DISABLED)
        inspect_menu.add_command(label="Modules", state=tk.DISABLED)
        inspect_menu.add_command(label="Memory", state=tk.DISABLED)
        inspect_menu.add_separator()
        inspect_menu.add_command(label="String Scan", state=tk.DISABLED)
        inspect_menu.add_command(label="Hook Scan", state=tk.DISABLED)
        inspect_menu.add_command(label="Performance", state=tk.DISABLED)
        menu.add_cascade(label="Inspect", menu=inspect_menu)
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            menu.grab_release()

    def _copy_to_clipboard(self, text: str):
        try:
            self._win.clipboard_clear()
            self._win.clipboard_append(text)
        except Exception:
            pass

    # ── Attach ──
    def _do_attach(self):
        if self._selected_pid <= 0:
            return
        proc = next((p for p in self._processes if p["pid"] == self._selected_pid), None)
        if not proc:
            return
        name, pid = proc["name"], proc["pid"]
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
                    _cache_result(sp, gp, name=name, pid=found_pid)
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
        if self._auto_refresh_id is not None:
            try:
                self.root.after_cancel(self._auto_refresh_id)
            except Exception:
                pass
            self._auto_refresh_id = None
        if self._win:
            try:
                self._win.withdraw()
            except Exception:
                pass
        maybe_stop_fisheye = getattr(self.owner, '_maybe_stop_fisheye', None)
        if callable(maybe_stop_fisheye):
            try:
                maybe_stop_fisheye()
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
        if self._auto_refresh_id is not None:
            try:
                self.root.after_cancel(self._auto_refresh_id)
            except Exception:
                pass
            self._auto_refresh_id = None
        if self._win:
            try:
                self._win.destroy()
            except Exception:
                pass
            self._win = None


# Backward compatibility
ProcessSelectorPanel = ProcessManagerPanel
