# -*- coding: utf-8 -*-
from __future__ import annotations

import json
import re
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
    from render.overlay_adapter import CompositorBgraPresenter
    from render.gpu_overlay_window import _get_unified_overlay as get_unified_overlay
except Exception:
    CompositorBgraPresenter = None  # type: ignore[assignment]
    get_unified_overlay = None  # type: ignore[assignment]

try:
    from render.model3d_backend import (
        diagnose_model3d_node,
        get_backend_status,
        model3d_node_key,
        probe_model3d_backend,
    )
except Exception:
    diagnose_model3d_node = None  # type: ignore[assignment]
    get_backend_status = None  # type: ignore[assignment]
    model3d_node_key = None  # type: ignore[assignment]
    probe_model3d_backend = None  # type: ignore[assignment]

from act_platform.runtime import ensure_act_event_bus, render_overlays
from act_platform.ui_spec import normalize_ui_spec
from gui_modules.sao_plugin_ui_render import _canvas_fill, _pal

try:
    from render.model3d_overlay import render_model3d_node
except Exception:
    render_model3d_node = None  # type: ignore[assignment]

_FONT_CACHE: dict[tuple[int, bool], Any] = {}
_FONT_FILES = {
    False: ("msyh.ttc", "segoeui.ttf", "arial.ttf"),
    True: ("msyhbd.ttc", "segoeuib.ttf", "arialbd.ttf", "msyh.ttc"),
}
_CONTAINER_TYPES = {"panel", "section", "card", "row", "group"}
_LAYER_KINDS = {"canvas", "model3d"}
_BASE_Z = 140
_TITLE_RE = re.compile(r"[^A-Za-z0-9_.-]+")


def _cursor_screen_pos(fallback_x: int = 0, fallback_y: int = 0) -> tuple[int, int]:
    try:
        import ctypes
        import ctypes.wintypes as wt

        pt = wt.POINT()
        if ctypes.windll.user32.GetCursorPos(ctypes.byref(pt)):
            return int(pt.x), int(pt.y)
    except Exception:
        pass
    return int(fallback_x), int(fallback_y)


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


def _node_int(node: Mapping[str, Any], key: str, default: int = 0) -> int:
    try:
        value = float(node.get(key, default))
        if value != value:
            raise ValueError("nan")
        return int(round(value))
    except Exception:
        return int(default)


def _node_size(node: Mapping[str, Any], key: str, default: int) -> int:
    return max(1, _node_int(node, key, default))


def _layer_title(kind: str, layer_key: str) -> str:
    text = _TITLE_RE.sub("_", f"sao_plugin_{kind}_{layer_key}")[:120]
    return text or "sao_plugin_overlay"


def _drawable_key(plugin_id: str, kind: str, node: Mapping[str, Any], order: int) -> tuple[str, str]:
    if kind == "model3d" and callable(model3d_node_key):
        try:
            display_key = model3d_node_key(plugin_id, node, order=order)
            return f"model3d:{display_key}", display_key
        except Exception:
            pass
    node_id = str(node.get("id") or "").strip()
    base = f"{plugin_id or 'plugin'}/{node_id}" if node_id else f"{plugin_id or 'plugin'}/{kind}#{order}"
    return f"{kind}:{base}", base


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


def _wrap_line(draw: Any, text: str, font: Any, max_width: int) -> list[str]:
    words = str(text or "").split()
    if not words:
        return [""]
    lines: list[str] = []
    cur = ""
    for word in words:
        candidate = word if not cur else f"{cur} {word}"
        if _text_size(draw, candidate, font)[0] <= max_width:
            cur = candidate
            continue
        if cur:
            lines.append(cur)
        cur = word
    if cur:
        lines.append(cur)
    out: list[str] = []
    for line in lines:
        if _text_size(draw, line, font)[0] <= max_width:
            out.append(line)
            continue
        clipped = line
        while clipped and _text_size(draw, clipped + "...", font)[0] > max_width:
            clipped = clipped[:-1]
        out.append((clipped + "...") if clipped else "...")
    return out


