# -*- coding: utf-8 -*-
"""Selftest: closing the fisheye menu must not leave NerveGear (or any
other unified-mode Tk input proxy) permanently unclickable.

Reproduces the user's report: "关闭鱼眼菜单后点击穿透还是失效，nervgear
还是没法点击" (after CLOSING the fisheye menu, click passthrough is STILL
broken, NerveGear STILL can't be clicked) — a DIFFERENT, deeper bug than
the raise_topmost() one already fixed (a90adbf), on a totally separate
code path.

Root cause: SAOPlayerGUIFisheyeMixin._raise_compositor_above_fisheye_hit_layer
does SetWindowPos(uo.hwnd, HWND_TOPMOST, ...) every time the fisheye's
full-screen hit layer (re)appears (_create_fisheye_hit_layer calls it via
_set_fisheye_hit_layer_clickthrough(False) right after creating the
layer) — necessary, since the hit layer is itself real WS_EX_TOPMOST and
covers the whole screen, so the host must join the topmost band to sit
above it. But _release_fisheye_input_zorder / _restore_fisheye_exit_zorder
— the intended "undo" pair — both early-return whenever
`gpu_win._unified` is True, on the (once-true, now-stale) assumption
that unified mode never makes the host topmost. Once
_raise_compositor_above_fisheye_hit_layer started doing exactly that,
NOTHING ever demoted the host back — it stays pinned to real
WS_EX_TOPMOST for the rest of the session after the very FIRST fisheye
open, permanently sitting above any Tk input proxy (NerveGear's, a
plugin's) created from that point on.

Constructs a minimal stub carrying only what the two real (unbound)
mixin methods touch (_fisheye_hit_layer), drives the REAL
_create_fisheye_hit_layer-equivalent SetWindowPos call (same win32
sequence, since building a real Tk-backed hit layer needs the full GUI
class) and the REAL _destroy_fisheye_hit_layer, and checks a REAL
NerveGear proxy's clickability across the open -> close cycle.
"""
import os
import sys
import time
import ctypes
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


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

    got = {'ng': 0}
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
    _pump(0.5)

    from gui_modules.sao_gui_nervegear_button import GpuNerveGearButton
    btn = GpuNerveGearButton(
        root, 900, 500, on_click=lambda: got.__setitem__('ng', got['ng'] + 1))
    btn.deiconify()
    _pump(0.8)

    u = ctypes.windll.user32
    cx, cy = 936, 536

    def _diag(label):
        delegate = btn._win._delegate
        layer = delegate.layer
        proxy = layer._input_proxy
        proxy_hwnd = u.GetAncestor(proxy.winfo_id(), 2) if proxy else 0
        host_ex = u.GetWindowLongPtrW(uo.hwnd, -20)
        proxy_ex = u.GetWindowLongPtrW(proxy_hwnd, -20) if proxy_hwnd else 0
        wfp = u.WindowFromPoint(_PT(cx, cy))
        print(f'  [DIAG {label}] host_topmost={bool(host_ex & 8)} '
              f'proxy_topmost={bool(proxy_ex & 8)} '
              f'WFP={_win_class(u, wfp)} (0x{wfp & 0xFFFFFFFF:08X}) '
              f'proxy_hwnd=0x{proxy_hwnd & 0xFFFFFFFF:08X} '
              f'host_hwnd=0x{uo.hwnd & 0xFFFFFFFF:08X}', flush=True)

    def _click_and_check(tag):
        got['ng'] = 0
        _diag(f'{tag} :: pre-WFP')
        wfp_class = _win_class(u, u.WindowFromPoint(_PT(cx, cy)))
        u.SetCursorPos(cx, cy)
        time.sleep(0.08)
        _diag(f'{tag} :: pre-mousedown')
        u.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.05)
        u.mouse_event(0x0004, 0, 0, 0, 0)
        _pump(0.6)
        clicked = bool(got['ng'])
        print(f'[{tag}] WFP_class={wfp_class!r} click_registered={clicked}',
              flush=True)
        return wfp_class, clicked

    failures = []
    _, clicked0 = _click_and_check('before fisheye ever opens')
    if not clicked0:
        failures.append('ball unclickable even before any fisheye activity — '
                        'unrelated regression')

    # ── Simulate a real fisheye open: a full-screen, real-topmost Tk hit
    # layer appears, then the REAL mixin code pushes the host topmost to
    # sit above it (the exact SetWindowPos this module's
    # _raise_compositor_above_fisheye_hit_layer performs).
    from gui_modules import sao_gui_fisheye_mixin as fem

    class _Stub(fem.SAOPlayerGUIFisheyeMixin):
        _fisheye_hit_layer = None
    stub = _Stub()

    hit_layer = tk.Toplevel(root)
    hit_layer.overrideredirect(True)
    hit_layer.attributes('-topmost', True)
    hit_layer.attributes('-alpha', 0.01)
    hit_layer.geometry('1920x1080+0+0')
    hit_layer.configure(bg='black')
    stub._fisheye_hit_layer = hit_layer
    _pump(0.3)

    stub._raise_compositor_above_fisheye_hit_layer()
    _pump(0.3)
    wfp_open, clicked_open = _click_and_check('fisheye OPEN (host pushed topmost)')
    if clicked_open:
        # Not strictly a failure of the CLOSE-time leak we're hunting,
        # but worth surfacing: expected broken while the hit layer sits
        # above everything.
        print('  (note: still clickable while fisheye open — host may not '
              'have actually out-ranked the proxy yet)', flush=True)

    # ── Close the fisheye: destroy the hit layer via the REAL mixin
    # method (this is where the fix lives).
    stub._destroy_fisheye_hit_layer()
    _pump(0.5)

    if stub._fisheye_hit_layer is not None:
        failures.append('_destroy_fisheye_hit_layer did not clear the '
                        'hit-layer reference')

    wfp_after, clicked_after = _click_and_check('AFTER fisheye closed')
    if 'TkChild' not in wfp_after and 'Tk' not in wfp_after:
        failures.append(f'after close: WindowFromPoint reports {wfp_after!r} '
                        f'— the host is STILL sitting above the proxy '
                        f'(the leak this test targets)')
    if not clicked_after:
        failures.append('after close: ball click did not register — '
                        'NerveGear is STILL broken after the fisheye closed')

    # A second open/close cycle must not regress either (leak could be
    # cumulative/order-dependent).
    hit_layer2 = tk.Toplevel(root)
    hit_layer2.overrideredirect(True)
    hit_layer2.attributes('-topmost', True)
    hit_layer2.attributes('-alpha', 0.01)
    hit_layer2.geometry('1920x1080+0+0')
    hit_layer2.configure(bg='black')
    stub._fisheye_hit_layer = hit_layer2
    _pump(0.2)
    stub._raise_compositor_above_fisheye_hit_layer()
    _pump(0.2)
    stub._destroy_fisheye_hit_layer()
    _pump(0.5)
    wfp_after2, clicked_after2 = _click_and_check('AFTER second open/close cycle')
    if not clicked_after2:
        failures.append('second cycle: ball click did not register')

    if failures:
        print('\nFAIL:', flush=True)
        for f in failures:
            print('  -', f, flush=True)
        return 1
    print('\nFisheye hit-layer HWND_TOPMOST leak is fixed — NerveGear stays '
          'clickable after close.', flush=True)
    return 0


if __name__ == '__main__':
    rc = main()
    sys.stdout.flush()
    os._exit(rc)
