# -*- coding: utf-8 -*-
"""Declarative ACT plugin UI spec.

A *render spec* is plain JSON-safe data describing a small UI tree.  Both the
Entity (Tk) renderer in ``gui_modules.sao_plugin_ui_render`` and the WebView
renderer in ``web/plugin_layer.js`` consume the **same** normalized spec so a
plugin draws identically on both surfaces.

Plugins build specs with the :class:`UI` helper (exposed as ``ctx.ui``)::

    return ctx.ui.panel("Live Metrics", [
        ctx.ui.bar("Primary Meter", pct=0.62, color="bad", caption="3.1 / 5.0"),
        ctx.ui.table(
            columns=[{"key": "name", "title": "Name"}, {"key": "value", "title": "Value", "align": "right"}],
            rows=[{"name": "Sample", "value": "1.2M"}],
            highlight_key="is_self",
        ),
        ctx.ui.button("Reset", action="reset", style="danger"),
    ])

The host never trusts plugin output blindly: :func:`normalize_ui_spec` clamps
sizes, drops unknown node types, escapes/limits text and bounds recursion so a
misbehaving plugin cannot bloat or crash the UI thread.
"""

from __future__ import annotations

import re
from typing import Any, Iterable, List, Mapping, Optional

UI_SPEC_VERSION = 1

# Safety bounds — a plugin spec that exceeds these is truncated, never raised.
MAX_DEPTH = 8
MAX_NODES = 400
MAX_TABLE_ROWS = 200
MAX_TABLE_COLS = 16
MAX_TEXT_LEN = 4000
MAX_TITLE_LEN = 200
MAX_CANVAS_OPS = 4000
MAX_CANVAS_DIM = 4096
MAX_LAYER_POS = 32768
MAX_LAYER_Z = 10000

# Style tokens understood by *both* renderers (Tk maps to colors, web to CSS classes).
TEXT_STYLES = (
    "title", "subtitle", "value", "label", "muted",
    "ok", "warn", "bad", "gold", "accent", "mono",
)
BADGE_STYLES = ("muted", "ok", "warn", "bad", "gold", "accent")
BUTTON_STYLES = ("default", "primary", "danger", "ghost")
BAR_COLORS = ("cyan", "gold", "ok", "warn", "bad", "accent", "heal")
ALIGNS = ("left", "center", "right")
INPUT_TYPES = ("text", "number", "password")
MAX_INPUT_VAL = 2000

# Container + leaf node kinds.
CONTAINER_KINDS = ("panel", "section", "card", "row", "group")
LEAF_KINDS = (
    "text", "kv", "bar", "badge", "divider", "spacer",
    "button", "input", "slider", "table", "canvas", "rgba_frame",
)
NODE_KINDS = CONTAINER_KINDS + LEAF_KINDS

# Free-form 2D drawing (piano keyboards, note rolls, meters…). Ops are a tiny
# fixed vocabulary both renderers understand; colors are theme tokens or #hex.
CANVAS_OPS = ("rect", "oval", "line", "text")
_HEX_RE = re.compile(r"^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$")
CANVAS_COLOR_TOKENS = set(TEXT_STYLES) | set(BAR_COLORS) | {
    "white", "black", "bg", "body", "border", "sep", "grid", "header", "transparent",
}
CANVAS_ANCHORS = ("nw", "n", "ne", "w", "center", "e", "sw", "s", "se")
def _s(value: Any, limit: int = MAX_TEXT_LEN) -> str:
    text = "" if value is None else str(value)
    if len(text) > limit:
        text = text[: limit - 1] + "…"
    return text


def _clamp01(value: Any) -> float:
    try:
        num = float(value)
    except Exception:
        return 0.0
    if num != num:  # NaN
        return 0.0
    return max(0.0, min(1.0, num))


def _choice(value: Any, allowed: Iterable[str], default: str) -> str:
    text = str(value or "").strip().lower()
    return text if text in allowed else default