def _draw_model_diagnostic(
    draw: Any,
    model_node: Mapping[str, Any],
    pal: Mapping[str, Any],
    plugin_id: str,
    order: int,
    width: int,
    height: int,
    backend_status: Any,
) -> tuple[str, ...]:
    bg = _color_to_rgba(model_node.get("background"), pal, "")
    if bg is None:
        bg = (4, 10, 18, 220)
    draw.rectangle((0, 0, width, height), fill=bg)
    border = _color_to_rgba("gold", pal, "gold") or (222, 166, 32, 255)
    value = _color_to_rgba("value", pal, "value") or (235, 245, 255, 255)
    muted = _color_to_rgba("muted", pal, "muted") or (145, 160, 176, 255)
    bad = _color_to_rgba("warn", pal, "warn") or (255, 194, 84, 255)
    draw.rectangle((0, 0, max(0, width - 1), max(0, height - 1)), outline=border, width=1)
    draw.rectangle((0, 0, width, min(height, 28)), fill=(12, 22, 36, 235))

    title_font = _load_font(13, True)
    body_font = _load_font(10, False)
    small_font = _load_font(9, False)
    if title_font is not None:
        draw.text((10, 7), "Model3D", fill=border, font=title_font)

    if callable(diagnose_model3d_node):
        try:
            _key, lines = diagnose_model3d_node(
                plugin_id, model_node, order=order, status=backend_status)
        except Exception as exc:
            lines = (f"model3d {plugin_id or 'plugin'}/model3d#{order}", str(exc))
    else:
        lines = (
            f"model3d {plugin_id or 'plugin'}/model3d#{order}",
            "model3d backend probe is unavailable",
        )

    y = 38
    max_text_w = max(20, width - 20)
    for idx, line in enumerate(lines):
        font = body_font if idx == 0 else small_font
        if font is None:
            continue
        fill = value if idx == 0 else (bad if idx >= 2 else muted)
        for wrapped in _wrap_line(draw, line, font, max_text_w):
            if y > height - 14:
                return tuple(lines)
            draw.text((10, y), wrapped, fill=fill, font=font)
            y += 14
    return tuple(lines)


def _iter_layer_drawables(overlays: list[Mapping[str, Any]]) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    order = 0
    for entry in overlays:
        if not isinstance(entry, Mapping):
            continue
        plugin_id = str(entry.get("plugin_id") or "")
        spec = entry.get("spec")
        normalized = normalize_ui_spec(spec if isinstance(spec, Mapping) else {})
        stack = list(normalized.get("nodes") or [])
        while stack:
            node = stack.pop(0)
            if not isinstance(node, Mapping):
                continue
            kind = str(node.get("type") or "").lower()
            if kind in _LAYER_KINDS:
                order += 1
                layer_key, display_key = _drawable_key(plugin_id, kind, node, order)
                out.append({
                    "kind": kind,
                    "plugin_id": plugin_id,
                    "node": node,
                    "order": order,
                    "key": layer_key,
                    "display_key": display_key,
                    "x": _node_int(node, "x", 0),
                    "y": _node_int(node, "y", 0),
                    "z": _node_int(node, "z", 0),
                    "width": _node_size(node, "width", 1),
                    "height": _node_size(node, "height", 1),
                })
                continue
            if kind in _CONTAINER_TYPES:
                stack[0:0] = list(node.get("children") or [])
    out.sort(key=lambda item: (int(item.get("z") or 0), int(item.get("order") or 0)))
    return out


def _image_to_bgra(image: Any, width: int, height: int) -> bytes:
    rgba = image.tobytes()
    if callable(_premultiply_bgra_bytes_floor):
        try:
            return bytes(_premultiply_bgra_bytes_floor(rgba, height, width))
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
    return bytes(out)


