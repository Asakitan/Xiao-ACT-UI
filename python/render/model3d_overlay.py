# -*- coding: utf-8 -*-
"""Small model3d overlay renderer used by script plugins.

The public contract is a ``model3d`` UI node.  This module keeps failures
contained: a missing model, unsupported backend, or malformed action file
produces a transparent diagnostic frame instead of failing plugin load.
"""

from __future__ import annotations

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
    from render.model3d_backend import (
        evaluate_retarget_pose,
        get_action_metadata,
        get_backend_status,
        get_retarget_plan,
        resolve_model_path,
    )
except Exception:  # pragma: no cover - imported defensively by overlay host
    evaluate_retarget_pose = None  # type: ignore[assignment]
    get_action_metadata = None  # type: ignore[assignment]
    get_backend_status = None  # type: ignore[assignment]
    get_retarget_plan = None  # type: ignore[assignment]
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
    if callable(get_backend_status):
        try:
            status = get_backend_status()
            if getattr(status, "render_available", False):
                return "AssimpNet renderer ready"
            if getattr(status, "files_present", False):
                return "AssimpNet files present; render unavailable"
            return "AssimpNet backend not bundled"
        except Exception:
            pass
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    vendor = os.path.join(base, "vendor", "model3d", "assimpnet")
    managed = os.path.join(vendor, "AssimpNet.dll")
    native = os.path.join(vendor, "runtimes", "win-x64", "native", "assimp.dll")
    if os.path.isfile(managed) and os.path.isfile(native):
        return "AssimpNet files present; render unavailable"
    return "AssimpNet backend not bundled"


def _load_action(node: Mapping[str, Any]) -> tuple[str, dict[str, Any]]:
    if callable(get_action_metadata):
        try:
            meta = get_action_metadata(node)
            selected = meta.get("selected") if isinstance(meta.get("selected"), Mapping) else {}
            return str(meta.get("name") or "idle"), dict(selected or {})
        except Exception:
            pass
    action = node.get("action") if isinstance(node.get("action"), Mapping) else {}
    action = dict(action or {})
    name = str(action.get("name") or "idle")
    data = action.get("json") if isinstance(action.get("json"), Mapping) else {}
    selected = data.get(name) if isinstance(data.get(name), Mapping) else {}
    return name, dict(selected or {})


def _resolved_path(path: str) -> str:
    if callable(resolve_model_path):
        try:
            return str(resolve_model_path(path))
        except Exception:
            return path
    return path


def _cfg_float(cfg: Mapping[str, Any], key: str, default: float,
               lo: float = -999.0, hi: float = 999.0) -> float:
    try:
        value = float(cfg.get(key, default))
        if value != value:
            raise ValueError("nan")
    except Exception:
        value = float(default)
    return max(lo, min(hi, value))


def _limb(draw: Any, start: tuple[float, float], length_a: float, length_b: float,
          angle_a: float, bend: float, color: tuple[int, int, int, int],
          width: int) -> tuple[float, float]:
    x0, y0 = start
    elbow = (x0 + math.cos(angle_a) * length_a, y0 + math.sin(angle_a) * length_a)
    angle_b = angle_a + bend
    end = (elbow[0] + math.cos(angle_b) * length_b, elbow[1] + math.sin(angle_b) * length_b)
    draw.line((x0, y0, elbow[0], elbow[1], end[0], end[1]), fill=color, width=width, joint="curve")
    return end