def _json_scalar(value: Any) -> Any:
    if isinstance(value, bool) or value is None:
        return value
    if isinstance(value, (int, float)):
        return value
    return _s(value)


def _ci(value: Any, default: int = 0) -> int:
    try:
        num = float(value)
        if num != num:  # NaN
            return default
        return int(round(num))
    except Exception:
        return default


def _cpos(value: Any, default: int = 0) -> int:
    return max(-MAX_LAYER_POS, min(MAX_LAYER_POS, _ci(value, default)))


def _cz(value: Any, default: int = 0) -> int:
    return max(-MAX_LAYER_Z, min(MAX_LAYER_Z, _ci(value, default)))


def _canvas_color(value: Any, default: str = "") -> str:
    """A canvas color: ``#rgb``/``#rrggbb`` passes through; else a theme token."""
    text = str(value or "").strip()
    if not text:
        return default
    if _HEX_RE.match(text):
        return text
    low = text.lower()
    return low if low in CANVAS_COLOR_TOKENS else default


def _normalize_canvas(node: Mapping[str, Any]) -> dict:
    width = max(1, min(MAX_CANVAS_DIM, _ci(node.get("width"), 320)))
    height = max(1, min(MAX_CANVAS_DIM, _ci(node.get("height"), 160)))
    ops: List[dict] = []
    raw_ops = node.get("ops")
    if isinstance(raw_ops, (list, tuple)):
        for op in list(raw_ops)[:MAX_CANVAS_OPS]:
            if not isinstance(op, Mapping):
                continue
            kind = str(op.get("op") or "").strip().lower()
            if kind in ("rect", "oval"):
                ops.append({
                    "op": kind,
                    "x": _ci(op.get("x")), "y": _ci(op.get("y")),
                    "w": max(0, _ci(op.get("w"))), "h": max(0, _ci(op.get("h"))),
                    "fill": _canvas_color(op.get("fill")),
                    "outline": _canvas_color(op.get("outline")),
                    "width": max(0, min(20, _ci(op.get("width"), 0))),
                })
            elif kind == "line":
                ops.append({
                    "op": "line",
                    "x1": _ci(op.get("x1")), "y1": _ci(op.get("y1")),
                    "x2": _ci(op.get("x2")), "y2": _ci(op.get("y2")),
                    "fill": _canvas_color(op.get("fill"), "value"),
                    "width": max(1, min(20, _ci(op.get("width"), 1))),
                })
            elif kind == "text":
                ops.append({
                    "op": "text",
                    "x": _ci(op.get("x")), "y": _ci(op.get("y")),
                    "text": _s(op.get("text"), 200),
                    "fill": _canvas_color(op.get("fill"), "value"),
                    "size": max(6, min(48, _ci(op.get("size"), 10))),
                    "anchor": _choice(op.get("anchor"), CANVAS_ANCHORS, "nw"),
                    "bold": bool(op.get("bold")),
                })
    return {
        "type": "canvas",
        "id": _s(node.get("id"), 120),
        "x": _cpos(node.get("x"), 0),
        "y": _cpos(node.get("y"), 0),
        "z": _cz(node.get("z"), 0),
        "width": width,
        "height": height,
        "draggable": bool(node.get("draggable", False)),
        "bg": _canvas_color(node.get("bg"), "body"),
        "ops": ops,
    }


def _normalize_rgba_frame(node: Mapping[str, Any]) -> dict:
    width = max(1, min(MAX_CANVAS_DIM, _ci(node.get("width"), 320)))
    height = max(1, min(MAX_CANVAS_DIM, _ci(node.get("height"), 480)))
    return {
        "type": "rgba_frame",
        "id": _s(node.get("id"), 120),
        "x": _cpos(node.get("x"), 0),
        "y": _cpos(node.get("y"), 0),
        "z": _cz(node.get("z"), 0),
        "width": width,
        "height": height,
        "draggable": bool(node.get("draggable", True)),
        "hit_test": _choice(node.get("hit_test"), ("none", "rect", "alpha"), "rect"),
        "frame_rgba_b64": _s(node.get("frame_rgba_b64"), width * height * 8),
        "frame_key": _s(node.get("frame_key"), 240),
        "premultiplied": bool(node.get("premultiplied", False)),
        "background": _s(node.get("background") or "transparent", 40),
        "diagnostic": _s(node.get("diagnostic"), 500),
    }


