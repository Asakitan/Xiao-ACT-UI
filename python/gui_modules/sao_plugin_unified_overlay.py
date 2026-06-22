# -*- coding: utf-8 -*-
from __future__ import annotations

import json
from collections.abc import Mapping
from typing import Any, Optional

try:
    from PIL import Image, ImageColor, ImageDraw, ImageFont
except Exception:
    Image = None  # type: ignore[assignment]
    ImageColor = None  # type: ignore[assignment]
    ImageDraw = None  # type: ignore[assignment]
    ImageFont = None  # type: ignore[assignment]

try:
    from _sao_cy_pixels import premultiply_bgra_bytes_floor as _premultiply_bgra_bytes_floor
except Exception:
    _premultiply_bgra_bytes_floor = None  # type: ignore[assignment]

try:
    from render.overlay_adapter import CompositorBgraPresenter, create_overlay_window
except Exception:
    CompositorBgraPresenter = None  # type: ignore[assignment]
    create_overlay_window = None  # type: ignore[assignment]

from act_platform.runtime import ensure_act_event_bus, render_overlays
from act_platform.ui_spec import normalize_ui_spec
from gui_modules.sao_plugin_ui_render import _canvas_fill, _pal

_FONT_CACHE: dict[tuple[int, bool], Any] = {}
_FONT_FILES = {
    False: ("msyh.ttc", "segoeui.ttf", "arial.ttf"),
    True: ("msyhbd.ttc", "segoeuib.ttf", "arialbd.ttf", "msyh.ttc"),
}
_CONTAINER_TYPES = {"panel", "section", "card", "row", "group"}


def _load_font(size: int, bold: bool) -> Any:
    key = (int(size), bool(bold))
    cached = _FONT_CACHE.get(key)
    if cached is not None:
        return cached
    if ImageFont is None:
        return None
    for filename in _FONT_FILES[bool(bold)]:
        try:
            font = ImageFont.truetype(filename, size=max(6, int(size)))
            _FONT_CACHE[key] = font
            return font
        except Exception:
            continue
    try:
        font = ImageFont.load_default()
    except Exception:
        font = None
    _FONT_CACHE[key] = font
    return font


def _color_to_rgba(value: Any, pal: Mapping[str, Any], default: str = "") -> tuple[int, int, int, int] | None:
    if ImageColor is None:
        return None
    concrete = _canvas_fill(value, dict(pal), default)
    if not concrete:
        return None
    try:
        rgb = ImageColor.getrgb(str(concrete))
    except Exception:
        return None
    return int(rgb[0]), int(rgb[1]), int(rgb[2]), 255


def _background_to_rgba(value: Any, pal: Mapping[str, Any]) -> tuple[int, int, int, int] | None:
    token = str(value or "").strip().lower()
    if token in ("", "transparent", "body", "bg"):
        return None
    return _color_to_rgba(value, pal, "")


def _text_size(draw: Any, text: str, font: Any) -> tuple[int, int]:
    if not text:
        return 0, 0
    try:
        left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
        return max(0, int(right - left)), max(0, int(bottom - top))
    except Exception:
        try:
            return font.getsize(text)
        except Exception:
            return max(0, len(text) * 8), 12


def _anchor_xy(draw: Any, text: str, font: Any, x: int, y: int, anchor: str) -> tuple[int, int]:
    width, height = _text_size(draw, text, font)
    ax = str(anchor or "nw").lower()
    px = int(x)
    py = int(y)
    if "e" in ax:
        px -= width
    elif ax in ("n", "center", "s"):
        px -= width // 2
    if "s" in ax:
        py -= height
    elif ax in ("w", "center", "e"):
        py -= height // 2
    return px, py


