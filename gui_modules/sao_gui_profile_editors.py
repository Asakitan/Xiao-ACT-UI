from __future__ import annotations

import copy
import tkinter as tk
from tkinter import filedialog, messagebox
from typing import Any, Callable, Dict, List, Optional, Tuple

from sao_web_panel_common import (
    PANEL_BG,
    PANEL_BG_ALT,
    PANEL_CARD,
    PANEL_CARD_ALT,
    PANEL_EDGE,
    PANEL_HEADER,
    TEXT_DIM,
    TEXT_MAIN,
    TEXT_MUTED,
    GOLD,
    CYAN,
    DANGER,
    READY,
    apply_badge,
    apply_surface_chrome,
    bind_drag,
    calc_panel_geometry,
    clear_frame,
    create_scrollable_area,
    make_action_button,
    make_section_title,
    panel_font,
    place_corner_accents,
)

from auto_key_engine import (
    clone_profile as clone_auto_key_profile,
    delete_profile as delete_auto_key_profile,
    export_profile_to_default_path as export_auto_key_profile,
    find_profile as find_auto_key_profile,
    import_profile_from_path as import_auto_key_profile,
    make_default_action,
    make_default_profile as make_default_auto_key_profile,
    normalize_profile as normalize_auto_key_profile,
    upsert_profile as upsert_auto_key_profile,
)

from boss_raid_engine import (
    clone_profile as clone_boss_raid_profile,
    delete_profile as delete_boss_raid_profile,
    export_profile_to_default_path as export_boss_raid_profile,
    find_profile as find_boss_raid_profile,
    import_profile_from_path as import_boss_raid_profile,
    make_default_phase,
    make_default_profile as make_default_boss_raid_profile,
    make_default_timeline,
    normalize_profile as normalize_boss_raid_profile,
    upsert_profile as upsert_boss_raid_profile,
)


def _as_int(value: Any, default: int = 0, lo: Optional[int] = None,
            hi: Optional[int] = None) -> int:
    try:
        result = int(float(str(value).strip()))
    except Exception:
        result = default
    if lo is not None:
        result = max(lo, result)
    if hi is not None:
        result = min(hi, result)
    return result


def _as_float(value: Any, default: float = 0.0,
              lo: Optional[float] = None,
              hi: Optional[float] = None) -> float:
    try:
        result = float(str(value).strip())
    except Exception:
        result = default
    if lo is not None:
        result = max(lo, result)
    if hi is not None:
        result = min(hi, result)
    return result


def _as_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    text = str(value or '').strip().lower()
    return text in ('1', 'true', 'yes', 'on', 'ready')


def _clone(value: Any) -> Any:
    return copy.deepcopy(value)


def _fmt_int(value: Any) -> str:
    try:
        return f'{int(float(value or 0)):,}'
    except Exception:
        return '0'


def _first_profile_id(config: Dict[str, Any]) -> str:
    active = str(config.get('active_profile_id') or '')
    profiles = list(config.get('profiles') or [])
    if active:
        return active
    return str((profiles[0] or {}).get('id') or '') if profiles else ''


