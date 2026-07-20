from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path


fixture_dir = Path(__file__).resolve().parent
python_root = fixture_dir.parents[4] / "python"
sys.path.insert(0, str(python_root))

from gui_modules.sao_gui_nervegear_button import _premultiply_bgra, render_button
from gui_modules.sao_child_bar_gpu import (
    BarColors,
    _ChildBarSnapshot,
    _RowSnapshot,
    _compose_child_bar,
)


image = render_button("dark", 0.0, hover=True, pressed=False, alpha=1.0)
if image is None:
    raise RuntimeError("authoritative NerveGear renderer returned no image")

pixels = _premultiply_bgra(image)
output = fixture_dir / "nervegear_hover.bgra"
output.write_bytes(pixels)

metadata = {
    "format": "premultiplied-bgra",
    "width": image.width,
    "height": image.height,
    "stride": image.width * 4,
    "bytes": len(pixels),
    "sha256": hashlib.sha256(pixels).hexdigest(),
    "source": "gui_modules.sao_gui_nervegear_button.render_button",
    "state": {
        "theme": "dark",
        "glow_phase": 0.0,
        "hover": True,
        "pressed": False,
        "alpha": 1.0,
    },
}
(fixture_dir / "nervegear_hover.json").write_text(
    json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
print(json.dumps(metadata, sort_keys=True))


def lerp_color(left: str, right: str, amount: float) -> str:
    def parse(color: str) -> tuple[int, int, int]:
        value = color.lstrip("#")
        return int(value[0:2], 16), int(value[2:4], 16), int(value[4:6], 16)

    lo = parse(left)
    hi = parse(right)
    return "#%02x%02x%02x" % tuple(
        round(lo[channel] + (hi[channel] - lo[channel]) * amount)
        for channel in range(3)
    )


popup_colors = BarColors(
    "#202020", "#303030", "#eeeeee", "#ffffff", "#cccccc", "#d4af37", lerp_color
)
popup_snapshot = _ChildBarSnapshot(
    2,
    86,
    12,
    0.15,
    [
        _RowSnapshot(">", "Settings", 0.5, 120),
        _RowSnapshot("*", "Exit", 0.0, 120),
    ],
    "#202020",
    popup_colors,
)
popup = _compose_child_bar(popup_snapshot, 147, 91)
popup_pixels = _premultiply_bgra(popup)
popup_output = fixture_dir / "popup.bgra"
popup_output.write_bytes(popup_pixels)
popup_metadata = {
    "format": "premultiplied-bgra",
    "width": popup.width,
    "height": popup.height,
    "stride": popup.width * 4,
    "bytes": len(popup_pixels),
    "sha256": hashlib.sha256(popup_pixels).hexdigest(),
    "source": "gui_modules.sao_child_bar_gpu._compose_child_bar",
    "state": {
        "fade_t": 0.15,
        "rows": [
            {"icon": ">", "label": "Settings", "hover_t": 0.5, "row_w": 120},
            {"icon": "*", "label": "Exit", "hover_t": 0.0, "row_w": 120},
        ],
    },
}
(fixture_dir / "popup.json").write_text(
    json.dumps(popup_metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
print(json.dumps(popup_metadata, sort_keys=True))