def _render_drawable_frame(
    drawable: Mapping[str, Any],
    pal: Mapping[str, Any],
    backend_status: Any = None,
) -> tuple[str, bytes, int, int] | None:
    if Image is None or ImageDraw is None:
        return None
    node = drawable.get("node")
    if not isinstance(node, Mapping):
        return None
    width = _node_size(node, "width", int(drawable.get("width") or 1))
    height = _node_size(node, "height", int(drawable.get("height") or 1))
    image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    kind = str(drawable.get("kind") or "").lower()
    diagnostic_lines: tuple[str, ...] = ()
    if kind == "canvas":
        _draw_canvas(draw, node, pal, width, height)
    elif kind == "model3d":
        rendered = None
        if callable(render_model3d_node):
            try:
                rendered = render_model3d_node(node, pal)
            except Exception:
                rendered = None
        if rendered is not None:
            image = rendered
        else:
            diagnostic_lines = _draw_model_diagnostic(
                draw,
                node,
                pal,
                str(drawable.get("plugin_id") or ""),
                int(drawable.get("order") or 0),
                width,
                height,
                backend_status,
            )
    else:
        return None
    signature = json.dumps({
        "kind": kind,
        "key": drawable.get("key"),
        "x": drawable.get("x"),
        "y": drawable.get("y"),
        "z": drawable.get("z"),
        "node": node,
        "diagnostic": diagnostic_lines,
    }, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return signature, _image_to_bgra(image, width, height), width, height


def _backend_status_signature(backend_status: Any) -> dict[str, Any]:
    if backend_status is None:
        return {}
    return {
        "backend": str(getattr(backend_status, "backend", "") or ""),
        "files_present": bool(getattr(backend_status, "files_present", False)),
        "import_available": bool(getattr(backend_status, "import_available", False)),
        "render_available": bool(getattr(backend_status, "render_available", False)),
        "reason": str(getattr(backend_status, "reason", "") or ""),
        "managed_files": tuple(getattr(backend_status, "managed_files", ()) or ()),
        "native_files": tuple(getattr(backend_status, "native_files", ()) or ()),
    }


def _drawable_raster_node(node: Any) -> Any:
    if not isinstance(node, Mapping):
        return node
    out = dict(node)
    for key in ("x", "y", "z", "draggable", "surface"):
        out.pop(key, None)
    return out


def _drawable_input_signature(
    drawable: Mapping[str, Any],
    pal: Mapping[str, Any],
    backend_status: Any = None,
) -> str:
    kind = str(drawable.get("kind") or "").lower()
    payload: dict[str, Any] = {
        "kind": kind,
        "key": drawable.get("key"),
        "width": drawable.get("width"),
        "height": drawable.get("height"),
        "node": _drawable_raster_node(drawable.get("node")),
        "pal": dict(pal or {}),
    }
    if kind == "model3d":
        payload["backend"] = _backend_status_signature(backend_status)
    return json.dumps(payload, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), default=str)


def _compose_overlay_frame(overlays: list[Mapping[str, Any]]) -> tuple[str, bytes, int, int] | None:
    """Compatibility helper used by focused tests.

    Runtime uses per-node compositor layers, but this still returns the first
    drawable frame so older smoke checks that imported the helper keep working.
    """
    drawables = _iter_layer_drawables(overlays)
    if not drawables:
        return None
    backend_status = get_backend_status() if callable(get_backend_status) else (
        probe_model3d_backend() if callable(probe_model3d_backend) else None
    )
    return _render_drawable_frame(drawables[0], _pal(), backend_status)


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


