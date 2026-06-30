# -*- coding: utf-8 -*-
"""Entity (Tk) renderer for the declarative plugin UI spec.

This is the Tk twin of ``web/plugin_layer.js``: both consume the same normalized
spec from :mod:`act_platform.ui_spec` so a plugin draws identically on the Entity
overlay and the WebView windows.

The renderer **reconciles**: :class:`SpecRenderer` keeps the widget tree across
renders and updates widgets *in place* (text, bar %, canvas ops, button state…)
whenever the spec structure is unchanged, only tearing down + rebuilding the
subtrees that actually changed shape. This keeps animated panels (e.g. a note
roll) smooth and — crucially — never destroys interactive widgets (buttons) out
from under the user during playback, so clicks are never lost and nothing flickers.

Public surface:
    SpecRenderer(mount, on_action)                  -> persistent reconciling renderer
    render_spec_into(parent, spec, on_action=None)  -> one-shot (clear + build)
    PluginPanelList(parent, owner)                  -> auto-redrawing panel list
    attach_overlay_layer(toplevel, surface, owner)  -> plugin overlay on a window
"""

from __future__ import annotations

import math
import tkinter as tk
from typing import Any, Callable, Mapping, Optional

import gui_modules.sao_panel_ui as _theme

# Status colors are theme-independent enough to read on both light and dark.
_OK = "#2bb673"
_WARN = "#d79b06"
_BAD = "#e0455a"
_MONO = ("Consolas", 9)

_ANCHOR = {"left": "w", "center": "center", "right": "e"}
_JUSTIFY = {"left": "left", "center": "center", "right": "right"}
_TALIGN = {"right": "e", "center": "center", "left": "w"}
_CONTAINER_TYPES = ("panel", "section", "card", "row", "group")


def _finite_float(value: Any, default: float = 0.0, *, lo: float | None = None, hi: float | None = None) -> float:
    try:
        num = float(default if value is None or value == "" else value)
    except Exception:
        num = float(default or 0.0)
    if not math.isfinite(num):
        num = float(default or 0.0)
    if lo is not None:
        num = max(float(lo), num)
    if hi is not None:
        num = min(float(hi), num)
    return num


def _finite_int(value: Any, default: int = 0, *, lo: int | None = None, hi: int | None = None) -> int:
    num = int(_finite_float(value, float(default), lo=lo, hi=hi))
    if lo is not None:
        num = max(int(lo), num)
    if hi is not None:
        num = min(int(hi), num)
    return num


def _validate_number(proposed: str) -> bool:
    """Soft numeric guard for input_type=number: allow empty / sign / digits /
    one dot / hex-ish chars (so 0x.. addresses can be typed). Never hard-blocks."""
    if proposed in ("", "-", "+", "0x", "0X"):
        return True
    allowed = set("0123456789abcdefABCDEFxX.-+")
    return all(ch in allowed for ch in proposed)


def _input_width_px(width: Any) -> int:
    return _finite_int(width, 0, lo=0)


def _input_width_units(width: Any) -> int:
    return max(4, int(_input_width_px(width) / 8))


def _input_pack_kw(width: Any) -> dict:
    width_px = _input_width_px(width)
    return {"pady": 2, "fill": "none"} if width_px > 0 else {"fill": "x", "pady": 2}


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
        "title": pal["gold"], "subtitle": pal["accent"], "value": pal["value"],
        "label": pal["label"], "muted": pal["label"], "ok": _OK, "warn": _WARN,
        "bad": _BAD, "gold": pal["gold"], "accent": pal["accent"], "mono": pal["value"],
    }.get(style, pal["value"])


def _bar_color(color: str, pal: dict) -> str:
    return {
        "cyan": pal["accent"], "accent": pal["accent"], "gold": pal["gold"],
        "ok": _OK, "heal": _OK, "warn": _WARN, "bad": _BAD,
    }.get(color, pal["accent"])


def _text_font(style: str):
    if style == "title":
        return ("Segoe UI", 12, "bold")
    if style == "subtitle":
        return ("Segoe UI", 10, "bold")
    if style == "mono":
        return _MONO
    return ("Segoe UI", 10)


def _badge_fg(style: str, pal: dict) -> str:
    return {"ok": _OK, "warn": _WARN, "bad": _BAD, "gold": pal["gold"],
            "accent": pal["accent"]}.get(style, pal["label"])


def _btn_fg(style: str, pal: dict) -> str:
    return {"primary": pal["gold"], "danger": _BAD, "ghost": pal["label"]}.get(
        style, pal["header_fg"])