def _draw_stylized_avatar(
    draw: Any,
    w: int,
    h: int,
    t: float,
    action_name: str,
    cfg: Mapping[str, Any],
    accent: tuple[int, int, int, int],
) -> None:
    scale = min(w, h) / 420.0
    cx = w * 0.52 + math.sin(t * 0.75) * w * 0.010
    floor_y = h * 0.80
    stroke = max(3, int(6 * scale))
    limb_w = max(2, int(4 * scale))
    bounce = _cfg_float(cfg, "bounce", 0.035, 0.0, 0.25)
    tilt = _cfg_float(cfg, "tilt", 0.035, -0.4, 0.4)
    arm_swing = _cfg_float(cfg, "armSwing", 0.18, 0.0, 0.85)
    leg_swing = _cfg_float(cfg, "legSwing", 0.16, 0.0, 0.9)
    right_lift = _cfg_float(cfg, "rightArmLift", 0.45, 0.0, 1.2)
    left_lift = _cfg_float(cfg, "leftArmLift", 0.12, 0.0, 1.0)
    wave = math.sin(t * 3.2)
    step = math.sin(t * 2.3)
    bob = math.sin(t * 2.0) * h * bounce
    if action_name == "idle":
        bob *= 0.35
        step *= 0.2
    elif action_name == "jump":
        bob = -abs(math.sin(t * 1.9)) * h * max(bounce, 0.08)
    elif action_name == "walk":
        step = math.sin(t * 4.1)

    skin = (255, 228, 210, 255)
    hair = (38, 48, 72, 255)
    hair_hi = (72, 92, 128, 255)
    dress = (76, 178, 184, 255)
    dress_hi = (244, 190, 116, 235)
    boot = (43, 55, 78, 255)
    outline = (24, 34, 56, 255)
    glow = (accent[0], accent[1], accent[2], 52)

    hip = (cx, h * 0.57 + bob)
    chest = (cx + math.sin(t * 1.1) * w * tilt, h * 0.39 + bob)
    shoulder_y = chest[1] + h * 0.018
    shoulder_l = (chest[0] - w * 0.090, shoulder_y)
    shoulder_r = (chest[0] + w * 0.090, shoulder_y)
    head_r = h * 0.063
    head = (chest[0] + w * 0.012, h * 0.245 + bob + math.sin(t * 1.6) * h * 0.006)

    draw.ellipse((cx - w * 0.19, floor_y + h * 0.050, cx + w * 0.21, floor_y + h * 0.100),
                 fill=(0, 0, 0, 70))
    draw.ellipse((cx - w * 0.22, h * 0.11, cx + w * 0.22, h * 0.86), outline=glow, width=max(1, int(2 * scale)))

    leg_len_a = h * 0.135
    leg_len_b = h * 0.145
    left_foot = _limb(
        draw, (hip[0] - w * 0.038, hip[1] + h * 0.055), leg_len_a, leg_len_b,
        math.pi * 0.48 + step * leg_swing, math.pi * 0.08 - abs(step) * 0.22,
        outline, limb_w)
    right_foot = _limb(
        draw, (hip[0] + w * 0.038, hip[1] + h * 0.055), leg_len_a, leg_len_b,
        math.pi * 0.52 - step * leg_swing, -math.pi * 0.08 + abs(step) * 0.22,
        outline, limb_w)
    foot_w = max(5, int(13 * scale))
    draw.line((left_foot[0] - foot_w, left_foot[1], left_foot[0] + foot_w * 0.5, left_foot[1]),
              fill=boot, width=max(2, limb_w))
    draw.line((right_foot[0] - foot_w * 0.5, right_foot[1], right_foot[0] + foot_w, right_foot[1]),
              fill=boot, width=max(2, limb_w))

    waist = (hip[0], hip[1] - h * 0.018)
    skirt = [
        (waist[0] - w * 0.075, waist[1]),
        (waist[0] + w * 0.078, waist[1]),
        (waist[0] + w * 0.125, waist[1] + h * 0.115),
        (waist[0] - w * 0.128, waist[1] + h * 0.115),
    ]
    torso = [
        (shoulder_l[0], shoulder_l[1]),
        (shoulder_r[0], shoulder_r[1]),
        (waist[0] + w * 0.060, waist[1]),
        (waist[0] - w * 0.060, waist[1]),
    ]
    draw.polygon(torso, fill=dress, outline=outline)
    draw.polygon(skirt, fill=dress, outline=outline)
    draw.line((skirt[0][0], skirt[2][1], skirt[2][0], skirt[2][1]), fill=dress_hi, width=max(1, int(2 * scale)))
    draw.line((shoulder_l[0], shoulder_l[1], shoulder_r[0], shoulder_r[1]), fill=dress_hi, width=max(1, int(2 * scale)))

    left_angle = math.pi * (0.92 + arm_swing * 0.18 * wave - left_lift * 0.22)
    right_angle = math.pi * (0.08 - right_lift * 0.38 - arm_swing * 0.16 * wave)
    _limb(draw, shoulder_l, h * 0.105, h * 0.095, left_angle, math.pi * 0.18,
          outline, limb_w)
    right_hand = _limb(draw, shoulder_r, h * 0.105, h * 0.095, right_angle, -math.pi * 0.22 + wave * 0.35,
                       outline, limb_w)
    hand_r = max(2, int(4 * scale))
    draw.ellipse((right_hand[0] - hand_r, right_hand[1] - hand_r,
                  right_hand[0] + hand_r, right_hand[1] + hand_r), fill=skin)

    draw.line((chest[0], chest[1] - h * 0.02, head[0], head[1] + head_r * 0.84),
              fill=outline, width=max(2, int(3 * scale)))
    hair_r = head_r * 1.24
    draw.ellipse((head[0] - hair_r, head[1] - hair_r * 1.05,
                  head[0] + hair_r, head[1] + hair_r * 1.10), fill=hair)
    draw.ellipse((head[0] - head_r * 0.92, head[1] - head_r * 0.78,
                  head[0] + head_r * 0.92, head[1] + head_r * 0.98), fill=skin, outline=outline)
    bang = [
        (head[0] - head_r * 0.95, head[1] - head_r * 0.55),
        (head[0] - head_r * 0.20, head[1] - head_r * 1.02),
        (head[0] + head_r * 0.92, head[1] - head_r * 0.44),
        (head[0] + head_r * 0.24, head[1] - head_r * 0.20),
    ]
    draw.polygon(bang, fill=hair_hi)
    eye_r = max(1, int(2.4 * scale))
    eye_y = head[1] - head_r * 0.08
    draw.ellipse((head[0] - head_r * 0.34 - eye_r, eye_y - eye_r,
                  head[0] - head_r * 0.34 + eye_r, eye_y + eye_r), fill=outline)
    draw.ellipse((head[0] + head_r * 0.30 - eye_r, eye_y - eye_r,
                  head[0] + head_r * 0.30 + eye_r, eye_y + eye_r), fill=outline)
    blush = (255, 155, 175, 110)
    blush_r = max(2, int(5 * scale))
    draw.ellipse((head[0] - head_r * 0.56 - blush_r, head[1] + head_r * 0.18 - blush_r,
                  head[0] - head_r * 0.56 + blush_r, head[1] + head_r * 0.18 + blush_r), fill=blush)
    draw.ellipse((head[0] + head_r * 0.55 - blush_r, head[1] + head_r * 0.18 - blush_r,
                  head[0] + head_r * 0.55 + blush_r, head[1] + head_r * 0.18 + blush_r), fill=blush)


