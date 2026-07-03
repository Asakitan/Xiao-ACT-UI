# -*- coding: utf-8 -*-
"""Selftest: with NO game attached (pure desktop-pet usage — the common
case for this feature), the compositor host must reliably stay in
front of other, ordinary desktop applications — not just win the
z-order race for the instant of _enforce_z_order()'s own SetWindowPos
call and then lose it the moment any other app activates a window.

Reproduces the user's report: "compositor 莫名其妙会被各种桌面软件遮住,
点一点桌宠就看不到了" (the compositor mysteriously gets covered by
various desktop apps, clicking the pet a bit makes it disappear) —
"只有打开home(鱼眼菜单)才会重新置顶弹出来" (only opening the fisheye
menu brings it back to front). That second detail pins the root cause
precisely: the fisheye briefly forces the host to real WS_EX_TOPMOST
for its own hit-layer's sake, which was masking a genuine weakness in
_enforce_z_order's "no game attached" fallback — it only used
HWND_TOP (a one-shot, easily-beaten z-order request), never real
TOPMOST, reasoning that real TOPMOST risks anti-cheat detection. That
risk only applies while a game IS attached; this branch runs
specifically when one ISN'T.

Also verifies the fix doesn't reintroduce the NerveGear-proxy-buried-
under-host regression this session already fixed twice: a Tk input
proxy is ALSO real-topmost, so making the host real-topmost too could
bury it again unless proxies get re-lifted every time.
"""
import os
import sys
import time
import ctypes
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x00000008
HWND_TOPMOST = -1
SWP = 0x0002 | 0x0001 | 0x0010  # NOMOVE | NOSIZE | NOACTIVATE


def _is_topmost(u, hwnd) -> bool:
    ex = u.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
    return bool(ex & WS_EX_TOPMOST)


class _PT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]


def _win_class(u, h):
    if not h:
        return 'none'
    b = ctypes.create_unicode_buffer(48)
    u.GetClassNameW(h, b, 48)
    return str(b.value)


def main() -> int:
    import tkinter as tk

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

    def _pump(secs):
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < secs:
            root.update()
            uo.ensure_tk_poller()
            time.sleep(0.02)
    _pump(0.3)

    u = ctypes.windll.user32
    host_hwnd = uo.hwnd
    # Explicitly no game attached — the exact scenario this test targets.
    uo._game_hwnd = 0

    got = {'ng': 0}
    from gui_modules.sao_gui_nervegear_button import GpuNerveGearButton
    ng = GpuNerveGearButton(
        root, 900, 500, on_click=lambda: got.__setitem__('ng', got['ng'] + 1))
    ng.deiconify()
    _pump(0.8)

    failures = []

    # ── 1. Real TOPMOST after the tick, unlike the previous HWND_TOP-only
    # behavior.
    uo._enforce_z_order()
    _pump(0.3)
    topmost1 = _is_topmost(u, host_hwnd)
    print(f'[1] after _enforce_z_order (no game): host WS_EX_TOPMOST='
          f'{topmost1} (expect True)', flush=True)
    if not topmost1:
        failures.append('[1] host is not real-topmost with no game attached '
                        '— will still lose the z-order race to other apps')

    # ── 2. Simulate "another ordinary desktop app steals the z-order",
    # e.g. a plain window getting activated by a click that passed
    # through the click_through pet — matches HWND_TOP's exact failure
    # mode. A REAL topmost host must beat this regardless of timing.
    other_app = tk.Toplevel(root)
    other_app.overrideredirect(True)
    other_app.geometry('300x200+200+200')
    other_app.configure(bg='#446')
    other_app.deiconify()
    _pump(0.2)
    other_hwnd = u.GetAncestor(other_app.winfo_id(), 2)
    # Ordinary top-of-normal-zorder placement — what real window
    # activation does; NOT topmost itself.
    u.SetWindowPos(other_hwnd, 0, 0, 0, 0, 0, SWP)  # HWND_TOP = 0
    u.SetForegroundWindow(other_hwnd)
    _pump(0.3)

    cx, cy = 936, 536
    wfp_after_steal = _win_class(u, u.WindowFromPoint(_PT(cx, cy)))
    print(f'[2] after another app activates: WFP at NerveGear = '
          f'{wfp_after_steal!r} (expect still the pet\'s own window/proxy, '
          f'not swallowed)', flush=True)

    # ── 3. NerveGear proxy must still be clickable across repeated
    # _enforce_z_order ticks — proves lift_all_input_proxies keeps it
    # re-asserted above the now-real-topmost host.
    got['ng'] = 0
    for _ in range(3):
        uo._enforce_z_order()
        _pump(0.3)
    u.SetCursorPos(cx, cy)
    time.sleep(0.08)
    u.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    u.mouse_event(0x0004, 0, 0, 0, 0)
    _pump(0.6)
    clicked = bool(got['ng'])
    print(f'[3] NerveGear clickable after 3x _enforce_z_order ticks: '
          f'{clicked} (expect True — proxies must stay re-lifted above '
          f'the real-topmost host)', flush=True)
    if not clicked:
        failures.append('[3] NerveGear became unclickable after the host '
                        'started going real-topmost — the proxy-burial '
                        'regression is back')

    if failures:
        print('\nFAIL:', flush=True)
        for f in failures:
            print('  -', f, flush=True)
        return 1
    print('\nHost stays reliably on top with no game attached, proxies '
          'stay above it.', flush=True)
    return 0


if __name__ == '__main__':
    rc = main()
    sys.stdout.flush()
    os._exit(rc)