def _draw_canvas(draw: Any, canvas_node: Mapping[str, Any], pal: Mapping[str, Any], width: int, height: int) -> None:
    bg = _background_to_rgba(canvas_node.get("bg"), pal)
    if bg is not None:
        draw.rectangle((0, 0, width, height), fill=bg)
    for op in canvas_node.get("ops") or []:
        if not isinstance(op, Mapping):
            continue
        kind = str(op.get("op") or "").lower()
        if kind in ("rect", "oval"):
            x = int(op.get("x") or 0)
            y = int(op.get("y") or 0)
            w = max(0, int(op.get("w") or 0))
            h = max(0, int(op.get("h") or 0))
            fill = _color_to_rgba(op.get("fill"), pal, "")
            outline = _color_to_rgba(op.get("outline"), pal, "")
            line_w = max(0, int(op.get("width") or 0))
            box = (x, y, x + w, y + h)
            if kind == "rect":
                draw.rectangle(box, fill=fill, outline=outline, width=line_w)
            else:
                draw.ellipse(box, fill=fill, outline=outline, width=line_w)
        elif kind == "line":
            fill = _color_to_rgba(op.get("fill"), pal, "value") or (255, 255, 255, 255)
            draw.line(
                (
                    int(op.get("x1") or 0),
                    int(op.get("y1") or 0),
                    int(op.get("x2") or 0),
                    int(op.get("y2") or 0),
                ),
                fill=fill,
                width=max(1, int(op.get("width") or 1)),
            )
        elif kind == "text":
            text = str(op.get("text") or "")
            if not text:
                continue
            fill = _color_to_rgba(op.get("fill"), pal, "value") or (255, 255, 255, 255)
            font = _load_font(int(op.get("size") or 10), bool(op.get("bold")))
            if font is None:
                continue
            tx, ty = _anchor_xy(
                draw,
                text,
                font,
                int(op.get("x") or 0),
                int(op.get("y") or 0),
                str(op.get("anchor") or "nw"),
            )
            draw.text((tx, ty), text, fill=fill, font=font)


def _iter_canvas_nodes(spec: Mapping[str, Any]) -> list[Mapping[str, Any]]:
    out: list[Mapping[str, Any]] = []
    stack = list(spec.get("nodes") or [])
    while stack:
        node = stack.pop(0)
        if not isinstance(node, Mapping):
            continue
        kind = str(node.get("type") or "").lower()
        if kind == "canvas":
            out.append(node)
            continue
        if kind in _CONTAINER_TYPES:
            stack[0:0] = list(node.get("children") or [])
    return out


def _compose_overlay_frame(overlays: list[Mapping[str, Any]]) -> tuple[str, bytes, int, int] | None:
    if Image is None or ImageDraw is None:
        return None
    pal = _pal()
    canvas_nodes: list[Mapping[str, Any]] = []
    for entry in overlays:
        spec = entry.get("spec") if isinstance(entry, Mapping) else None
        normalized = normalize_ui_spec(spec if isinstance(spec, Mapping) else {})
        canvas_nodes.extend(_iter_canvas_nodes(normalized))
    if not canvas_nodes:
        return None

    width = max(max(1, int(node.get("width") or 1)) for node in canvas_nodes)
    height = max(max(1, int(node.get("height") or 1)) for node in canvas_nodes)
    signature = json.dumps(canvas_nodes, ensure_ascii=False, sort_keys=True, separators=(",", ":"))

    image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    for node in canvas_nodes:
        _draw_canvas(draw, node, pal, width, height)

    rgba = image.tobytes()
    if callable(_premultiply_bgra_bytes_floor):
        try:
            bgra = _premultiply_bgra_bytes_floor(rgba, height, width)
            return signature, bytes(bgra), width, height
        except Exception:
            pass

    out = bytearray(width * height * 4)
    src = memoryview(rgba)
    dst = memoryview(out)
    for idx in range(0, len(src), 4):
        r = src[idx]
        g = src[idx + 1]
        b = src[idx + 2]
        a = src[idx + 3]
        dst[idx] = (b * a + 127) // 255
        dst[idx + 1] = (g * a + 127) // 255
        dst[idx + 2] = (r * a + 127) // 255
        dst[idx + 3] = a
    return signature, bytes(out), width, height


