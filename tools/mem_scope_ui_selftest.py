# -*- coding: utf-8 -*-
"""Selftest for the declarative `input` leaf (Tk + ui_spec parity bits).

Covers normalize bounds, the UI.input builder, and — when a display is available
— the Tk SpecRenderer round-trip: typed text survives a redraw with an empty
server value (clobber guard) and a button fire collects payload["inputs"].

    python tools/mem_scope_ui_selftest.py
"""
from __future__ import annotations

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from act_platform.ui_spec import UI, normalize_ui_spec, LEAF_KINDS  # noqa: E402

_passed = 0
_failed = 0


def check(name: str, cond: bool, detail: str = "") -> None:
    global _passed, _failed
    if cond:
        _passed += 1
        print(f"  [ok] {name}")
    else:
        _failed += 1
        print(f"  [FAIL] {name} :: {detail}")


def test_normalize():
    print("[ui_spec input normalize]")
    check("input is a leaf kind", "input" in LEAF_KINDS)
    n = normalize_ui_spec(UI.input("query", value="x" * 5000, placeholder="find",
                                   input_type="number", width=300))["nodes"][0]
    check("kind=input", n["type"] == "input")
    check("id kept", n["id"] == "query")
    check("value clamped <=2000", len(n["value"]) == 2000)
    check("input_type honored", n["input_type"] == "number")
    check("width kept", n["width"] == 300)
    bad = normalize_ui_spec(UI.input("x", input_type="evil"))["nodes"][0]
    check("bad input_type -> text", bad["input_type"] == "text")
    longid = normalize_ui_spec(UI.input("z" * 200))["nodes"][0]
    check("id clamped <=80", len(longid["id"]) == 80)
    wide = normalize_ui_spec(UI.input("w", width=99999))["nodes"][0]
    check("width clamped <=2000", wide["width"] == 2000)


def test_tk_roundtrip():
    print("[tk input round-trip]")
    try:
        import tkinter as tk
        root = tk.Tk()
        root.withdraw()
    except Exception as exc:
        print(f"  [skip] no display ({exc})")
        return
    try:
        from gui_modules.sao_plugin_ui_render import SpecRenderer
        captured = {}

        def on_action(a, p):
            captured["a"] = a
            captured["p"] = p

        mount = tk.Frame(root)
        mount.pack()
        r = SpecRenderer(mount, on_action)
        spec = normalize_ui_spec(UI.panel("t", [
            UI.input("query", placeholder="find"),
            UI.button("Go", "search"),
        ]))
        r.render(spec)
        rn = r._inputs["query"]
        rn.parts["is_placeholder"] = False
        rn.parts["var"].set("12345")
        # redraw with the same (empty server value) spec must not clobber user text
        r.render(spec)
        check("typed text survives redraw", r._inputs["query"].parts["var"].get() == "12345")
        # fire the button -> inputs collected
        for node in r._nodes or []:
            if node.type == "button":
                r._fire(node)
        check("button fired search", captured.get("a") == "search")
        check("inputs collected", (captured.get("p") or {}).get("inputs", {}).get("query") == "12345")
    finally:
        try:
            root.destroy()
        except Exception:
            pass


def main():
    test_normalize()
    test_tk_roundtrip()
    print(f"\n{_passed} passed, {_failed} failed")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
