"""Deterministic overlay-layout fixture (no external input).

Mirrors `OverlayLayoutPlanner.PlaceLane` from `SaoAuto.Overlay`:
- Stack lanes vertically with `gap` between them.
- Right-align inside `screenWidth`, top-anchor at `topMargin + cumulative`.

The C# parity test reads `data.lanes[*]` and asserts position drift
within `numericEpsilon = 0.5`.
"""

from __future__ import annotations

import argparse

from _common import add_common_args, write_fixture


def plan(screen_w: int, screen_h: int, top: int, right: int, gap: int, lanes: list[tuple[str, int, int]]):
    out = []
    cy = top
    for name, w, h in lanes:
        x = screen_w - right - w
        y = cy
        out.append({"name": name, "x": x, "y": y, "w": w, "h": h})
        cy += h + gap
    return {
        "screen": {"w": screen_w, "h": screen_h},
        "margins": {"top": top, "right": right, "gap": gap},
        "lanes": out,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser, "overlay_layout.json")
    args = parser.parse_args()

    data = plan(
        screen_w=2560,
        screen_h=1440,
        top=80,
        right=24,
        gap=12,
        lanes=[
            ("dps", 320, 96),
            ("boss", 320, 64),
            ("entity", 320, 200),
        ],
    )
    write_fixture(
        args.output,
        kind="overlay_layout",
        source="pure-math (no external input)",
        data=data,
        force=args.force,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