class PluginUnifiedOverlayHost:
    """Bridge plugin positioned overlay nodes into the unified GPU overlay.

    Each canvas/model3d node is rendered into its own compositor layer keyed by
    plugin id plus node id (or a stable order fallback for invalid specs). Menu
    open events only hide these layers temporarily; plugin enabled/disabled
    state is untouched.
    """

    def __init__(self, owner: Any, *, surface: str = "unioverlay", interval_ms: int = 33) -> None:
        self.owner = owner
        self.root = getattr(owner, "root", None)
        self.surface = str(surface or "unioverlay")
        self.interval_ms = max(16, int(interval_ms))
        self._after_id: Any = None
        self._sub_token = ""
        self._lifecycle_sub_token = ""
        self._dirty = True
        self._destroyed = False
        self._hidden = False
        self._overlay: Any = None
        self._layers: dict[str, dict[str, Any]] = {}
        self._position_overrides: dict[str, tuple[int, int]] = {}
        self._drag_state: dict[str, dict[str, int]] = {}

    def start(self) -> bool:
        if self.root is None or get_unified_overlay is None or CompositorBgraPresenter is None:
            return False
        if self._ensure_overlay() is None:
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
        if self._lifecycle_sub_token:
            try:
                ensure_act_event_bus(self.owner).unsubscribe(self._lifecycle_sub_token)
            except Exception:
                pass
            self._lifecycle_sub_token = ""
        self._destroy_all_layers()

    def hide_temporarily(self) -> None:
        self._hidden = True
        for state in list(self._layers.values()):
            layer = state.get("layer")
            if layer is None:
                continue
            try:
                layer.hide()
                layer.sync_input_proxy()
            except Exception:
                pass

    def restore(self) -> None:
        self._hidden = False
        self._dirty = True
        for state in list(self._layers.values()):
            if not state.get("has_frame"):
                continue
            layer = state.get("layer")
            if layer is None:
                continue
            try:
                layer.show()
                layer.sync_input_proxy()
                layer.request_redraw()
            except Exception:
                pass
        self._sync_host_input_mode()

    def mark_dirty(self) -> None:
        self._dirty = True
        self._schedule(0)

    def _subscribe(self) -> None:
        try:
            bus = ensure_act_event_bus(self.owner)

            def _on_invalidate(event: Mapping[str, Any]) -> None:
                payload = event.get("payload") if isinstance(event, Mapping) else None
                if not isinstance(payload, Mapping):
                    self._dirty = True
                    self._schedule(0)
                    return
                surface = str(payload.get("surface") or "")
                if not surface or surface == self.surface:
                    self._dirty = True
                    self._schedule(0)

            self._sub_token = bus.subscribe(
                "plugin_ui_invalidate",
                _on_invalidate,
                owner_id=f"plugin_unified_overlay_{self.surface}",
            )
            self._lifecycle_sub_token = bus.subscribe(
                "plugin_lifecycle",
                self._handle_plugin_lifecycle,
                owner_id=f"plugin_unified_overlay_lifecycle_{self.surface}",
            )
        except Exception:
            self._sub_token = ""
            self._lifecycle_sub_token = ""

    def _schedule(self, delay_ms: int | None = None) -> None:
        if self._destroyed or self.root is None:
            return
        if self._after_id is not None:
            return
        try:
            self._after_id = self.root.after(self.interval_ms if delay_ms is None else int(delay_ms), self._tick)
        except Exception:
            self._after_id = None

    def _ensure_overlay(self) -> Any:
        if self._overlay is not None:
            return self._overlay
        if get_unified_overlay is None:
            return None
        try:
            overlay = get_unified_overlay(self.root)
            self._overlay = overlay
            self._sync_host_input_mode()
            return overlay
        except Exception:
            self._overlay = None
            return None

    def _sync_host_input_mode(self) -> None:
        overlay = self._overlay
        if overlay is None:
            return
        fn = getattr(overlay, "sync_host_input_mode", None)
        if not callable(fn):
            fn = getattr(overlay, "force_host_input_passthrough", None)
        if callable(fn):
            try:
                fn()
            except Exception:
                pass

    def _destroy_layer(self, key: str) -> None:
        state = self._layers.pop(str(key), None)
        self._position_overrides.pop(str(key), None)
        self._drag_state.pop(str(key), None)
        if not state:
            return
        layer = state.get("layer")
        if layer is not None:
            try:
                layer.destroy_input_proxy()
            except Exception:
                pass
        overlay = self._ensure_overlay()
        layer_name = str(state.get("layer_name") or "")
        if overlay is not None and layer_name:
            try:
                overlay.destroy_layer(layer_name)
            except Exception:
                pass

    def _destroy_all_layers(self) -> None:
        for key in list(self._layers):
            self._destroy_layer(key)

    def _destroy_stale_layers(self, active_keys: set[str]) -> None:
        for key in list(self._layers):
            if key not in active_keys:
                self._destroy_layer(key)

    def _plugin_id_from_layer_key(self, key: str) -> str:
        tail = str(key or "")
        if ":" in tail:
            tail = tail.split(":", 1)[1]
        return tail.split("/", 1)[0] if "/" in tail else ""

    def _destroy_plugin_layers(self, plugin_id: str) -> None:
        pid = str(plugin_id or "")
        if not pid:
            return
        for key in list(self._layers):
            if self._plugin_id_from_layer_key(key) == pid:
                self._destroy_layer(key)

    def _lifecycle_targets_surface(self, payload: Mapping[str, Any]) -> bool:
        surface = str(payload.get("surface") or "")
        if surface and surface != self.surface:
            return False
        surfaces = payload.get("surfaces")
        if isinstance(surfaces, (list, tuple, set)):
            normalized = {str(item or "") for item in surfaces}
            return not normalized or self.surface in normalized or "" in normalized
        return True

    def _handle_plugin_lifecycle(self, event: Mapping[str, Any]) -> None:
        payload = event.get("payload") if isinstance(event, Mapping) else None
        if not isinstance(payload, Mapping):
            self._dirty = True
            self._schedule(0)
            return
        if not self._lifecycle_targets_surface(payload):
            return
        action = str(payload.get("action") or "").lower()
        if action in {"unloaded", "disabled", "load_failed", "uninstalled", "forgotten"}:
            self._destroy_plugin_layers(str(payload.get("plugin_id") or ""))
        self._dirty = True
        self._schedule(0)

    def _drawable_xy(self, drawable: Mapping[str, Any]) -> tuple[int, int]:
        key = str(drawable.get("key") or "")
        override = self._position_overrides.get(key)
        if override is not None:
            return int(override[0]), int(override[1])
        return int(drawable.get("x") or 0), int(drawable.get("y") or 0)

    def _drawable_draggable(self, drawable: Mapping[str, Any]) -> bool:
        kind = str(drawable.get("kind") or "").lower()
        node = drawable.get("node")
        if not isinstance(node, Mapping):
            return kind == "model3d"
        if "draggable" in node:
            return bool(node.get("draggable"))
        return kind == "model3d"

    def _install_drag_callbacks(self, key: str, state: dict[str, Any]) -> None:
        layer = state.get("layer")
        if layer is None or not hasattr(layer, "set_input_callbacks"):
            return

        def _motion(local_x: float, local_y: float) -> None:
            drag = self._drag_state.get(key)
            if not drag:
                return
            cur_x = int(state.get("x") or 0) + int(round(local_x))
            cur_y = int(state.get("y") or 0) + int(round(local_y))
            sx, sy = _cursor_screen_pos(cur_x, cur_y)
            new_x = sx - int(drag.get("offset_x") or 0)
            new_y = sy - int(drag.get("offset_y") or 0)
            self._position_overrides[key] = (new_x, new_y)
            state["x"] = new_x
            state["y"] = new_y
            try:
                layer.set_geometry(
                    new_x,
                    new_y,
                    max(1, int(state.get("width") or 1)),
                    max(1, int(state.get("height") or 1)),
                )
                layer.sync_input_proxy()
                layer.request_redraw()
            except Exception:
                pass

        def _mouse(button: int, action: int, _mods: int, local_x: float, local_y: float) -> None:
            if int(button or 0) != 0:
                return
            if int(action or 0):
                cur_x = int(state.get("x") or 0) + int(round(local_x))
                cur_y = int(state.get("y") or 0) + int(round(local_y))
                sx, sy = _cursor_screen_pos(cur_x, cur_y)
                self._drag_state[key] = {
                    "offset_x": sx - int(state.get("x") or 0),
                    "offset_y": sy - int(state.get("y") or 0),
                }
            else:
                self._drag_state.pop(key, None)
                self._persist_drag_position(key, state)

        try:
            layer.set_input_callbacks(cursor_pos_fn=_motion, mouse_button_fn=_mouse)
            state["input_ready"] = True
            self._sync_host_input_mode()
        except Exception:
            state["input_ready"] = False
            pass

    def _persist_drag_position(self, key: str, state: Mapping[str, Any]) -> None:
        plugin_id = str(state.get("plugin_id") or "")
        if not plugin_id:
            return
        payload = {
            "x": int(state.get("x") or 0),
            "y": int(state.get("y") or 0),
            "z": int(state.get("node_z") or 0),
            "key": str(key or ""),
            "display_key": str(state.get("display_key") or ""),
            "surface": self.surface,
        }

        def _dispatch() -> None:
            try:
                from act_platform.runtime import act_plugin_action
                act_plugin_action(
                    self.owner,
                    "script.overlay.set_position",
                    payload,
                    plugin_id=plugin_id,
                )
            except Exception:
                pass

        try:
            after = getattr(self.root, "after", None)
            if callable(after):
                after(0, _dispatch)
                return
        except Exception:
            pass
        _dispatch()

    def _ensure_layer(self, drawable: Mapping[str, Any], width: int, height: int) -> dict[str, Any] | None:
        overlay = self._ensure_overlay()
        if overlay is None or CompositorBgraPresenter is None:
            return None
        key = str(drawable.get("key") or "")
        if not key:
            return None
        x, y = self._drawable_xy(drawable)
        z = _BASE_Z + int(drawable.get("z") or 0)
        draggable = self._drawable_draggable(drawable)
        click_through = not draggable
        state = self._layers.get(key)
        if state is not None:
            layer = state.get("layer")
            if layer is not None:
                try:
                    w = max(1, int(width))
                    h = max(1, int(height))
                    geometry_changed = (
                        int(state.get("x") or 0) != x
                        or int(state.get("y") or 0) != y
                        or int(state.get("width") or 1) != w
                        or int(state.get("height") or 1) != h
                    )
                    if geometry_changed:
                        layer.set_geometry(x, y, w, h)
                    click_through_changed = bool(state.get("click_through", True)) != click_through
                    if click_through_changed:
                        try:
                            layer.click_through = click_through
                            if click_through:
                                layer.destroy_input_proxy()
                            layer.sync_input_proxy()
                        except Exception:
                            pass
                        state["click_through"] = click_through
                    if int(state.get("z") or 0) != z:
                        try:
                            layer.z_order = z
                            set_layer_z = getattr(overlay, "set_layer_z", None)
                            if callable(set_layer_z):
                                set_layer_z(str(state.get("layer_name") or ""), z)
                        except Exception:
                            pass
                        state["z"] = z
                        geometry_changed = True
                    state["x"] = x
                    state["y"] = y
                    state["width"] = w
                    state["height"] = h
                    state["plugin_id"] = str(drawable.get("plugin_id") or "")
                    state["display_key"] = str(drawable.get("display_key") or "")
                    state["node_z"] = int(drawable.get("z") or 0)
                    if bool(state.get("draggable", False)) != draggable:
                        state["draggable"] = draggable
                        if draggable:
                            self._install_drag_callbacks(key, state)
                        else:
                            try:
                                layer.set_input_callbacks()
                            except Exception:
                                pass
                            state["input_ready"] = False
                    elif draggable and not state.get("input_ready"):
                        self._install_drag_callbacks(key, state)
                    if geometry_changed:
                        try:
                            layer.sync_input_proxy()
                            layer.request_redraw()
                        except Exception:
                            pass
                    if geometry_changed or click_through_changed:
                        self._sync_host_input_mode()
                    return state
                except Exception:
                    self._destroy_layer(key)
        try:
            layer_name = _layer_title(str(drawable.get("kind") or "overlay"), key)
            layer = overlay.create_layer(
                layer_name,
                max(1, int(width)),
                max(1, int(height)),
                x,
                y,
                z,
                click_through=click_through,
            )
            presenter = CompositorBgraPresenter(layer)
            if not self._hidden:
                layer.show()
                layer.sync_input_proxy()
            state = {
                "layer": layer,
                "layer_name": layer_name,
                "presenter": presenter,
                "last_signature": "",
                "has_frame": False,
                "z": z,
                "x": x,
                "y": y,
                "width": max(1, int(width)),
                "height": max(1, int(height)),
                "click_through": click_through,
                "draggable": draggable,
                "plugin_id": str(drawable.get("plugin_id") or ""),
                "display_key": str(drawable.get("display_key") or ""),
                "node_z": int(drawable.get("z") or 0),
                "input_ready": False,
                "visible": False,
            }
            self._layers[key] = state
            if draggable:
                self._install_drag_callbacks(key, state)
            self._sync_host_input_mode()
            return state
        except Exception:
            self._destroy_layer(key)
            return None

    def _clear_and_hide(self) -> None:
        for state in list(self._layers.values()):
            state["has_frame"] = False
            state["last_signature"] = ""
            layer = state.get("layer")
            if layer is None:
                continue
            try:
                layer.request_redraw()
            except Exception:
                pass
            try:
                layer.hide()
                layer.sync_input_proxy()
            except Exception:
                pass
        self._sync_host_input_mode()

    def _refresh_now(self) -> None:
        try:
            payload = render_overlays(self.owner, self.surface) or {}
            overlays = payload.get("overlays") or []
        except Exception:
            overlays = []
        if not overlays:
            self._destroy_all_layers()
            return
        drawables = _iter_layer_drawables(overlays)
        if not drawables:
            self._destroy_all_layers()
            return
        pal = _pal()
        needs_model3d = any(str(item.get("kind") or "").lower() == "model3d" for item in drawables)
        backend_status = None
        if needs_model3d:
            backend_status = get_backend_status() if callable(get_backend_status) else (
                probe_model3d_backend() if callable(probe_model3d_backend) else None
            )
        active_keys: set[str] = set()
        for drawable in drawables:
            key = str(drawable.get("key") or "")
            if not key:
                continue
            input_signature = _drawable_input_signature(drawable, pal, backend_status)
            state = self._layers.get(key)
            if state is not None and state.get("has_frame") and state.get("input_signature") == input_signature:
                state = self._ensure_layer(
                    drawable,
                    max(1, int(drawable.get("width") or 1)),
                    max(1, int(drawable.get("height") or 1)),
                )
                if state is None:
                    continue
                active_keys.add(key)
                layer = state.get("layer")
                if layer is None:
                    continue
                try:
                    target_visible = not self._hidden
                    if bool(state.get("visible", False)) != target_visible:
                        if target_visible:
                            layer.show()
                        else:
                            layer.hide()
                        state["visible"] = target_visible
                        layer.sync_input_proxy()
                        layer.request_redraw()
                except Exception:
                    self._destroy_layer(key)
                    active_keys.discard(key)
                continue
            frame = _render_drawable_frame(drawable, pal, backend_status)
            if frame is None:
                continue
            signature, bgra, width, height = frame
            state = self._ensure_layer(drawable, width, height)
            if state is None:
                continue
            active_keys.add(key)
            layer = state.get("layer")
            presenter = state.get("presenter")
            if layer is None or presenter is None:
                continue
            try:
                previous_input_signature = state.get("input_signature")
                frame_changed = (
                    signature != state.get("last_signature")
                    or input_signature != previous_input_signature
                )
                target_visible = not self._hidden
                visible_changed = bool(state.get("visible", False)) != target_visible
                if frame_changed:
                    presenter.set_frame(bgra, int(width), int(height))
                    state["last_signature"] = signature
                    state["input_signature"] = input_signature
                state["has_frame"] = True
                if visible_changed:
                    if target_visible:
                        layer.show()
                    else:
                        layer.hide()
                    state["visible"] = target_visible
                if frame_changed or visible_changed:
                    layer.sync_input_proxy()
                    layer.request_redraw()
            except Exception:
                self._destroy_layer(key)
                active_keys.discard(key)
        self._destroy_stale_layers(active_keys)
        self._sync_host_input_mode()

    def _tick(self) -> None:
        self._after_id = None
        if self._destroyed:
            return
        if self._dirty:
            self._dirty = False
            self._refresh_now()
        if self._dirty:
            self._schedule(0)


__all__ = ["PluginUnifiedOverlayHost"]
