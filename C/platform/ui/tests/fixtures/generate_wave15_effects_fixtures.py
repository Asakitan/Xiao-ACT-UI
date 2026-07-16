from __future__ import annotations

# Generates Wave 15 Python-authority render fixtures for the production C++
# compositor. The panel implementations remain the authority; this script only
# fixes inputs, converts their RGBA output to premultiplied BGRA, and records
# the explicit transit contract consumed by the standalone C++ test.

import hashlib
import json
import sys
from pathlib import Path
from typing import Any

from PIL import Image


FIXTURE_DIR = Path(__file__).resolve().parent
PYTHON_ROOT = FIXTURE_DIR.parents[4] / "python"
sys.path.insert(0, str(PYTHON_ROOT))

from gui_modules.sao_gui_nervegear_button import _premultiply_bgra
from plugins.star_resonance_plugin.panels.sao_gui_alert import AlertOverlay
from plugins.star_resonance_plugin.panels.sao_gui_buffmon import SelfBuffOverlay
from plugins.star_resonance_plugin.panels.sao_gui_skillfx import BurstReadyOverlay


def alpha_bbox(image: Image.Image) -> dict[str, int]:
    alpha = image.convert("RGBA").getchannel("A")
    box = alpha.getbbox()
    if box is None:
        return {"x": 0, "y": 0, "width": 0, "height": 0}
    left, top, right, bottom = box
    return {"x": left, "y": top, "width": right - left, "height": bottom - top}


