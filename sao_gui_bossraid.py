from __future__ import annotations

import tkinter as tk
from typing import Any, Callable, Dict, Optional, Tuple

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
    LINE,
    apply_surface_chrome,
    apply_badge,
    attach_tab_underline,
    bind_drag,
    calc_panel_geometry,
    clear_frame,
    create_scrollable_area,
    make_action_button,
    make_section_title,
    make_tab_label,
    panel_font,
    place_corner_accents,
    set_tab_active,
)


def _fmt_num(value: Any) -> str:
    try:
        num = float(value or 0)
    except Exception:
        return '0'
    if num >= 1_000_000_000:
        return f'{num / 1_000_000_000:.1f}B'
    if num >= 1_000_000:
        return f'{num / 1_000_000:.1f}M'
    if num >= 1_000:
        return f'{num / 1_000:.1f}K'
    return str(int(round(num)))


def _fmt_time(seconds: Any) -> str:
    try:
        total = max(0, int(float(seconds or 0)))
    except Exception:
        total = 0
    return f'{total // 60}:{total % 60:02d}'


def _trigger_text(trigger: Optional[Dict[str, Any]]) -> str:
    if not isinstance(trigger, dict):
        return 'Manual'
    trigger_type = str(trigger.get('type') or 'manual')
    value = trigger.get('value') or 0
    if trigger_type == 'manual':
        return 'Manual (F8)'
    if trigger_type == 'time':
        return f'{int(float(value or 0))}s elapsed'
    if trigger_type == 'hp_pct':
        return f'HP ≤ {int(float(value or 0))}%'
    if trigger_type == 'dps_total':
        return f'DMG ≥ {_fmt_num(value)}'
    if trigger_type == 'breaking':
        return 'Break event'
    if trigger_type == 'shield_broken':
        return 'Shield broken'
    if trigger_type == 'overdrive':
        return 'Overdrive'
    if trigger_type == 'extinction_pct':
        return f'Break bar ≥ {int(float(value or 0))}%'
    if trigger_type == 'breaking_stage':
        return f'Break stage ≥ {int(float(value or 0))}'
    return f'{trigger_type}: {value}'