def _canvas_fill(value: Any, pal: dict, default: str = "") -> str:
    """Map a canvas color token (or #hex passthrough) to a concrete Tk color."""
    text = str(value or "")
    if not text:
        return default
    if text.startswith("#"):
        return text
    return {
        "white": "#f3f5f7", "black": "#1b1f24", "grid": pal["sep"],
        "bg": pal["body"], "body": pal["body"], "border": pal["border"], "sep": pal["sep"],
        "header": pal["header_bg"], "transparent": "",
        "title": pal["gold"], "subtitle": pal["accent"], "value": pal["value"],
        "label": pal["label"], "muted": pal["label"], "gold": pal["gold"],
        "accent": pal["accent"], "cyan": pal["accent"], "heal": _OK,
        "ok": _OK, "warn": _WARN, "bad": _BAD, "mono": pal["value"],
    }.get(text.lower(), default)


def _draw_canvas_ops(cv: tk.Canvas, node: Mapping[str, Any], pal: dict) -> None:
    """(Re)draw a canvas node's ops into ``cv`` in place (clear + redraw)."""
    cv.delete("all")
    anchors = {"nw": "nw", "n": "n", "ne": "ne", "w": "w", "center": "center",
               "e": "e", "sw": "sw", "s": "s", "se": "se"}
    for op in node.get("ops") or []:
        kind = str(op.get("op") or "")
        if kind in ("rect", "oval"):
            x, y = _finite_int(op.get("x"), 0), _finite_int(op.get("y"), 0)
            x2, y2 = x + _finite_int(op.get("w"), 0), y + _finite_int(op.get("h"), 0)
            fill = _canvas_fill(op.get("fill"), pal, "")
            outline = _canvas_fill(op.get("outline"), pal, "")
            kwargs = {"fill": fill or "", "outline": outline or "",
                      "width": _finite_int(op.get("width"), 1 if outline else 0, lo=0)}
            if kind == "rect":
                cv.create_rectangle(x, y, x2, y2, **kwargs)
            else:
                cv.create_oval(x, y, x2, y2, **kwargs)
        elif kind == "line":
            cv.create_line(_finite_int(op.get("x1"), 0), _finite_int(op.get("y1"), 0),
                           _finite_int(op.get("x2"), 0), _finite_int(op.get("y2"), 0),
                           fill=_canvas_fill(op.get("fill"), pal, pal["value"]) or pal["value"],
                           width=_finite_int(op.get("width"), 1, lo=1))
        elif kind == "text":
            font = ("Segoe UI", _finite_int(op.get("size"), 10, lo=1),
                    "bold" if op.get("bold") else "normal")
            cv.create_text(_finite_int(op.get("x"), 0), _finite_int(op.get("y"), 0),
                           text=str(op.get("text") or ""),
                           fill=_canvas_fill(op.get("fill"), pal, pal["value"]) or pal["value"],
                           font=font, anchor=anchors.get(str(op.get("anchor") or "nw"), "nw"))


def _table_key(spec: Mapping[str, Any]):
    """Identity of a table's shape — used to decide in-place update vs rebuild."""
    cols = spec.get("columns") or []
    return (tuple((c.get("key"), c.get("title"), c.get("align")) for c in cols),
            len(spec.get("rows") or []))


class _RNode:
    """A rendered node: its top Tk widget + handles for in-place updates."""
    __slots__ = ("type", "widget", "parts", "children", "spec", "pack_kw")

    def __init__(self, ntype: str, widget: Any, parts: Optional[dict] = None,
                 children: Optional[list] = None, spec: Any = None,
                 pack_kw: Optional[dict] = None) -> None:
        self.type = ntype
        self.widget = widget
        self.parts = parts if parts is not None else {}
        self.children = children
        self.spec = spec
        self.pack_kw = pack_kw if pack_kw is not None else {"fill": "x"}


