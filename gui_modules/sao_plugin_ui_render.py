# -*- coding: utf-8 -*-
"""Entity (Tk) renderer for the declarative plugin UI spec.

This is the Tk twin of ``web/plugin_layer.js``: both consume the same
normalized spec from :mod:`act_platform.ui_spec` so a plugin draws identically
on the Entity overlay and the WebView windows.

Public surface:
    render_spec_into(parent, spec, on_action=None)  -> render a spec tree
    PluginPanelList(parent, owner)                  -> auto-redrawing panel list
    attach_overlay_layer(toplevel, surface, owner)  -> plugin overlay on a window
"""

from __future__ import annotations

import tkinter as tk
from typing import Any, Callable, Mapping, Optional

import gui_modules.sao_panel_ui as _theme

# Status colors are theme-independent enough to read on both light and dark.
_OK = "#2bb673"
_WARN = "#d79b06"
_BAD = "#e0455a"
_MONO = ("Consolas", 9)


def _pal() -> dict:
    """Resolve the live SAO panel palette (respects runtime theme switches)."""
    g = lambda name, fallback: getattr(_theme, name, fallback)
    return {
        "body": g("_SAO_PANEL_BODY_BG", "#f6f7f7"),
        "border": g("_SAO_PANEL_BORDER", "#babec4"),
        "sep": g("_SAO_PANEL_SEP", "#d8dde2"),
        "label": g("_SAO_PANEL_LABEL_FG", "#8c878a"),
        "value": g("_SAO_PANEL_VALUE_FG", "#646364"),
        "gold": g("_SAO_PANEL_GOLD", "#dea620"),
        "accent": g("_SAO_PANEL_ACCENT", "#68e4ff"),
        "header_bg": g("_SAO_PANEL_HEADER_BG", "#fcfcfc"),
        "header_fg": g("_SAO_PANEL_HEADER_FG", "#646364"),
    }


def _text_color(style: str, pal: dict) -> str:
    return {
        "title": pal["gold"],
        "subtitle": pal["accent"],
        "value": pal["value"],
        "label": pal["label"],
        "muted": pal["label"],
        "ok": _OK,
        "warn": _WARN,
        "bad": _BAD,
        "gold": pal["gold"],
        "accent": pal["accent"],
        "mono": pal["value"],
    }.get(style, pal["value"])


def _bar_color(color: str, pal: dict) -> str:
    return {
        "cyan": pal["accent"],
        "accent": pal["accent"],
        "gold": pal["gold"],
        "ok": _OK,
        "heal": _OK,
        "warn": _WARN,
        "bad": _BAD,
    }.get(color, pal["accent"])


def _text_font(style: str):
    if style == "title":
        return ("Segoe UI", 12, "bold")
    if style == "subtitle":
        return ("Segoe UI", 10, "bold")
    if style == "mono":
        return _MONO
    return ("Segoe UI", 10)


# ── node renderers ───────────────────────────────────────────────────────────

def _render_node(parent: tk.Misc, node: Mapping[str, Any], pal: dict,
                 on_action: Optional[Callable[[str, dict], None]]) -> None:
    kind = str(node.get("type") or "")
    if kind in ("panel", "section", "card", "group", "row"):
        _render_container(parent, node, pal, on_action)
    elif kind == "text":
        _render_text(parent, node, pal)
    elif kind == "kv":
        _render_kv(parent, node, pal)
    elif kind == "bar":
        _render_bar(parent, node, pal)
    elif kind == "badge":
        _render_badge(parent, node, pal)
    elif kind == "divider":
        tk.Frame(parent, bg=pal["sep"], height=1).pack(fill="x", pady=5)
    elif kind == "spacer":
        tk.Frame(parent, bg=parent["bg"], height=int(node.get("size") or 8)).pack()
    elif kind == "button":
        _render_button(parent, node, pal, on_action)
    elif kind == "table":
        _render_table(parent, node, pal)