# ── Normalization ────────────────────────────────────────────────────────────

def _normalize_children(children: Any, depth: int, budget: list[int]) -> List[dict]:
    out: List[dict] = []
    if not isinstance(children, (list, tuple)):
        return out
    for child in children:
        if budget[0] <= 0:
            break
        node = _normalize_node(child, depth + 1, budget)
        if node is not None:
            out.append(node)
    return out


def _normalize_table(node: Mapping[str, Any]) -> dict:
    raw_cols = node.get("columns")
    columns: List[dict] = []
    if isinstance(raw_cols, (list, tuple)):
        for col in list(raw_cols)[:MAX_TABLE_COLS]:
            if isinstance(col, Mapping):
                key = _s(col.get("key") or col.get("id"), 80)
                if not key:
                    continue
                columns.append({
                    "key": key,
                    "title": _s(col.get("title") or key, 80),
                    "align": _choice(col.get("align"), ALIGNS, "left"),
                })
            else:
                key = _s(col, 80)
                if key:
                    columns.append({"key": key, "title": key, "align": "left"})
    highlight_key = _s(node.get("highlight_key"), 80)
    raw_rows = node.get("rows")
    rows: List[dict] = []
    if isinstance(raw_rows, (list, tuple)):
        col_keys = [col["key"] for col in columns] or None
        for row in list(raw_rows)[:MAX_TABLE_ROWS]:
            if not isinstance(row, Mapping):
                continue
            if col_keys is not None:
                keys = list(col_keys)
                # Keep the highlight flag even when it is not a displayed column
                # (otherwise row highlighting silently never fires), but only if
                # the row actually carries it — don't inject spurious None cells.
                if highlight_key and highlight_key not in keys and highlight_key in row:
                    keys.append(highlight_key)
            else:
                keys = list(row.keys())[:MAX_TABLE_COLS]
            rows.append({str(k): _json_scalar(row.get(k)) for k in keys})
    return {
        "type": "table",
        "columns": columns,
        "rows": rows,
        "highlight_key": highlight_key,
        "title": _s(node.get("title"), MAX_TITLE_LEN),
    }


