from __future__ import annotations

import datetime
import hashlib
import json
import sys
from pathlib import Path
from typing import Any
from unittest import mock


FIXTURE_DIR = Path(__file__).resolve().parent
ASSET_DIR = FIXTURE_DIR.parents[1] / "assets" / "entity"
PYTHON_ROOT = FIXTURE_DIR.parents[4] / "python"
FONT_DIR = PYTHON_ROOT / "assets" / "fonts"
SAO_FONT = FONT_DIR / "SAOUI.ttf"
CJK_FONT = FONT_DIR / "ZhuZiAYuanJWD.ttf"
FROZEN_DATETIME = "2026-07-14T12:34:56"
SCREEN_WIDTH = 1920
SCREEN_HEIGHT = 1080
HUD_PHASE = 0.375
FADE_ALPHA = 1.0
EXPECTED_WIDTH = 442
EXPECTED_HEIGHT = 430
EXPECTED_STRIDE = 1768
EXPECTED_BYTES = 760240

sys.path.insert(0, str(PYTHON_ROOT))

from gui_modules import sao_menu_hud
from ui_gpu import composer, hud_layout, menu_bar_layout
from ui_gpu.state import PopupState


_REAL_DATETIME = datetime.datetime


class FrozenDateTime(_REAL_DATETIME):
    @classmethod
    def now(cls, tz: datetime.tzinfo | None = None) -> FrozenDateTime:
        value = cls.fromisoformat(FROZEN_DATETIME)
        if tz is not None:
            value = value.replace(tzinfo=tz)
        return value

    @classmethod
    def today(cls) -> FrozenDateTime:
        return cls.fromisoformat(FROZEN_DATETIME)


def menu_items() -> list[dict[str, Any]]:
    return [
        {"name": "Control", "icon": "C", "can_active": False},
        {"name": "Tools", "icon": "T", "can_active": False},
        {"name": "Plugins", "icon": "P", "can_active": False},
        {"name": "Skins", "icon": "S", "can_active": False},
        {"name": "About", "icon": "?", "can_active": True},
    ]


def popup_state(hover_index: int | None) -> PopupState:
    state = PopupState()
    state.is_open = True
    state.fade_alpha = FADE_ALPHA
    state.menu_items = menu_items()
    state.btn_size = [54.0] * len(state.menu_items)
    state.btn_hover_t = [0.0] * len(state.menu_items)
    state.hover_btn_idx = hover_index
    state.active_menu_idx = None
    state.child_rows = []
    state.pending_child_rows = []
    state.hud_phase = HUD_PHASE
    if hover_index is not None:
        state.btn_size[hover_index] = 70.0
        state.btn_hover_t[hover_index] = 1.0
    return state


def state_metadata(state: PopupState) -> dict[str, Any]:
    return {
        "menu_items": [dict(item) for item in state.menu_items],
        "btn_size": list(state.btn_size),
        "btn_hover_t": list(state.btn_hover_t),
        "hover": state.hover_btn_idx,
        "active": state.active_menu_idx,
        "child_rows": list(state.child_rows),
    }


def validate_frame(image_width: int, image_height: int, pixels: bytes) -> None:
    stride = image_width * 4
    expected = (EXPECTED_WIDTH, EXPECTED_HEIGHT, EXPECTED_STRIDE, EXPECTED_BYTES)
    actual = (image_width, image_height, stride, len(pixels))
    if actual != expected:
        raise RuntimeError(
            f"Entity menu authority dimensions drifted: expected {expected}, got {actual}"
        )
    for offset in range(0, len(pixels), 4):
        alpha = pixels[offset + 3]
        if any(channel > alpha for channel in pixels[offset : offset + 3]):
            raise RuntimeError(
                f"non-premultiplied BGRA pixel at byte offset {offset}"
            )


def write_frame(name: str, hover_index: int | None) -> dict[str, Any]:
    state = popup_state(hover_index)
    rgba = composer.compose_rgba(
        state,
        HUD_PHASE,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        reserved_rows=0,
    )
    pixels = composer.to_premultiplied_bgra(rgba, FADE_ALPHA)
    validate_frame(rgba.width, rgba.height, pixels)
    filename = f"w20_entity_menu_hud_{name}.bgra"
    ASSET_DIR.mkdir(parents=True, exist_ok=True)
    (ASSET_DIR / filename).write_bytes(pixels)
    return {
        "name": name,
        "file": filename,
        "asset": f"assets/entity/{filename}",
        "width": rgba.width,
        "height": rgba.height,
        "stride": rgba.width * 4,
        "bytes": len(pixels),
        "sha256": hashlib.sha256(pixels).hexdigest(),
        "state": state_metadata(state),
    }


def hit_rects() -> list[dict[str, Any]]:
    return [
        {
            "index": index,
            "rect": [40, 40 + index * 70, 110, 110 + index * 70],
        }
        for index in range(5)
    ]


def main() -> None:
    for font in (SAO_FONT, CJK_FONT):
        if not font.is_file():
            raise RuntimeError(f"required authority font is missing: {font}")
    if composer.HUD_PAD != 40:
        raise RuntimeError(
            f"Entity menu authority HUD pad drifted: expected 40, got {composer.HUD_PAD}"
        )
    if menu_bar_layout.MAX_VISIBLE != 9:
        raise RuntimeError(
            "Entity menu max-slot authority drifted: "
            f"expected 9, got {menu_bar_layout.MAX_VISIBLE}"
        )

    hud_layout.reset()
    with (
        mock.patch.object(sao_menu_hud._dt, "datetime", FrozenDateTime),
        mock.patch.object(sao_menu_hud, "_FONT_SAO", str(SAO_FONT)),
        mock.patch.object(sao_menu_hud, "_FONT_CJK", str(CJK_FONT)),
    ):
        frames = [write_frame("idle", None)]
        frames.extend(
            write_frame(f"hover_{index}", index) for index in range(5)
        )

    metadata = {
        "schema": "sao.ui.python-authority.w20.entity-menu-hud.v1",
        "format": "premultiplied-bgra",
        "dimensions": {
            "width": EXPECTED_WIDTH,
            "height": EXPECTED_HEIGHT,
            "stride": EXPECTED_STRIDE,
            "bytes": EXPECTED_BYTES,
        },
        "source": {
            "state": "ui_gpu.state.PopupState",
            "composition": "ui_gpu.composer.compose_rgba",
            "transport": "ui_gpu.composer.to_premultiplied_bgra",
        },
        "frozen_environment": {
            "datetime": FROZEN_DATETIME,
            "fonts": [
                "python/assets/fonts/SAOUI.ttf",
                "python/assets/fonts/ZhuZiAYuanJWD.ttf",
            ],
            "screen": [SCREEN_WIDTH, SCREEN_HEIGHT],
            "hud_phase": HUD_PHASE,
            "fade_alpha": FADE_ALPHA,
        },
        "menu": {
            "max_slots": menu_bar_layout.MAX_VISIBLE,
            "item_count": 5,
            "items": menu_items(),
            "child_rows": [],
            "geometry": {
                "hud_pad": 40,
                "column_origin": [40, 40],
                "slot": 70,
                "base_size": 54,
                "hover_size": 70,
                "hit_rects": hit_rects(),
            },
        },
        "frames": frames,
    }
    output = FIXTURE_DIR / "w20_entity_menu_hud.json"
    output.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {
                "manifest": str(output),
                "manifest_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                "frames": {
                    frame["name"]: frame["sha256"] for frame in frames
                },
            },
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()
