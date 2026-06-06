# -*- coding: utf-8 -*-
"""Static regression checks for the WebView SAO menu column layout."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MENU_HTML = ROOT / "web" / "menu.html"
ITEM_SIZE_PX = 54
ITEM_GAP_PX = 15
FRAME_MARGIN_PX = 6


def _read_menu() -> str:
    return MENU_HTML.read_text(encoding="utf-8")


def _css_block(source: str, selector: str) -> str:
    match = re.search(rf"{re.escape(selector)}\s*\{{(?P<body>.*?)\}}", source, re.S)
    if not match:
        raise AssertionError(f"missing CSS block: {selector}")
    return match.group("body")


def _px_property(block: str, prop: str) -> int:
    match = re.search(rf"\b{re.escape(prop)}\s*:\s*(\d+)px\b", block)
    if not match:
        raise AssertionError(f"missing px property: {prop}")
    return int(match.group(1))


def main() -> int:
    html = _read_menu()
    item_count = len(re.findall(r'<li\s+class="[^"]*\bitem\b[^"]*"\s+data-name=', html))
    if item_count <= 0:
        raise AssertionError("no SAO menu items found")

    required_column_height = item_count * ITEM_SIZE_PX + max(0, item_count - 1) * ITEM_GAP_PX
    item_box_height = _px_property(_css_block(html, ".item_box"), "height")
    frame_height = _px_property(_css_block(html, ".menu-frame"), "height")

    scan_match = re.search(r"100%\s*\{\s*top:\s*(\d+)px;\s*opacity:\s*0;\s*\}", html)
    if not scan_match:
        raise AssertionError("missing menu-frame-scan final top")
    scan_top = int(scan_match.group(1))

    js_height_match = re.search(r"var\s+menuFrameHeight\s*=\s*(\d+)\s*;", html)
    if not js_height_match:
        raise AssertionError("openMenu() must bound by the actual menu frame height")
    js_frame_height = int(js_height_match.group(1))

    if item_box_height < required_column_height:
        raise AssertionError(
            f"item_box height {item_box_height}px clips {item_count} items "
            f"(needs >= {required_column_height}px)"
        )
    if frame_height < item_box_height + FRAME_MARGIN_PX * 2:
        raise AssertionError("menu-frame must wrap item_box plus both HUD margins")
    if scan_top < item_box_height:
        raise AssertionError("menu frame scan line does not sweep through the full item column")
    if js_frame_height != frame_height:
        raise AssertionError("openMenu() viewport clamp must match .menu-frame height")

    print(
        f"OK web menu layout: {item_count} items, "
        f"item_box={item_box_height}px, frame={frame_height}px"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL web menu layout selftest: {exc}", file=sys.stderr)
        raise SystemExit(1)