def _normalize_node(node: Any, depth: int, budget: list[int]) -> Optional[dict]:
    if budget[0] <= 0 or depth > MAX_DEPTH:
        return None
    if not isinstance(node, Mapping):
        # Bare strings become muted text — convenient for quick specs.
        text = _s(node)
        if not text:
            return None
        budget[0] -= 1
        return {"type": "text", "text": text, "style": "value", "align": "left"}
    kind = str(node.get("type") or "").strip().lower()
    if kind not in NODE_KINDS:
        return None
    budget[0] -= 1

    if kind in CONTAINER_KINDS:
        out = {"type": kind, "children": _normalize_children(node.get("children"), depth, budget)}
        title = node.get("title")
        if title is not None:
            out["title"] = _s(title, MAX_TITLE_LEN)
        if kind == "row":
            out["align"] = _choice(node.get("align"), ALIGNS, "left")
        if node.get("accent") is not None:
            out["accent"] = _choice(node.get("accent"), BAR_COLORS, "cyan")
        return out

    if kind == "text":
        return {
            "type": "text",
            "text": _s(node.get("text")),
            "style": _choice(node.get("style"), TEXT_STYLES, "value"),
            "align": _choice(node.get("align"), ALIGNS, "left"),
        }
    if kind == "kv":
        return {
            "type": "kv",
            "label": _s(node.get("label"), 200),
            "value": _s(node.get("value"), 400),
            "style": _choice(node.get("style"), TEXT_STYLES, "value"),
        }
    if kind == "bar":
        d = {
            "type": "bar",
            "label": _s(node.get("label"), 200),
            "pct": _clamp01(node.get("pct")),
            "color": _choice(node.get("color"), BAR_COLORS, "cyan"),
            "caption": _s(node.get("caption"), 200),
        }
        if node.get("action"):
            d["action"] = _s(node["action"], 120)
            d["lo"] = float(node.get("lo", 0.0))
            d["hi"] = float(node.get("hi", 1.0))
            d["step"] = float(node.get("step", 0.0))
        return d
    if kind == "slider":
        return {
            "type": "slider",
            "label": _s(node.get("label"), 200),
            "id": _s(node.get("id"), 80),
            "value": float(node.get("value", 0)),
            "lo": float(node.get("lo", 0)),
            "hi": float(node.get("hi", 1)),
            "step": float(node.get("step", 0.01)),
            "color": _choice(node.get("color"), BAR_COLORS, "cyan"),
        }
    if kind == "badge":
        return {
            "type": "badge",
            "text": _s(node.get("text"), 120),
            "style": _choice(node.get("style"), BADGE_STYLES, "muted"),
        }
    if kind == "divider":
        return {"type": "divider"}
    if kind == "spacer":
        try:
            size = int(node.get("size") or 8)
        except Exception:
            size = 8
        return {"type": "spacer", "size": max(0, min(64, size))}
    if kind == "button":
        out = {
            "type": "button",
            "label": _s(node.get("label"), 120),
            "action": _s(node.get("action"), 120),
            "style": _choice(node.get("style"), BUTTON_STYLES, "default"),
        }
        payload = node.get("payload")
        if isinstance(payload, Mapping):
            out["payload"] = {str(k): _json_scalar(v) for k, v in list(payload.items())[:32]}
        if node.get("disabled"):
            out["disabled"] = True
        return out
    if kind == "input":
        return {
            "type": "input",
            "id": _s(node.get("id"), 80),
            "value": _s(node.get("value"), MAX_INPUT_VAL),
            "placeholder": _s(node.get("placeholder"), 200),
            "input_type": _choice(node.get("input_type"), INPUT_TYPES, "text"),
            "width": max(0, min(2000, _ci(node.get("width"), 0))),
        }
    if kind == "table":
        return _normalize_table(node)
    if kind == "canvas":
        return _normalize_canvas(node)
    if kind == "rgba_frame":
        return _normalize_rgba_frame(node)
    return None


def normalize_ui_spec(spec: Any, *, title: str = "") -> dict:
    """Return a canonical, bounded, JSON-safe render spec.

    Accepts a full ``{"nodes": [...]}`` mapping, a single node mapping, or a
    bare list of nodes.  Always returns ``{"version", "title", "nodes"}``.
    """
    budget = [MAX_NODES]
    nodes: List[dict] = []
    spec_title = title

    if isinstance(spec, Mapping):
        if spec.get("title") is not None:
            spec_title = spec.get("title")
        if isinstance(spec.get("nodes"), (list, tuple)):
            nodes = _normalize_children(spec.get("nodes"), 0, budget)
        elif isinstance(spec.get("children"), (list, tuple)):
            nodes = _normalize_children(spec.get("children"), 0, budget)
        elif str(spec.get("type") or "") in NODE_KINDS:
            node = _normalize_node(spec, 0, budget)
            if node is not None:
                # A top-level ``panel`` is unwrapped: its title becomes the spec
                # title and its children become the spec nodes.  Any other single
                # node (section/card/leaf) is kept intact so its own chrome shows.
                if node["type"] == "panel":
                    spec_title = spec_title or node.get("title") or ""
                    nodes = list(node.get("children") or [])
                else:
                    nodes = [node]
    elif isinstance(spec, (list, tuple)):
        nodes = _normalize_children(spec, 0, budget)

    return {
        "version": UI_SPEC_VERSION,
        "title": _s(spec_title, MAX_TITLE_LEN),
        "nodes": nodes,
    }


