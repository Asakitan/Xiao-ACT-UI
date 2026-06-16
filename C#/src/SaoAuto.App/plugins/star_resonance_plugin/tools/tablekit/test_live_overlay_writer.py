# -*- coding: utf-8 -*-
"""Plain-script tests for LiveNameOverlayWriter.

Run:
    python -m tools.tablekit.test_live_overlay_writer
"""
from __future__ import annotations

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
if _SAO not in sys.path:
    sys.path.insert(0, _SAO)

from tools.tablekit.live_overlay_writer import LiveNameOverlayWriter


_RESULTS = []


def check(name: str, cond: bool, detail: str = "") -> None:
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"   {detail}" if detail else ""))
    _RESULTS.append((name, bool(cond)))


class FakeCache:
    def __init__(self):
        self.calls = []

    def observe_name(self, kind, id_, text, **kwargs):
        self.calls.append((kind, id_, text, kwargs))


def test_valid_text():
    check("CJK name is valid", LiveNameOverlayWriter.valid_text("敌方木桩"))
    check("empty text invalid", not LiveNameOverlayWriter.valid_text(""))
    check("control char invalid", not LiveNameOverlayWriter.valid_text("敌\x00方"))
    check("ASCII-only name is valid", LiveNameOverlayWriter.valid_text("dummy"))
    check("very long invalid", not LiveNameOverlayWriter.valid_text("怪" * 65))


def test_observe_persists_after_threshold():
    fc = FakeCache()
    wr = LiveNameOverlayWriter(fc, min_confirmations=2)
    check("first observation below threshold", wr.observe("monster", 114, "敌方木桩") is False)
    check("nothing written below threshold", len(fc.calls) == 0)
    check("second observation persists", wr.observe("monster", 114, "敌方木桩") is True)
    check("observe_name called once", len(fc.calls) == 1)
    kind, iid, text, kwargs = fc.calls[0]
    check("kind preserved", kind == "monster")
    check("id preserved", iid == 114)
    check("text preserved", text == "敌方木桩")
    check("confidence mem", kwargs.get("confidence") == "mem")


def test_invalid_inputs_do_not_write():
    fc = FakeCache()
    wr = LiveNameOverlayWriter(fc)
    check("invalid kind rejected", wr.observe("player", 1, "玩家") is False)
    check("invalid id rejected", wr.observe("monster", 0, "敌方木桩") is False)
    check("control text rejected", wr.observe("monster", 114, "bad\x00name") is False)
    check("no writes for invalid input", len(fc.calls) == 0)


def main() -> int:
    test_valid_text()
    test_observe_persists_after_threshold()
    test_invalid_inputs_do_not_write()
    n_pass = sum(1 for _, ok in _RESULTS if ok)
    n_fail = len(_RESULTS) - n_pass
    print()
    print(f"[live_overlay_writer] {n_pass} passed, {n_fail} failed")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