class _DetailEditorBase:
    def __init__(self, master: tk.Tk, title: str, subtitle: str):
        self._master = master
        self._title = title
        self._subtitle = subtitle
        self._win: Optional[tk.Toplevel] = None
        self._visible = False
        self._drag_ox = 0
        self._drag_oy = 0
        self._status_label: Optional[tk.Label] = None
        self._badge: Optional[tk.Label] = None
        self._list_body: Optional[tk.Frame] = None
        self._editor_body: Optional[tk.Frame] = None
        self._list_canvas: Optional[tk.Canvas] = None
        self._editor_canvas: Optional[tk.Canvas] = None

    def is_visible(self) -> bool:
        return bool(self._visible and self._win and self._win.winfo_exists())

    def show(self) -> None:
        if self._win is None or not self._win.winfo_exists():
            self._build()
        self._visible = True
        try:
            self._win.deiconify()
            self._win.lift()
            self._win.focus_force()
        except Exception:
            pass
        self._on_show()

    def hide(self) -> None:
        if self._win is not None:
            try:
                self._win.withdraw()
            except Exception:
                pass
        self._visible = False

    def toggle(self) -> None:
        if self.is_visible():
            self.hide()
        else:
            self.show()

    def destroy(self) -> None:
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._visible = False

    def _build(self) -> None:
        width, height, pos_x, pos_y = calc_panel_geometry(
            self._master,
            min_w=900,
            min_h=620,
            width_ratio=0.52,
            height_ratio=0.72,
            x_ratio=0.05,
            y_ratio=0.10,
        )
        win = tk.Toplevel(self._master)
        win.overrideredirect(True)
        win.configure(bg=PANEL_EDGE)
        win.geometry(f'{width}x{height}+{pos_x}+{pos_y}')
        try:
            win.attributes('-topmost', True)
        except Exception:
            pass
        win.bind('<Escape>', lambda _event: self.hide())
        self._win = win

        shell = tk.Frame(win, bg=PANEL_BG, highlightthickness=1,
                         highlightbackground=PANEL_EDGE)
        shell.pack(fill=tk.BOTH, expand=True)
        apply_surface_chrome(shell, accent=CYAN, accent_side='top')
        place_corner_accents(shell)

        header = tk.Frame(shell, bg=PANEL_HEADER, height=64)
        header.pack(fill=tk.X)
        header.pack_propagate(False)
        apply_surface_chrome(header)
        bind_drag(header, self._on_drag_start, self._on_drag_move)

        tk.Label(header, text=self._subtitle, bg=PANEL_HEADER,
                 fg=TEXT_MUTED, font=panel_font(8)).pack(
            anchor='w', padx=16, pady=(10, 0))

        title_row = tk.Frame(header, bg=PANEL_HEADER)
        title_row.pack(fill=tk.X, padx=16, pady=(2, 8))
        tk.Label(title_row, text=self._title, bg=PANEL_HEADER, fg=TEXT_MAIN,
                 font=panel_font(14, bold=True)).pack(side=tk.LEFT)
        self._badge = tk.Label(title_row, text='DETAIL', bg=GOLD,
                               fg='#ffffff', font=panel_font(8, bold=True),
                               padx=8, pady=2)
        self._badge.pack(side=tk.LEFT, padx=(10, 0))
        make_action_button(title_row, 'CLOSE', self.hide).pack(side=tk.RIGHT)

        split = tk.Frame(shell, bg=PANEL_BG, padx=12, pady=10)
        split.pack(fill=tk.BOTH, expand=True)

        left = tk.Frame(split, bg=PANEL_BG_ALT, width=286,
                        highlightbackground=PANEL_EDGE, highlightthickness=1)
        left.pack(side=tk.LEFT, fill=tk.Y)
        left.pack_propagate(False)
        apply_surface_chrome(left, accent=GOLD)
        tk.Label(left, text='Profiles', bg=PANEL_BG_ALT, fg=TEXT_MAIN,
                 font=panel_font(11, bold=True)).pack(anchor='w', padx=12,
                                                       pady=(10, 4))
        _, list_canvas, list_body = create_scrollable_area(left, PANEL_BG_ALT)
        self._list_canvas = list_canvas
        self._list_body = list_body

        right = tk.Frame(split, bg=PANEL_BG, padx=10)
        right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        _, editor_canvas, editor_body = create_scrollable_area(right, PANEL_BG)
        self._editor_canvas = editor_canvas
        self._editor_body = editor_body

        tk.Frame(shell, bg=PANEL_EDGE, height=1).pack(fill=tk.X)
        status = tk.Frame(shell, bg=PANEL_BG, padx=14, pady=5)
        status.pack(fill=tk.X)
        self._status_label = tk.Label(status, text='', bg=PANEL_BG,
                                      fg=TEXT_MUTED,
                                      font=panel_font(8, bold=True),
                                      anchor='w')
        self._status_label.pack(side=tk.LEFT, fill=tk.X, expand=True)

    def _on_show(self) -> None:
        raise NotImplementedError

    def _set_status(self, text: str, ok: bool = True) -> None:
        if self._status_label is not None:
            self._status_label.configure(text=text, fg=READY if ok else DANGER)

    def _on_drag_start(self, event) -> None:
        if self._win is None:
            return
        self._drag_ox = event.x_root - self._win.winfo_x()
        self._drag_oy = event.y_root - self._win.winfo_y()

    def _on_drag_move(self, event) -> None:
        if self._win is None:
            return
        self._win.geometry(
            f'+{event.x_root - self._drag_ox}+{event.y_root - self._drag_oy}')

    def _refresh_scrollregions(self) -> None:
        for canvas in (self._list_canvas, self._editor_canvas):
            try:
                if canvas is not None:
                    canvas.configure(scrollregion=canvas.bbox('all'))
            except Exception:
                pass

    def _entry(self, parent: tk.Widget, label: str, var: tk.Variable,
               *, width: int = 12) -> tk.Entry:
        frame = tk.Frame(parent, bg=parent.cget('bg'))
        frame.pack(fill=tk.X, pady=3)
        tk.Label(frame, text=label, bg=frame.cget('bg'), fg=TEXT_MUTED,
                 font=panel_font(8, bold=True), anchor='w').pack(fill=tk.X)
        entry = tk.Entry(frame, textvariable=var, bg=PANEL_CARD_ALT,
                         fg=TEXT_MAIN, insertbackground=TEXT_MAIN,
                         relief=tk.FLAT, highlightthickness=1,
                         highlightbackground=PANEL_EDGE,
                         font=panel_font(9), width=width)
        entry.pack(fill=tk.X)
        return entry

    def _grid_entry(self, parent: tk.Widget, label: str, var: tk.Variable,
                    row: int, col: int, *, width: int = 10) -> tk.Entry:
        cell = tk.Frame(parent, bg=parent.cget('bg'))
        cell.grid(row=row, column=col, sticky='ew', padx=4, pady=3)
        parent.grid_columnconfigure(col, weight=1)
        tk.Label(cell, text=label, bg=cell.cget('bg'), fg=TEXT_MUTED,
                 font=panel_font(8, bold=True), anchor='w').pack(fill=tk.X)
        entry = tk.Entry(cell, textvariable=var, width=width,
                         bg=PANEL_CARD_ALT, fg=TEXT_MAIN,
                         insertbackground=TEXT_MAIN, relief=tk.FLAT,
                         highlightthickness=1,
                         highlightbackground=PANEL_EDGE,
                         font=panel_font(9))
        entry.pack(fill=tk.X)
        return entry

    def _option(self, parent: tk.Widget, label: str, var: tk.StringVar,
                values: Tuple[str, ...], row: int, col: int) -> tk.OptionMenu:
        cell = tk.Frame(parent, bg=parent.cget('bg'))
        cell.grid(row=row, column=col, sticky='ew', padx=4, pady=3)
        parent.grid_columnconfigure(col, weight=1)
        tk.Label(cell, text=label, bg=cell.cget('bg'), fg=TEXT_MUTED,
                 font=panel_font(8, bold=True), anchor='w').pack(fill=tk.X)
        opt = tk.OptionMenu(cell, var, *values)
        opt.configure(bg=PANEL_CARD_ALT, fg=TEXT_MAIN, activebackground=PANEL_HEADER,
                      activeforeground=TEXT_MAIN, relief=tk.FLAT,
                      highlightthickness=1, highlightbackground=PANEL_EDGE,
                      font=panel_font(9))
        opt.pack(fill=tk.X)
        return opt