def is_ui_spec(value: Any) -> bool:
    return isinstance(value, Mapping) and (
        "nodes" in value or "children" in value or str(value.get("type") or "") in NODE_KINDS
    )


# ── Builder DSL (exposed to plugins as ``ctx.ui``) ───────────────────────────

class UI:
    """Tiny builder returning plain spec dicts.  All methods are static."""

    @staticmethod
    def panel(title: Any = "", children: Optional[Iterable[Any]] = None) -> dict:
        return {"type": "panel", "title": _s(title, MAX_TITLE_LEN), "children": list(children or [])}

    @staticmethod
    def section(title: Any = "", children: Optional[Iterable[Any]] = None,
                accent: str = "cyan") -> dict:
        return {"type": "section", "title": _s(title, MAX_TITLE_LEN),
                "accent": accent, "children": list(children or [])}

    @staticmethod
    def card(title: Any = "", children: Optional[Iterable[Any]] = None) -> dict:
        return {"type": "card", "title": _s(title, MAX_TITLE_LEN), "children": list(children or [])}

    @staticmethod
    def row(children: Optional[Iterable[Any]] = None, align: str = "left") -> dict:
        return {"type": "row", "align": align, "children": list(children or [])}

    @staticmethod
    def group(children: Optional[Iterable[Any]] = None) -> dict:
        return {"type": "group", "children": list(children or [])}

    @staticmethod
    def text(text: Any, style: str = "value", align: str = "left") -> dict:
        return {"type": "text", "text": _s(text), "style": style, "align": align}

    @staticmethod
    def title(text: Any) -> dict:
        return {"type": "text", "text": _s(text), "style": "title", "align": "left"}

    @staticmethod
    def kv(label: Any, value: Any, style: str = "value") -> dict:
        return {"type": "kv", "label": _s(label, 200), "value": _s(value, 400), "style": style}

    @staticmethod
    def bar(label: Any = "", pct: float = 0.0, color: str = "cyan", caption: Any = "") -> dict:
        return {"type": "bar", "label": _s(label, 200), "pct": _clamp01(pct),
                "color": color, "caption": _s(caption, 200)}

    @staticmethod
    def slider(label: Any = "", id: str = "", value: float = 0.0,
               lo: float = 0.0, hi: float = 1.0, step: float = 0.01,
               color: str = "cyan") -> dict:
        return {"type": "slider", "label": _s(label, 200), "id": _s(id, 80),
                "value": float(value), "lo": float(lo), "hi": float(hi),
                "step": float(step), "color": color}

    @staticmethod
    def badge(text: Any, style: str = "muted") -> dict:
        return {"type": "badge", "text": _s(text, 120), "style": style}

    @staticmethod
    def divider() -> dict:
        return {"type": "divider"}

    @staticmethod
    def spacer(size: int = 8) -> dict:
        return {"type": "spacer", "size": size}

    @staticmethod
    def button(label: Any, action: Any, style: str = "default",
               payload: Optional[Mapping[str, Any]] = None, disabled: bool = False) -> dict:
        node = {"type": "button", "label": _s(label, 120), "action": _s(action, 120), "style": style}
        if payload:
            node["payload"] = dict(payload)
        if disabled:
            node["disabled"] = True
        return node

    @staticmethod
    def input(id: Any, value: Any = "", placeholder: Any = "",
              input_type: str = "text", width: int = 0) -> dict:
        """A text field. ``id`` keys the value into ``payload["inputs"]`` when a
        sibling button fires. ``input_type``: text/number/password. ``width`` px
        (0 = flex). Rendered identically on Tk and WebView."""
        return {"type": "input", "id": _s(id, 80), "value": _s(value, MAX_INPUT_VAL),
                "placeholder": _s(placeholder, 200), "input_type": input_type,
                "width": max(0, min(2000, _ci(width, 0)))}

    @staticmethod
    def table(columns: Optional[Iterable[Any]] = None, rows: Optional[Iterable[Any]] = None,
              highlight_key: str = "", title: Any = "") -> dict:
        return {"type": "table", "columns": list(columns or []), "rows": list(rows or []),
                "highlight_key": _s(highlight_key, 80), "title": _s(title, MAX_TITLE_LEN)}

    # ── free-form 2D drawing (piano keyboards, note rolls, meters…) ──────────
    @staticmethod
    def canvas(width: int, height: int, ops: Optional[Iterable[Any]] = None,
               bg: str = "body", x: int = 0, y: int = 0, z: int = 0,
               id: Any = "", draggable: bool = False) -> dict:
        """A drawing surface. ``ops`` are op dicts (see :meth:`rect`/:meth:`line`/
        :meth:`ctext`); colors are theme tokens (accent/gold/ok/white/black/…) or
        ``#hex``. Rendered identically on Tk and WebView."""
        return {"type": "canvas",
                "id": _s(id, 120),
                "x": _cpos(x, 0), "y": _cpos(y, 0), "z": _cz(z, 0),
                "width": max(1, min(MAX_CANVAS_DIM, _ci(width, 320))),
                "height": max(1, min(MAX_CANVAS_DIM, _ci(height, 160))),
                "draggable": bool(draggable),
                "bg": bg, "ops": list(ops or [])}

    @staticmethod
    def rgba_frame(id: Any, width: int, height: int, frame_rgba_b64: Any,
                   x: int = 0, y: int = 0, z: int = 0,
                   frame_key: Any = "", draggable: bool = True,
                   hit_test: str = "alpha", premultiplied: bool = False) -> dict:
        """A pre-rendered RGBA8888 overlay frame supplied by a plugin.

        The platform only decodes and composites these pixels; it does not
        inspect model, mesh, or animation data.
        """
        return {
            "type": "rgba_frame",
            "id": _s(id, 120),
            "x": _cpos(x, 0),
            "y": _cpos(y, 0),
            "z": _cz(z, 0),
            "width": max(1, min(MAX_CANVAS_DIM, _ci(width, 320))),
            "height": max(1, min(MAX_CANVAS_DIM, _ci(height, 480))),
            "draggable": bool(draggable),
            "hit_test": hit_test,
            "frame_rgba_b64": _s(frame_rgba_b64, max(0, _ci(width, 320) * _ci(height, 480) * 8)),
            "frame_key": _s(frame_key, 240),
            "premultiplied": bool(premultiplied),
        }

    @staticmethod
    def rect(x, y, w, h, fill: str = "", outline: str = "", width: int = 0) -> dict:
        return {"op": "rect", "x": x, "y": y, "w": w, "h": h,
                "fill": fill, "outline": outline, "width": width}

    @staticmethod
    def oval(x, y, w, h, fill: str = "", outline: str = "", width: int = 0) -> dict:
        return {"op": "oval", "x": x, "y": y, "w": w, "h": h,
                "fill": fill, "outline": outline, "width": width}

    @staticmethod
    def line(x1, y1, x2, y2, fill: str = "value", width: int = 1) -> dict:
        return {"op": "line", "x1": x1, "y1": y1, "x2": x2, "y2": y2, "fill": fill, "width": width}

    @staticmethod
    def ctext(x, y, text: Any, fill: str = "value", size: int = 10,
              anchor: str = "nw", bold: bool = False) -> dict:
        return {"op": "text", "x": x, "y": y, "text": _s(text, 200),
                "fill": fill, "size": size, "anchor": anchor, "bold": bool(bold)}


__all__ = [
    "UI",
    "UI_SPEC_VERSION",
    "NODE_KINDS",
    "TEXT_STYLES",
    "BADGE_STYLES",
    "BUTTON_STYLES",
    "BAR_COLORS",
    "normalize_ui_spec",
    "is_ui_spec",
]