def _render_container(parent: tk.Misc, node: Mapping[str, Any], pal: dict,
                      on_action) -> None:
    kind = str(node.get("type") or "")
    is_row = kind == "row"
    framed = kind in ("section", "card")
    accent = _bar_color(str(node.get("accent") or "cyan"), pal)
    box = tk.Frame(parent, bg=pal["body"],
                   highlightthickness=(1 if framed else 0),
                   highlightbackground=pal["border"])
    box.pack(fill="x", pady=(4 if framed else 0), padx=(2 if framed else 0))
    if framed:
        # Left accent stripe shown for every framed container — matches the web
        # renderer's border-left accent (which is independent of any title).
        shell = tk.Frame(box, bg=pal["body"])
        shell.pack(fill="x")
        tk.Frame(shell, bg=accent, width=3).pack(side="left", fill="y")
        content = tk.Frame(shell, bg=pal["body"])
        content.pack(side="left", fill="x", expand=True, padx=(7, 8), pady=(2, 6))
    else:
        content = box
    title = str(node.get("title") or "")
    if title:
        head = tk.Frame(content, bg=pal["body"])
        head.pack(fill="x", pady=(3, 2))
        tk.Frame(head, bg=accent, width=3, height=14).pack(side="left", padx=(0, 6))
        tk.Label(head, text=title, bg=pal["body"], fg=pal["gold"], anchor="w",
                 font=("Segoe UI", 10, "bold")).pack(side="left")
    if is_row:
        align = str(node.get("align") or "left")
        inner = tk.Frame(content, bg=content["bg"])
        inner.pack(anchor={"left": "w", "center": "center", "right": "e"}.get(align, "w"))
        for child in node.get("children") or []:
            cell = tk.Frame(inner, bg=content["bg"])
            cell.pack(side="left", padx=(0, 10))
            _render_node(cell, child, pal, on_action)
    else:
        for child in node.get("children") or []:
            _render_node(content, child, pal, on_action)


def _render_text(parent: tk.Misc, node: Mapping[str, Any], pal: dict) -> None:
    style = str(node.get("style") or "value")
    align = str(node.get("align") or "left")
    tk.Label(parent, text=str(node.get("text") or ""), bg=parent["bg"],
             fg=_text_color(style, pal), anchor={"left": "w", "center": "center", "right": "e"}.get(align, "w"),
             justify={"left": "left", "center": "center", "right": "right"}.get(align, "left"),
             wraplength=560, font=_text_font(style)).pack(
        fill="x", anchor={"left": "w", "center": "center", "right": "e"}.get(align, "w"), pady=1)


def _render_kv(parent: tk.Misc, node: Mapping[str, Any], pal: dict) -> None:
    row = tk.Frame(parent, bg=parent["bg"])
    row.pack(fill="x", pady=1)
    tk.Label(row, text=str(node.get("label") or ""), bg=parent["bg"], fg=pal["label"],
             anchor="w", font=("Segoe UI", 9)).pack(side="left")
    tk.Label(row, text=str(node.get("value") or ""), bg=parent["bg"],
             fg=_text_color(str(node.get("style") or "value"), pal),
             anchor="e", font=("Segoe UI", 9, "bold")).pack(side="right")


def _render_bar(parent: tk.Misc, node: Mapping[str, Any], pal: dict) -> None:
    pct = max(0.0, min(1.0, float(node.get("pct") or 0.0)))
    color = _bar_color(str(node.get("color") or "cyan"), pal)
    label = str(node.get("label") or "")
    caption = str(node.get("caption") or "")
    if label or caption:
        top = tk.Frame(parent, bg=parent["bg"])
        top.pack(fill="x", pady=(3, 1))
        if label:
            tk.Label(top, text=label, bg=parent["bg"], fg=pal["label"], anchor="w",
                     font=("Segoe UI", 9)).pack(side="left")
        if caption:
            tk.Label(top, text=caption, bg=parent["bg"], fg=pal["value"], anchor="e",
                     font=("Segoe UI", 9)).pack(side="right")
    track = tk.Frame(parent, bg=pal["sep"], height=8)
    track.pack(fill="x", pady=(0, 3))
    track.pack_propagate(False)
    fill = tk.Frame(track, bg=color)
    fill.place(relx=0, rely=0, relwidth=pct, relheight=1)


def _render_badge(parent: tk.Misc, node: Mapping[str, Any], pal: dict) -> None:
    style = str(node.get("style") or "muted")
    fg = {"ok": _OK, "warn": _WARN, "bad": _BAD, "gold": pal["gold"],
          "accent": pal["accent"]}.get(style, pal["label"])
    tk.Label(parent, text=" " + str(node.get("text") or "") + " ", bg=parent["bg"], fg=fg,
             highlightthickness=1, highlightbackground=fg, font=("Segoe UI", 8, "bold")).pack(
        side="left", padx=(0, 4), pady=2)


def _render_button(parent: tk.Misc, node: Mapping[str, Any], pal: dict, on_action) -> None:
    action = str(node.get("action") or "")
    payload = dict(node.get("payload") or {})
    style = str(node.get("style") or "default")
    fg = {"primary": pal["gold"], "danger": _BAD, "ghost": pal["label"]}.get(style, pal["header_fg"])
    disabled = bool(node.get("disabled"))

    def _cmd():
        if callable(on_action) and not disabled:
            on_action(action, payload)

    tk.Button(parent, text=str(node.get("label") or action), command=_cmd,
              state=("disabled" if disabled else "normal"),
              bg=pal["header_bg"], fg=fg, activebackground=pal["accent"],
              activeforeground="white", relief="flat", bd=0, padx=10, pady=3,
              font=("Segoe UI", 9, "bold")).pack(side="left", padx=(0, 6), pady=4)