class AutoKeyDetailPanel(_DetailEditorBase):
    CONDITION_TYPES = (
        'burst_ready_is',
        'slot_state_is',
        'hp_pct_gte',
        'hp_pct_lte',
        'sta_pct_gte',
        'profession_is',
        'player_name_is',
    )
    SLOT_STATES = ('ready', 'active', 'cooldown', 'insufficient_energy')

    def __init__(self, master: tk.Tk, load_fn: Callable[[], dict],
                 save_fn: Callable[[dict], Any],
                 author_fn: Optional[Callable[[], dict]] = None):
        super().__init__(master, 'AutoKey Detail Editor',
                         'Profile, action and condition editor')
        self._load = load_fn
        self._save = save_fn
        self._author_fn = author_fn or (lambda: {})
        self._cfg: Dict[str, Any] = {}
        self._selected_id = ''
        self._draft: Optional[Dict[str, Any]] = None
        self._profile_vars: Dict[str, tk.Variable] = {}
        self._engine_vars: Dict[str, tk.Variable] = {}
        self._description_text: Optional[tk.Text] = None
        self._action_vars: List[Dict[str, Any]] = []

    def _on_show(self) -> None:
        self._reload(keep_selected=True)

    def _reload(self, keep_selected: bool = True) -> None:
        try:
            self._cfg = self._load() or {}
        except Exception as exc:
            self._cfg = {}
            self._set_status(f'Load failed: {exc}', ok=False)
        if not keep_selected or not find_auto_key_profile(self._cfg, self._selected_id):
            self._selected_id = _first_profile_id(self._cfg)
        self._load_draft(self._selected_id)
        self._render()

    def _load_draft(self, profile_id: str) -> None:
        profile = find_auto_key_profile(self._cfg, profile_id)
        self._draft = _clone(profile) if profile else None

    def _render(self) -> None:
        if self._list_body is None or self._editor_body is None:
            return
        clear_frame(self._list_body)
        clear_frame(self._editor_body)
        self._render_profile_list()
        self._render_editor()
        self._refresh_scrollregions()

    def _render_profile_list(self) -> None:
        assert self._list_body is not None
        toolbar = tk.Frame(self._list_body, bg=PANEL_BG_ALT)
        toolbar.pack(fill=tk.X, padx=8, pady=(0, 8))
        make_action_button(toolbar, 'NEW', self._create_profile,
                           kind='accent').pack(side=tk.LEFT, fill=tk.X,
                                               expand=True)
        make_action_button(toolbar, 'IMPORT', self._import_profile).pack(
            side=tk.LEFT, fill=tk.X, expand=True, padx=(6, 0))

        profiles = list(self._cfg.get('profiles') or [])
        active_id = str(self._cfg.get('active_profile_id') or '')
        if not profiles:
            tk.Label(self._list_body, text='No profiles yet', bg=PANEL_BG_ALT,
                     fg=TEXT_DIM, font=panel_font(9)).pack(pady=18)
            return
        for profile in profiles:
            pid = str(profile.get('id') or '')
            selected = pid == self._selected_id
            active = pid == active_id
            card = tk.Frame(self._list_body,
                            bg=PANEL_CARD_ALT if selected else PANEL_CARD,
                            highlightthickness=1,
                            highlightbackground=GOLD if selected else PANEL_EDGE,
                            padx=8, pady=7, cursor='hand2')
            card.pack(fill=tk.X, padx=8, pady=(0, 6))
            apply_surface_chrome(card, accent=GOLD if active else CYAN)
            tk.Label(card, text=str(profile.get('profile_name') or 'AutoKey'),
                     bg=card.cget('bg'), fg=TEXT_MAIN,
                     font=panel_font(9, bold=True), anchor='w').pack(fill=tk.X)
            meta = (
                f'{int(profile.get("enabled_action_count") or len(profile.get("actions") or []))}/'
                f'{len(profile.get("actions") or [])} actions'
            )
            tk.Label(card, text=meta, bg=card.cget('bg'), fg=TEXT_MUTED,
                     font=panel_font(8), anchor='w').pack(fill=tk.X,
                                                           pady=(1, 5))
            row = tk.Frame(card, bg=card.cget('bg'))
            row.pack(fill=tk.X)
            make_action_button(row, 'OPEN',
                               lambda pid=pid: self._select_profile(pid),
                               width=4).pack(side=tk.LEFT)
            make_action_button(row, 'ON',
                               lambda pid=pid: self._activate_profile(pid),
                               kind='ready' if active else 'default',
                               width=3).pack(side=tk.LEFT, padx=(5, 0))
            make_action_button(row, 'COPY',
                               lambda pid=pid: self._copy_profile(pid),
                               width=4).pack(side=tk.LEFT, padx=(5, 0))
            for child in card.winfo_children():
                child.bind('<Button-1>',
                           lambda _event, pid=pid: self._select_profile(pid),
                           add='+')

    def _render_editor(self) -> None:
        assert self._editor_body is not None
        if not self._draft:
            self._render_empty_editor()
            return
        profile = self._draft
        make_section_title(self._editor_body, 'Profile')
        self._profile_vars = {
            'profile_name': tk.StringVar(value=str(profile.get('profile_name') or '')),
            'profession_name': tk.StringVar(value=str(profile.get('profession_name') or '')),
            'profession_id': tk.StringVar(value=str(profile.get('profession_id') or 0)),
        }
        top_grid = tk.Frame(self._editor_body, bg=PANEL_BG)
        top_grid.pack(fill=tk.X)
        self._grid_entry(top_grid, 'Name', self._profile_vars['profile_name'], 0, 0)
        self._grid_entry(top_grid, 'Profession', self._profile_vars['profession_name'], 0, 1)
        self._grid_entry(top_grid, 'Profession ID', self._profile_vars['profession_id'], 0, 2)
        tk.Label(self._editor_body, text='Description', bg=PANEL_BG,
                 fg=TEXT_MUTED, font=panel_font(8, bold=True),
                 anchor='w').pack(fill=tk.X, pady=(8, 2))
        self._description_text = tk.Text(self._editor_body, height=3,
                                         bg=PANEL_CARD_ALT, fg=TEXT_MAIN,
                                         insertbackground=TEXT_MAIN,
                                         relief=tk.FLAT, highlightthickness=1,
                                         highlightbackground=PANEL_EDGE,
                                         font=panel_font(9), wrap='word')
        self._description_text.insert('1.0', str(profile.get('description') or ''))
        self._description_text.pack(fill=tk.X)

        make_section_title(self._editor_body, 'Engine')
        engine = profile.get('engine') or {}
        self._engine_vars = {
            'tick_ms': tk.StringVar(value=str(engine.get('tick_ms') or 50)),
            'require_foreground': tk.BooleanVar(value=bool(engine.get('require_foreground', True))),
            'pause_on_death': tk.BooleanVar(value=bool(engine.get('pause_on_death', True))),
        }
        engine_grid = tk.Frame(self._editor_body, bg=PANEL_BG)
        engine_grid.pack(fill=tk.X)
        self._grid_entry(engine_grid, 'Tick ms', self._engine_vars['tick_ms'], 0, 0)
        for idx, key in enumerate(('require_foreground', 'pause_on_death'), start=1):
            cb = tk.Checkbutton(engine_grid, text=key.replace('_', ' ').title(),
                                variable=self._engine_vars[key],
                                bg=PANEL_BG, fg=TEXT_MAIN,
                                activebackground=PANEL_BG,
                                selectcolor=PANEL_CARD_ALT,
                                font=panel_font(9))
            cb.grid(row=0, column=idx, sticky='w', padx=8, pady=18)

        make_section_title(self._editor_body, 'Actions')
        action_toolbar = tk.Frame(self._editor_body, bg=PANEL_BG)
        action_toolbar.pack(fill=tk.X, pady=(0, 6))
        make_action_button(action_toolbar, '+ ACTION', self._add_action,
                           kind='accent').pack(side=tk.LEFT)
        make_action_button(action_toolbar, 'SAVE PROFILE', self._save_profile,
                           kind='ready').pack(side=tk.RIGHT)
        make_action_button(action_toolbar, 'EXPORT', self._export_selected).pack(
            side=tk.RIGHT, padx=(0, 6))
        make_action_button(action_toolbar, 'DELETE', self._delete_selected,
                           kind='danger').pack(side=tk.RIGHT, padx=(0, 6))

        self._action_vars = []
        for idx, action in enumerate(list(profile.get('actions') or [])):
            self._render_action_card(idx, action)

    def _render_empty_editor(self) -> None:
        assert self._editor_body is not None
        tk.Label(self._editor_body,
                 text='Create or import an AutoKey profile to start editing.',
                 bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(11),
                 justify='center').pack(pady=40)
        make_action_button(self._editor_body, 'CREATE PROFILE',
                           self._create_profile, kind='accent').pack()

    def _render_action_card(self, index: int, action: Dict[str, Any]) -> None:
        assert self._editor_body is not None
        card = tk.Frame(self._editor_body, bg=PANEL_CARD,
                        highlightbackground=PANEL_EDGE, highlightthickness=1,
                        padx=10, pady=8)
        card.pack(fill=tk.X, pady=(0, 8))
        apply_surface_chrome(card, accent=GOLD if action.get('enabled', True) else DANGER)
        head = tk.Frame(card, bg=PANEL_CARD)
        head.pack(fill=tk.X)
        tk.Label(head, text=f'Action {index + 1}', bg=PANEL_CARD,
                 fg=TEXT_MAIN, font=panel_font(10, bold=True)).pack(side=tk.LEFT)
        for label, cmd in (
            ('UP', lambda i=index: self._move_action(i, -1)),
            ('DOWN', lambda i=index: self._move_action(i, 1)),
            ('COPY', lambda i=index: self._copy_action(i)),
            ('DEL', lambda i=index: self._delete_action(i)),
        ):
            make_action_button(head, label, cmd, width=4,
                               kind='danger' if label == 'DEL' else 'default').pack(
                side=tk.RIGHT, padx=(5, 0))

        vars_for_action = {
            'id': str(action.get('id') or ''),
            'enabled': tk.BooleanVar(value=bool(action.get('enabled', True))),
            'label': tk.StringVar(value=str(action.get('label') or '')),
            'slot_index': tk.StringVar(value=str(action.get('slot_index') or 1)),
            'key': tk.StringVar(value=str(action.get('key') or '1')),
            'press_mode': tk.StringVar(value=str(action.get('press_mode') or 'tap')),
            'press_count': tk.StringVar(value=str(action.get('press_count') or 1)),
            'press_interval_ms': tk.StringVar(value=str(action.get('press_interval_ms') or 40)),
            'hold_ms': tk.StringVar(value=str(action.get('hold_ms') or 80)),
            'ready_delay_ms': tk.StringVar(value=str(action.get('ready_delay_ms') or 0)),
            'min_rearm_ms': tk.StringVar(value=str(action.get('min_rearm_ms') or 800)),
            'post_delay_ms': tk.StringVar(value=str(action.get('post_delay_ms') or 120)),
            'conditions': [],
        }
        self._action_vars.append(vars_for_action)
        tk.Checkbutton(card, text='Enabled', variable=vars_for_action['enabled'],
                       bg=PANEL_CARD, fg=TEXT_MAIN, activebackground=PANEL_CARD,
                       selectcolor=PANEL_CARD_ALT, font=panel_font(9)).pack(
            anchor='w', pady=(6, 0))
        grid = tk.Frame(card, bg=PANEL_CARD)
        grid.pack(fill=tk.X)
        self._grid_entry(grid, 'Label', vars_for_action['label'], 0, 0)
        self._grid_entry(grid, 'Slot', vars_for_action['slot_index'], 0, 1)
        self._grid_entry(grid, 'Key', vars_for_action['key'], 0, 2)
        self._option(grid, 'Press Mode', vars_for_action['press_mode'],
                     ('tap', 'hold'), 0, 3)
        self._grid_entry(grid, 'Count', vars_for_action['press_count'], 1, 0)
        self._grid_entry(grid, 'Interval ms', vars_for_action['press_interval_ms'], 1, 1)
        self._grid_entry(grid, 'Hold ms', vars_for_action['hold_ms'], 1, 2)
        self._grid_entry(grid, 'Ready Delay', vars_for_action['ready_delay_ms'], 1, 3)
        self._grid_entry(grid, 'Min Rearm', vars_for_action['min_rearm_ms'], 2, 0)
        self._grid_entry(grid, 'Post Delay', vars_for_action['post_delay_ms'], 2, 1)

        cond_wrap = tk.Frame(card, bg=PANEL_CARD)
        cond_wrap.pack(fill=tk.X, pady=(8, 0))
        tk.Label(cond_wrap, text='Conditions', bg=PANEL_CARD, fg=TEXT_MUTED,
                 font=panel_font(8, bold=True)).pack(anchor='w')
        for cond_idx, cond in enumerate(list(action.get('conditions') or [])):
            self._render_condition_row(cond_wrap, vars_for_action, index,
                                       cond_idx, cond)
        make_action_button(cond_wrap, '+ CONDITION',
                           lambda i=index: self._add_condition(i)).pack(
            anchor='w', pady=(5, 0))

    def _render_condition_row(self, parent: tk.Widget, action_vars: Dict[str, Any],
                              action_index: int, condition_index: int,
                              condition: Dict[str, Any]) -> None:
        row = tk.Frame(parent, bg=PANEL_CARD_ALT, padx=6, pady=5,
                       highlightbackground=PANEL_EDGE, highlightthickness=1)
        row.pack(fill=tk.X, pady=(4, 0))
        ctype = str(condition.get('type') or 'burst_ready_is')
        cond_vars = {
            'type': tk.StringVar(value=ctype if ctype in self.CONDITION_TYPES else 'burst_ready_is'),
            'value': tk.StringVar(value=str(condition.get('value', ''))),
            'slot_index': tk.StringVar(value=str(condition.get('slot_index') or 0)),
            'state': tk.StringVar(value=str(condition.get('state') or 'ready')),
        }
        action_vars['conditions'].append(cond_vars)
        grid = tk.Frame(row, bg=PANEL_CARD_ALT)
        grid.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self._option(grid, 'Type', cond_vars['type'], self.CONDITION_TYPES, 0, 0)
        self._grid_entry(grid, 'Value', cond_vars['value'], 0, 1)
        self._grid_entry(grid, 'Slot', cond_vars['slot_index'], 0, 2)
        self._option(grid, 'State', cond_vars['state'], self.SLOT_STATES, 0, 3)
        make_action_button(row, 'DEL',
                           lambda ai=action_index, ci=condition_index:
                           self._delete_condition(ai, ci),
                           kind='danger', width=4).pack(side=tk.RIGHT,
                                                        padx=(6, 0))

    def _sync_draft_from_widgets(self) -> None:
        if not self._draft:
            return
        self._draft['profile_name'] = self._profile_vars['profile_name'].get().strip()
        self._draft['profession_name'] = self._profile_vars['profession_name'].get().strip()
        self._draft['profession_id'] = _as_int(self._profile_vars['profession_id'].get(), 0, 0)
        if self._description_text is not None:
            self._draft['description'] = self._description_text.get('1.0', 'end').strip()
        self._draft['engine'] = {
            'tick_ms': _as_int(self._engine_vars['tick_ms'].get(), 50, 10, 1000),
            'require_foreground': bool(self._engine_vars['require_foreground'].get()),
            'pause_on_death': bool(self._engine_vars['pause_on_death'].get()),
        }
        actions: List[Dict[str, Any]] = []
        for av in self._action_vars:
            conditions = []
            for cv in av.get('conditions') or []:
                ctype = str(cv['type'].get() or '').strip()
                cond: Dict[str, Any] = {'type': ctype}
                if ctype in ('hp_pct_gte', 'hp_pct_lte', 'sta_pct_gte'):
                    cond['value'] = _as_float(cv['value'].get(), 0.0, 0.0, 1.0)
                elif ctype == 'burst_ready_is':
                    cond['value'] = _as_bool(cv['value'].get())
                elif ctype == 'slot_state_is':
                    cond['slot_index'] = _as_int(cv['slot_index'].get(), 0, 0, 9)
                    cond['state'] = str(cv['state'].get() or 'ready')
                elif ctype in ('profession_is', 'player_name_is'):
                    cond['value'] = str(cv['value'].get() or '').strip()
                if ctype:
                    conditions.append(cond)
            actions.append({
                'id': av.get('id') or '',
                'enabled': bool(av['enabled'].get()),
                'label': av['label'].get().strip(),
                'slot_index': _as_int(av['slot_index'].get(), 1, 1, 9),
                'key': av['key'].get().strip().upper(),
                'press_mode': av['press_mode'].get().strip(),
                'press_count': _as_int(av['press_count'].get(), 1, 1, 20),
                'press_interval_ms': _as_int(av['press_interval_ms'].get(), 40, 0, 10000),
                'hold_ms': _as_int(av['hold_ms'].get(), 80, 0, 10000),
                'ready_delay_ms': _as_int(av['ready_delay_ms'].get(), 0, 0, 60000),
                'min_rearm_ms': _as_int(av['min_rearm_ms'].get(), 800, 0, 120000),
                'post_delay_ms': _as_int(av['post_delay_ms'].get(), 120, 0, 120000),
                'conditions': conditions,
            })
        self._draft['actions'] = actions

    def _select_profile(self, profile_id: str) -> None:
        self._selected_id = str(profile_id or '')
        self._load_draft(self._selected_id)
        self._render()

    def _create_profile(self) -> None:
        config = self._load() or {}
        profile = make_default_auto_key_profile(self._author_fn())
        upsert_auto_key_profile(config, profile, activate=True)
        self._save(config)
        self._selected_id = str(profile.get('id') or '')
        self._set_status('Created AutoKey profile')
        self._reload(keep_selected=True)

    def _copy_profile(self, profile_id: str) -> None:
        config = self._load() or {}
        copied = clone_auto_key_profile(config, profile_id, self._author_fn())
        if not copied:
            self._set_status('Profile not found', ok=False)
            return
        self._save(config)
        self._selected_id = str(copied.get('id') or '')
        self._set_status('Copied profile')
        self._reload(keep_selected=True)

    def _activate_profile(self, profile_id: str) -> None:
        config = self._load() or {}
        if not find_auto_key_profile(config, profile_id):
            self._set_status('Profile not found', ok=False)
            return
        config['active_profile_id'] = str(profile_id or '')
        self._save(config)
        self._selected_id = str(profile_id or '')
        self._set_status('Activated profile')
        self._reload(keep_selected=True)

    def _delete_selected(self) -> None:
        if not self._draft:
            return
        if not messagebox.askyesno('AutoKey', 'Delete this AutoKey profile?'):
            return
        config = self._load() or {}
        delete_auto_key_profile(config, self._draft.get('id'))
        self._save(config)
        self._selected_id = _first_profile_id(config)
        self._set_status('Deleted profile')
        self._reload(keep_selected=True)

    def _save_profile(self) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        config = self._load() or {}
        active_before = str(config.get('active_profile_id') or '')
        normalized = normalize_auto_key_profile(self._draft,
                                               author_snapshot=self._author_fn())
        upsert_auto_key_profile(config, normalized,
                                activate=active_before == str(normalized.get('id') or ''))
        self._save(config)
        self._selected_id = str(normalized.get('id') or '')
        self._set_status('Saved AutoKey profile')
        self._reload(keep_selected=True)

    def _export_selected(self) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        normalized = normalize_auto_key_profile(self._draft,
                                               author_snapshot=self._author_fn())
        path = export_auto_key_profile(normalized)
        self._set_status(f'Exported: {path}')

    def _import_profile(self) -> None:
        path = filedialog.askopenfilename(
            parent=self._win,
            title='Import AutoKey Profile',
            filetypes=(('JSON files', '*.json'), ('All files', '*.*')),
        )
        if not path:
            return
        try:
            config = self._load() or {}
            profile = import_auto_key_profile(path, self._author_fn())
            upsert_auto_key_profile(config, profile, activate=False)
            self._save(config)
            self._selected_id = str(profile.get('id') or '')
            self._set_status('Imported profile')
            self._reload(keep_selected=True)
        except Exception as exc:
            self._set_status(f'Import failed: {exc}', ok=False)

    def _add_action(self) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        idx = len(self._draft.get('actions') or []) + 1
        self._draft.setdefault('actions', []).append(make_default_action(idx))
        self._render()

    def _move_action(self, index: int, delta: int) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        actions = self._draft.get('actions') or []
        target = index + delta
        if target < 0 or target >= len(actions):
            return
        item = actions.pop(index)
        actions.insert(target, item)
        self._render()

    def _copy_action(self, index: int) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        actions = self._draft.get('actions') or []
        if 0 <= index < len(actions):
            copied = _clone(actions[index])
            copied['id'] = ''
            copied['label'] = f'{copied.get("label") or "Action"} Copy'
            actions.insert(index + 1, copied)
        self._render()

    def _delete_action(self, index: int) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        actions = self._draft.get('actions') or []
        if 0 <= index < len(actions):
            actions.pop(index)
        self._render()

    def _add_condition(self, action_index: int) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        actions = self._draft.get('actions') or []
        if 0 <= action_index < len(actions):
            actions[action_index].setdefault('conditions', []).append({
                'type': 'burst_ready_is',
                'value': True,
            })
        self._render()

    def _delete_condition(self, action_index: int, condition_index: int) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        actions = self._draft.get('actions') or []
        if 0 <= action_index < len(actions):
            conditions = actions[action_index].get('conditions') or []
            if 0 <= condition_index < len(conditions):
                conditions.pop(condition_index)
        self._render()


