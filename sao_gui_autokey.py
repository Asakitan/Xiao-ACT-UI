from __future__ import annotations

import time
import tkinter as tk
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
    DANGER,
    READY,
    ACTIVE,
    COOLDOWN,
    CYAN,
    apply_surface_chrome,
    apply_badge,
    bind_drag,
    calc_panel_geometry,
    clear_frame,
    create_scrollable_area,
    make_action_button,
    make_section_title,
    panel_font,
    place_corner_accents,
    set_action_button_kind,
)


class AutoKeyPanel:
    def __init__(
        self,
        master: tk.Tk,
        load_fn: Callable[[], dict],
        save_fn: Callable[[dict], Any],
        engine_ref: Callable[[], Any],
        on_toggle: Callable[[bool], None],
        author_fn: Optional[Callable[[], dict]] = None,
        load_burst_actions: Optional[Callable[[], List[dict]]] = None,
        save_burst_actions: Optional[Callable[[List[dict]], None]] = None,
    ):
        self._master = master
        self._load = load_fn
        self._save = save_fn
        self._engine_ref = engine_ref
        self._on_toggle = on_toggle
        self._author_fn = author_fn or (lambda: {})
        self._load_burst_actions = load_burst_actions
        self._save_burst_actions = save_burst_actions

        self._win: Optional[tk.Toplevel] = None
        self._visible = False
        self._cfg: Dict[str, Any] = {}
        self._slots: List[Dict[str, Any]] = []
        self._actions: List[Dict[str, Any]] = []
        self._recording = False
        self._recording_trigger_slot: Optional[int] = None
        self._selected_slot: Optional[int] = None
        self._burst_ready = False
        self._profession = ''
        self._drag_ox = 0
        self._drag_oy = 0
        self._poll_after_id: Optional[str] = None
        self._slot_prev_states: Dict[int, str] = {}
        self._ready_flash_until: Dict[int, float] = {}
        self._last_signature: Optional[Tuple[Any, ...]] = None

        self._badge: Optional[tk.Label] = None
        self._burst_indicator: Optional[tk.Label] = None
        self._content_body: Optional[tk.Frame] = None
        self._rec_indicator: Optional[tk.Label] = None
        self._record_btn: Optional[tk.Label] = None
        self._status_profession: Optional[tk.Label] = None
        self._status_burst: Optional[tk.Label] = None
        self._canvas: Optional[tk.Canvas] = None

    def is_visible(self) -> bool:
        return bool(self._visible and self._win and self._win.winfo_exists())

    def show(self) -> None:
        self._cfg = self._load()
        self._actions = self._load_actions()
        if self._win is None or not self._win.winfo_exists():
            self._build()
        self._visible = True
        try:
            self._win.deiconify()
            self._win.lift()
            self._win.focus_force()
        except Exception:
            pass
        self._refresh_live(force=True)

    def hide(self) -> None:
        self._cancel_poll()
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
        self._cancel_poll()
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
            min_w=340,
            min_h=400,
            width_ratio=0.20,
            height_ratio=0.44,
            x_ratio=0.012,
            y_ratio=0.64,
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

        shell = tk.Frame(win, bg=PANEL_BG, highlightthickness=1, highlightbackground=PANEL_EDGE)
        shell.pack(fill=tk.BOTH, expand=True)
        apply_surface_chrome(shell, accent=CYAN, accent_side='top')
        place_corner_accents(shell)

        header = tk.Frame(shell, bg=PANEL_HEADER, height=58)
        header.pack(fill=tk.X)
        header.pack_propagate(False)
        apply_surface_chrome(header)

        tk.Label(
            header,
            text='Live Skill Monitor',
            bg=PANEL_HEADER,
            fg=TEXT_MUTED,
            font=panel_font(8),
        ).pack(anchor='w', padx=16, pady=(10, 0))

        title_row = tk.Frame(header, bg=PANEL_HEADER)
        title_row.pack(fill=tk.X, padx=16, pady=(2, 8))
        tk.Label(
            title_row,
            text='Auto Key',
            bg=PANEL_HEADER,
            fg=TEXT_MAIN,
            font=panel_font(13, bold=True),
        ).pack(side=tk.LEFT)
        self._badge = tk.Label(
            title_row,
            text='OFF',
            bg=TEXT_MUTED,
            fg='#ffffff',
            font=panel_font(8, bold=True),
            padx=8,
            pady=2,
        )
        self._badge.pack(side=tk.LEFT, padx=(10, 0))
        bind_drag(header, self._on_drag_start, self._on_drag_move)

        self._burst_indicator = tk.Label(
            shell,
            text='✦ BURST READY ✦',
            bg=PANEL_CARD_ALT,
            fg=READY,
            font=panel_font(9, bold=True),
            padx=12,
            pady=4,
            highlightthickness=1,
            highlightbackground=READY,
        )

        content = tk.Frame(shell, bg=PANEL_BG, padx=12, pady=8)
        content.pack(fill=tk.BOTH, expand=True)
        _, canvas, body = create_scrollable_area(content, PANEL_BG)
        self._canvas = canvas
        self._content_body = body

        action_row = tk.Frame(shell, bg=PANEL_BG, padx=12, pady=8)
        action_row.pack(fill=tk.X)
        self._record_btn = make_action_button(action_row, 'RECORD', self._toggle_record)
        self._record_btn.pack(side=tk.LEFT, fill=tk.X, expand=True)
        make_action_button(action_row, 'CLEAR', self._clear_actions).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(8, 0))
        make_action_button(action_row, 'SAVE', self._save_actions, kind='accent').pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(8, 0))

        tk.Frame(shell, bg=PANEL_EDGE, height=1).pack(fill=tk.X)
        status_bar = tk.Frame(shell, bg=PANEL_BG, padx=14, pady=5)
        status_bar.pack(fill=tk.X)
        self._status_profession = tk.Label(status_bar, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(8, bold=True))
        self._status_profession.pack(side=tk.LEFT)
        self._status_burst = tk.Label(status_bar, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(8, bold=True))
        self._status_burst.pack(side=tk.RIGHT)

    def _cancel_poll(self) -> None:
        if self._poll_after_id and self._win is not None:
            try:
                self._win.after_cancel(self._poll_after_id)
            except Exception:
                pass
        self._poll_after_id = None

    def _schedule_poll(self) -> None:
        self._cancel_poll()
        if self._win is None or not self._visible:
            return
        self._poll_after_id = self._win.after(180, self._refresh_live)

    def _refresh_live(self, force: bool = False) -> None:
        if self._win is None or not self._visible:
            return
        try:
            self._cfg = self._load()
        except Exception:
            pass
        engine = self._engine_ref() if self._engine_ref else None
        gs = getattr(getattr(engine, '_state_mgr', None), 'state', None)
        self._burst_ready = bool(getattr(gs, 'burst_ready', False)) if gs is not None else False
        self._profession = str(getattr(gs, 'profession_name', '') or getattr(gs, 'profession', '') or '')
        if not self._profession:
            try:
                self._profession = str((self._author_fn() or {}).get('profession_name') or '')
            except Exception:
                self._profession = ''
        self._slots = self._normalize_slots(getattr(gs, 'skill_slots', []) if gs is not None else [])
        self._update_ready_transitions()
        self._update_header_status()
        self._render_if_needed(force=force)
        self._schedule_poll()

    def _load_actions(self) -> List[Dict[str, Any]]:
        if self._load_burst_actions is not None:
            try:
                return list(self._load_burst_actions() or [])
            except Exception:
                pass
        engine = self._engine_ref() if self._engine_ref else None
        if engine and hasattr(engine, 'get_burst_actions'):
            try:
                return list(engine.get_burst_actions() or [])
            except Exception:
                pass
        return []

    def _normalize_slots(self, slots: Any) -> List[Dict[str, Any]]:
        result: List[Dict[str, Any]] = []
        for raw in list(slots or []):
            if not isinstance(raw, dict):
                continue
            index = int(raw.get('index') or raw.get('slot_index') or 0)
            if index <= 0:
                continue
            result.append({
                'index': index,
                'name': raw.get('skill_name') or raw.get('name') or f'Slot {index}',
                'state': str(raw.get('state') or 'ready').lower(),
                'cooldown_pct': float(raw.get('cooldown_pct') or 0.0),
                'remaining_ms': float(raw.get('remaining_ms') or 0.0),
                'charge_count': int(raw.get('charge_count') or 0),
            })
        result.sort(key=lambda item: item['index'])
        return result

    def _update_ready_transitions(self) -> None:
        now = time.time()
        seen = set()
        for slot in self._slots:
            index = int(slot['index'])
            state = str(slot.get('state') or 'ready')
            prev = self._slot_prev_states.get(index)
            if prev == 'cooldown' and state == 'ready':
                self._ready_flash_until[index] = now + 0.62
            self._slot_prev_states[index] = state
            seen.add(index)
        for index in list(self._slot_prev_states.keys()):
            if index not in seen:
                self._slot_prev_states.pop(index, None)
        for index, until in list(self._ready_flash_until.items()):
            if until <= now:
                self._ready_flash_until.pop(index, None)

    def _update_header_status(self) -> None:
        enabled = bool(self._cfg.get('enabled', False))
        if self._badge is not None:
            apply_badge(self._badge, 'ON' if enabled else 'OFF', 'on' if enabled else 'off')
        if self._burst_indicator is not None:
            if self._burst_ready:
                self._burst_indicator.pack(fill=tk.X)
            else:
                self._burst_indicator.pack_forget()
        if self._status_profession is not None:
            self._status_profession.configure(text=self._profession or '—')
        if self._status_burst is not None:
            self._status_burst.configure(text='Burst: Ready ✦' if self._burst_ready else 'Burst: Not Ready')
        if self._rec_indicator is not None and self._recording:
            self._rec_indicator.configure(
                text='● Slot selection in progress' if self._recording_trigger_slot is not None
                else '● RECORDING — click slot to set trigger condition'
            )

    def _render_if_needed(self, force: bool = False) -> None:
        if self._content_body is None:
            return
        signature = (
            bool(self._cfg.get('enabled', False)),
            self._burst_ready,
            self._profession,
            self._selected_slot,
            self._recording,
            self._recording_trigger_slot,
            tuple(
                (
                    int(slot['index']),
                    str(slot.get('name') or ''),
                    str(slot.get('state') or ''),
                    round(float(slot.get('cooldown_pct') or 0.0), 3),
                    int(float(slot.get('remaining_ms') or 0.0)),
                    int(slot.get('charge_count') or 0),
                    int(self._ready_flash_until.get(int(slot['index']), 0) > time.time()),
                )
                for slot in self._slots
            ),
            tuple(
                (
                    int(action.get('trigger_slot') or 0),
                    int(action.get('action_slot') or 0),
                    str(action.get('trigger_name') or ''),
                    str(action.get('action_name') or ''),
                )
                for action in self._actions
            ),
        )
        if not force and signature == self._last_signature:
            return
        self._last_signature = signature
        clear_frame(self._content_body)
        self._render_content()
        try:
            self._canvas.configure(scrollregion=self._canvas.bbox('all'))
        except Exception:
            pass

    def _render_content(self) -> None:
        make_section_title(self._content_body, 'Skill Slots')
        grid = tk.Frame(self._content_body, bg=PANEL_BG)
        grid.pack(fill=tk.X, pady=(0, 10))

        if not self._slots:
            tk.Label(grid, text='No skill slot data', bg=PANEL_BG, fg=TEXT_DIM, font=panel_font(9)).pack(pady=12)
        else:
            for idx, slot in enumerate(self._slots):
                row = idx // 3
                col = idx % 3
                card = self._make_skill_card(grid, slot)
                card.grid(row=row, column=col, padx=3, pady=3, sticky='nsew')
                grid.grid_columnconfigure(col, weight=1)

        make_section_title(self._content_body, 'Auto Actions')
        self._rec_indicator = tk.Label(
            self._content_body,
            bg=PANEL_BG,
            fg=DANGER,
            font=panel_font(8, bold=True),
            text='● Slot selection in progress' if self._recording_trigger_slot is not None
            else '● RECORDING — click slot to set trigger condition',
        )
        if self._recording:
            self._rec_indicator.pack(fill=tk.X, pady=(0, 6))

        action_list = tk.Frame(self._content_body, bg=PANEL_BG)
        action_list.pack(fill=tk.X)
        if not self._actions:
            row = tk.Frame(action_list, bg=PANEL_CARD, highlightbackground=PANEL_EDGE, highlightthickness=1, padx=10, pady=8)
            row.pack(fill=tk.X)
            apply_surface_chrome(row, accent=CYAN)
            tk.Label(row, text='No auto-key actions recorded', bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(9)).pack()
            return

        for index, action in enumerate(self._actions):
            row = tk.Frame(action_list, bg=PANEL_CARD, highlightbackground=PANEL_EDGE, highlightthickness=1, padx=10, pady=8)
            row.pack(fill=tk.X, pady=(0, 4))
            apply_surface_chrome(row, accent=GOLD)
            trigger_name = str(action.get('trigger_name') or f'Slot {int(action.get("trigger_slot") or 0)}')
            action_name = str(action.get('action_name') or f'Slot {int(action.get("action_slot") or 0)}')
            tk.Label(row, text=f'When {trigger_name} is Burst Ready', bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(9), anchor='w').pack(side=tk.LEFT, fill=tk.X, expand=True)
            tk.Label(row, text=f'→ Press {action_name}', bg=PANEL_CARD, fg=GOLD, font=panel_font(9, bold=True)).pack(side=tk.LEFT, padx=(8, 8))
            remove = tk.Label(row, text='×', bg=PANEL_CARD, fg=DANGER, font=panel_font(11, bold=True), cursor='hand2')
            remove.pack(side=tk.RIGHT)
            remove.bind('<Button-1>', lambda _event, idx=index: self._remove_action(idx))

    def _make_skill_card(self, parent: tk.Widget, slot: Dict[str, Any]) -> tk.Frame:
        state = str(slot.get('state') or 'ready')
        index = int(slot.get('index') or 0)
        selected = self._selected_slot == index
        flash = self._ready_flash_until.get(index, 0.0) > time.time()

        left_color = PANEL_EDGE
        if state == 'ready':
            left_color = READY
        elif state == 'active':
            left_color = ACTIVE
        elif state == 'cooldown':
            left_color = COOLDOWN

        frame = tk.Frame(
            parent,
            bg=PANEL_CARD_ALT if selected else PANEL_CARD,
            highlightbackground=ACTIVE if selected else PANEL_EDGE,
            highlightthickness=2 if flash else 1,
            padx=8,
            pady=8,
            cursor='hand2',
        )
        if flash:
            frame.configure(highlightbackground=READY)
        tk.Frame(frame, bg=left_color, width=3, height=64).place(x=0, y=0, relheight=1.0)
        apply_surface_chrome(frame, accent=left_color)

        slot_label = f'Slot {index}'
        charges = int(slot.get('charge_count') or 0)
        if charges > 0:
            slot_label += f' ×{charges}'
        tk.Label(frame, text=slot_label, bg=frame.cget('bg'), fg=TEXT_MUTED, font=panel_font(8)).pack(anchor='w')
        tk.Label(frame, text=str(slot.get('name') or f'Slot {index}'), bg=frame.cget('bg'), fg=TEXT_MAIN, font=panel_font(9, bold=True), anchor='w').pack(fill=tk.X, pady=(2, 0))

        state_row = tk.Frame(frame, bg=frame.cget('bg'))
        state_row.pack(fill=tk.X, pady=(4, 0))
        dot = tk.Frame(state_row, bg=left_color, width=6, height=6)
        dot.pack(side=tk.LEFT)
        dot.pack_propagate(False)
        remain_ms = float(slot.get('remaining_ms') or 0.0)
        if state == 'ready':
            state_text = 'Ready'
        elif state == 'active':
            state_text = 'Active'
        else:
            state_text = f'{remain_ms / 1000.0:.1f}s' if remain_ms > 0 else 'Cooldown'
        tk.Label(state_row, text=state_text, bg=frame.cget('bg'), fg=TEXT_MUTED, font=panel_font(8)).pack(side=tk.LEFT, padx=(4, 0))

        bar_bg = tk.Frame(frame, bg=PANEL_BG_ALT, height=3)
        bar_bg.pack(fill=tk.X, pady=(4, 0))
        fill = tk.Frame(bar_bg, bg=COOLDOWN, height=3)
        fill.place(relwidth=max(0.0, min(1.0, float(slot.get('cooldown_pct') or 0.0))), relheight=1.0)

        def _bind_all(widget: tk.Widget) -> None:
            widget.bind('<Button-1>', lambda _event, idx=index: self._on_slot_click(idx))
            for child in widget.winfo_children():
                _bind_all(child)

        _bind_all(frame)
        return frame

    def _on_slot_click(self, index: int) -> None:
        if self._recording:
            if self._recording_trigger_slot is None:
                self._recording_trigger_slot = index
            else:
                trigger_slot = self._find_slot(self._recording_trigger_slot)
                action_slot = self._find_slot(index)
                if trigger_slot and action_slot:
                    self._actions.append({
                        'trigger_slot': int(self._recording_trigger_slot),
                        'trigger_name': str(trigger_slot.get('name') or f'Slot {self._recording_trigger_slot}'),
                        'trigger_condition': 'burst_ready',
                        'action_slot': int(index),
                        'action_name': str(action_slot.get('name') or f'Slot {index}'),
                        'action_key': str(index),
                    })
                self._recording_trigger_slot = None
            self._render_if_needed(force=True)
            return
        self._selected_slot = None if self._selected_slot == index else index
        self._render_if_needed(force=True)

    def _find_slot(self, index: int) -> Optional[Dict[str, Any]]:
        for slot in self._slots:
            if int(slot.get('index') or 0) == int(index):
                return slot
        return None

    def _toggle_record(self) -> None:
        self._recording = not self._recording
        self._recording_trigger_slot = None
        if self._record_btn is not None:
            if self._recording:
                set_action_button_kind(self._record_btn, 'danger', text='■ STOP')
            else:
                set_action_button_kind(self._record_btn, 'default', text='RECORD')
        self._render_if_needed(force=True)

    def _remove_action(self, index: int) -> None:
        if 0 <= index < len(self._actions):
            self._actions.pop(index)
            self._render_if_needed(force=True)

    def _clear_actions(self) -> None:
        self._actions = []
        self._render_if_needed(force=True)

    def _save_actions(self) -> None:
        if self._save_burst_actions is not None:
            try:
                self._save_burst_actions(list(self._actions))
            except Exception:
                pass
        else:
            engine = self._engine_ref() if self._engine_ref else None
            if engine and hasattr(engine, 'set_burst_actions'):
                try:
                    engine.set_burst_actions(list(self._actions))
                except Exception:
                    pass

    def _on_drag_start(self, event) -> None:
        if self._win is None:
            return
        self._drag_ox = event.x_root - self._win.winfo_x()
        self._drag_oy = event.y_root - self._win.winfo_y()

    def _on_drag_move(self, event) -> None:
        if self._win is None:
            return
        self._win.geometry(f'+{event.x_root - self._drag_ox}+{event.y_root - self._drag_oy}')