def _render_table(parent: tk.Misc, node: Mapping[str, Any], pal: dict) -> None:
    columns = list(node.get("columns") or [])
    rows = list(node.get("rows") or [])
    title = str(node.get("title") or "")
    if title:
        tk.Label(parent, text=title, bg=parent["bg"], fg=pal["label"], anchor="w",
                 font=("Segoe UI", 9, "bold")).pack(fill="x", pady=(4, 2))
    grid = tk.Frame(parent, bg=parent["bg"])
    grid.pack(fill="x")
    if not columns and rows:
        keys = list(rows[0].keys())
        columns = [{"key": k, "title": k, "align": "left"} for k in keys]
    for col_i, col in enumerate(columns):
        grid.grid_columnconfigure(col_i, weight=(1 if col_i == 0 else 0))
        tk.Label(grid, text=str(col.get("title") or col.get("key") or ""), bg=parent["bg"],
                 fg=pal["label"], anchor={"right": "e", "center": "center"}.get(col.get("align"), "w"),
                 font=("Segoe UI", 8, "bold")).grid(row=0, column=col_i, sticky="ew", padx=4, pady=(0, 2))
    hk = str(node.get("highlight_key") or "")
    for r_i, row in enumerate(rows, start=1):
        hi = bool(hk and row.get(hk))
        for col_i, col in enumerate(columns):
            val = row.get(col.get("key"))
            tk.Label(grid, text="" if val is None else str(val), bg=parent["bg"],
                     fg=(pal["gold"] if hi else pal["value"]),
                     anchor={"right": "e", "center": "center"}.get(col.get("align"), "w"),
                     font=("Segoe UI", 9, "bold" if hi else "normal")).grid(
                row=r_i, column=col_i, sticky="ew", padx=4, pady=1)


def render_spec_into(parent: tk.Misc, spec: Any,
                     on_action: Optional[Callable[[str, dict], None]] = None) -> None:
    """Clear ``parent`` and render a normalized plugin UI spec into it."""
    for child in list(parent.winfo_children()):
        try:
            child.destroy()
        except Exception:
            pass
    pal = _pal()
    spec = spec or {}
    title = str(spec.get("title") or "")
    if title:
        tk.Label(parent, text=title, bg=parent["bg"], fg=pal["gold"], anchor="w",
                 font=("Segoe UI", 12, "bold")).pack(fill="x", pady=(0, 4))
    nodes = spec.get("nodes") or []
    if not nodes and not title:
        tk.Label(parent, text="(empty)", bg=parent["bg"], fg=pal["label"],
                 font=("Segoe UI", 9, "italic")).pack(anchor="w")
        return
    for node in nodes:
        try:
            _render_node(parent, node, pal, on_action)
        except Exception:
            # One bad node must never break the whole panel.
            tk.Label(parent, text="[render error]", bg=parent["bg"], fg=_BAD,
                     font=_MONO).pack(anchor="w")


# ── higher-level helpers ─────────────────────────────────────────────────────