class SpecRenderer:
    """Reconciling renderer: keeps the widget tree and updates it in place.

    Call :meth:`render` repeatedly with fresh specs; widgets are reused (only
    their values change) when the structure matches, so interactive/animated
    panels never flicker or lose clicks. ``on_action(action, payload)`` fires on
    button presses and may be swapped any time via :meth:`set_on_action`.
    """

    def __init__(self, mount: tk.Misc, on_action: Optional[Callable[[str, dict], None]] = None) -> None:
        self.mount = mount
        self.on_action = on_action
        self._nodes: Optional[list] = None
        self._title: Optional[tk.Widget] = None
        # Live ``input`` widgets by id, read into payload["inputs"] on a button fire.
        # Per-renderer, so naturally scoped to one panel (PluginPanelList builds one
        # SpecRenderer per card; each detached window owns its own).
        self._inputs: dict = {}

    def set_on_action(self, on_action: Optional[Callable[[str, dict], None]]) -> None:
        self.on_action = on_action

    def render(self, spec: Any) -> None:
        pal = _pal()
        spec = spec or {}
        nodes = list(spec.get("nodes") or [])
        title = str(spec.get("title") or "")
        if not nodes and not title:
            nodes = [{"type": "text", "text": "(empty)", "style": "muted", "align": "left"}]
        # Spec title (single label above the nodes).
        if title:
            if self._title is None or not self._title.winfo_exists():
                self._title = tk.Label(self.mount, text=title, bg=self.mount["bg"],
                                       fg=pal["gold"], anchor="w", font=("Segoe UI", 12, "bold"))
                self._title.pack(fill="x", pady=(0, 4))
            else:
                self._title.config(text=title, fg=pal["gold"])
        elif self._title is not None:
            try:
                self._title.destroy()
            except Exception:
                pass
            self._title = None
        self._nodes = self._reconcile(self.mount, self._nodes, nodes, pal, False)
        if self._title is not None and self._nodes:
            try:
                self._title.pack_configure(before=self._nodes[0].widget)
            except Exception:
                pass

    # ── reconcile ──────────────────────────────────────────────────────────
    def _reconcile(self, parent, old_list, new_specs, pal, is_row) -> list:
        old_list = old_list or []
        result: list = []
        for i, ns in enumerate(new_specs):
            old = old_list[i] if i < len(old_list) else None
            if old is not None and self._compatible(old, ns):
                try:
                    self._update(old, ns, pal)
                    result.append(old)
                    continue
                except Exception:
                    try:
                        old.widget.destroy()
                    except Exception:
                        pass
                    old = None
            else:
                if old is not None:
                    try:
                        old.widget.destroy()
                    except Exception:
                        pass
            rn = self._build(parent, ns, pal, is_row)
            if rn is not None:
                self._place(rn, is_row)
                result.append(rn)
        for old in old_list[len(new_specs):]:
            try:
                old.widget.destroy()
            except Exception:
                pass
        # Enforce pack order (handles rebuilt middle children + reorders cheaply).
        prev = None
        for rn in result:
            if prev is not None:
                try:
                    rn.widget.pack_configure(after=prev.widget)
                except Exception:
                    pass
            prev = rn
        return result

    def _place(self, rn: _RNode, is_row: bool) -> None:
        try:
            if is_row:
                rn.widget.pack(side="left", padx=(0, 10))
            else:
                rn.widget.pack(**rn.pack_kw)
        except Exception:
            pass

    def _compatible(self, rn: _RNode, ns: Mapping[str, Any]) -> bool:
        t = str(ns.get("type") or "")
        if rn.type != t:
            return False
        if t in _CONTAINER_TYPES:
            return ("title" in rn.parts) == bool(str(ns.get("title") or ""))
        if t == "table":
            return _table_key(rn.spec or {}) == _table_key(ns)
        if t == "input":
            # Reuse only when the id matches, so two inputs never swap identity
            # (and the live tk.Entry / user text survives the redraw).
            return str((rn.spec or {}).get("id") or "") == str(ns.get("id") or "")
        return True

    def _build(self, parent, ns, pal, is_row) -> Optional[_RNode]:
        t = str(ns.get("type") or "")
        if t in _CONTAINER_TYPES:
            return self._build_container(parent, ns, pal)
        builder = {
            "text": self._build_text, "kv": self._build_kv, "bar": self._build_bar,
            "badge": self._build_badge, "divider": self._build_divider,
            "spacer": self._build_spacer, "button": self._build_button,
            "input": self._build_input,
            "table": self._build_table, "canvas": self._build_canvas,
        }.get(t)
        return builder(parent, ns, pal) if builder else None

    def _update(self, rn: _RNode, ns, pal) -> None:
        t = rn.type
        if t in _CONTAINER_TYPES:
            self._update_container(rn, ns, pal)
        elif t == "text":
            self._update_text(rn, ns, pal)
        elif t == "kv":
            self._update_kv(rn, ns, pal)
        elif t == "bar":
            self._update_bar(rn, ns, pal)
        elif t == "badge":
            self._update_badge(rn, ns, pal)
        elif t == "spacer":
            rn.widget.config(height=_finite_int(ns.get("size"), 8, lo=0))
        elif t == "button":
            self._update_button(rn, ns, pal)
        elif t == "input":
            self._update_input(rn, ns, pal)
        elif t == "table":
            self._update_table(rn, ns, pal)
        elif t == "canvas":
            self._update_canvas(rn, ns, pal)
        # divider: nothing to update
        rn.spec = ns

    # ── leaf builders / updaters ───────────────────────────────────────────
    def _build_text(self, parent, ns, pal) -> _RNode:
        style, align = str(ns.get("style") or "value"), str(ns.get("align") or "left")
        w = tk.Label(parent, text=str(ns.get("text") or ""), bg=parent["bg"],
                     fg=_text_color(style, pal), anchor=_ANCHOR.get(align, "w"),
                     justify=_JUSTIFY.get(align, "left"), wraplength=560, font=_text_font(style))
        return _RNode("text", w, spec=ns,
                      pack_kw={"fill": "x", "anchor": _ANCHOR.get(align, "w"), "pady": 1})

    def _update_text(self, rn, ns, pal) -> None:
        style, align = str(ns.get("style") or "value"), str(ns.get("align") or "left")
        rn.widget.config(text=str(ns.get("text") or ""), fg=_text_color(style, pal),
                         anchor=_ANCHOR.get(align, "w"), justify=_JUSTIFY.get(align, "left"),
                         font=_text_font(style))

    def _build_kv(self, parent, ns, pal) -> _RNode:
        w = tk.Frame(parent, bg=parent["bg"])
        k = tk.Label(w, text=str(ns.get("label") or ""), bg=parent["bg"], fg=pal["label"],
                     anchor="w", font=("Segoe UI", 9))
        k.pack(side="left")
        v = tk.Label(w, text=str(ns.get("value") or ""), bg=parent["bg"],
                     fg=_text_color(str(ns.get("style") or "value"), pal),
                     anchor="e", font=("Segoe UI", 9, "bold"))
        v.pack(side="right")
        return _RNode("kv", w, parts={"k": k, "v": v}, spec=ns, pack_kw={"fill": "x", "pady": 1})

    def _update_kv(self, rn, ns, pal) -> None:
        rn.parts["k"].config(text=str(ns.get("label") or ""))
        rn.parts["v"].config(text=str(ns.get("value") or ""),
                             fg=_text_color(str(ns.get("style") or "value"), pal))

    def _build_bar(self, parent, ns, pal) -> _RNode:
        pct = _finite_float(ns.get("pct"), 0.0, lo=0.0, hi=1.0)
        color = _bar_color(str(ns.get("color") or "cyan"), pal)
        interactive = bool(ns.get("action"))
        w = tk.Frame(parent, bg=parent["bg"])
        top = tk.Frame(w, bg=parent["bg"])
        top.pack(fill="x", pady=(3, 1))
        lbl = tk.Label(top, text=str(ns.get("label") or ""), bg=parent["bg"], fg=pal["label"],
                       anchor="w", font=("Segoe UI", 9))
        lbl.pack(side="left")
        cap = tk.Label(top, text=str(ns.get("caption") or ""), bg=parent["bg"], fg=pal["value"],
                       anchor="e", font=("Segoe UI", 9))
        cap.pack(side="right")
        bar_h = 16 if interactive else 8
        track = tk.Frame(w, bg=pal["sep"], height=bar_h,
                         cursor="hand2" if interactive else "")
        track.pack(fill="x", pady=(0, 3))
        track.pack_propagate(False)
        fill = tk.Frame(track, bg=color)
        fill.place(relx=0, rely=0, relwidth=max(pct, 0.001), relheight=1)

        if interactive and self._on_action:
            action = str(ns["action"])
            lo = float(ns.get("lo", 0.0))
            hi = float(ns.get("hi", 1.0))
            step = float(ns.get("step", 0.0))
            on_act = self._on_action

            def _on_click(event, src_is_fill=False):
                tw = track.winfo_width()
                if tw < 1:
                    return
                x = event.x + (fill.winfo_x() if src_is_fill else 0)
                raw = max(0.0, min(1.0, x / tw))
                val = lo + raw * (hi - lo)
                if step > 0:
                    val = round(val / step) * step
                val = max(lo, min(hi, val))
                rel = (val - lo) / (hi - lo) if hi > lo else 0.0
                fill.place_configure(relwidth=max(rel, 0.001))
                cap.config(text=f"{val:.2f}" if isinstance(val, float) and hi <= 1.01 else
                           (f"{val:.1f}" if isinstance(val, float) else str(int(val))))
                on_act(action, {"value": val})

            track.bind("<Button-1>", lambda e: _on_click(e, False))
            track.bind("<B1-Motion>", lambda e: _on_click(e, False))
            fill.bind("<Button-1>", lambda e: _on_click(e, True))
            fill.bind("<B1-Motion>", lambda e: _on_click(e, True))

        return _RNode("bar", w, parts={"label": lbl, "caption": cap, "fill": fill,
                                        "track": track},
                      spec=ns, pack_kw={"fill": "x"})

    def _update_bar(self, rn, ns, pal) -> None:
        rn.parts["label"].config(text=str(ns.get("label") or ""))
        rn.parts["fill"].config(bg=_bar_color(str(ns.get("color") or "cyan"), pal))
        if not ns.get("action"):
            pct = _finite_float(ns.get("pct"), 0.0, lo=0.0, hi=1.0)
            rn.parts["caption"].config(text=str(ns.get("caption") or ""))
            rn.parts["fill"].place_configure(relwidth=pct)

    def _build_badge(self, parent, ns, pal) -> _RNode:
        fg = _badge_fg(str(ns.get("style") or "muted"), pal)
        w = tk.Label(parent, text=" " + str(ns.get("text") or "") + " ", bg=parent["bg"],
                     fg=fg, highlightthickness=1, highlightbackground=fg, font=("Segoe UI", 8, "bold"))
        return _RNode("badge", w, spec=ns, pack_kw={"side": "left", "padx": (0, 4), "pady": 2})

    def _update_badge(self, rn, ns, pal) -> None:
        fg = _badge_fg(str(ns.get("style") or "muted"), pal)
        rn.widget.config(text=" " + str(ns.get("text") or "") + " ", fg=fg, highlightbackground=fg)

    def _build_divider(self, parent, ns, pal) -> _RNode:
        return _RNode("divider", tk.Frame(parent, bg=pal["sep"], height=1),
                      spec=ns, pack_kw={"fill": "x", "pady": 5})

    def _build_spacer(self, parent, ns, pal) -> _RNode:
        return _RNode("spacer", tk.Frame(parent, bg=parent["bg"], height=_finite_int(ns.get("size"), 8, lo=0)),
                      spec=ns, pack_kw={})

    def _build_button(self, parent, ns, pal) -> _RNode:
        rn = _RNode("button", None, spec=ns, pack_kw={"side": "left", "padx": (0, 6), "pady": 4})
        rn.parts = {"action": str(ns.get("action") or ""), "payload": dict(ns.get("payload") or {}),
                    "disabled": bool(ns.get("disabled"))}
        fg = _btn_fg(str(ns.get("style") or "default"), pal)
        rn.widget = tk.Button(parent, text=str(ns.get("label") or ns.get("action") or ""),
                              command=lambda r=rn: self._fire(r),
                              state=("disabled" if rn.parts["disabled"] else "normal"),
                              bg=pal["header_bg"], fg=fg, activebackground=pal["accent"],
                              activeforeground="white", relief="flat", bd=0, padx=10, pady=3,
                              font=("Segoe UI", 9, "bold"))
        return rn

    def _update_button(self, rn, ns, pal) -> None:
        rn.parts["action"] = str(ns.get("action") or "")
        rn.parts["payload"] = dict(ns.get("payload") or {})
        rn.parts["disabled"] = bool(ns.get("disabled"))
        rn.widget.config(text=str(ns.get("label") or ns.get("action") or ""),
                         fg=_btn_fg(str(ns.get("style") or "default"), pal),
                         state=("disabled" if rn.parts["disabled"] else "normal"))

    # ── text input (round-trips into payload["inputs"] on a button fire) ───────
    def _build_input(self, parent, ns, pal) -> _RNode:
        iid = str(ns.get("id") or "")
        seed = str(ns.get("value") or "")
        placeholder = str(ns.get("placeholder") or "")
        itype = str(ns.get("input_type") or "text")
        var = tk.StringVar(value=seed)
        w = tk.Entry(parent, textvariable=var, bg=pal["header_bg"], fg=pal["value"],
                     insertbackground=pal["value"], relief="flat", highlightthickness=1,
                     highlightbackground=pal["border"], highlightcolor=pal["accent"],
                     font=("Segoe UI", 10))
        width = _input_width_px(ns.get("width"))
        pack_kw = _input_pack_kw(width)
        if width > 0:
            w.config(width=_input_width_units(width))
        is_placeholder = False
        if itype == "password":
            w.config(show="•")
        if not seed and placeholder and itype != "password":
            is_placeholder = True
            var.set(placeholder)
            w.config(fg=pal["label"])
        rn = _RNode("input", w, spec=ns, pack_kw=pack_kw)
        rn.parts = {"var": var, "id": iid, "placeholder": placeholder, "itype": itype,
                    "is_placeholder": is_placeholder, "last_seed": seed}

        def _on_focus_in(_e=None, r=rn, p=pal):
            if r.parts.get("is_placeholder"):
                r.parts["is_placeholder"] = False
                r.parts["var"].set("")
                r.widget.config(fg=p["value"])

        def _on_focus_out(_e=None, r=rn, p=pal):
            ph = r.parts.get("placeholder") or ""
            if ph and not r.parts["var"].get() and r.parts.get("itype") != "password":
                r.parts["is_placeholder"] = True
                r.parts["var"].set(ph)
                r.widget.config(fg=p["label"])

        w.bind("<FocusIn>", _on_focus_in)
        w.bind("<FocusOut>", _on_focus_out)
        if itype == "number":
            w.config(validate="key",
                     validatecommand=(w.register(_validate_number), "%P"))
        if iid:
            self._inputs[iid] = rn
        return rn

    def _update_input(self, rn, ns, pal) -> None:
        # Clobber guard: only overwrite the field when the server value actually
        # changed AND the user is not editing it — a steady/empty spec value must
        # never wipe what the user typed across the timed redraw.
        seed = str(ns.get("value") or "")
        placeholder = str(ns.get("placeholder") or "")
        itype = str(ns.get("input_type") or "text")
        old_placeholder = str(rn.parts.get("placeholder") or "")
        old_itype = str(rn.parts.get("itype") or "text")
        was_placeholder = bool(rn.parts.get("is_placeholder"))
        width = _input_width_px(ns.get("width"))
        try:
            if width > 0:
                rn.widget.config(width=_input_width_units(width))
            rn.pack_kw = _input_pack_kw(width)
            rn.widget.pack_configure(**rn.pack_kw)
        except Exception:
            pass
        try:
            rn.widget.config(show=("•" if itype == "password" else ""))
        except Exception:
            pass
        try:
            if itype == "number":
                rn.widget.config(validate="key",
                                 validatecommand=(rn.widget.register(_validate_number), "%P"))
            else:
                rn.widget.config(validate="none")
        except Exception:
            pass
        try:
            focused = (rn.widget.focus_get() is rn.widget)
        except Exception:
            focused = False
        if not focused and (
            seed != rn.parts.get("last_seed", "")
            or (was_placeholder and placeholder != old_placeholder)
            or itype != old_itype
        ):
            if seed:
                rn.parts["var"].set(seed)
                rn.parts["is_placeholder"] = False
                rn.widget.config(fg=pal["value"])
            elif placeholder and itype != "password":
                rn.parts["var"].set(placeholder)
                rn.parts["is_placeholder"] = True
                rn.widget.config(fg=pal["label"])
            else:
                rn.parts["var"].set("")
                rn.parts["is_placeholder"] = False
                rn.widget.config(fg=pal["value"])
        rn.parts["placeholder"] = placeholder
        rn.parts["itype"] = itype
        rn.parts["last_seed"] = seed

    def _collect_inputs(self) -> dict:
        out: dict = {}
        for iid, rn in list(self._inputs.items()):
            try:
                if not rn.widget.winfo_exists():
                    self._inputs.pop(iid, None)
                    continue
                out[iid] = "" if rn.parts.get("is_placeholder") else rn.parts["var"].get()
            except Exception:
                self._inputs.pop(iid, None)
        return out

    def _fire(self, rn: _RNode) -> None:
        if rn.parts.get("disabled"):
            return
        if callable(self.on_action):
            payload = dict(rn.parts.get("payload") or {})
            # Only attach input values when this panel actually has inputs, so a
            # button in an input-less panel keeps its exact payload (backward compat).
            inputs = self._collect_inputs()
            if inputs:
                payload["inputs"] = inputs
            try:
                self.on_action(rn.parts.get("action", ""), payload)
            except Exception:
                pass

    def _build_table(self, parent, ns, pal) -> _RNode:
        w = tk.Frame(parent, bg=parent["bg"])
        parts: dict = {"cells": []}
        title = str(ns.get("title") or "")
        if title:
            tl = tk.Label(w, text=title, bg=parent["bg"], fg=pal["label"], anchor="w",
                          font=("Segoe UI", 9, "bold"))
            tl.pack(fill="x", pady=(4, 2))
            parts["title"] = tl
        grid = tk.Frame(w, bg=parent["bg"])
        grid.pack(fill="x")
        parts["grid"] = grid
        columns = list(ns.get("columns") or [])
        rows = list(ns.get("rows") or [])
        if not columns and rows:
            columns = [{"key": k, "title": k, "align": "left"} for k in rows[0].keys()]
        parts["columns"] = columns
        for ci, col in enumerate(columns):
            grid.grid_columnconfigure(ci, weight=(1 if ci == 0 else 0))
            tk.Label(grid, text=str(col.get("title") or col.get("key") or ""), bg=parent["bg"],
                     fg=pal["label"], anchor=_TALIGN.get(col.get("align"), "w"),
                     font=("Segoe UI", 8, "bold")).grid(row=0, column=ci, sticky="ew", padx=4, pady=(0, 2))
        hk = str(ns.get("highlight_key") or "")
        for ri, row in enumerate(rows, start=1):
            hi = bool(hk and row.get(hk))
            cellrow = []
            for ci, col in enumerate(columns):
                val = row.get(col.get("key"))
                c = tk.Label(grid, text="" if val is None else str(val), bg=parent["bg"],
                             fg=(pal["gold"] if hi else pal["value"]),
                             anchor=_TALIGN.get(col.get("align"), "w"),
                             font=("Segoe UI", 9, "bold" if hi else "normal"))
                c.grid(row=ri, column=ci, sticky="ew", padx=4, pady=1)
                cellrow.append(c)
            parts["cells"].append(cellrow)
        return _RNode("table", w, parts=parts, spec=ns, pack_kw={"fill": "x"})

    def _update_table(self, rn, ns, pal) -> None:
        columns = rn.parts.get("columns") or []
        rows = list(ns.get("rows") or [])
        hk = str(ns.get("highlight_key") or "")
        cells = rn.parts.get("cells") or []
        for ri, row in enumerate(rows):
            if ri >= len(cells):
                break
            hi = bool(hk and row.get(hk))
            for ci, col in enumerate(columns):
                if ci >= len(cells[ri]):
                    break
                val = row.get(col.get("key"))
                cells[ri][ci].config(text="" if val is None else str(val),
                                     fg=(pal["gold"] if hi else pal["value"]),
                                     font=("Segoe UI", 9, "bold" if hi else "normal"))
        if "title" in rn.parts:
            rn.parts["title"].config(text=str(ns.get("title") or ""))

    def _build_canvas(self, parent, ns, pal) -> _RNode:
        w, h = _finite_int(ns.get("width"), 1, lo=1), _finite_int(ns.get("height"), 1, lo=1)
        bg = _canvas_fill(ns.get("bg"), pal, pal["body"]) or pal["body"]
        cv = tk.Canvas(parent, width=w, height=h, bg=bg, highlightthickness=0, bd=0)
        _draw_canvas_ops(cv, ns, pal)
        return _RNode("canvas", cv, spec=ns, pack_kw={"fill": "x", "pady": 4})

    def _update_canvas(self, rn, ns, pal) -> None:
        w, h = _finite_int(ns.get("width"), 1, lo=1), _finite_int(ns.get("height"), 1, lo=1)
        try:
            rn.widget.config(width=w, height=h,
                             bg=_canvas_fill(ns.get("bg"), pal, pal["body"]) or pal["body"])
        except Exception:
            pass
        _draw_canvas_ops(rn.widget, ns, pal)

    # ── container builder / updater ────────────────────────────────────────
    def _build_container(self, parent, ns, pal) -> _RNode:
        t = str(ns.get("type") or "")
        framed = t in ("section", "card")
        accent = _bar_color(str(ns.get("accent") or "cyan"), pal)
        box = tk.Frame(parent, bg=pal["body"], highlightthickness=(1 if framed else 0),
                       highlightbackground=pal["border"])
        parts: dict = {}
        if framed:
            shell = tk.Frame(box, bg=pal["body"])
            shell.pack(fill="x")
            stripe = tk.Frame(shell, bg=accent, width=3)
            stripe.pack(side="left", fill="y")
            content = tk.Frame(shell, bg=pal["body"])
            content.pack(side="left", fill="x", expand=True, padx=(7, 8), pady=(2, 6))
            parts["accent"] = stripe
        else:
            content = box
        title = str(ns.get("title") or "")
        if title:
            head = tk.Frame(content, bg=pal["body"])
            head.pack(fill="x", pady=(3, 2))
            hstripe = tk.Frame(head, bg=accent, width=3, height=14)
            hstripe.pack(side="left", padx=(0, 6))
            tlbl = tk.Label(head, text=title, bg=pal["body"], fg=pal["gold"], anchor="w",
                            font=("Segoe UI", 10, "bold"))
            tlbl.pack(side="left")
            parts["title"] = tlbl
            parts["title_stripe"] = hstripe
        is_row = (t == "row")
        if is_row:
            inner = tk.Frame(content, bg=content["bg"])
            inner.pack(anchor=_ANCHOR.get(str(ns.get("align") or "left"), "w"))
            child_mount = inner
            parts["inner"] = inner
        else:
            child_mount = content
        parts["content"] = content
        parts["child_mount"] = child_mount
        children = self._reconcile(child_mount, None, ns.get("children") or [], pal, is_row)
        return _RNode(t, box, parts=parts, children=children, spec=ns,
                      pack_kw={"fill": "x", "pady": (4 if framed else 0), "padx": (2 if framed else 0)})

    def _update_container(self, rn, ns, pal) -> None:
        accent = _bar_color(str(ns.get("accent") or "cyan"), pal)
        if "accent" in rn.parts:
            try:
                rn.parts["accent"].config(bg=accent)
            except Exception:
                pass
        if "title_stripe" in rn.parts:
            try:
                rn.parts["title_stripe"].config(bg=accent)
            except Exception:
                pass
        if "title" in rn.parts:
            try:
                rn.parts["title"].config(text=str(ns.get("title") or ""))
            except Exception:
                pass
        is_row = (rn.type == "row")
        if is_row and "inner" in rn.parts:
            try:
                rn.parts["inner"].pack_configure(anchor=_ANCHOR.get(str(ns.get("align") or "left"), "w"))
            except Exception:
                pass
        rn.children = self._reconcile(rn.parts["child_mount"], rn.children,
                                      ns.get("children") or [], pal, is_row)


