# -*- coding: utf-8 -*-
"""Process Selector Panel — platform-level process attach + module filter."""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wintypes
import os
import threading
import tkinter as tk
from typing import Any, List, Optional

from utils.sao_sound import get_sao_font as _sao_font, get_cjk_font as _cjk_font, play_sound
from gui_modules.sao_panel_ui import (
    _sao_panel_header, _sao_panel_body, _bind_panel_drag,
)
from gui_modules.sao_panel_components import action_button

_PROCESS_QUERY_LIMITED = 0x1000

try:
    _psapi = ctypes.WinDLL("psapi")

    class _PROCESS_MEMORY_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    _GetProcessMemoryInfo = _psapi.GetProcessMemoryInfo
    _GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(_PROCESS_MEMORY_COUNTERS),
        wintypes.DWORD,
    ]
    _GetProcessMemoryInfo.restype = wintypes.BOOL
    _HAS_PSAPI = True
except Exception:
    _HAS_PSAPI = False


def _get_working_set_mb(pid: int) -> float:
    if not _HAS_PSAPI:
        return 0.0
    try:
        h = ctypes.windll.kernel32.OpenProcess(_PROCESS_QUERY_LIMITED, False, pid)
        if not h:
            return 0.0
        try:
            pmc = _PROCESS_MEMORY_COUNTERS()
            pmc.cb = ctypes.sizeof(pmc)
            if _GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
                return pmc.WorkingSetSize / (1024 * 1024)
        finally:
            ctypes.windll.kernel32.CloseHandle(h)
    except Exception:
        pass
    return 0.0


def _list_processes_fallback() -> Optional[List[dict]]:
    """Enumerate via low-level walk (Engine A)."""
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


def _list_processes_toolhelp() -> List[dict]:
    """Enumerate via CreateToolhelp32Snapshot (fallback)."""
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
    return _list_processes_fallback() or _list_processes_toolhelp()


MODULE_NONE = "none"
MODULE_PRIMARY = "primary"
MODULE_SELECTED = "selected"
MODULE_ALL = "all"

_cached_sp_v1 = None
_cached_gp_v1 = None


def _set_cached_result(sp, gp):
    global _cached_sp_v1, _cached_gp_v1
    _cached_sp_v1, _cached_gp_v1 = sp, gp


def _get_cached_game_process():
    return _cached_gp_v1


_BG = "#1a1d23"
_BG_HEADER = "#22262e"
_BG_ROW = "#1e2128"
_BG_ROW_ALT = "#252930"
_BG_ROW_HOVER = "#2a3040"
_BG_ROW_SEL = "#1a3050"
_FG = "#c8cdd5"
_FG_DIM = "#6b7280"
_FG_PID = "#8b95a5"
_ACCENT = "#00c896"
_BORDER = "#2e333b"
_FG_WARN = "#e5a63e"