def _pose_points(pose: Mapping[str, Any]) -> dict[str, tuple[float, float, float]]:
    points: dict[str, tuple[float, float, float]] = {}
    raw = pose.get("positions") if isinstance(pose.get("positions"), Mapping) else {}
    for key, value in dict(raw or {}).items():
        if isinstance(value, (list, tuple)) and len(value) >= 2:
            try:
                points[str(key)] = (
                    float(value[0]),
                    float(value[1]),
                    float(value[2]) if len(value) >= 3 else 0.0,
                )
            except Exception:
                continue
    return points


def _screen_points(points: Mapping[str, tuple[float, float, float]],
                   width: int, height: int) -> dict[str, tuple[float, float]]:
    if not points:
        return {}
    xs = [point[0] for point in points.values()]
    ys = [point[1] for point in points.values()]
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    extent_x = max(0.1, max_x - min_x)
    extent_y = max(0.1, max_y - min_y)
    scale = min(width * 0.62 / extent_x, height * 0.72 / extent_y)
    cx = (min_x + max_x) * 0.5
    floor_y = height * 0.83
    screen: dict[str, tuple[float, float]] = {}
    for key, point in points.items():
        x = width * 0.52 + (point[0] - cx) * scale
        y = floor_y - (point[1] - min_y) * scale
        screen[key] = (x, y)
    return screen