def render_spec_into(parent: tk.Misc, spec: Any,
                     on_action: Optional[Callable[[str, dict], None]] = None) -> None:
    """One-shot: clear ``parent`` and render a normalized plugin UI spec into it."""
    for child in list(parent.winfo_children()):
        try:
            child.destroy()
        except Exception:
            pass
    try:
        SpecRenderer(parent, on_action).render(spec or {})
    except Exception:
        tk.Label(parent, text="[render error]", bg=parent["bg"], fg=_BAD, font=_MONO).pack(anchor="w")


# ── auto-redrawing panel list (manager "Panels" tab) ─────────────────────────

class PluginPanelList:
    """A scrollable, auto-redrawing list of plugin UI panels.

    Each panel keeps its own :class:`SpecRenderer`, so a redraw reconciles in
    place (smooth, no flicker, buttons stay live). The card scaffolding is only
    rebuilt when the *set* of shown panels changes.
    """

    def __init__(self, parent: tk.Misc, owner: Any, *, interval_ms: int = 700) -> None:
        self.parent = parent
        self.owner = owner
        self.interval_ms = max(150, int(interval_ms))
        self._dirty = True
        self._after_id: Optional[str] = None
        self._sub_token = ""
        #: panel ids currently shown — a redraw targeted at a panel we don't list
        #: (a plugin's hidden/animating viz window) must not rebuild us.
        self._shown_ids: set = set()
        #: pid -> (card_frame, SpecRenderer)
        self._cards: dict = {}
        self._card_order: list = []
        self._empty_lbl: Optional[tk.Widget] = None
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

            def _on_invalidate(event):
                payload = (event or {}).get("payload") or {}
                surface = str(payload.get("surface") or payload.get("panel_id") or "")
                if surface and surface not in self._shown_ids:
                    return
                self._dirty = True

            self._sub_token = bus.subscribe(
                "plugin_ui_invalidate", _on_invalidate, owner_id="entity_plugin_panel_list")
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

    def _clear_cards(self) -> None:
        for _pid, (card, _r) in self._cards.items():
            try:
                card.destroy()
            except Exception:
                pass
        self._cards = {}
        self._card_order = []

    def _rebuild(self) -> None:
        list_panels, render_panel, _ = self._runtime()
        pal = _pal()
        try:
            panels = (list_panels(self.owner) or {}).get("panels") or []
        except Exception:
            panels = []
        # Only top-level panels show here; ``hidden`` sub-panels are summoned by
        # their plugin (ctx.open_window), not listed as separate entries.
        active = [p for p in panels if p.get("available") and not p.get("hidden")]
        order = [str(p.get("id") or "") for p in active]
        self._shown_ids = set(order)

        if not active:
            self._clear_cards()
            if self._empty_lbl is None or not self._empty_lbl.winfo_exists():
                self._empty_lbl = tk.Label(
                    self._frame, text="无插件 UI 面板\n启用注册了 register_ui_panel 的插件后显示在这里。",
                    bg=self._frame["bg"], fg=pal["label"], justify="center",
                    font=("Segoe UI", 10), pady=24)
                self._empty_lbl.pack(fill="x")
            return
        if self._empty_lbl is not None:
            try:
                self._empty_lbl.destroy()
            except Exception:
                pass
            self._empty_lbl = None

        # Rebuild card scaffolding only when the set/order of panels changes.
        if order != self._card_order:
            self._clear_cards()
            self._card_order = order
            for pid in order:
                card = tk.Frame(self._frame, bg=pal["body"], highlightthickness=1,
                                highlightbackground=pal["border"])
                card.pack(fill="x", pady=6, padx=2)
                body = tk.Frame(card, bg=pal["body"])
                body.pack(fill="x", padx=10, pady=8)
                self._cards[pid] = (card, SpecRenderer(body, self._on_action(pid)))

        for pid in order:
            entry = self._cards.get(pid)
            if not entry:
                continue
            _card, renderer = entry
            renderer.set_on_action(self._on_action(pid))
            try:
                spec = (render_panel(self.owner, pid) or {}).get("spec") or {}
            except Exception:
                spec = {"version": 1, "title": pid, "nodes": []}
            try:
                renderer.render(spec)
            except Exception:
                pass


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
        self._renderer = SpecRenderer(self.frame, None)
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
            from act_platform.runtime import render_overlays
            overlays = (render_overlays(self.owner, self.surface) or {}).get("overlays") or []
        except Exception:
            overlays = []
        if not overlays:
            self._renderer.render({"version": 1, "title": "", "nodes": []})
            if self._visible:
                self.frame.pack_forget()
                self._visible = False
            return
        # Merge all overlay specs into one node list (first title wins) and
        # reconcile in place — overlays no longer flicker on every poll.
        merged_nodes: list = []
        merged_title = ""
        for ov in overlays:
            spec = ov.get("spec") or {}
            merged_title = merged_title or str(spec.get("title") or "")
            merged_nodes.extend(list(spec.get("nodes") or []))
        self._renderer.render({"version": 1, "title": merged_title, "nodes": merged_nodes})
        if not self._visible:
            self.frame.pack(fill="x", side="top")
            self._visible = True


__all__ = ["SpecRenderer", "render_spec_into", "PluginPanelList",
           "attach_overlay_layer", "OverlayLayer"]
