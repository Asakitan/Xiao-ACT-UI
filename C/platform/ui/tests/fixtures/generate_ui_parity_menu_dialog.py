from __future__ import annotations

import datetime as dt
import hashlib
import json
import sys
from pathlib import Path


FIXTURE_DIR = Path(__file__).resolve().parent
PYTHON_ROOT = FIXTURE_DIR.parents[4] / "python"
sys.path.insert(0, str(PYTHON_ROOT))

from ui_gpu import composer
from ui_gpu.state import PopupState
import gui_modules.sao_menu_hud as menu_hud


class FrozenDateTime(dt.datetime):
    @classmethod
    def now(cls, tz=None):
        value = cls(2026, 7, 14, 12, 34, 56)
        return value.replace(tzinfo=tz) if tz is not None else value


def freeze_menu_renderer() -> dict[str, str]:
    font_sao = PYTHON_ROOT / "assets" / "fonts" / "SAOUI.ttf"
    font_cjk = PYTHON_ROOT / "assets" / "fonts" / "ZhuZiAYuanJWD.ttf"
    if not font_sao.is_file() or not font_cjk.is_file():
        raise RuntimeError("the authoritative SAO and CJK font fixtures are required")
    menu_hud._dt.datetime = FrozenDateTime
    menu_hud._FONT_SAO = str(font_sao)
    menu_hud._FONT_CJK = str(font_cjk)
    return {
        "sao": str(font_sao.relative_to(PYTHON_ROOT)).replace("\\", "/"),
        "cjk": str(font_cjk.relative_to(PYTHON_ROOT)).replace("\\", "/"),
    }


def build_state() -> PopupState:
    state = PopupState()
    state.is_open = True
    state.fade_alpha = 1.0
    state.open_t0 = 0.0
    state.menu_items = [
        {"name": "Status", "icon": "S", "can_active": True},
        {"name": "Party", "icon": "P", "can_active": True},
        {"name": "System", "icon": "*", "can_active": True},
    ]
    state.btn_size = [54.0, 70.0, 54.0]
    state.btn_hover_t = [0.0, 1.0, 0.0]
    state.hover_btn_idx = 1
    state.active_menu_idx = 1
    state.child_menus = {
        "Party": [
            {"icon": ">", "label": "Invite"},
            {"icon": "*", "label": "Leave"},
        ]
    }
    state.child_rows = list(state.child_menus["Party"])
    state.pending_child_rows = []
    state.row_hover_t = [0.25, 0.0]
    state.row_anim_w = [240, 240]
    state.row_anim_t0 = 0.0
    state.child_fade_t = 0.0
    state.child_phase = "idle"
    state.hover_row_idx = 0
    state.line_h = 137
    state.hud_phase = 0.375
    state.hud_dx = 0
    state.hud_dy = 0
    return state


def main() -> None:
    fonts = freeze_menu_renderer()
    state = build_state()
    screen_w, screen_h = 1920, 1080
    rgba = composer.compose_rgba(
        state,
        hud_phase=0.375,
        screen_w=screen_w,
        screen_h=screen_h,
        reserved_rows=len(state.child_rows),
    )
    bgra = composer.to_premultiplied_bgra(rgba, master_alpha=1.0)
    output = FIXTURE_DIR / "menu_dialog.bgra"
    output.write_bytes(bgra)

    metadata = {
        "bytes": len(bgra),
        "format": "premultiplied-bgra",
        "height": rgba.height,
        "sha256": hashlib.sha256(bgra).hexdigest(),
        "source": {
            "composer": "ui_gpu.composer.compose_rgba",
            "state": "ui_gpu.state.PopupState",
            "transport": "ui_gpu.composer.to_premultiplied_bgra",
        },
        "state": {
            "child_rows": state.child_rows,
            "fade_alpha": state.fade_alpha,
            "fonts": fonts,
            "frozen_hud_time": "2026-07-14T12:34:56",
            "hud_phase": 0.375,
            "screen": [screen_w, screen_h],
        },
        "stride": rgba.width * 4,
        "width": rgba.width,
    }
    (FIXTURE_DIR / "menu_dialog.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(metadata, sort_keys=True))


if __name__ == "__main__":
    main()