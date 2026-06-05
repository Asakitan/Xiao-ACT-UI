# -*- coding: utf-8 -*-
"""Reusable SAO-style Tk components for Entity panels."""

from __future__ import annotations

import tkinter as tk
from typing import Any, Callable, Iterable, Mapping, Optional

from gui_modules import sao_panel_ui as ui


PAD_X = 12
PAD_Y = 8
FONT_SMALL = ('Segoe UI', 8)
FONT_BODY = ('Segoe UI', 9)
FONT_BODY_BOLD = ('Segoe UI', 9, 'bold')
FONT_VALUE = ('Segoe UI', 13, 'bold')
FONT_TITLE = ('Segoe UI', 10, 'bold')


def _accent(kind: str = "gold") -> str:
    kind = str(kind or "gold").lower()
    if kind in {"cyan", "accent", "info"}:
        return ui._SAO_PANEL_ACCENT
    if kind in {"ok", "good", "heal"}:
        return ui._SAO_PANEL_PALETTES[ui._SAO_PANEL_THEME].get('ok', '#7df2bf')
    if kind in {"danger", "bad", "error"}:
        return ui._SAO_PANEL_PALETTES[ui._SAO_PANEL_THEME].get('danger', '#ff707a')
    return ui._SAO_PANEL_GOLD


def status_badge(parent: tk.Misc, text: str, *, kind: str = "gold") -> tk.Label:
    fg = _accent(kind)
    bg = ui._SAO_PANEL_HEADER_BG if kind not in {"danger", "error"} else ui._SAO_PANEL_PALETTES[ui._SAO_PANEL_THEME].get('danger_soft', ui._SAO_PANEL_HEADER_BG)
    label = tk.Label(parent, text=str(text or "-"), bg=bg, fg=fg, font=FONT_SMALL, padx=8, pady=2, highlightthickness=1, highlightbackground=fg)
    return label


def action_button(parent: tk.Misc, text: str, command: Optional[Callable[[], Any]] = None, *, kind: str = "normal") -> tk.Button:
    bg = ui._SAO_PANEL_HEADER_BG
    fg = ui._SAO_PANEL_HEADER_FG
    active = _accent(kind)
    return tk.Button(
        parent,
        text=text,
        command=command,
        bg=bg,
        fg=fg,
        activebackground=active,
        activeforeground=ui._SAO_PANEL_PALETTES[ui._SAO_PANEL_THEME].get('active_fg', 'white'),
        relief='flat',
        bd=0,
        padx=10,
        pady=5,
        font=FONT_BODY_BOLD,
        cursor='hand2' if callable(command) else '',
    )


def metric_tile(parent: tk.Misc, label: str, value: Any, *, sub: str = "", accent: str = "gold") -> tk.Frame:
    color = _accent(accent)
    card = tk.Frame(parent, bg=ui._SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=ui._SAO_PANEL_BORDER)
    tk.Frame(card, bg=color, height=3).pack(fill='x')
    tk.Label(card, text=str(label or "-"), bg=ui._SAO_PANEL_BODY_BG, fg=ui._SAO_PANEL_LABEL_FG, font=FONT_SMALL).pack(anchor='w', padx=10, pady=(7, 0))
    tk.Label(card, text=str(value or "0"), bg=ui._SAO_PANEL_BODY_BG, fg=ui._SAO_PANEL_VALUE_FG, font=FONT_VALUE).pack(anchor='w', padx=10, pady=(1, 0))
    if sub:
        tk.Label(card, text=str(sub), bg=ui._SAO_PANEL_BODY_BG, fg=color, font=FONT_SMALL).pack(anchor='w', padx=10, pady=(0, 7))
    else:
        tk.Frame(card, bg=ui._SAO_PANEL_BODY_BG, height=7).pack(fill='x')
    return card


