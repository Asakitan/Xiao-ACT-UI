from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path
from typing import Any


FIXTURE_DIR = Path(__file__).resolve().parent
ASSET_DIR = FIXTURE_DIR.parents[1] / "assets" / "entity"
PYTHON_ROOT = FIXTURE_DIR.parents[4] / "python"
sys.path.insert(0, str(PYTHON_ROOT))

import _sao_cy_uihelpers as cy_ui
from gui_modules.sao_gui_nervegear_button import (
    GpuNerveGearButton,
    _premultiply_bgra,
    render_button,
)


def write_frame(name: str, *, hover: bool, pressed: bool) -> dict[str, Any]:
    image = render_button(
        "dark",
        0.0,
        hover=hover,
        pressed=pressed,
        alpha=1.0,
    )
    if image is None:
        raise RuntimeError("authoritative NerveGear renderer returned no image")
    pixels = _premultiply_bgra(image)
    filename = f"w19_entity_nervegear_{name}.bgra"
    ASSET_DIR.mkdir(parents=True, exist_ok=True)
    (ASSET_DIR / filename).write_bytes(pixels)
    return {
        "name": name,
        "asset": f"assets/entity/{filename}",
        "file": filename,
        "width": image.width,
        "height": image.height,
        "stride": image.width * 4,
        "bytes": len(pixels),
        "sha256": hashlib.sha256(pixels).hexdigest(),
        "state": {
            "theme": "dark",
            "glow_phase": 0.0,
            "hover": hover,
            "pressed": pressed,
            "alpha": 1.0,
        },
    }


def phase_checkpoint(
    target: float,
    elapsed_ms: int,
    expected_phase: str,
) -> dict[str, Any]:
    elapsed = elapsed_ms / 1000.0
    alpha, done, _ = cy_ui.popup_fade_alpha(elapsed, 0.0, 0.0, target)
    return {
        "elapsed_ms": elapsed_ms,
        "alpha": round(float(alpha), 6),
        "done": bool(done),
        "phase": expected_phase,
    }


class InteractionProbe:
    def __init__(self) -> None:
        self._hover = False
        self._pressed = False
        self._drag_start = None
        self._x = 100
        self._y = 200
        self.click_count = 0
        self.menu_visible = False
        self.hover_events: list[bool] = []
        self._on_hover = self.hover_events.append
        self._on_click = self._open_menu
        self._on_move_end = None
        self._on_right_click = None

    def _open_menu(self) -> None:
        self.click_count += 1
        self.menu_visible = True

    def _cursor_screen_pos(
        self,
        local_x: float = 0.0,
        local_y: float = 0.0,
    ) -> tuple[float, float]:
        return self._x + local_x, self._y + local_y

    def _stage_frame(self, force: bool = False) -> None:
        del force

    def geometry(self, value: str) -> None:
        x_text, y_text = value.removeprefix("+").split("+")
        self._x = int(x_text)
        self._y = int(y_text)

    def snapshot(self) -> dict[str, Any]:
        state = "pressed" if self._pressed else "hover" if self._hover else "idle"
        return {
            "nervegear_state": state,
            "menu_visible": self.menu_visible,
            "click_count": self.click_count,
            "hover_events": list(self.hover_events),
        }


def build_interaction_trace() -> dict[str, Any]:
    probe = InteractionProbe()
    steps = [{"event": "initial", "expected": probe.snapshot()}]
    events = [
        {"op": "mouse_move", "x": 36, "y": 36},
        {"op": "left_down", "x": 36, "y": 36},
        {"op": "left_up", "x": 36, "y": 36},
        {"op": "mouse_leave", "x": 36, "y": 36},
    ]
    for event in events:
        op = event["op"]
        if op == "mouse_move":
            GpuNerveGearButton._handle_cursor_pos(probe, event["x"], event["y"])
        elif op == "left_down":
            GpuNerveGearButton._handle_mouse_button(
                probe, 0, 1, 0, event["x"], event["y"]
            )
        elif op == "left_up":
            GpuNerveGearButton._handle_mouse_button(
                probe, 0, 0, 0, event["x"], event["y"]
            )
        elif op == "mouse_leave":
            GpuNerveGearButton._handle_cursor_leave(probe)
        else:
            raise RuntimeError(f"unsupported interaction event: {op}")
        steps.append({"event": op, "expected": probe.snapshot()})
    return {"events": events, "steps": steps}


def main() -> None:
    metadata = {
        "schema": "sao.ui.python-authority.w19.entity.v1",
        "source": {
            "frame": "gui_modules.sao_gui_nervegear_button.render_button",
            "transport": "gui_modules.sao_gui_nervegear_button._premultiply_bgra",
            "phase": "_sao_cy_uihelpers.popup_fade_alpha",
            "interaction": "gui_modules.sao_gui_nervegear_button.GpuNerveGearButton",
        },
        "format": "premultiplied-bgra",
        "frames": [
            write_frame("idle", hover=False, pressed=False),
            write_frame("hover", hover=True, pressed=False),
            write_frame("pressed", hover=True, pressed=True),
        ],
        "phase": {
            "opening": {
                "duration_ms": 450,
                "checkpoints": [
                    phase_checkpoint(1.0, 0, "opening"),
                    phase_checkpoint(1.0, 225, "opening"),
                    phase_checkpoint(1.0, 449, "opening"),
                    phase_checkpoint(1.0, 450, "open"),
                ],
            },
            "closing": {
                "duration_ms": 300,
                "checkpoints": [
                    phase_checkpoint(0.0, 0, "closing"),
                    phase_checkpoint(0.0, 150, "closing"),
                    phase_checkpoint(0.0, 299, "closing"),
                    phase_checkpoint(0.0, 300, "closed"),
                ],
            },
        },
        "interaction": build_interaction_trace(),
    }
    output = FIXTURE_DIR / "w19_entity_parity.json"
    output.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(metadata, sort_keys=True))


if __name__ == "__main__":
    main()