def write_fixture(
    name: str,
    image: Image.Image,
    *,
    source: str,
    geometry: dict[str, Any],
    layers: list[dict[str, Any]],
    event_order: list[dict[str, Any]],
    authority: str,
) -> dict[str, Any]:
    rgba = image.convert("RGBA")
    pixels = _premultiply_bgra(rgba)
    metadata = {
        "format": "premultiplied-bgra",
        "width": rgba.width,
        "height": rgba.height,
        "stride": rgba.width * 4,
        "bytes": len(pixels),
        "sha256": hashlib.sha256(pixels).hexdigest(),
        "source": source,
        "authority": authority,
        "geometry": geometry,
        "alpha_bbox": alpha_bbox(rgba),
        "layers": layers,
        "event_order": event_order,
    }
    (FIXTURE_DIR / f"{name}.bgra").write_bytes(pixels)
    (FIXTURE_DIR / f"{name}.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(metadata, sort_keys=True))
    return metadata


def render_alert() -> None:
    # Calling the unchanged private renderer avoids Tk window creation while
    # retaining AlertOverlay's real shadow, wrapping, typography, and panels.
    overlay = object.__new__(AlertOverlay)
    overlay._frame_height = 0
    image = overlay._render_frame(
        "MECHANIC ALERT",
        "Move clockwise to the safe sector\nKeep the party stack intact",
    )
    write_fixture(
        "wave15_alert",
        image,
        source="plugins.star_resonance_plugin.panels.sao_gui_alert.AlertOverlay._render_frame",
        authority="python-alert-overlay",
        geometry={
            "content_width": AlertOverlay.WIDTH,
            "content_height": overlay._frame_height,
            "shadow_padding": 40,
            "canvas_width": image.width,
            "canvas_height": image.height,
        },
        layers=[
            {"name": "alert", "z_order": 20, "x": 8, "y": 6, "alpha": 1.0},
        ],
        event_order=[
            {"event": "alert.render_frame", "title": "MECHANIC ALERT"},
            {"event": "compositor.create_layer", "layer": "alert", "z_order": 20},
            {"event": "compositor.update_bgra", "layer": "alert"},
        ],
    )


def render_buffmon() -> None:
    # Bypass the GPU window lifecycle only. _render_base remains the unchanged
    # BuffMon production composer and receives a fixed, non-urgent row set so
    # its output has no wall-clock animation dependency.
    overlay = object.__new__(SelfBuffOverlay)
    overlay._shell_parts_cache = None
    overlay._cached_base = None
    overlay._cached_sig = ()
    overlay._cached_seq = 0
    overlay._theme = "dark"
    overlay._apply_theme("dark")
    rows = [
        {
            "id": 1001,
            "uuid": 11,
            "name": "Astral Resonance",
            "rem_s": 42.5,
            "layer": 3,
            "count": 2,
            "apply_count": 4,
            "uptime_pct": 0.875,
        },
        {
            "id": 1002,
            "uuid": 12,
            "name": "Guardian Aura",
            "rem_s": 12.0,
            "layer": 1,
            "count": 1,
            "apply_count": 1,
            "uptime_pct": 0.500,
        },
    ]
    total_height = overlay._compute_total_h(rows)
    image = overlay._render_base(rows, total_height)
    write_fixture(
        "wave15_buffmon_base",
        image,
        source="plugins.star_resonance_plugin.panels.sao_gui_buffmon._BuffPanelBase._render_base",
        authority="python-buffmon-base-compose",
        geometry={
            "panel_width": overlay.WIDTH,
            "panel_height": total_height,
            "shadow_padding": overlay.SHADOW_PAD,
            "row_count": len(rows),
            "row_height": overlay.ROW_H,
            "row_gap": overlay.ROW_GAP,
        },
        layers=[
            {"name": "buffmon", "z_order": 10, "x": 3, "y": 14, "alpha": 1.0},
        ],
        event_order=[
            {"event": "buffmon.render_base", "row_count": len(rows)},
            {"event": "compositor.create_layer", "layer": "buffmon", "z_order": 10},
            {"event": "compositor.update_bgra", "layer": "buffmon"},
        ],
    )


def make_skillfx_overlay() -> BurstReadyOverlay:
    # Minimal deterministic state for the unchanged SkillFX sprite/SDF methods.
    overlay = object.__new__(BurstReadyOverlay)
    overlay._win_w = 800
    overlay._win_h = 450
    overlay._callout = {"x": 42, "y": 34, "w": 460, "h": 128}
    overlay._show_t = 1000.0
    overlay._exit_t = 0.0
    overlay._exiting = False
    overlay._anchor = (612.0, 326.0)
    overlay._anchor_radius = 56.0
    overlay._ring_size = 176
    overlay._cap_static = None
    overlay._cap_sig = ()
    overlay._cap_base_static = None
    overlay._cap_title_static = None
    overlay._cap_glow_static = None
    overlay._cap_mask_static = None
    overlay._cap_shine_static = None
    overlay._cap_glow_offset = (0, 0)
    overlay._beam_cache_sig = ()
    overlay._beam_cache_img = None
    overlay._beam_cache_pos = (0.0, 0.0)
    overlay._beam_tail_cache_sig = ()
    overlay._beam_tail_cache_img = None
    overlay._ring_field_cache = {}
    overlay._ring_layer_cache = {}
    overlay._layer_bufs = {}
    overlay._glfx = None
    overlay._glfx_failed = False
    overlay._gl_anchor = overlay._anchor
    overlay._gl_label = (70.0, 105.0)
    overlay._gl_panel_size = (460.0, 128.0)
    overlay._gl_target_anchor = overlay._anchor
    overlay._gl_target_label = overlay._gl_label
    overlay._gl_target_panel_size = overlay._gl_panel_size
    overlay._gl_seed = 1.0
    return overlay


def render_skillfx() -> None:
    overlay = make_skillfx_overlay()
    fixed_now = 1001.0

    # Caption sprites are deterministic Python-raster authority. They remain
    # useful even where a standalone GL context is unavailable.
    caption = Image.new("RGBA", (overlay._win_w, overlay._win_h), (0, 0, 0, 0))
    overlay._draw_caption(caption, 1.0, 1.0, 0.0, fixed_now)
    write_fixture(
        "wave15_skillfx_caption",
        caption,
        source="plugins.star_resonance_plugin.panels.sao_gui_skillfx.BurstReadyOverlay._draw_caption",
        authority="python-skillfx-caption-raster",
        geometry={
            "viewport_width": overlay._win_w,
            "viewport_height": overlay._win_h,
            "callout": overlay._callout,
            "anchor": {"x": overlay._anchor[0], "y": overlay._anchor[1]},
            "fixed_now": fixed_now,
        },
        layers=[
            {"name": "skillfx_caption", "z_order": 30, "x": 0, "y": 0, "alpha": 1.0},
        ],
        event_order=[
            {"event": "skillfx.draw_caption", "fixed_now": fixed_now},
            {"event": "compositor.create_layer", "layer": "skillfx_caption", "z_order": 30},
            {"event": "compositor.update_bgra", "layer": "skillfx_caption"},
        ],
    )

    # The actual SDF pipeline requires a real standalone GL context. When it
    # is unavailable, emit a machine-readable skip rather than inventing GPU
    # pixels; a D3D/GL live gate remains required for that layer.
    gl_metadata_path = FIXTURE_DIR / "wave15_skillfx_sdf_gl.json"
    try:
        sdf_frame = overlay._compose_frame_gpu(fixed_now)
    except Exception as exc:
        sdf_frame = None
        skip_reason = f"SkillFX SDF/GL headless initialization raised {type(exc).__name__}: {exc}"
    else:
        skip_reason = "SkillFX SDF/GL headless initialization returned no exact frame"

    if sdf_frame is None:
        metadata = {
            "format": "premultiplied-bgra",
            "source": "plugins.star_resonance_plugin.panels.sao_gui_skillfx.BurstReadyOverlay._compose_frame_gpu",
            "authority": "python-skillfx-sdf-gl",
            "headless_status": "skip",
            "headless_skip": {
                "reason": skip_reason,
                "required_live_gate": "real D3D/GL presentation validation",
            },
            "geometry": {
                "viewport_width": overlay._win_w,
                "viewport_height": overlay._win_h,
                "callout": overlay._callout,
                "anchor": {"x": overlay._anchor[0], "y": overlay._anchor[1]},
                "fixed_now": fixed_now,
            },
            "layers": [
                {"name": "skillfx_sdf_gl", "z_order": 30, "x": 0, "y": 0, "alpha": 1.0},
            ],
            "event_order": [
                {"event": "skillfx.compose_frame_gpu", "fixed_now": fixed_now},
                {"event": "live_gate.required", "backend": "D3D/GL"},
            ],
        }
        (FIXTURE_DIR / "wave15_skillfx_sdf_gl.bgra").unlink(missing_ok=True)
        gl_metadata_path.write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(json.dumps(metadata, sort_keys=True))
        return

    write_fixture(
        "wave15_skillfx_sdf_gl",
        sdf_frame,
        source="plugins.star_resonance_plugin.panels.sao_gui_skillfx.BurstReadyOverlay._compose_frame_gpu",
        authority="python-skillfx-sdf-gl",
        geometry={
            "viewport_width": overlay._win_w,
            "viewport_height": overlay._win_h,
            "callout": overlay._callout,
            "anchor": {"x": overlay._anchor[0], "y": overlay._anchor[1]},
            "fixed_now": fixed_now,
        },
        layers=[
            {"name": "skillfx_sdf_gl", "z_order": 30, "x": 0, "y": 0, "alpha": 1.0},
        ],
        event_order=[
            {"event": "skillfx.compose_frame_gpu", "fixed_now": fixed_now},
            {"event": "compositor.create_layer", "layer": "skillfx_sdf_gl", "z_order": 30},
            {"event": "compositor.update_bgra", "layer": "skillfx_sdf_gl"},
        ],
    )


def main() -> None:
    render_alert()
    render_buffmon()
    render_skillfx()


if __name__ == "__main__":
    main()
