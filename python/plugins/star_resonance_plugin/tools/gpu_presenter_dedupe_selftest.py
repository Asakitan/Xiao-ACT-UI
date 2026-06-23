# -*- coding: utf-8 -*-
"""Selftest for GPU presenter frame-dedupe sequencing.

The BGRA/fisheye presenters must dedup texture uploads by a monotonic
frame sequence carried in an atomic (bytes, w, h, seq) snapshot — not by
``id()`` of the bytes object: same-size frame buffers are freed and
reallocated at the same address frequently, so an id collision with the
last uploaded object silently skips a real frame.

    python tools/gpu_presenter_dedupe_selftest.py
"""
from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
_PY_ROOT = _HERE.parents[2]
for _path in (str(_ROOT), str(_PY_ROOT)):
    if _path in sys.path:
        sys.path.remove(_path)
    sys.path.insert(0, _path)

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


def test_bgra_presenter_sequencing() -> None:
    print("[BgraPresenter frame sequencing]")
    from render.gpu_overlay_window import BgraPresenter

    p = BgraPresenter()
    check("starts with no snapshot", p._frame_snap is None)

    # bytes(bytearray(...)) defeats constant folding: equal content,
    # guaranteed distinct objects.
    frame_a = bytes(bytearray(16))
    p.set_frame(frame_a, 2, 2)
    check("first frame snapshot staged", p._frame_snap == (frame_a, 2, 2, 1))

    p.set_frame(frame_a, 2, 2)
    check("same bytes object keeps seq (alpha-only tick)", p._frame_snap[3] == 1)

    frame_b = bytes(bytearray(16))  # equal content, distinct object
    p.set_frame(frame_b, 2, 2)
    check("distinct bytes object bumps seq", p._frame_snap == (frame_b, 2, 2, 2))

    p.set_frame(frame_a, 2, 2)
    check("returning to a prior object still bumps seq", p._frame_snap[3] == 3)

    p.clear()
    check("clear drops snapshot", p._frame_snap is None and p._frame_bytes is None)

    p.set_frame(frame_b, 2, 2)
    check("seq survives clear monotonically", p._frame_snap[3] == 4)


def test_no_id_identity_left() -> None:
    print("[id() identity comparisons removed]")
    gow = (_PY_ROOT / "render" / "gpu_overlay_window.py").read_text(encoding="utf-8")
    check("BgraPresenter no longer tracks _last_uploaded_id", "_last_uploaded_id" not in gow)
    check("BgraPresenter no longer keys uploads on id()", "id(bgra)" not in gow)
    check("BgraPresenter render reads atomic snapshot", "snap = self._frame_snap" in gow)

    fisheye = (_PY_ROOT / "gui_modules" / "sao_gui_fisheye_mixin.py").read_text(encoding="utf-8")
    check("fisheye presenter no longer keys uploads on id()", "id(rgb)" not in fisheye)
    check(
        "fisheye presenter stages atomic snapshot",
        "self._frame_snap = (rgb_bytes, int(w), int(h), self._frame_seq)" in fisheye,
    )
    check("fisheye presenter dedups by seq", "elif seq != self._uploaded_seq:" in fisheye)


def main() -> int:
    test_bgra_presenter_sequencing()
    test_no_id_identity_left()
    print(f"\n{_passed} passed, {_failed} failed")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
