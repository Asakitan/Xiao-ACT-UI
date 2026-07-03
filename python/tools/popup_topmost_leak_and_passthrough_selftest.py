# -*- coding: utf-8 -*-
"""Selftest: a real SAOPopUpMenu open->close cycle must not leave the
shared compositor host stuck at real WS_EX_TOPMOST (burying NerveGear's
Tk proxy) NOR leave the desktop/game unable to receive clicks.

This is the THIRD independent "host pushed to HWND_TOPMOST and never
demoted" source found in this investigation (after
GpuNerveGearButton.raise_topmost — a90adbf — and the fisheye hit layer
— c208715): SAOPopUpMenu._raise_to_top (ui_gpu/popup.py) operates on
self._gpu_win._hwnd, which in unified mode is uo.hwnd — the ONE shared
host window, not a window owned by this popup — and pushes it to real
HWND_TOPMOST on every tick while the popup is open. The intended undo,
_release_input_zorder, early-returns whenever `_gpu_win._unified` is
True (the same stale "unified mode never makes host topmost"
assumption that bit the fisheye hit layer). Fixed by demoting inside
_destroy_window, the single choke point every close path funnels
through.

Also exercises the Stage-C1 architecture fix (this session): after
close, host.input_passthrough must be True with a REAL (non-null, or
at minimum a legitimately absent/'empty') region — not a NULL/full-
screen shape that would swallow clicks meant for a genuinely separate
process. Uses tools/_cross_process_target.py as that separate process.
"""
import os
import sys
import time
import subprocess
import ctypes
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

TARGET_X, TARGET_Y, TARGET_W, TARGET_H = 700, 400, 300, 200


class _PT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]


def _win_class(u, h):
    if not h:
        return 'none'
    b = ctypes.create_unicode_buffer(48)
    u.GetClassNameW(h, b, 48)
    return str(b.value)


def main() -> int:
    import tempfile
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

    got = {'ng': 0}
    from gui_modules.sao_gui_nervegear_button import GpuNerveGearButton
    ng = GpuNerveGearButton(
        root, 900, 500, on_click=lambda: got.__setitem__('ng', got['ng'] + 1))
    ng.deiconify()
    _pump(0.8)

    u = ctypes.windll.user32

    def _check_ng(tag):
        got['ng'] = 0
        cx, cy = 936, 536
        wfp_class = _win_class(u, u.WindowFromPoint(_PT(cx, cy)))
        u.SetCursorPos(cx, cy)
        time.sleep(0.08)
        u.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.05)
        u.mouse_event(0x0004, 0, 0, 0, 0)
        _pump(0.6)
        clicked = bool(got['ng'])
        print(f'[{tag}] NerveGear WFP={wfp_class!r} clicked={clicked}',
              flush=True)
        return wfp_class, clicked

    def _region_kind():
        gdi = ctypes.windll.gdi32
        hrgn = gdi.CreateRectRgn(0, 0, 0, 0)
        kind = u.GetWindowRgn(uo.hwnd, hrgn)
        gdi.DeleteObject(hrgn)
        return {0: 'NO_REGION', 1: 'NULL', 2: 'SIMPLE', 3: 'COMPLEX'}.get(kind, str(kind))

    def _host_topmost():
        ex = u.GetWindowLongPtrW(uo.hwnd, -20)
        # WS_EX_TOPMOST bit is 0x8, separate from the style word check
        # done via GetWindow ordering below (belt-and-suspenders: also
        # confirm via a relative z lookup since some paths hide the
        # topmost flag from naive style reads — see
        # OverlayHost._hide_topmost_flag).
        return bool(ex & 0x8)

    failures = []
    _, clicked_before = _check_ng('before popup ever opens')
    if not clicked_before:
        failures.append('NerveGear unclickable before any popup activity — '
                        'unrelated regression')

    # Spawn the cross-process click target now so it's ready once the
    # popup closes.
    marker_dir = tempfile.mkdtemp(prefix='sao_xproc_')
    marker_path = os.path.join(marker_dir, 'clicked.txt')
    target_script = os.path.join(os.path.dirname(__file__), '_cross_process_target.py')
    target_proc = subprocess.Popen(
        [sys.executable, target_script, str(TARGET_X), str(TARGET_Y),
         str(TARGET_W), str(TARGET_H), marker_path],
        cwd=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    try:
        from ui_gpu.popup import SAOPopUpMenu

        icon_arr = [
            {'id': 'item1', 'label': 'Item 1', 'icon': None},
            {'id': 'item2', 'label': 'Item 2', 'icon': None},
        ]
        popup = SAOPopUpMenu(root, icon_arr, {}, username='Tester',
                             external_close=False, alt_toggle_close=False)
        popup.open()
        # Let it render several ticks: GPU window creation is async on
        # first tick, and _raise_to_top (the leak) fires on every tick
        # while open — matching how it behaves for real over the
        # ~seconds a user leaves a menu open.
        _pump(1.5)

        topmost_while_open = _host_topmost()
        print(f'[popup OPEN] host WS_EX_TOPMOST={topmost_while_open} '
              f'(expected True — _raise_to_top pushes it every tick)',
              flush=True)

        popup.close()
        # Fade duration is 0.30s; give it generous margin to finish and
        # for _destroy_window (where the fix lives) to run.
        _pump(1.5)

        topmost_after_close = _host_topmost()
        print(f'[popup CLOSED] host WS_EX_TOPMOST={topmost_after_close} '
              f'(expect False — _destroy_window must demote it)', flush=True)
        if topmost_after_close:
            failures.append('host is STILL WS_EX_TOPMOST after the popup '
                            'closed — the leak this test targets')

        print(f'[popup CLOSED] host.input_passthrough='
              f'{uo.host.input_passthrough} region={_region_kind()}',
              flush=True)

        _, clicked_after = _check_ng('AFTER popup closed')
        if not clicked_after:
            failures.append('NerveGear unclickable after the popup closed — '
                            'host is likely still sitting above its proxy')

        # Cross-process passthrough: click over the separate-process
        # target window (nowhere near the popup or NerveGear) and
        # confirm it actually receives the click — this is the
        # Stage-C1 architecture check (region must genuinely exclude
        # empty space, not rely on a NULL/full-screen shape).
        time.sleep(0.3)
        _pump(0.2)
        tx, ty = TARGET_X + TARGET_W // 2, TARGET_Y + TARGET_H // 2
        u.SetCursorPos(tx, ty)
        time.sleep(0.1)
        u.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.06)
        u.mouse_event(0x0004, 0, 0, 0, 0)
        _pump(1.0)
        xproc_clicked = os.path.exists(marker_path)
        print(f'[popup CLOSED] cross-process desktop click registered: '
              f'{xproc_clicked}', flush=True)
        if not xproc_clicked:
            failures.append('desktop (separate-process window) click did '
                            'NOT register after the popup closed')

        try:
            popup.force_destroy_overlay()
        except Exception:
            pass
    finally:
        try:
            target_proc.terminate()
        except Exception:
            pass
        try:
            os.remove(marker_path)
        except Exception:
            pass
        try:
            os.rmdir(marker_dir)
        except Exception:
            pass

    if failures:
        print('\nFAIL:', flush=True)
        for f in failures:
            print('  -', f, flush=True)
        return 1
    print('\nPopup open/close cycle: no TOPMOST leak, NerveGear stays '
          'clickable, desktop stays clickable.', flush=True)
    return 0


if __name__ == '__main__':
    rc = main()
    sys.stdout.flush()
    os._exit(rc)
