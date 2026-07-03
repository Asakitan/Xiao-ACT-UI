# -*- coding: utf-8 -*-
"""Selftest: GpuNerveGearButton.raise_topmost() must not break its own
click routing in unified compositor mode.

Reproduces the user's report: "the button stopped being clickable [and
window clipping was gone too]" a few seconds after opening it. Root
cause: raise_topmost() operated on self._win._hwnd, which in unified
mode is uo.hwnd — the ONE shared host window every layer draws into,
not a window owned by this button. Pushing that SHARED host to real
WS_EX_TOPMOST via SetWindowPos placed it ABOVE the button's own Tk
input proxy (a separate, different-thread window) — a real click then
landed on the (transparent, but now literally on top) host instead of
handing off to the proxy underneath, the same same-thread-only
limitation documented for HTTRANSPARENT/WS_EX_TRANSPARENT elsewhere in
this codebase. _start_topmost_loop calls this every 2 seconds in the
real app, so the break happened within seconds of the button
appearing — matching the user's report closely.

Asserts: WindowFromPoint at the ball's center keeps reporting the
button's OWN proxy (not the host) and a real synthesized click keeps
registering, across several raise_topmost() calls.
"""
import os
import sys
import time
import ctypes

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main() -> int:
    import tkinter as tk

    got = {'ng': 0}

    class _PT(ctypes.Structure):
        _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]

    def _win_class(u, h):
        if not h:
            return 'none'
        b = ctypes.create_unicode_buffer(48)
        u.GetClassNameW(h, b, 48)
        return str(b.value)

    root = tk.Tk()
    root.geometry('80x40+0+0')
    root.withdraw()

    from render.gpu_overlay_window import (
        set_unified_overlay_mode, prestart_unified_overlay,
        _get_unified_overlay)
    set_unified_overlay_mode(True)
    prestart_unified_overlay(root)
    uo = _get_unified_overlay(root)
    if not uo.wait_ready(timeout=15.0):
        print('COMPOSITOR_TIMEOUT', flush=True)
        return 3

    from gui_modules.sao_gui_nervegear_button import GpuNerveGearButton
    btn = GpuNerveGearButton(
        root, 900, 500, on_click=lambda: got.__setitem__('ng', got['ng'] + 1))
    btn.deiconify()

    def _pump(secs):
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < secs:
            root.update()
            uo.ensure_tk_poller()
            time.sleep(0.02)
    _pump(1.0)

    u = ctypes.windll.user32
    cx, cy = 936, 536

    def _click_and_check(tag):
        got['ng'] = 0
        wfp = u.WindowFromPoint(_PT(cx, cy))
        wfp_class = _win_class(u, wfp)
        u.SetCursorPos(cx, cy)
        time.sleep(0.08)
        u.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.05)
        u.mouse_event(0x0004, 0, 0, 0, 0)
        _pump(0.6)
        clicked = bool(got['ng'])
        print(f'[{tag}] WFP_class={wfp_class!r} click_registered={clicked}',
              flush=True)
        return wfp_class, clicked

    failures = []
    _, clicked0 = _click_and_check('before any raise_topmost')
    if not clicked0:
        failures.append('ball was not clickable even before raise_topmost — '
                        'unrelated regression, check proxy setup')

    for batch in ((3, 'after 3x raise_topmost'),
                  (3, 'after 6x raise_topmost'),
                  (3, 'after 9x raise_topmost')):
        n, tag = batch
        for _ in range(n):
            btn.raise_topmost()
            _pump(0.3)
        wfp_class, clicked = _click_and_check(tag)
        if 'TkChild' not in wfp_class and 'Tk' not in wfp_class:
            failures.append(f'{tag}: WindowFromPoint reports {wfp_class!r} — '
                            f'the host (or something else) is sitting above '
                            f'the proxy again')
        if not clicked:
            failures.append(f'{tag}: ball click did not register — '
                            f'raise_topmost broke the proxy')

    if failures:
        print('\nFAIL:', flush=True)
        for f in failures:
            print('  -', f, flush=True)
        return 1
    print('\nraise_topmost() no longer breaks the NerveGear proxy.', flush=True)
    return 0


if __name__ == '__main__':
    rc = main()
    sys.stdout.flush()
    os._exit(rc)