class PluginUnifiedOverlayHost:
    """Bridge plugin canvas overlays into the platform unified GPU overlay.

    This host does not create a separate overlay stack. It creates one normal
    platform overlay layer via ``render.overlay_adapter`` and feeds merged
    plugin canvas frames into that layer. SAO/fisheye menu open events only
    hide this layer temporarily; plugin enabled/disabled state is untouched.
    """

    def __init__(self, owner: Any, *, surface: str = "unioverlay", interval_ms: int = 120) -> None:
        self.owner = owner
        self.root = getattr(owner, "root", None)
        self.surface = str(surface or "unioverlay")
        self.interval_ms = max(60, int(interval_ms))
        self._after_id: Any = None
        self._sub_token = ""
        self._dirty = True
        self._destroyed = False
        self._hidden = False
        self._has_frame = False
        self._last_signature = ""
        self._overlay_window: Any = None
        self._presenter: Any = None

    def start(self) -> bool:
        if self.root is None or create_overlay_window is None or CompositorBgraPresenter is None:
            return False
        self._subscribe()
        self._schedule(0)
        return True

    def destroy(self) -> None:
        if self._destroyed:
            return
        self._destroyed = True
        if self._after_id is not None and self.root is not None:
            try:
                self.root.after_cancel(self._after_id)
            except Exception:
                pass
            self._after_id = None
        if self._sub_token:
            try:
                ensure_act_event_bus(self.owner).unsubscribe(self._sub_token)
            except Exception:
                pass
            self._sub_token = ""
        self._presenter = None
        if self._overlay_window is not None:
            try:
                self._overlay_window.destroy()
            except Exception:
                pass
            self._overlay_window = None

    def hide_temporarily(self) -> None:
        self._hidden = True
        if self._overlay_window is not None:
            try:
                self._overlay_window.hide()
            except Exception:
                pass

    def restore(self) -> None:
        self._hidden = False
        self._dirty = True
        if self._overlay_window is not None and self._has_frame:
            try:
                self._overlay_window.show()
                self._overlay_window.request_redraw()
            except Exception:
                pass

    def mark_dirty(self) -> None:
        self._dirty = True

    def _subscribe(self) -> None:
        try:
            def _on_invalidate(event: Mapping[str, Any]) -> None:
                payload = event.get("payload") if isinstance(event, Mapping) else None
                if not isinstance(payload, Mapping):
                    self._dirty = True
                    return
                surface = str(payload.get("surface") or "")
                if not surface or surface == self.surface:
                    self._dirty = True

            self._sub_token = ensure_act_event_bus(self.owner).subscribe(
                "plugin_ui_invalidate",
                _on_invalidate,
                owner_id=f"plugin_unified_overlay_{self.surface}",
            )
        except Exception:
            self._sub_token = ""

    def _schedule(self, delay_ms: int | None = None) -> None:
        if self._destroyed or self.root is None:
            return
        if self._after_id is not None:
            return
        try:
            self._after_id = self.root.after(self.interval_ms if delay_ms is None else int(delay_ms), self._tick)
        except Exception:
            self._after_id = None

    def _ensure_window(self, width: int, height: int) -> bool:
        if self._overlay_window is not None and self._presenter is not None:
            return True
        if create_overlay_window is None or CompositorBgraPresenter is None:
            return False
        try:
            window = create_overlay_window(
                root=self.root,
                w=max(1, int(width)),
                h=max(1, int(height)),
                x=0,
                y=0,
                render_fn=None,
                click_through=True,
                title="sao_plugin_unified_overlay",
                z=140,
            )
            presenter = CompositorBgraPresenter(window.layer)
            if not self._hidden:
                window.show()
            self._presenter = presenter
            self._overlay_window = window
            return True
        except Exception:
            self._presenter = None
            self._overlay_window = None
            return False

    def _clear_and_hide(self) -> None:
        self._has_frame = False
        self._last_signature = ""
        if self._overlay_window is not None:
            try:
                self._overlay_window.request_redraw()
            except Exception:
                pass
            try:
                self._overlay_window.hide()
            except Exception:
                pass

    def _refresh_now(self) -> None:
        try:
            payload = render_overlays(self.owner, self.surface) or {}
            overlays = payload.get("overlays") or []
        except Exception:
            overlays = []
        if not overlays:
            self._clear_and_hide()
            return
        frame = _compose_overlay_frame(overlays)
        if frame is None:
            self._clear_and_hide()
            return
        signature, bgra, width, height = frame
        if signature == self._last_signature:
            if not self._hidden and self._overlay_window is not None and self._has_frame:
                try:
                    self._overlay_window.show()
                except Exception:
                    pass
            return
        if not self._ensure_window(width, height):
            return
        self._last_signature = signature
        self._has_frame = True
        try:
            self._overlay_window.set_geometry(0, 0, int(width), int(height))
            self._presenter.set_frame(bgra, int(width), int(height))
            if not self._hidden:
                self._overlay_window.show()
            else:
                self._overlay_window.hide()
            self._overlay_window.request_redraw()
        except Exception:
            self._clear_and_hide()

    def _tick(self) -> None:
        self._after_id = None
        if self._destroyed:
            return
        if self._dirty:
            self._dirty = False
            self._refresh_now()
        self._schedule()


__all__ = ["PluginUnifiedOverlayHost"]