def _draw_retarget_pose_avatar(
    draw: Any,
    width: int,
    height: int,
    pose: Mapping[str, Any],
    accent: tuple[int, int, int, int],
) -> bool:
    points = _pose_points(pose)
    screen = _screen_points(points, width, height)
    required = ("hips", "spine", "head", "right_hand", "left_hand")
    if not all(key in screen for key in required):
        return False

    scale = min(width, height) / 420.0
    outline = (24, 34, 56, 255)
    limb = (41, 58, 88, 255)
    skin = (255, 228, 210, 255)
    dress = (76, 178, 184, 255)
    dress_hi = (244, 190, 116, 235)
    hair = (38, 48, 72, 255)
    glow = (accent[0], accent[1], accent[2], 44)
    line_w = max(3, int(4.5 * scale))
    joint_r = max(2, int(3.5 * scale))

    hip = screen.get("hips")
    left_foot = screen.get("left_foot", hip)
    right_foot = screen.get("right_foot", hip)
    if hip:
        shadow_y = max(left_foot[1] if left_foot else hip[1], right_foot[1] if right_foot else hip[1])
        draw.ellipse((hip[0] - width * 0.20, shadow_y + height * 0.030,
                      hip[0] + width * 0.20, shadow_y + height * 0.080), fill=(0, 0, 0, 60))
        draw.ellipse((hip[0] - width * 0.23, height * 0.10,
                      hip[0] + width * 0.23, shadow_y + height * 0.08),
                     outline=glow, width=max(1, int(2 * scale)))

    segments = pose.get("segments") if isinstance(pose.get("segments"), (list, tuple)) else ()
    for seg in segments:
        if not isinstance(seg, Mapping):
            continue
        parent = str(seg.get("parent") or "")
        child = str(seg.get("child") or "")
        if parent not in screen or child not in screen:
            continue
        color = outline if bool(seg.get("clamped")) else limb
        draw.line((screen[parent][0], screen[parent][1], screen[child][0], screen[child][1]),
                  fill=color, width=line_w, joint="curve")

    chest = screen.get("chest") or screen.get("spine")
    left_shoulder = screen.get("left_shoulder") or screen.get("left_arm")
    right_shoulder = screen.get("right_shoulder") or screen.get("right_arm")
    if hip and chest and left_shoulder and right_shoulder:
        waist_l = (hip[0] - width * 0.055, hip[1])
        waist_r = (hip[0] + width * 0.055, hip[1])
        draw.polygon((left_shoulder, right_shoulder, waist_r, waist_l), fill=dress, outline=outline)
        skirt = (
            waist_l,
            waist_r,
            (hip[0] + width * 0.120, hip[1] + height * 0.105),
            (hip[0] - width * 0.120, hip[1] + height * 0.105),
        )
        draw.polygon(skirt, fill=dress, outline=outline)
        draw.line((left_shoulder[0], left_shoulder[1], right_shoulder[0], right_shoulder[1]),
                  fill=dress_hi, width=max(1, int(2 * scale)))

    head = screen.get("head")
    neck = screen.get("neck") or screen.get("chest") or screen.get("spine")
    if head and neck:
        head_r = max(height * 0.045, abs(neck[1] - head[1]) * 0.56)
        hair_r = head_r * 1.20
        draw.ellipse((head[0] - hair_r, head[1] - hair_r * 1.05,
                      head[0] + hair_r, head[1] + hair_r * 1.08), fill=hair)
        draw.ellipse((head[0] - head_r, head[1] - head_r * 0.84,
                      head[0] + head_r, head[1] + head_r * 1.02), fill=skin, outline=outline)
        eye_r = max(1, int(2.3 * scale))
        for ex in (-0.32, 0.30):
            draw.ellipse((head[0] + ex * head_r - eye_r, head[1] - 0.08 * head_r - eye_r,
                          head[0] + ex * head_r + eye_r, head[1] - 0.08 * head_r + eye_r),
                         fill=outline)

    for key in ("left_hand", "right_hand"):
        if key in screen:
            x, y = screen[key]
            draw.ellipse((x - joint_r, y - joint_r, x + joint_r, y + joint_r), fill=skin, outline=outline)
    for key in ("left_foot", "right_foot"):
        if key in screen:
            x, y = screen[key]
            draw.line((x - joint_r * 2.2, y, x + joint_r * 2.2, y),
                      fill=outline, width=max(2, int(3 * scale)))
    return True


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
    pose = None
    if callable(evaluate_retarget_pose):
        try:
            pose = evaluate_retarget_pose(node)
        except Exception:
            pose = None
    used_pose = False
    if isinstance(pose, Mapping) and pose.get("ok"):
        used_pose = _draw_retarget_pose_avatar(draw, width, height, pose, accent)
    if not used_pose:
        _draw_stylized_avatar(draw, width, height, now * speed, action_name, action_cfg, accent)

    status = "ready"
    diagnostic = False
    if not path:
        status = "no model_path"
        diagnostic = True
    elif not os.path.isfile(resolved_path):
        status = "model missing"
        diagnostic = True
    else:
        status = _backend_status()
    if action_cfg.get("_load_error"):
        diagnostic = True
    try:
        diagnostic = diagnostic or bool(node.get("diagnostic") or node.get("debug"))
    except Exception:
        pass
    lines = [
        status,
        _short_path(path) if path else "set model_path to an FBX/model file",
        f"action: {action_name}",
    ]
    retarget = node.get("retarget") if isinstance(node.get("retarget"), Mapping) else {}
    if retarget:
        plan = None
        if callable(get_retarget_plan):
            try:
                plan = get_retarget_plan(node)
            except Exception:
                plan = None
        if isinstance(plan, Mapping):
            lines.append(
                f"retarget: {plan.get('mode', 'auto')} "
                f"{int(plan.get('resolved_count') or 0)}/{int(plan.get('total_count') or 0)}"
            )
            if isinstance(pose, Mapping) and pose.get("ok"):
                lines.append(
                    f"pose: {pose.get('rest_source', 'auto')} "
                    f"stretch {float(pose.get('max_stretch') or 0.0):.2f}"
                )
        else:
            lines.append(f"retarget: {retarget.get('mode', 'auto')}")
    if action_cfg.get("_load_error"):
        lines.append("action file error")
    if diagnostic:
        font_title = _font(13, True)
        if font_title:
            draw.text((12, 10), "MODEL3D", fill=accent, font=font_title)
    font_body = _font(10, False)
    if diagnostic and font_body:
        y = height - 58
        for line in lines[-3:]:
            draw.text((12, y), str(line), fill=muted, font=font_body)
            y += 15
    return image


__all__ = ["render_model3d_node"]
