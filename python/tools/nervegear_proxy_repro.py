# -*- coding: utf-8 -*-
# Diagnose: NerveGear input proxy — what actually swallows clicks.
#
# Boots the real compositor + real GpuNerveGearButton (which now attaches
# a Tk input proxy), then measures ground truth instead of guessing:
#
# - host WS_EX_TRANSPARENT + window region
# - the proxy toplevel's real HWND geometry
# - WindowFromPoint at: ball center, near-ball, far corner, screen
# center — prints WHICH window owns each point (class + rect), so a
# click-swallower is identified by name
# - synthesizes a click on a probe Tk window far from the ball:
# PROBE_CLICKED / PROBE_BLOCKED
# - synthesizes a click on the ball: NG_CLICKED? and whether foreground
# focus got stolen
import os
import sys
import time
import ctypes
from ctypes import wintypes
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

user32 = ctypes.windll.user32
BX, BY = 900, 500          # ball position
PXW, PYW = 300, 300        # probe window pos (far from ball)

got = {'ng': 0, 'probe': 0}


def _win_at(x: int, y: int) -> str:
    class _PT(ctypes.Structure):
        _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]
    h = user32.WindowFromPoint(_PT(x, y))
    if not h:
        return 'none'
    buf = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(h, buf, 64)
    r = wintypes.RECT()
    user32.GetWindowRect(h, ctypes.byref(r))
    ex = user32.GetWindowLongPtrW(h, -20)
    return (f'hwnd=0x{h & 0xFFFFFFFF:08X} class={buf.value!r} '
            f'rect=({r.left},{r.top},{r.right},{r.bottom}) '
            f'EXTRANSPARENT={bool(ex & 0x20)}')


def _region(hwnd: int) -> str:
    gdi = ctypes.windll.gdi32
    hrgn = gdi.CreateRectRgn(0, 0, 0, 0)
    kind = user32.GetWindowRgn(hwnd, hrgn)
    r = wintypes.RECT()
    gdi.GetRgnBox(hrgn, ctypes.byref(r))
    gdi.DeleteObject(hrgn)
    names = {0: 'NO_REGION', 1: 'NULL', 2: 'SIMPLE', 3: 'COMPLEX'}
    return f'{names.get(kind, kind)} box=({r.left},{r.top},{r.right},{r.bottom})'


def _click(x: int, y: int) -> None:
    user32.SetCursorPos(x, y)
    time.sleep(0.08)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)