class BossRaidPanel:
    def __init__(
        self,
        master: tk.Tk,
        load_fn: Callable[[], dict],
        save_fn: Callable[[dict], Any],
        engine_ref: Callable[[], Any],
        on_toggle: Callable[[bool], None],
        on_start: Callable[[], None],
        on_next: Callable[[], None],
        on_reset: Optional[Callable[[], None]] = None,
    ):
        self._master = master
        self._load = load_fn
        self._save = save_fn
        self._engine_ref = engine_ref
        self._on_toggle = on_toggle
        self._on_start = on_start
        self._on_next = on_next
        self._on_reset = on_reset or (lambda: None)
        self._win: Optional[tk.Toplevel] = None
        self._visible = False
        self._cfg: Dict[str, Any] = {}
        self._status: Dict[str, Any] = {}
        self._current_tab = 'entities'
        self._drag_ox = 0
        self._drag_oy = 0
        self._poll_after_id: Optional[str] = None
        self._tab_labels: Dict[str, tk.Label] = {}
        self._content_body: Optional[tk.Frame] = None
        self._canvas: Optional[tk.Canvas] = None
        self._badge: Optional[tk.Label] = None
        self._status_elapsed: Optional[tk.Label] = None
        self._status_dps: Optional[tk.Label] = None
        self._status_phase: Optional[tk.Label] = None
        self._last_render_signature: Optional[Tuple[Any, ...]] = None

    def is_visible(self) -> bool:
        return bool(self._visible and self._win and self._win.winfo_exists())

    def show(self) -> None:
        self._cfg = self._load()
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
            min_w=360,
            min_h=460,
            width_ratio=0.22,
            height_ratio=0.52,
            x_ratio=0.012,
            y_ratio=0.18,
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
            text='Live Raid Editor',
            bg=PANEL_HEADER,
            fg=TEXT_MUTED,
            font=panel_font(8),
        ).pack(anchor='w', padx=16, pady=(10, 0))

        title_row = tk.Frame(header, bg=PANEL_HEADER)
        title_row.pack(fill=tk.X, padx=16, pady=(2, 8))
        tk.Label(
            title_row,
            text='Boss Raid',
            bg=PANEL_HEADER,
            fg=TEXT_MAIN,
            font=panel_font(13, bold=True),
        ).pack(side=tk.LEFT)
        self._badge = tk.Label(
            title_row,
            text='IDLE',
            bg=TEXT_MUTED,
            fg='#ffffff',
            font=panel_font(8, bold=True),
            padx=8,
            pady=2,
        )
        self._badge.pack(side=tk.LEFT, padx=(10, 0))
        bind_drag(header, self._on_drag_start, self._on_drag_move)

        tk.Frame(shell, bg=LINE, height=1).pack(fill=tk.X)

        tab_bar = tk.Frame(shell, bg=PANEL_BG_ALT, height=34)
        tab_bar.pack(fill=tk.X)
        tab_bar.pack_propagate(False)
        for key, text in (
            ('entities', 'Entities'),
            ('phases', 'Phases'),
            ('timeline', 'Timeline'),
        ):
            slot = tk.Frame(tab_bar, bg=PANEL_BG_ALT)
            slot.pack(side=tk.LEFT, expand=True, fill=tk.BOTH)
            label = make_tab_label(slot, text, command=lambda tab=key: self._switch_tab(tab))
            label.pack(fill=tk.X, expand=True)
            attach_tab_underline(label, slot)
            self._tab_labels[key] = label
        self._refresh_tabs()

        content = tk.Frame(shell, bg=PANEL_BG, padx=12, pady=8)
        content.pack(fill=tk.BOTH, expand=True)
        _, canvas, body = create_scrollable_area(content, PANEL_BG)
        self._canvas = canvas
        self._content_body = body

        action_row = tk.Frame(shell, bg=PANEL_BG, padx=12, pady=8)
        action_row.pack(fill=tk.X)
        make_action_button(action_row, 'NEXT PHASE', self._handle_next_phase, kind='accent').pack(side=tk.LEFT, fill=tk.X, expand=True)
        make_action_button(action_row, 'RESET', self._handle_reset).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(8, 0))

        tk.Frame(shell, bg=PANEL_EDGE, height=1).pack(fill=tk.X)

        status_bar = tk.Frame(shell, bg=PANEL_BG, padx=14, pady=5)
        status_bar.pack(fill=tk.X)
        self._status_elapsed = tk.Label(status_bar, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(8, bold=True))
        self._status_elapsed.pack(side=tk.LEFT)
        self._status_dps = tk.Label(status_bar, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(8, bold=True))
        self._status_dps.pack(side=tk.LEFT, expand=True)
        self._status_phase = tk.Label(status_bar, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(8, bold=True))
        self._status_phase.pack(side=tk.RIGHT)

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
        self._poll_after_id = self._win.after(250, self._refresh_live)

    def _refresh_live(self, force: bool = False) -> None:
        if self._win is None or not self._visible:
            return
        try:
            self._cfg = self._load()
        except Exception:
            pass
        engine = self._engine_ref() if self._engine_ref else None
        if engine and hasattr(engine, 'get_status'):
            try:
                self._status = engine.get_status() or {}
            except Exception:
                self._status = {}
        else:
            self._status = {}
        self._update_header_status()
        self._render_if_needed(force=force)
        self._schedule_poll()

    def _update_header_status(self) -> None:
        state = str(self._status.get('state') or 'idle').lower()
        if self._badge is not None:
            if state == 'running':
                apply_badge(self._badge, 'RUNNING', 'running')
            elif state == 'completed':
                apply_badge(self._badge, 'DONE', 'active')
            else:
                apply_badge(self._badge, 'IDLE', 'off')
        if self._status_elapsed is not None:
            self._status_elapsed.configure(text=_fmt_time(self._status.get('elapsed_s') or 0))
        if self._status_dps is not None:
            self._status_dps.configure(text=f'{_fmt_num(self._status.get("dps") or 0)} DPS')
        if self._status_phase is not None:
            phase_name = self._status.get('phase_name') or f'P{int(self._status.get("phase_idx") or 0) + 1}'
            self._status_phase.configure(text=str(phase_name))

    def _render_if_needed(self, force: bool = False) -> None:
        if self._content_body is None:
            return
        signature = self._build_signature()
        if not force and signature == self._last_render_signature:
            return
        self._last_render_signature = signature
        clear_frame(self._content_body)
        if self._current_tab == 'entities':
            self._render_entities_tab()
        elif self._current_tab == 'phases':
            self._render_phases_tab()
        else:
            self._render_timeline_tab()
        try:
            self._canvas.configure(scrollregion=self._canvas.bbox('all'))
        except Exception:
            pass

    def _build_signature(self) -> Tuple[Any, ...]:
        profile = self._active_profile()
        phases = list((profile or {}).get('phases') or [])
        entities = list(self._status.get('entities') or [])
        entity_sig = tuple(
            (
                int(item.get('uuid') or 0),
                str(item.get('role') or ''),
                round(float(item.get('hp_pct') or 0.0), 3),
                int(item.get('damage_dealt') or 0),
                bool(item.get('shield_active')),
                int(item.get('breaking_stage') or 0),
                bool(item.get('in_overdrive')),
            )
            for item in entities
        )
        phase_sig = tuple(
            (
                str(phase.get('name') or ''),
                str((phase.get('trigger') or {}).get('type') or ''),
                (phase.get('trigger') or {}).get('value') or 0,
                tuple(
                    (
                        round(float(timeline.get('time_s') or 0.0), 1),
                        str(timeline.get('label') or ''),
                        str(timeline.get('alert_type') or ''),
                    )
                    for timeline in list(phase.get('timelines') or [])
                ),
            )
            for phase in phases
        )
        return (
            self._current_tab,
            str(self._status.get('state') or ''),
            int(self._status.get('phase_idx') or 0),
            round(float(self._status.get('elapsed_s') or 0.0), 1),
            int(self._status.get('dps') or 0),
            entity_sig,
            phase_sig,
        )

    def _active_profile(self) -> Optional[Dict[str, Any]]:
        profiles = list(self._cfg.get('profiles') or [])
        active_id = self._cfg.get('active_profile_id')
        for profile in profiles:
            if profile.get('id') == active_id:
                return profile
        return profiles[0] if profiles else None

    def _switch_tab(self, tab: str) -> None:
        if tab == self._current_tab:
            return
        self._current_tab = tab
        self._refresh_tabs()
        self._render_if_needed(force=True)

    def _refresh_tabs(self) -> None:
        for key, label in self._tab_labels.items():
            set_tab_active(label, key == self._current_tab)

    def _render_entities_tab(self) -> None:
        entities = list(self._status.get('entities') or [])
        if not entities:
            self._render_empty('Waiting for combat data...', 'Attack a monster to begin tracking')
            return
        container = tk.Frame(self._content_body, bg=PANEL_BG)
        container.pack(fill=tk.X)
        for idx, entity in enumerate(entities, start=1):
            role = str(entity.get('role') or 'enemy')
            is_boss = role == 'boss'
            card = tk.Frame(
                container,
                bg=PANEL_CARD,
                highlightbackground=PANEL_EDGE,
                highlightthickness=1,
                padx=10,
                pady=8,
            )
            card.pack(fill=tk.X, pady=(0, 6))
            apply_surface_chrome(card, accent=DANGER if is_boss else GOLD)
            tk.Frame(card, bg=DANGER if is_boss else GOLD, width=3, height=52).pack(side=tk.LEFT, fill=tk.Y, padx=(0, 10))
            rank = tk.Label(
                card,
                text=str(idx),
                bg=DANGER if is_boss else GOLD,
                fg='#ffffff',
                width=2,
                font=panel_font(9, bold=True),
            )
            rank.pack(side=tk.LEFT, padx=(0, 10))

            info = tk.Frame(card, bg=PANEL_CARD)
            info.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
            name = str(entity.get('name') or f'Entity {int(entity.get("uuid") or 0) & 0xFFFF}')
            tk.Label(info, text=name, bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(10, bold=True), anchor='w').pack(fill=tk.X)
            hp_pct = int(round(float(entity.get('hp_pct') or 0.0) * 100.0))
            parts = [f'DMG: {_fmt_num(entity.get("damage_dealt") or 0)}', f'HP: {hp_pct}%']
            if entity.get('shield_active'):
                parts.append('Shield')
            if entity.get('in_overdrive'):
                parts.append('OD')
            if int(entity.get('breaking_stage') or 0) > 0:
                parts.append('Break')
            tk.Label(info, text=' · '.join(parts), bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(8), anchor='w').pack(fill=tk.X, pady=(1, 0))

            bar_bg = tk.Frame(info, bg=PANEL_CARD_ALT, height=4)
            bar_bg.pack(fill=tk.X, pady=(4, 0))
            fill_width = max(0, min(100, hp_pct))
            bar_fill = tk.Frame(bar_bg, bg=READY, height=4)
            bar_fill.place(relwidth=fill_width / 100.0, relheight=1.0)

            role_text = 'BOSS' if is_boss else 'ENEMY'
            role_bg = DANGER if is_boss else GOLD
            role_btn = tk.Label(
                card,
                text=role_text,
                bg=PANEL_CARD,
                fg=TEXT_MAIN,
                font=panel_font(8, bold=True),
                padx=10,
                pady=4,
                cursor='hand2',
            )
            role_btn.pack(side=tk.RIGHT)
            apply_badge(role_btn, role_text, 'running' if is_boss else 'active')
            role_btn.bind(
                '<Button-1>',
                lambda _event, uuid=int(entity.get('uuid') or 0), current=role: self._toggle_role(uuid, current),
            )

    def _render_phases_tab(self) -> None:
        make_section_title(self._content_body, 'Raid Phases')
        profile = self._active_profile()
        phases = list((profile or {}).get('phases') or [])
        if not phases:
            self._render_empty('No phases configured', 'Create or download a raid profile first')
            return
        current_idx = int(self._status.get('phase_idx') or 0)
        for idx, phase in enumerate(phases):
            current = idx == current_idx
            bg = PANEL_CARD_ALT if current else PANEL_CARD
            border = GOLD if current else PANEL_EDGE
            card = tk.Frame(self._content_body, bg=bg, highlightbackground=border, highlightthickness=1, padx=12, pady=8)
            card.pack(fill=tk.X, pady=(0, 5))
            apply_surface_chrome(card, accent=GOLD if current else CYAN)
            tk.Label(card, text=str(phase.get('name') or f'P{idx + 1}'), bg=bg, fg=TEXT_MAIN, font=panel_font(10, bold=True)).pack(side=tk.LEFT, fill=tk.X, expand=True)
            tk.Label(card, text=_trigger_text(phase.get('trigger')), bg=bg, fg=TEXT_MUTED, font=panel_font(8)).pack(side=tk.LEFT, padx=(8, 8))
            if current:
                make_action_button(card, '→', self._handle_next_phase, kind='accent', width=2).pack(side=tk.RIGHT)

    def _render_timeline_tab(self) -> None:
        profile = self._active_profile()
        phases = list((profile or {}).get('phases') or [])
        current_idx = int(self._status.get('phase_idx') or 0)
        current_phase = phases[current_idx] if 0 <= current_idx < len(phases) else None
        timelines = list((current_phase or {}).get('timelines') or [])
        if not timelines:
            self._render_empty('Phase timeline alerts will appear here during combat.', '')
            return
        title = str((current_phase or {}).get('name') or f'P{current_idx + 1}')
        make_section_title(self._content_body, f'{title} Timeline')
        for idx, timeline in enumerate(timelines, start=1):
            row = tk.Frame(self._content_body, bg=PANEL_CARD, highlightbackground=PANEL_EDGE, highlightthickness=1, padx=10, pady=6)
            row.pack(fill=tk.X, pady=(0, 4))
            apply_surface_chrome(row, accent=CYAN)
            tk.Label(row, text=f'{idx}.', bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(8, bold=True), width=3).pack(side=tk.LEFT)
            tk.Label(row, text=f'{round(float(timeline.get("time_s") or 0.0), 1)}s', bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(9, bold=True), width=6).pack(side=tk.LEFT)
            tk.Label(row, text=str(timeline.get('label') or 'Alert'), bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(9), anchor='w').pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(6, 0))
            tk.Label(row, text=str(timeline.get('alert_type') or 'both').upper(), bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(8)).pack(side=tk.RIGHT)

    def _render_empty(self, title: str, subtitle: str) -> None:
        wrap = tk.Frame(self._content_body, bg=PANEL_BG)
        wrap.pack(fill=tk.BOTH, expand=True, pady=26)
        tk.Label(wrap, text=title, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(10), justify='center').pack()
        if subtitle:
            tk.Label(wrap, text=subtitle, bg=PANEL_BG, fg=TEXT_DIM, font=panel_font(8), justify='center').pack(pady=(4, 0))

    def _toggle_role(self, uuid: int, current_role: str) -> None:
        engine = self._engine_ref() if self._engine_ref else None
        if not engine or not hasattr(engine, 'set_entity_role'):
            return
        next_role = 'enemy' if current_role == 'boss' else 'boss'
        try:
            engine.set_entity_role(int(uuid), next_role)
        except Exception:
            pass
        self._refresh_live(force=True)

    def _handle_next_phase(self) -> None:
        try:
            self._on_next()
        except Exception:
            pass
        self._refresh_live(force=True)

    def _handle_reset(self) -> None:
        try:
            self._on_reset()
        except Exception:
            pass
        self._refresh_live(force=True)

    def _on_drag_start(self, event) -> None:
        if self._win is None:
            return
        self._drag_ox = event.x_root - self._win.winfo_x()
        self._drag_oy = event.y_root - self._win.winfo_y()

    def _on_drag_move(self, event) -> None:
        if self._win is None:
            return
        self._win.geometry(f'+{event.x_root - self._drag_ox}+{event.y_root - self._drag_oy}')