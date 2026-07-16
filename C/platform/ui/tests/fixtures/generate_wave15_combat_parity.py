from __future__ import annotations

import hashlib
import importlib
import json
import sys
from pathlib import Path
from typing import Any
from unittest.mock import patch


FIXTURE_DIR = Path(__file__).resolve().parent / "wave15_combat"
PYTHON_ROOT = Path(__file__).resolve().parents[5] / "python"
CYTHON_ROOT = PYTHON_ROOT / "plugins" / "star_resonance_plugin" / "cython"
PIXELS_ROOT = PYTHON_ROOT.parents[1] / "build" / "lib.win-amd64-cpython-311"
NOW = 1_700_000_000.0


def add_runtime_paths() -> None:
    for path in (CYTHON_ROOT, PIXELS_ROOT, PYTHON_ROOT):
        value = str(path)
        if value not in sys.path:
            sys.path.insert(0, value)


def mark_headless(panel: Any) -> None:
    panel._visible = True
    panel._win = panel
    panel._gpu_managed = True
    panel._registered = True


def stabilize_dps(panel: Any) -> None:
    panel._fade_alpha = 1.0
    panel._fade_target = 1.0
    panel._disp_total_damage = panel._target_total_damage
    panel._disp_total_dps = panel._target_total_dps
    panel._disp_total_heal = panel._target_total_heal
    panel._disp_total_hps = panel._target_total_hps
    panel._disp_elapsed = panel._target_elapsed
    for row in panel._rows.values():
        row.disp_damage = row.target_damage
        row.disp_dps = row.target_dps
        row.disp_heal = row.target_heal
        row.disp_hps = row.target_hps
        row.disp_bar_pct = row.target_bar_pct
        row.disp_y = row.target_y


def stabilize_boss(panel: Any) -> None:
    panel._fade_alpha = 1.0
    panel._fade_target = 1.0
    panel._enter_translate = 0.0
    panel._disp_hp_pct = panel._target_hp_pct
    panel._disp_trail_pct = panel._target_trail_pct
    panel._disp_shield_pct = panel._target_shield_pct
    panel._disp_break_pct = panel._target_break_pct


def stabilize_hp(panel: Any) -> None:
    panel._fade_alpha = 1.0
    panel._fade_target = 1.0
    panel._enter_scale_t = 1.0
    panel._hp_pct_disp = panel._hp_pct_target
    panel._sta_pct_disp = panel._sta_pct_target
    panel._hp_flash_start = 0.0


def premultiplied_bgra(image: Any) -> bytes:
    rgba = image.convert("RGBA").tobytes()
    output = bytearray(len(rgba))
    for index in range(0, len(rgba), 4):
        red, green, blue, alpha = rgba[index:index + 4]
        output[index:index + 4] = bytes((
            (blue * alpha + 127) // 255,
            (green * alpha + 127) // 255,
            (red * alpha + 127) // 255,
            alpha,
        ))
    return bytes(output)


def alpha_bbox(pixels: bytes, width: int, height: int) -> list[int]:
    left, top = width, height
    right = bottom = 0
    for y in range(height):
        for x in range(width):
            if pixels[(y * width + x) * 4 + 3] == 0:
                continue
            left = min(left, x)
            top = min(top, y)
            right = max(right, x + 1)
            bottom = max(bottom, y + 1)
    if right == 0:
        return [0, 0, 0, 0]
    return [left, top, right, bottom]