class BossRaidDetailPanel(_DetailEditorBase):
    TRIGGER_TYPES = (
        'manual',
        'time',
        'hp_pct',
        'dps_total',
        'breaking',
        'shield_broken',
        'overdrive',
        'extinction_pct',
        'breaking_stage',
    )
    ALERT_TYPES = ('both', 'visual', 'sound')
    CONDITION_TYPES = ('always', 'hp_pct', 'shield_active', 'breaking')
    COMPARATORS = ('>=', '<=', '>', '<', '==')

    def __init__(self, master: tk.Tk, load_fn: Callable[[], dict],
                 save_fn: Callable[[dict], Any],
                 author_fn: Optional[Callable[[], dict]] = None):
        super().__init__(master, 'BossRaid Detail Editor',
                         'Profile, phase and timeline editor')
        self._load = load_fn
        self._save = save_fn
        self._author_fn = author_fn or (lambda: {})
        self._cfg: Dict[str, Any] = {}
        self._selected_id = ''
        self._draft: Optional[Dict[str, Any]] = None
        self._profile_vars: Dict[str, tk.Variable] = {}
        self._description_text: Optional[tk.Text] = None
        self._phase_vars: List[Dict[str, Any]] = []

    def _on_show(self) -> None:
        self._reload(keep_selected=True)

    def _reload(self, keep_selected: bool = True) -> None:
        try:
            self._cfg = self._load() or {}
        except Exception as exc:
            self._cfg = {}
            self._set_status(f'Load failed: {exc}', ok=False)
        if not keep_selected or not find_boss_raid_profile(self._cfg, self._selected_id):
            self._selected_id = _first_profile_id(self._cfg)
        profile = find_boss_raid_profile(self._cfg, self._selected_id)
        self._draft = _clone(profile) if profile else None
        self._render()

    def _render(self) -> None:
        if self._list_body is None or self._editor_body is None:
            return
        clear_frame(self._list_body)
        clear_frame(self._editor_body)
        self._render_profile_list()
        self._render_editor()
        self._refresh_scrollregions()

    def _render_profile_list(self) -> None:
        assert self._list_body is not None
        toolbar = tk.Frame(self._list_body, bg=PANEL_BG_ALT)
        toolbar.pack(fill=tk.X, padx=8, pady=(0, 8))
        make_action_button(toolbar, 'NEW', self._create_profile,
                           kind='accent').pack(side=tk.LEFT, fill=tk.X,
                                               expand=True)
        make_action_button(toolbar, 'IMPORT', self._import_profile).pack(
            side=tk.LEFT, fill=tk.X, expand=True, padx=(6, 0))

        profiles = list(self._cfg.get('profiles') or [])
        active_id = str(self._cfg.get('active_profile_id') or '')
        if not profiles:
            tk.Label(self._list_body, text='No raid profiles yet',
                     bg=PANEL_BG_ALT, fg=TEXT_DIM,
                     font=panel_font(9)).pack(pady=18)
            return
        for profile in profiles:
            pid = str(profile.get('id') or '')
            selected = pid == self._selected_id
            active = pid == active_id
            phases = list(profile.get('phases') or [])
            timelines = sum(len(p.get('timelines') or []) for p in phases)
            card = tk.Frame(self._list_body,
                            bg=PANEL_CARD_ALT if selected else PANEL_CARD,
                            highlightthickness=1,
                            highlightbackground=GOLD if selected else PANEL_EDGE,
                            padx=8, pady=7, cursor='hand2')
            card.pack(fill=tk.X, padx=8, pady=(0, 6))
            apply_surface_chrome(card, accent=DANGER if active else CYAN)
            tk.Label(card, text=str(profile.get('profile_name') or 'Boss Raid'),
                     bg=card.cget('bg'), fg=TEXT_MAIN,
                     font=panel_font(9, bold=True), anchor='w').pack(fill=tk.X)
            meta = (
                f'HP {_fmt_int(profile.get("boss_total_hp"))} | '
                f'P{len(phases)} | TL{timelines}'
            )
            tk.Label(card, text=meta, bg=card.cget('bg'), fg=TEXT_MUTED,
                     font=panel_font(8), anchor='w').pack(fill=tk.X,
                                                           pady=(1, 5))
            row = tk.Frame(card, bg=card.cget('bg'))
            row.pack(fill=tk.X)
            make_action_button(row, 'OPEN',
                               lambda pid=pid: self._select_profile(pid),
                               width=4).pack(side=tk.LEFT)
            make_action_button(row, 'ON',
                               lambda pid=pid: self._activate_profile(pid),
                               kind='ready' if active else 'default',
                               width=3).pack(side=tk.LEFT, padx=(5, 0))
            make_action_button(row, 'COPY',
                               lambda pid=pid: self._copy_profile(pid),
                               width=4).pack(side=tk.LEFT, padx=(5, 0))

    def _render_editor(self) -> None:
        assert self._editor_body is not None
        if not self._draft:
            tk.Label(self._editor_body,
                     text='Create or import a BossRaid profile to start editing.',
                     bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(11),
                     justify='center').pack(pady=40)
            make_action_button(self._editor_body, 'CREATE PROFILE',
                               self._create_profile, kind='accent').pack()
            return
        profile = self._draft
        make_section_title(self._editor_body, 'Profile')
        self._profile_vars = {
            'profile_name': tk.StringVar(value=str(profile.get('profile_name') or '')),
            'boss_total_hp': tk.StringVar(value=str(profile.get('boss_total_hp') or 0)),
            'enrage_time_s': tk.StringVar(value=str(profile.get('enrage_time_s') or 600)),
            'target_name_pattern': tk.StringVar(value=str(profile.get('target_name_pattern') or '')),
            'simple_mode': tk.BooleanVar(value=bool(profile.get('simple_mode', True))),
        }
        grid = tk.Frame(self._editor_body, bg=PANEL_BG)
        grid.pack(fill=tk.X)
        self._grid_entry(grid, 'Name', self._profile_vars['profile_name'], 0, 0)
        self._grid_entry(grid, 'Boss HP', self._profile_vars['boss_total_hp'], 0, 1)
        self._grid_entry(grid, 'Enrage sec', self._profile_vars['enrage_time_s'], 0, 2)
        self._grid_entry(grid, 'Target Pattern', self._profile_vars['target_name_pattern'], 1, 0)
        tk.Checkbutton(grid, text='Simple Mode',
                       variable=self._profile_vars['simple_mode'],
                       bg=PANEL_BG, fg=TEXT_MAIN, activebackground=PANEL_BG,
                       selectcolor=PANEL_CARD_ALT, font=panel_font(9)).grid(
            row=1, column=1, sticky='w', padx=8, pady=18)

        tk.Label(self._editor_body, text='Description', bg=PANEL_BG,
                 fg=TEXT_MUTED, font=panel_font(8, bold=True),
                 anchor='w').pack(fill=tk.X, pady=(8, 2))
        self._description_text = tk.Text(self._editor_body, height=3,
                                         bg=PANEL_CARD_ALT, fg=TEXT_MAIN,
                                         insertbackground=TEXT_MAIN,
                                         relief=tk.FLAT, highlightthickness=1,
                                         highlightbackground=PANEL_EDGE,
                                         font=panel_font(9), wrap='word')
        self._description_text.insert('1.0', str(profile.get('description') or ''))
        self._description_text.pack(fill=tk.X)

        make_section_title(self._editor_body, 'Phases')
        toolbar = tk.Frame(self._editor_body, bg=PANEL_BG)
        toolbar.pack(fill=tk.X, pady=(0, 6))
        make_action_button(toolbar, '+ PHASE', self._add_phase,
                           kind='accent').pack(side=tk.LEFT)
        make_action_button(toolbar, 'SAVE PROFILE', self._save_profile,
                           kind='ready').pack(side=tk.RIGHT)
        make_action_button(toolbar, 'EXPORT', self._export_selected).pack(
            side=tk.RIGHT, padx=(0, 6))
        make_action_button(toolbar, 'DELETE', self._delete_selected,
                           kind='danger').pack(side=tk.RIGHT, padx=(0, 6))

        self._phase_vars = []
        for idx, phase in enumerate(list(profile.get('phases') or [])):
            self._render_phase_card(idx, phase)

    def _render_phase_card(self, index: int, phase: Dict[str, Any]) -> None:
        assert self._editor_body is not None
        card = tk.Frame(self._editor_body, bg=PANEL_CARD,
                        highlightbackground=PANEL_EDGE, highlightthickness=1,
                        padx=10, pady=8)
        card.pack(fill=tk.X, pady=(0, 8))
        apply_surface_chrome(card, accent=GOLD)
        head = tk.Frame(card, bg=PANEL_CARD)
        head.pack(fill=tk.X)
        tk.Label(head, text=f'Phase {index + 1}', bg=PANEL_CARD,
                 fg=TEXT_MAIN, font=panel_font(10, bold=True)).pack(side=tk.LEFT)
        for label, cmd in (
            ('UP', lambda i=index: self._move_phase(i, -1)),
            ('DOWN', lambda i=index: self._move_phase(i, 1)),
            ('DEL', lambda i=index: self._delete_phase(i)),
        ):
            make_action_button(head, label, cmd,
                               kind='danger' if label == 'DEL' else 'default',
                               width=4).pack(side=tk.RIGHT, padx=(5, 0))

        trigger = phase.get('trigger') or {}
        phase_vars = {
            'id': str(phase.get('id') or ''),
            'name': tk.StringVar(value=str(phase.get('name') or f'P{index + 1}')),
            'trigger_type': tk.StringVar(value=str(trigger.get('type') or 'manual')),
            'trigger_value': tk.StringVar(value=str(trigger.get('value') or 0)),
            'timelines': [],
        }
        self._phase_vars.append(phase_vars)
        grid = tk.Frame(card, bg=PANEL_CARD)
        grid.pack(fill=tk.X)
        self._grid_entry(grid, 'Name', phase_vars['name'], 0, 0)
        self._option(grid, 'Trigger', phase_vars['trigger_type'],
                     self.TRIGGER_TYPES, 0, 1)
        self._grid_entry(grid, 'Value', phase_vars['trigger_value'], 0, 2)
        make_action_button(card, '+ TIMELINE',
                           lambda i=index: self._add_timeline(i)).pack(
            anchor='w', pady=(8, 2))
        for tl_idx, timeline in enumerate(list(phase.get('timelines') or [])):
            self._render_timeline_row(card, phase_vars, index, tl_idx,
                                      timeline)

    def _render_timeline_row(self, parent: tk.Widget, phase_vars: Dict[str, Any],
                             phase_index: int, timeline_index: int,
                             timeline: Dict[str, Any]) -> None:
        row = tk.Frame(parent, bg=PANEL_CARD_ALT, padx=6, pady=5,
                       highlightbackground=PANEL_EDGE, highlightthickness=1)
        row.pack(fill=tk.X, pady=(5, 0))
        condition = timeline.get('condition') or {}
        tl_vars = {
            'id': str(timeline.get('id') or ''),
            'time_s': tk.StringVar(value=str(timeline.get('time_s') or 0)),
            'label': tk.StringVar(value=str(timeline.get('label') or 'Alert')),
            'alert_type': tk.StringVar(value=str(timeline.get('alert_type') or 'both')),
            'repeat_interval_s': tk.StringVar(value=str(timeline.get('repeat_interval_s') or 0)),
            'pre_warn_s': tk.StringVar(value=str(timeline.get('pre_warn_s') or 0)),
            'duration_s': tk.StringVar(value=str(timeline.get('duration_s') or 0)),
            'condition_type': tk.StringVar(value=str(condition.get('type') or 'always')),
            'condition_comparator': tk.StringVar(value=str(condition.get('comparator') or '>=')),
            'condition_value': tk.StringVar(value=str(condition.get('value') or 0)),
        }
        phase_vars['timelines'].append(tl_vars)
        grid = tk.Frame(row, bg=PANEL_CARD_ALT)
        grid.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self._grid_entry(grid, 'Time', tl_vars['time_s'], 0, 0)
        self._grid_entry(grid, 'Label', tl_vars['label'], 0, 1)
        self._option(grid, 'Alert', tl_vars['alert_type'], self.ALERT_TYPES, 0, 2)
        self._grid_entry(grid, 'Repeat', tl_vars['repeat_interval_s'], 1, 0)
        self._grid_entry(grid, 'Prewarn', tl_vars['pre_warn_s'], 1, 1)
        self._grid_entry(grid, 'Duration', tl_vars['duration_s'], 1, 2)
        self._option(grid, 'Cond', tl_vars['condition_type'],
                     self.CONDITION_TYPES, 2, 0)
        self._option(grid, 'Cmp', tl_vars['condition_comparator'],
                     self.COMPARATORS, 2, 1)
        self._grid_entry(grid, 'Cond Value', tl_vars['condition_value'], 2, 2)
        make_action_button(row, 'DEL',
                           lambda pi=phase_index, ti=timeline_index:
                           self._delete_timeline(pi, ti),
                           kind='danger', width=4).pack(side=tk.RIGHT,
                                                        padx=(6, 0))

    def _sync_draft_from_widgets(self) -> None:
        if not self._draft:
            return
        self._draft['profile_name'] = self._profile_vars['profile_name'].get().strip()
        self._draft['boss_total_hp'] = _as_int(self._profile_vars['boss_total_hp'].get(), 0, 0)
        self._draft['enrage_time_s'] = _as_int(self._profile_vars['enrage_time_s'].get(), 600, 0, 86400)
        self._draft['target_name_pattern'] = self._profile_vars['target_name_pattern'].get().strip()
        self._draft['simple_mode'] = bool(self._profile_vars['simple_mode'].get())
        if self._description_text is not None:
            self._draft['description'] = self._description_text.get('1.0', 'end').strip()
        phases: List[Dict[str, Any]] = []
        for pv in self._phase_vars:
            timelines: List[Dict[str, Any]] = []
            for tv in pv.get('timelines') or []:
                condition_type = str(tv['condition_type'].get() or 'always')
                condition = None
                if condition_type != 'always':
                    condition = {
                        'type': condition_type,
                        'comparator': str(tv['condition_comparator'].get() or '>='),
                        'value': _as_float(tv['condition_value'].get(), 0.0),
                    }
                timelines.append({
                    'id': tv.get('id') or '',
                    'time_s': _as_float(tv['time_s'].get(), 0.0, 0.0, 86400.0),
                    'label': tv['label'].get().strip() or 'Alert',
                    'alert_type': tv['alert_type'].get().strip() or 'both',
                    'repeat_interval_s': _as_float(tv['repeat_interval_s'].get(), 0.0, 0.0, 86400.0),
                    'pre_warn_s': _as_float(tv['pre_warn_s'].get(), 0.0, 0.0, 600.0),
                    'duration_s': _as_float(tv['duration_s'].get(), 0.0, 0.0, 600.0),
                    'condition': condition,
                })
            phases.append({
                'id': pv.get('id') or '',
                'name': pv['name'].get().strip(),
                'trigger': {
                    'type': pv['trigger_type'].get().strip() or 'manual',
                    'value': _as_float(pv['trigger_value'].get(), 0.0, 0.0),
                },
                'timelines': timelines,
            })
        self._draft['phases'] = phases

    def _select_profile(self, profile_id: str) -> None:
        self._selected_id = str(profile_id or '')
        profile = find_boss_raid_profile(self._cfg, self._selected_id)
        self._draft = _clone(profile) if profile else None
        self._render()

    def _create_profile(self) -> None:
        config = self._load() or {}
        profile = make_default_boss_raid_profile(self._author_fn())
        upsert_boss_raid_profile(config, profile, activate=True)
        self._save(config)
        self._selected_id = str(profile.get('id') or '')
        self._set_status('Created BossRaid profile')
        self._reload(keep_selected=True)

    def _copy_profile(self, profile_id: str) -> None:
        config = self._load() or {}
        copied = clone_boss_raid_profile(config, profile_id, self._author_fn())
        if not copied:
            self._set_status('Profile not found', ok=False)
            return
        self._save(config)
        self._selected_id = str(copied.get('id') or '')
        self._set_status('Copied profile')
        self._reload(keep_selected=True)

    def _activate_profile(self, profile_id: str) -> None:
        config = self._load() or {}
        if not find_boss_raid_profile(config, profile_id):
            self._set_status('Profile not found', ok=False)
            return
        config['active_profile_id'] = str(profile_id or '')
        self._save(config)
        self._selected_id = str(profile_id or '')
        self._set_status('Activated profile')
        self._reload(keep_selected=True)

    def _delete_selected(self) -> None:
        if not self._draft:
            return
        if not messagebox.askyesno('BossRaid', 'Delete this BossRaid profile?'):
            return
        config = self._load() or {}
        delete_boss_raid_profile(config, self._draft.get('id'))
        self._save(config)
        self._selected_id = _first_profile_id(config)
        self._set_status('Deleted profile')
        self._reload(keep_selected=True)

    def _save_profile(self) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        config = self._load() or {}
        active_before = str(config.get('active_profile_id') or '')
        normalized = normalize_boss_raid_profile(
            self._draft, author_snapshot=self._author_fn())
        upsert_boss_raid_profile(
            config,
            normalized,
            activate=active_before == str(normalized.get('id') or ''),
        )
        self._save(config)
        self._selected_id = str(normalized.get('id') or '')
        self._set_status('Saved BossRaid profile')
        self._reload(keep_selected=True)

    def _export_selected(self) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        normalized = normalize_boss_raid_profile(
            self._draft, author_snapshot=self._author_fn())
        path = export_boss_raid_profile(normalized)
        self._set_status(f'Exported: {path}')

    def _import_profile(self) -> None:
        path = filedialog.askopenfilename(
            parent=self._win,
            title='Import BossRaid Profile',
            filetypes=(('JSON files', '*.json'), ('All files', '*.*')),
        )
        if not path:
            return
        try:
            config = self._load() or {}
            profile = import_boss_raid_profile(path, self._author_fn())
            upsert_boss_raid_profile(config, profile, activate=False)
            self._save(config)
            self._selected_id = str(profile.get('id') or '')
            self._set_status('Imported profile')
            self._reload(keep_selected=True)
        except Exception as exc:
            self._set_status(f'Import failed: {exc}', ok=False)

    def _add_phase(self) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        idx = len(self._draft.get('phases') or []) + 1
        self._draft.setdefault('phases', []).append(make_default_phase(idx))
        self._render()

    def _move_phase(self, index: int, delta: int) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        phases = self._draft.get('phases') or []
        target = index + delta
        if target < 0 or target >= len(phases):
            return
        item = phases.pop(index)
        phases.insert(target, item)
        self._render()

    def _delete_phase(self, index: int) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        phases = self._draft.get('phases') or []
        if len(phases) <= 1:
            self._set_status('Keep at least one phase', ok=False)
            return
        if 0 <= index < len(phases):
            phases.pop(index)
        self._render()

    def _add_timeline(self, phase_index: int) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        phases = self._draft.get('phases') or []
        if 0 <= phase_index < len(phases):
            phases[phase_index].setdefault('timelines', []).append(
                make_default_timeline())
        self._render()

    def _delete_timeline(self, phase_index: int, timeline_index: int) -> None:
        if not self._draft:
            return
        self._sync_draft_from_widgets()
        phases = self._draft.get('phases') or []
        if 0 <= phase_index < len(phases):
            timelines = phases[phase_index].get('timelines') or []
            if 0 <= timeline_index < len(timelines):
                timelines.pop(timeline_index)
        self._render()