class ProcessSelectorPanel:
    WIDTH = 420
    HEIGHT = 520
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

    def _build(self):
        win = tk.Toplevel(self.root)
        win.withdraw()
        win.overrideredirect(True)
        win.attributes("-topmost", True)
        win.configure(bg=_BG)
        self._win = win

        header = _sao_panel_header(win, "Process Selector", self.hide, flat=True)
        _bind_panel_drag(header)

        body = _sao_panel_body(win, flat=True)
        body.configure(bg=_BG)

        # ── Search bar ──
        search_frame = tk.Frame(body, bg=_BG)
        search_frame.pack(fill=tk.X, padx=8, pady=(6, 2))

        tk.Label(search_frame, text="🔍", bg=_BG, fg=_FG_DIM,
                 font=_cjk_font(9)).pack(side=tk.LEFT, padx=(0, 4))
        search_entry = tk.Entry(search_frame, textvariable=self._search_var,
                                bg="#2a2e36", fg=_FG, insertbackground=_FG,
                                relief=tk.FLAT, font=_sao_font(9),
                                highlightthickness=1, highlightcolor=_ACCENT)
        search_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, ipady=3)

        # ── Module loading filter ──
        mod_frame = tk.Frame(body, bg=_BG)
        mod_frame.pack(fill=tk.X, padx=8, pady=(4, 2))
        tk.Label(mod_frame, text="Module Loading", bg=_BG, fg=_FG_DIM,
                 font=_sao_font(8)).pack(side=tk.LEFT, padx=(0, 8))

        self._mod_buttons = {}
        for mode, label in [(MODULE_NONE, "None"), (MODULE_PRIMARY, "Primary"),
                            (MODULE_SELECTED, "Selected"), (MODULE_ALL, "All")]:
            btn = tk.Label(mod_frame, text=label, bg=_BG_HEADER, fg=_FG,
                           font=_sao_font(8), padx=8, pady=2, cursor="hand2",
                           relief=tk.FLAT)
            btn.pack(side=tk.LEFT, padx=2)
            btn.bind("<Button-1>", lambda e, m=mode: self._set_module_mode(m))
            self._mod_buttons[mode] = btn
        self._update_module_buttons()

        # ── Column headers ──
        hdr = tk.Frame(body, bg=_BORDER)
        hdr.pack(fill=tk.X, padx=8, pady=(6, 0))
        tk.Label(hdr, text="Process", bg=_BORDER, fg=_FG_DIM,
                 font=_sao_font(8), width=28, anchor="w").pack(side=tk.LEFT, padx=(6, 0))
        tk.Label(hdr, text="PID", bg=_BORDER, fg=_FG_DIM,
                 font=_sao_font(8), width=8, anchor="e").pack(side=tk.RIGHT, padx=(0, 6))

        # ── Scrollable process list ──
        list_frame = tk.Frame(body, bg=_BG)
        list_frame.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 4))

        canvas = tk.Canvas(list_frame, bg=_BG, highlightthickness=0, bd=0)
        scrollbar = tk.Scrollbar(list_frame, orient=tk.VERTICAL, command=canvas.yview)
        inner = tk.Frame(canvas, bg=_BG)

        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=inner, anchor="nw", width=self.WIDTH - 32)
        canvas.configure(yscrollcommand=scrollbar.set)

        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        def _on_mousewheel(event):
            canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")

        canvas.bind("<MouseWheel>", _on_mousewheel)
        inner.bind("<MouseWheel>", _on_mousewheel)

        self._scroll_canvas = canvas
        self._inner_frame = inner

        # ── Status line: mode indicator ──
        status_frame = tk.Frame(body, bg=_BG)
        status_frame.pack(fill=tk.X, padx=8, pady=(2, 0))

        self._mode_label = tk.Label(status_frame, text="", bg=_BG, fg=_FG_DIM,
                                    font=_sao_font(7), anchor="w")
        self._mode_label.pack(side=tk.LEFT, padx=(4, 0))
        self._update_mode_display()

        # ── Footer: attach label + buttons ──
        footer = tk.Frame(body, bg=_BG)
        footer.pack(fill=tk.X, padx=8, pady=(2, 8))

        self._count_label = tk.Label(footer, text="", bg=_BG, fg=_FG_DIM,
                                     font=_sao_font(7), anchor="w")
        self._count_label.pack(side=tk.LEFT, padx=(4, 0))

        self._attached_label = tk.Label(footer, text="Not attached", bg=_BG,
                                        fg=_FG_DIM, font=_sao_font(8), anchor="w")
        self._attached_label.pack(side=tk.LEFT, padx=(4, 0), fill=tk.X, expand=True)

        btn_close = action_button(footer, "Close", self.hide)
        btn_close.pack(side=tk.RIGHT, padx=(4, 0))

        btn_attach = action_button(footer, "Attach", self._do_attach)
        btn_attach.pack(side=tk.RIGHT, padx=(4, 0))

        btn_refresh = action_button(footer, "Refresh", self._do_refresh)
        btn_refresh.pack(side=tk.RIGHT, padx=(4, 0))

        # Restore last attached info
        self._restore_last_attach()
        self.root.after(50, self._do_refresh)

    def _update_mode_display(self):
        if not self._mode_label:
            return
        try:
            from mem_probe import rt_io
            parts = []
            if getattr(rt_io, '_r1_ok', None) or getattr(rt_io, '_DRIVER_OK', False):
                parts.append("EngA")
            if getattr(rt_io, '_r3h', None):
                parts.append("EngE")
            tier = ""
            try:
                tier = rt_io.memory_tier()
            except Exception:
                pass
            if tier:
                parts.append(f"Tier:{tier}")
            src = getattr(self, '_enum_source', '')
            if src:
                parts.append(f"Enum:{src}")
            if self._attach_mode:
                parts.append(self._attach_mode)
            text = " · ".join(parts) if parts else "No engine"
            self._mode_label.configure(text=text, fg=_ACCENT if parts else _FG_DIM)
        except Exception:
            self._mode_label.configure(text="", fg=_FG_DIM)

    def _restore_last_attach(self):
        try:
            name = self.owner.settings.get("attached_process_name", "")
            pid = self.owner.settings.get("attached_process_pid", 0)
            if name and pid and self._attached_label:
                self._attached_label.configure(
                    text=f"Last: {name} ({pid})", fg=_FG_DIM)
        except Exception:
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
                btn.configure(bg=_ACCENT, fg="#000000")
            else:
                btn.configure(bg=_BG_HEADER, fg=_FG)

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
            stealth = _list_processes_fallback()
            if stealth is not None:
                procs, src = stealth, "EngA"
            else:
                procs, src = _list_processes_toolhelp(), "Toolhelp"
            if self._exists():
                self.root.after(0, lambda: self._set_processes(procs, src))

        threading.Thread(target=_bg, daemon=True, name="proc-enum").start()

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
        query = self._search_var.get().strip().lower()
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

        display = self._filtered[:self._MAX_VISIBLE_ROWS]
        for i, proc in enumerate(display):
            bg = _BG_ROW_SEL if proc["pid"] == self._selected_pid else (
                _BG_ROW_ALT if i % 2 else _BG_ROW)
            row = tk.Frame(inner, bg=bg, cursor="hand2")
            row.pack(fill=tk.X, pady=0)

            name_lbl = tk.Label(row, text=proc["name"], bg=bg, fg=_FG,
                                font=_sao_font(9), anchor="w", width=24)
            name_lbl.pack(side=tk.LEFT, padx=(6, 0), pady=2)

            pid_lbl = tk.Label(row, text=str(proc["pid"]), bg=bg, fg=_FG_PID,
                               font=_sao_font(9), anchor="e", width=7)
            pid_lbl.pack(side=tk.RIGHT, padx=(0, 6), pady=2)

            pid = proc["pid"]
            for widget in (row, name_lbl, pid_lbl):
                widget.bind("<Button-1>", lambda e, p=pid: self._select_pid(p))
                widget.bind("<Enter>", lambda e, r=row: r.configure(bg=_BG_ROW_HOVER) or
                            [c.configure(bg=_BG_ROW_HOVER) for c in r.winfo_children()])
                widget.bind("<Leave>", lambda e, r=row, idx=i, p=pid: self._restore_row_bg(r, idx, p))
                widget.bind("<MouseWheel>", lambda e: self._scroll_canvas.yview_scroll(
                    int(-1 * (e.delta / 120)), "units"))
                widget.bind("<Double-Button-1>", lambda e, p=pid: self._do_attach())

            self._row_frames.append(row)

        if len(self._filtered) > self._MAX_VISIBLE_ROWS:
            trunc = tk.Label(inner, text=f"… {len(self._filtered) - self._MAX_VISIBLE_ROWS} more (use search to filter)",
                             bg=_BG, fg=_FG_DIM, font=_sao_font(7))
            trunc.pack(fill=tk.X, pady=4)

    def _restore_row_bg(self, row, idx, pid):
        bg = _BG_ROW_SEL if pid == self._selected_pid else (
            _BG_ROW_ALT if idx % 2 else _BG_ROW)
        row.configure(bg=bg)
        for c in row.winfo_children():
            c.configure(bg=bg)

    def _select_pid(self, pid: int):
        old_pid = self._selected_pid
        self._selected_pid = pid
        for i, row in enumerate(self._row_frames):
            display = self._filtered[:self._MAX_VISIBLE_ROWS]
            if i < len(display):
                p = display[i]["pid"]
                if p == pid or p == old_pid:
                    bg = _BG_ROW_SEL if p == pid else (_BG_ROW_ALT if i % 2 else _BG_ROW)
                    row.configure(bg=bg)
                    for c in row.winfo_children():
                        c.configure(bg=bg)
        proc = next((p for p in self._filtered if p["pid"] == pid), None)
        if proc and self._attached_label:
            self._attached_label.configure(text=f"Selected: {proc['name']} ({pid})", fg=_FG)

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
            self._attached_label.configure(text=f"Attaching {name}…", fg=_FG_WARN)

        def _bg_attach():
            try:
                import config
                config.GAME_PROCESS_NAMES = [name]
            except Exception:
                pass

            stealth_ok = False
            found_pid = pid
            try:
                from mem_probe._pm._core import PageResolver
                sp = PageResolver()
                result = sp.find_process_by_name(name)
                if result:
                    found_pid, cr3 = result
                    gp = sp.as_game_process(found_pid)
                    _set_cached_result(sp, gp)
                    stealth_ok = True
            except Exception:
                pass

            if not stealth_ok:
                try:
                    from mem_probe.process import set_game_process_names
                    set_game_process_names([name])
                except Exception:
                    pass

            try:
                self.owner.settings.set("attached_process_name", name)
                self.owner.settings.set("attached_process_pid", found_pid)
                self.owner.settings.set("process_module_mode", self._module_mode)
                self.owner.settings.save()
            except Exception:
                pass

            if self._exists():
                self.root.after(0, lambda: self._on_attach_done(name, found_pid, stealth_ok))

        threading.Thread(target=_bg_attach, daemon=True, name="proc-attach").start()

    def _on_attach_done(self, name: str, pid: int, stealth: bool):
        self._attach_mode = "Stealth" if stealth else "API"
        mode = "Stealth" if stealth else "Attached"
        if self._attached_label:
            self._attached_label.configure(text=f"✓ {mode}: {name} ({pid})", fg=_ACCENT)
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
        self._update_mode_display()
        if not self._win.geometry().startswith("1x1"):
            self._do_refresh()
            return
        try:
            sw = self.root.winfo_screenwidth()
            sh = self.root.winfo_screenheight()
            x = (sw - self.WIDTH) // 2
            y = (sh - self.HEIGHT) // 2
            self._win.geometry(f"{self.WIDTH}x{self.HEIGHT}+{x}+{y}")
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