def layer_metadata(frame_name: str, panel: str, state: str,
                   width: int, height: int, x: int, y: int,
                   z_order: int) -> tuple[list[dict[str, Any]], str]:
    layers = [{
        "id": f"star-resonance-{panel}-{state}",
        "source": f"{panel}.compose_frame",
        "visible": True,
        "x": x,
        "y": y,
        "width": width,
        "height": height,
        "z_order": z_order,
    }]
    signature = hashlib.sha256(
        json.dumps(layers, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    return layers, signature


def write_frame(frame_name: str, panel: str, state: str, image: Any,
                x: int, y: int, z_order: int,
                format_strings: dict[str, str],
                format_boundaries: list[dict[str, Any]]) -> None:
    pixels = premultiplied_bgra(image)
    width, height = image.size
    layers, signature = layer_metadata(
        frame_name, panel, state, width, height, x, y, z_order
    )
    metadata = {
        "schema": "sao.ui.python-authority.wave15.combat.v1",
        "frame": frame_name,
        "panel": panel,
        "state": state,
        "render_path": "PIL.compose_frame",
        "pixel_format": "premultiplied-bgra",
        "width": width,
        "height": height,
        "stride": width * 4,
        "bytes": len(pixels),
        "sha256": hashlib.sha256(pixels).hexdigest(),
        "alpha_bbox": alpha_bbox(pixels, width, height),
        "format_strings": format_strings,
        "number_format_boundaries": format_boundaries,
        "layers": layers,
        "layer_signature": signature,
    }
    (FIXTURE_DIR / f"{frame_name}.bgra").write_bytes(pixels)
    (FIXTURE_DIR / f"{frame_name}.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(metadata, sort_keys=True))


def main() -> None:
    add_runtime_paths()
    dps = importlib.import_module(
        "plugins.star_resonance_plugin.panels.sao_gui_dps"
    )
    boss = importlib.import_module(
        "plugins.star_resonance_plugin.panels.sao_gui_bosshp"
    )
    hp = importlib.import_module(
        "plugins.star_resonance_plugin.panels.sao_gui_hp"
    )
    FIXTURE_DIR.mkdir(parents=True, exist_ok=True)

    dps_boundaries = [
        {"id": "dps_sub_1k", "input": 999.5, "output": dps._fmt_num(999.5)},
        {"id": "dps_1k", "input": 1000, "output": dps._fmt_num(1000)},
        {"id": "dps_1m", "input": 1_000_000, "output": dps._fmt_num(1_000_000)},
    ]
    boss_boundaries = [
        {"id": "boss_sub_1m", "input": 999_999, "output": boss._fmt_hp(999_999)},
        {"id": "boss_1m", "input": 1_000_000, "output": boss._fmt_hp(1_000_000)},
        {"id": "boss_1b", "input": 1_000_000_000, "output": boss._fmt_hp(1_000_000_000)},
    ]
    hp_boundaries = [
        {"id": "hp_three_digits", "input": 999, "output": hp._fmt_int(999)},
        {"id": "hp_four_digits", "input": 1000, "output": hp._fmt_int(1000)},
        {"id": "hp_millions", "input": 1_234_567, "output": hp._fmt_int(1_234_567)},
    ]

    with patch.object(dps.time, "time", return_value=NOW):
        idle = dps.DpsOverlay(None)
        mark_headless(idle)
        stabilize_dps(idle)
        write_frame(
            "dps_idle", "dps", "idle", idle.compose_frame(NOW), 13, 7, 10,
            {
                "total_damage": dps._fmt_num(idle._disp_total_damage),
                "total_dps": dps._fmt_num(idle._disp_total_dps),
                "elapsed": dps._fmt_time(idle._disp_elapsed),
            },
            dps_boundaries,
        )

        active = dps.DpsOverlay(None)
        mark_headless(active)
        active.update({
            "encounter_active": True,
            "elapsed_s": 65.0,
            "total_damage": 1_234_567,
            "total_dps": 18_993,
            "total_heal": 54_321,
            "total_hps": 836,
            "entities": [{
                "uid": 1001,
                "name": "Alpha",
                "profession": "Guardian",
                "fight_point": 37_500,
                "is_self": True,
                "damage_total": 1_234_567,
                "dps": 18_993,
                "damage_pct": 1.0,
                "bar_pct": 1.0,
                "heal_total": 54_321,
                "hps": 836,
            }],
        })
        stabilize_dps(active)
        write_frame(
            "dps_active", "dps", "active", active.compose_frame(NOW), 13, 7, 20,
            {
                "total_damage": dps._fmt_num(active._disp_total_damage),
                "total_dps": dps._fmt_num(active._disp_total_dps),
                "elapsed": dps._fmt_time(active._disp_elapsed),
                "row_damage": dps._fmt_num(active._rows[1001].disp_damage),
                "row_dps": dps._fmt_num(active._rows[1001].disp_dps),
            },
            dps_boundaries,
        )

    with patch.object(boss.time, "time", return_value=NOW):
        idle = boss.BossHpOverlay(None)
        mark_headless(idle)
        stabilize_boss(idle)
        write_frame(
            "bosshp_idle", "bosshp", "idle", idle.compose_frame(NOW), 9, 11, 30,
            {
                "hp": f"{boss._fmt_hp(idle._current_hp)}/{boss._fmt_hp(idle._total_hp)}",
                "percent": f"{int(round(idle._disp_hp_pct * 100))}%",
            },
            boss_boundaries,
        )

        active = boss.BossHpOverlay(None)
        mark_headless(active)
        active.update({
            "active": True,
            "boss_name": "Astra Prime",
            "hp_pct": 0.625,
            "current_hp": 1_250_000,
            "total_hp": 2_000_000,
            "hp_source": "packet",
            "shield_active": True,
            "shield_pct": 0.20,
            "has_break_data": True,
            "extinction_pct": 0.45,
            "breaking_stage": 2,
        })
        stabilize_boss(active)
        write_frame(
            "bosshp_active", "bosshp", "active", active.compose_frame(NOW), 9, 11, 40,
            {
                "hp": f"{boss._fmt_hp(active._current_hp)}/{boss._fmt_hp(active._total_hp)}",
                "percent": f"{int(round(active._disp_hp_pct * 100))}%",
            },
            boss_boundaries,
        )

    with patch.object(hp.time, "time", return_value=NOW):
        idle = hp.HpOverlay(None)
        mark_headless(idle)
        idle._spawn_time = NOW - 10.0
        stabilize_hp(idle)
        write_frame(
            "hp_idle", "hp", "idle", idle.compose_frame(NOW), 5, 17, 50,
            {
                "hp": f"{hp._fmt_int(idle._hp_max * idle._hp_pct_disp)}/{hp._fmt_int(idle._hp_max)}",
                "sta": idle._format_sta_text(),
            },
            hp_boundaries,
        )

        active = hp.HpOverlay(None)
        mark_headless(active)
        active._spawn_time = NOW - 10.0
        active.update_hp(123_456, 200_000, 60)
        active.set_player_info({
            "name": "Alpha",
            "profession": "Guardian",
            "uid": 1001,
        })
        active.update_sta(85, 100)
        active.set_boss_timer("02:15", "warn")
        stabilize_hp(active)
        write_frame(
            "hp_active", "hp", "active", active.compose_frame(NOW), 5, 17, 60,
            {
                "hp": f"{hp._fmt_int(active._hp_max * active._hp_pct_disp)}/{hp._fmt_int(active._hp_max)}",
                "sta": active._format_sta_text(),
            },
            hp_boundaries,
        )


if __name__ == "__main__":
    main()