def main() -> int:
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

    # Probe window far from the ball — a click here must land.
    probe = tk.Toplevel(root)
    probe.overrideredirect(True)
    probe.geometry(f'200x160+{PXW}+{PYW}')
    probe.configure(bg='#204060')
    probe.bind('<ButtonPress-1>', lambda e: got.__setitem__('probe', got['probe'] + 1))
    probe.deiconify()

    # Second probe UNDER the ball's rect: a click on the transparent
    # corner of the ball's 72x72 square must fall through the proxy's
    # color-keyed pixels and land here. (WindowFromPoint can NOT verify
    # this — it ignores layered-window colorkey per-pixel hit-testing
    # and always reports the proxy; only a real click tells the truth.)
    got['corner'] = 0
    under = tk.Toplevel(root)
    under.overrideredirect(True)
    under.geometry(f'200x200+{BX - 60}+{BY - 60}')
    under.configure(bg='#446622')
    under.bind('<ButtonPress-1>',
               lambda e: got.__setitem__('corner', got['corner'] + 1))
    under.deiconify()

    from gui_modules.sao_gui_nervegear_button import GpuNerveGearButton
    btn = GpuNerveGearButton(
        root, BX, BY,
        on_click=lambda: got.__setitem__('ng', got['ng'] + 1))
    btn.deiconify()

    def _pump(secs: float) -> None:
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < secs:
            try:
                root.update()
                uo.ensure_tk_poller()
            except Exception:
                pass
            time.sleep(0.02)

    _pump(2.0)

    host = uo.host
    ex = user32.GetWindowLongPtrW(uo.hwnd, -20)
    print(f'HOST hwnd=0x{uo.hwnd & 0xFFFFFFFF:08X} '
          f'EXTRANSPARENT={bool(ex & 0x20)} tracked={host.input_passthrough} '
          f'region={_region(uo.hwnd)}', flush=True)

    layer = btn._win._delegate.layer if getattr(btn._win, '_delegate', None) else None
    proxy = layer._input_proxy if layer is not None else None
    print(f'LAYER rect=({layer.x},{layer.y},{layer.width},{layer.height}) '
          f'visible={layer.visible}' if layer else 'LAYER none', flush=True)
    if proxy is not None:
        try:
            root.update_idletasks()
            ph = user32.GetAncestor(proxy.winfo_id(), 2)  # GA_ROOT
            r = wintypes.RECT()
            user32.GetWindowRect(ph, ctypes.byref(r))
            pex = user32.GetWindowLongPtrW(ph, -20)
            print(f'PROXY hwnd=0x{ph & 0xFFFFFFFF:08X} '
                  f'rect=({r.left},{r.top},{r.right},{r.bottom}) '
                  f'mapped={proxy.winfo_viewable()} ex=0x{pex & 0xFFFFFFFF:08X} '
                  f'region={_region(ph)}', flush=True)
        except Exception as e:
            print(f'PROXY inspect err {e!r}', flush=True)
    else:
        print('PROXY none (attach failed or pending)', flush=True)

    failures = []
    cx, cy = BX + 36, BY + 36
    hits = {}
    for label, x, y in (
            ('ball_center', cx, cy),
            ('near_ball(+150,0)', cx + 150, cy),
            ('corner_of_ball_rect', BX + 70, BY + 2),
            ('probe_center', PXW + 100, PYW + 80),
            ('far(1700,900)', 1700, 900)):
        info = _win_at(x, y)
        hits[label] = info
        print(f'AT {label:22} -> {info}', flush=True)

    # Per-pixel: a real click on the transparent corner of the ball's
    # 72x72 rect must fall through to the window underneath.
    _click(BX + 70, BY + 2)
    _pump(0.6)
    print(f'CLICK corner  -> corner={got["corner"]} '
          f'{"CORNER_PASSTHROUGH" if got["corner"] else "CORNER_SWALLOWED"}',
          flush=True)
    if not got['corner']:
        failures.append('transparent rect corner still swallowed by the '
                        'proxy — per-pixel hit shape not effective')

    fg_before = user32.GetForegroundWindow()
    _click(PXW + 100, PYW + 80)
    _pump(0.6)
    print(f'CLICK probe   -> probe={got["probe"]} '
          f'{"PROBE_CLICKED" if got["probe"] else "PROBE_BLOCKED"}', flush=True)
    if not got['probe']:
        failures.append('click far from ball blocked')

    user32.SetForegroundWindow(fg_before)
    _pump(0.3)
    fg_before = user32.GetForegroundWindow()
    _click(cx, cy)
    _pump(0.8)
    fg_after = user32.GetForegroundWindow()
    stolen = fg_before != fg_after and fg_after != 0

    def _wname(h):
        if not h:
            return 'none'
        b = ctypes.create_unicode_buffer(48)
        user32.GetClassNameW(h, b, 48)
        return f'0x{h & 0xFFFFFFFF:08X}({b.value})'
    print(f'CLICK ball    -> ng={got["ng"]} focus_stolen={stolen} '
          f'fg {_wname(fg_before)} -> {_wname(fg_after)}', flush=True)
    if not got['ng']:
        failures.append('ball click did not fire')
    if stolen:
        failures.append('ball click stole foreground focus — the game '
                        'would lose all keyboard input')

    if failures:
        print('\nFAIL:', flush=True)
        for f in failures:
            print('  -', f, flush=True)
        return 1
    print('\nAll NerveGear proxy checks passed.', flush=True)
    return 0


if __name__ == '__main__':
    rc = main()
    sys.stdout.flush()
    os._exit(rc)