class PluginPanelList:
    """A scrollable, auto-redrawing list of plugin UI panels.

    Polls ``act_plugin_ui_panels`` for registered panels and
    ``act_plugin_ui_render`` for each panel's spec, repainting on a timer and
    whenever a ``plugin_ui_invalidate`` event marks the list dirty.
    """

    def __init__(self, parent: tk.Misc, owner: Any, *, interval_ms: int = 700) -> None:
        self.parent = parent
        self.owner = owner
        self.interval_ms = max(150, int(interval_ms))
        self._dirty = True
        self._after_id: Optional[str] = None
        self._sub_token = ""
        self._frame = tk.Frame(parent, bg=parent["bg"])
        self._frame.pack(fill="both", expand=True)
        self._subscribe()

    def _runtime(self):
        from act_platform.runtime import (act_plugin_ui_action, act_plugin_ui_panels,
                                           act_plugin_ui_render)
        return act_plugin_ui_panels, act_plugin_ui_render, act_plugin_ui_action

    def _subscribe(self) -> None:
        try:
            from act_platform.runtime import ensure_act_event_bus
            bus = ensure_act_event_bus(self.owner)
            self._sub_token = bus.subscribe(
                "plugin_ui_invalidate", lambda _e: setattr(self, "_dirty", True),
                owner_id="entity_plugin_panel_list")
        except Exception:
            self._sub_token = ""

    def start(self) -> None:
        self._dirty = True
        if not self._sub_token:
            self._subscribe()  # re-subscribe after a prior stop() (hide→show cycle)
        self._tick()

    def stop(self) -> None:
        if self._after_id is not None:
            try:
                self.parent.after_cancel(self._after_id)
            except Exception:
                pass
            self._after_id = None
        if self._sub_token:
            try:
                from act_platform.runtime import ensure_act_event_bus
                ensure_act_event_bus(self.owner).unsubscribe(self._sub_token)
                self._sub_token = ""  # only clear after a successful unsubscribe
            except Exception:
                pass

    def _tick(self) -> None:
        if self._dirty:
            self._dirty = False
            try:
                self._rebuild()
            except Exception:
                pass
        try:
            self._after_id = self.parent.after(self.interval_ms, self._tick)
        except Exception:
            self._after_id = None

    def _on_action(self, panel_id: str):
        _, _, act_action = self._runtime()

        def _handler(action: str, payload: dict) -> None:
            try:
                act_action(self.owner, panel_id, action, payload)
            except Exception:
                pass
            self._dirty = True
        return _handler

    def _rebuild(self) -> None:
        list_panels, render_panel, _ = self._runtime()
        pal = _pal()
        for child in list(self._frame.winfo_children()):
            child.destroy()
        try:
            panels = (list_panels(self.owner) or {}).get("panels") or []
        except Exception:
            panels = []
        active = [p for p in panels if p.get("available")]
        if not active:
            tk.Label(self._frame, text="无插件 UI 面板\n启用注册了 register_ui_panel 的插件后显示在这里。",
                     bg=self._frame["bg"], fg=pal["label"], justify="center",
                     font=("Segoe UI", 10), pady=24).pack(fill="x")
            return
        for panel in active:
            pid = str(panel.get("id") or "")
            card = tk.Frame(self._frame, bg=pal["body"], highlightthickness=1,
                            highlightbackground=pal["border"])
            card.pack(fill="x", pady=6, padx=2)
            body = tk.Frame(card, bg=pal["body"])
            body.pack(fill="x", padx=10, pady=8)
            try:
                spec = (render_panel(self.owner, pid) or {}).get("spec") or {}
            except Exception:
                spec = {"version": 1, "title": pid, "nodes": []}
            render_spec_into(body, spec, on_action=self._on_action(pid))


def attach_overlay_layer(toplevel: tk.Toplevel, surface: str, owner: Any,
                         *, interval_ms: int = 700) -> "OverlayLayer":
    """Attach a plugin overlay layer (top-anchored) to an existing Tk window."""
    return OverlayLayer(toplevel, surface, owner, interval_ms=interval_ms)


class OverlayLayer:
    """A thin frame placed at the top of a host window that paints overlays."""

    def __init__(self, toplevel: tk.Misc, surface: str, owner: Any, *, interval_ms: int = 700) -> None:
        self.toplevel = toplevel
        self.surface = str(surface or "")
        self.owner = owner
        self.interval_ms = max(150, int(interval_ms))
        self._after_id: Optional[str] = None
        self._dirty = True
        self._sub_token = ""
        pal = _pal()
        self.frame = tk.Frame(toplevel, bg=pal["body"], highlightthickness=0)
        # Hidden until there is at least one overlay to show.
        self._visible = False
        self._subscribe()
        self._tick()

    def _subscribe(self) -> None:
        try:
            from act_platform.runtime import ensure_act_event_bus
            self._sub_token = ensure_act_event_bus(self.owner).subscribe(
                "plugin_ui_invalidate", lambda _e: setattr(self, "_dirty", True),
                owner_id=f"overlay_{self.surface}")
        except Exception:
            self._sub_token = ""

    def stop(self) -> None:
        if self._after_id is not None:
            try:
                self.frame.after_cancel(self._after_id)
            except Exception:
                pass
            self._after_id = None
        if self._sub_token:
            try:
                from act_platform.runtime import ensure_act_event_bus
                ensure_act_event_bus(self.owner).unsubscribe(self._sub_token)
                self._sub_token = ""  # only clear after a successful unsubscribe
            except Exception:
                pass

    def _tick(self) -> None:
        if self._dirty:
            self._dirty = False
            try:
                self._repaint()
            except Exception:
                pass
        try:
            self._after_id = self.frame.after(self.interval_ms, self._tick)
        except Exception:
            self._after_id = None

    def _repaint(self) -> None:
        try:
            from act_platform.runtime import act_render_overlays
            overlays = (act_render_overlays(self.owner, self.surface) or {}).get("overlays") or []
        except Exception:
            overlays = []
        for child in list(self.frame.winfo_children()):
            child.destroy()
        if not overlays:
            if self._visible:
                self.frame.pack_forget()
                self._visible = False
            return
        for ov in overlays:
            render_spec_into(self.frame, ov.get("spec") or {}, on_action=None)
        if not self._visible:
            self.frame.pack(fill="x", side="top")
            self._visible = True


__all__ = ["render_spec_into", "PluginPanelList", "attach_overlay_layer", "OverlayLayer"]
