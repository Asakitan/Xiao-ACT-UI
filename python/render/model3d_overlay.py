# -*- coding: utf-8 -*-
"""Small model3d overlay renderer used by script plugins.

The public contract is a ``model3d`` UI node.  This module keeps failures
contained: a missing model, unsupported backend, or malformed action file
produces a transparent diagnostic frame instead of failing plugin load.
"""

from __future__ import annotations

import json
import math
import os
from collections.abc import Mapping
from typing import Any

try:
    from PIL import Image, ImageColor, ImageDraw, ImageFont
except Exception:  # pragma: no cover - imported defensively by overlay host
    Image = None  # type: ignore[assignment]
    ImageColor = None  # type: ignore[assignment]
    ImageDraw = None  # type: ignore[assignment]
    ImageFont = None  # type: ignore[assignment]

try:
    from render.model3d_backend import resolve_model_path
except Exception:  # pragma: no cover - imported defensively by overlay host
    resolve_model_path = None  # type: ignore[assignment]


def _rgba(value: Any, default: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    if ImageColor is None:
        return default
    try:
        rgb = ImageColor.getrgb(str(value))
        return int(rgb[0]), int(rgb[1]), int(rgb[2]), 255
    except Exception:
        return default


def _font(size: int, bold: bool = False) -> Any:
    if ImageFont is None:
        return None
    names = ("msyhbd.ttc", "segoeuib.ttf", "arialbd.ttf") if bold else ("msyh.ttc", "segoeui.ttf", "arial.ttf")
    for name in names:
        try:
            return ImageFont.truetype(name, size=max(6, int(size)))
        except Exception:
            continue
    try:
        return ImageFont.load_default()
    except Exception:
        return None


def _short_path(path: str, limit: int = 46) -> str:
    text = str(path or "").replace("\\", "/")
    if len(text) <= limit:
        return text
    return "..." + text[-max(4, limit - 3):]


def _backend_status() -> str:
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    vendor = os.path.join(base, "vendor", "model3d", "assimpnet")
    managed = os.path.join(vendor, "AssimpNet.dll")
    native = os.path.join(vendor, "runtimes", "win-x64", "native", "assimp.dll")
    if os.path.isfile(managed) and os.path.isfile(native):
        return "AssimpNet backend present"
    return "AssimpNet backend not bundled"


def _load_action(node: Mapping[str, Any]) -> tuple[str, dict[str, Any]]:
    action = node.get("action") if isinstance(node.get("action"), Mapping) else {}
    action = dict(action or {})
    name = str(action.get("name") or "idle")
    data = action.get("json") if isinstance(action.get("json"), Mapping) else {}
    data = dict(data or {})
    action_file = str(action.get("file") or "").strip()
    if action_file and os.path.isfile(action_file):
        try:
            with open(action_file, "r", encoding="utf-8") as fp:
                loaded = json.load(fp)
            if isinstance(loaded, Mapping):
                data.update(dict(loaded))
        except Exception as exc:
            data["_load_error"] = str(exc)
    selected = data.get(name) if isinstance(data.get(name), Mapping) else {}
    return name, dict(selected or {})


def _resolved_path(path: str) -> str:
    if callable(resolve_model_path):
        try:
            return str(resolve_model_path(path))
        except Exception:
            return path
    return path


def _draw_stickwoman(draw: Any, w: int, h: int, t: float, color: tuple[int, int, int, int]) -> None:
    cx = w * 0.52
    base = h * 0.76
    scale = min(w, h) / 360.0
    stroke = max(2, int(5 * scale))
    bob = math.sin(t * 2.0) * h * 0.018
    head_r = h * 0.055
    head_y = h * 0.26 + bob
    neck_y = head_y + head_r + h * 0.018
    hip_y = h * 0.55 + bob
    skirt_y = h * 0.64 + bob
    arm_y = h * 0.40 + bob
    leg_y = base + bob

    shadow = (0, 0, 0, 80)
    draw.ellipse((cx - w * 0.18, base + h * 0.06, cx + w * 0.20, base + h * 0.11), fill=shadow)
    draw.line((cx, neck_y, cx, hip_y), fill=color, width=stroke)
    draw.ellipse((cx - head_r, head_y - head_r, cx + head_r, head_y + head_r),
                 outline=color, width=stroke)
    hair = (color[0], color[1], color[2], 150)
    draw.arc((cx - head_r * 1.2, head_y - head_r * 1.2,
              cx + head_r * 1.2, head_y + head_r * 1.3), 205, 28, fill=hair, width=stroke)
    draw.polygon(
        ((cx, hip_y - h * 0.025), (cx - w * 0.105, skirt_y), (cx + w * 0.105, skirt_y)),
        outline=color,
    )
    wave = math.sin(t * 4.5)
    draw.line((cx, arm_y, cx - w * 0.15, arm_y + h * 0.09), fill=color, width=stroke)
    draw.line((cx, arm_y, cx + w * 0.13, arm_y - h * (0.10 + 0.035 * wave)),
              fill=color, width=stroke)
    draw.line((cx, skirt_y, cx - w * 0.09, leg_y), fill=color, width=stroke)
    draw.line((cx, skirt_y, cx + w * 0.10, leg_y - h * 0.015 * wave), fill=color, width=stroke)


def render_model3d_node(node: Mapping[str, Any], pal: Mapping[str, Any] | None = None) -> Any:
    if Image is None or ImageDraw is None:
        return None
    width = max(1, int(node.get("width") or 320))
    height = max(1, int(node.get("height") or 480))
    pal = dict(pal or {})
    image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    accent = _rgba(pal.get("accent") or "#8bdff2", (139, 223, 242, 255))
    muted = _rgba(pal.get("muted") or "#d2d2d2", (210, 210, 210, 255))

    model = node.get("model") if isinstance(node.get("model"), Mapping) else {}
    path = str((model or {}).get("path") or "").strip()
    resolved_path = _resolved_path(path) if path else ""
    action_name, action_cfg = _load_action(node)
    speed = 1.0
    try:
        speed = float(action_cfg.get("speed", 1.0) or 1.0)
    except Exception:
        speed = 1.0
    now = 0.0
    try:
        action = node.get("action") if isinstance(node.get("action"), Mapping) else {}
        now = float(action.get("time", node.get("phase", 0.0)) or 0.0)
    except Exception:
        now = 0.0
    _draw_stickwoman(draw, width, height, now * speed, accent)

    font_title = _font(13, True)
    font_body = _font(10, False)
    if font_title:
        draw.text((12, 10), "MODEL3D", fill=accent, font=font_title)
    status = "ready"
    if not path:
        status = "no model_path"
    elif not os.path.isfile(resolved_path):
        status = "model missing"
    else:
        status = _backend_status()
    lines = [
        status,
        _short_path(path) if path else "set model_path to an FBX/model file",
        f"action: {action_name}",
    ]
    if action_cfg.get("_load_error"):
        lines.append("action file error")
    if font_body:
        y = height - 58
        for line in lines[-3:]:
            draw.text((12, y), str(line), fill=muted, font=font_body)
            y += 15
    return image


__all__ = ["render_model3d_node"]