def section_card(parent: tk.Misc, title: str, *, subtitle: str = "", badge: str = "") -> tk.Frame:
    box = tk.Frame(parent, bg=ui._SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=ui._SAO_PANEL_BORDER)
    head = tk.Frame(box, bg=ui._SAO_PANEL_HEADER_BG)
    head.pack(fill='x')
    tk.Frame(head, bg=ui._SAO_PANEL_ACCENT, width=4).pack(side='left', fill='y')
    text_box = tk.Frame(head, bg=ui._SAO_PANEL_HEADER_BG)
    text_box.pack(side='left', fill='x', expand=True, padx=(8, 4), pady=6)
    tk.Label(text_box, text=str(title or "SECTION"), bg=ui._SAO_PANEL_HEADER_BG, fg=ui._SAO_PANEL_GOLD, font=FONT_TITLE, anchor='w').pack(fill='x')
    if subtitle:
        tk.Label(text_box, text=str(subtitle), bg=ui._SAO_PANEL_HEADER_BG, fg=ui._SAO_PANEL_LABEL_FG, font=FONT_SMALL, anchor='w').pack(fill='x')
    if badge:
        status_badge(head, badge, kind='cyan').pack(side='right', padx=8, pady=6)
    return box


def aggregate_row(parent: tk.Misc, *, title: str, meta: str = "", value: str = "", ratio: float = 0.0,
                  accent: str = "gold", zebra: bool = False, command: Optional[Callable[[], Any]] = None) -> tk.Frame:
    color = _accent(accent)
    bg = ui._SAO_PANEL_HEADER_BG if zebra else ui._SAO_PANEL_BODY_BG
    row = tk.Frame(parent, bg=bg, highlightthickness=1, highlightbackground=ui._SAO_PANEL_SEP, cursor='hand2' if callable(command) else '')
    bar_bg = tk.Frame(row, bg=bg)
    bar_bg.pack(fill='x')
    bar_width = max(3, min(280, int(280 * max(0.0, min(1.0, float(ratio or 0.0))))))
    tk.Frame(bar_bg, bg=color, width=bar_width, height=3).pack(anchor='w')
    body = tk.Frame(row, bg=bg)
    body.pack(fill='x', padx=8, pady=5)
    left = tk.Frame(body, bg=bg)
    left.pack(side='left', fill='x', expand=True)
    tk.Label(left, text=str(title or '-'), bg=bg, fg=ui._SAO_PANEL_VALUE_FG, font=FONT_BODY_BOLD, anchor='w').pack(fill='x')
    if meta:
        tk.Label(left, text=str(meta), bg=bg, fg=ui._SAO_PANEL_LABEL_FG, font=FONT_SMALL, anchor='w').pack(fill='x')
    if value:
        tk.Label(body, text=str(value), bg=bg, fg=color, font=FONT_BODY_BOLD, anchor='e').pack(side='right', padx=(8, 0))
    if callable(command):
        def _bind_recursive(widget: tk.Misc) -> None:
            try:
                widget.bind('<Button-1>', lambda _e: command())
                widget.configure(cursor='hand2')
            except Exception:
                pass
            try:
                children = widget.winfo_children()
            except Exception:
                children = []
            for child in children:
                _bind_recursive(child)

        _bind_recursive(row)
    return row


def empty_state(parent: tk.Misc, title: str, detail: str = "") -> tk.Frame:
    box = tk.Frame(parent, bg=ui._SAO_PANEL_BODY_BG)
    tk.Label(box, text=str(title or "No data"), bg=ui._SAO_PANEL_BODY_BG, fg=ui._SAO_PANEL_GOLD, font=FONT_TITLE, pady=12).pack(fill='x')
    if detail:
        tk.Label(box, text=str(detail), bg=ui._SAO_PANEL_BODY_BG, fg=ui._SAO_PANEL_LABEL_FG, font=FONT_BODY, wraplength=720, justify='center').pack(fill='x', padx=12, pady=(0, 12))
    return box


def source_badges(parent: tk.Misc, sources: Iterable[Mapping[str, Any]]) -> tk.Frame:
    frame = tk.Frame(parent, bg=ui._SAO_PANEL_BODY_BG)
    for item in sources or []:
        if not isinstance(item, Mapping):
            continue
        text = f"{item.get('source') or '-'} · {int(item.get('count') or 0)}"
        status_badge(frame, text, kind='cyan').pack(side='left', padx=(0, 6), pady=2)
    return frame
