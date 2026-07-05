# -*- coding: utf-8 -*-
# Selftest: with NO interactive layer open (host.input_passthrough is
# True, WS_EX_TRANSPARENT set), a click over EMPTY overlay space must
# still reach a window in a TRULY SEPARATE OS PROCESS underneath it.
#
# This is the exact scenario the earlier "Stage C1" regression broke:
# whenever host.input_passthrough was True, _sync_host_rgn cleared the
# SetWindowRgn clip region to NULL (the host's shape reverts to the full,
# unclipped screen rect) and skipped the alpha scan entirely, reasoning
# that WS_EX_TRANSPARENT alone would let clicks fall through everywhere.
# That reasoning is contradicted by this module's OWN docstring:
# SetWindowRgn's exclusion — not the ex-style — is what reliably lets a
# click reach the GAME PROCESS; WS_EX_TRANSPARENT does not work cross-
# process/cross-thread by itself. A NULL region means the host's shape
# covers the entire screen with nothing excluded, so the OS still
# considers the host to be sitting over every pixel regardless of the
# ex-style — clicks meant for the game/desktop landed on the host and
# died there.
#
# Every earlier selftest in this investigation used SAME-PROCESS Tk
# windows to stand in for "the game", which cannot catch a cross-process-
# specific delivery failure. This test spawns a genuinely separate OS
# process (tools/_cross_process_target.py) as the click target.
import os
import sys
import time
import subprocess
import ctypes
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

TARGET_X, TARGET_Y, TARGET_W, TARGET_H = 700, 400, 300, 200


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

    # A click_through "pet"-like layer, visible, but NOT interactive —
    # matches the exact baseline that made host.input_passthrough True
    # in the original bug report (desktop pet running, nothing else
    # open). Positioned AWAY from the click target so it can't
    # incidentally cover it.
    PW, PH = 150, 150
    pet_buf = bytearray(PW * PH * 4)
    for yy in range(PH):
        for xx in range(PW):
            if (xx - PW // 2) ** 2 + (yy - PH // 2) ** 2 <= (PW // 3) ** 2:
                o = (yy * PW + xx) * 4
                pet_buf[o:o + 4] = bytes((180, 160, 140, 255))
    pet = uo.create_layer('pet', PW, PH, x=1300, y=850, z=200,
                          click_through=True)
    pet.upload_bgra(bytes(pet_buf), PW, PH)
    pet.show()
    _pump(0.8)

    host = uo.host
    print(f'host.input_passthrough={host.input_passthrough} '
          f'(expect True — no interactive layer is open)', flush=True)

    marker_dir = tempfile.mkdtemp(prefix='sao_xproc_')
    marker_path = os.path.join(marker_dir, 'clicked.txt')
    target_script = os.path.join(os.path.dirname(__file__), '_cross_process_target.py')
    py = sys.executable

    proc = subprocess.Popen(
        [py, target_script, str(TARGET_X), str(TARGET_Y),
         str(TARGET_W), str(TARGET_H), marker_path],
        cwd=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    try:
        time.sleep(1.2)  # let the target process create + map its window
        _pump(0.3)

        u = ctypes.windll.user32
        tx = TARGET_X + TARGET_W // 2
        ty = TARGET_Y + TARGET_H // 2

        class _PT(ctypes.Structure):
            _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]

        wfp = u.WindowFromPoint(_PT(tx, ty))
        buf = ctypes.create_unicode_buffer(48)
        u.GetClassNameW(wfp, buf, 48)
        target_pid = wt.DWORD(0)
        u.GetWindowThreadProcessId(wfp, ctypes.byref(target_pid))
        print(f'WFP at target center: class={buf.value!r} '
              f'owning_pid={target_pid.value} (target proc pid={proc.pid}, '
              f'this proc pid={os.getpid()})', flush=True)

        u.SetCursorPos(tx, ty)
        time.sleep(0.1)
        u.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.06)
        u.mouse_event(0x0004, 0, 0, 0, 0)
        _pump(1.0)

        clicked = os.path.exists(marker_path)
        print(f'cross-process click registered: {clicked}', flush=True)

        # Also confirm the pet's own opaque pixels don't falsely count
        # as "captured" by the host in some way that would show up as a
        # hang/exception, and that region is genuinely non-null now.
        region_kind = 'unknown'
        try:
            gdi = ctypes.windll.gdi32
            hrgn = gdi.CreateRectRgn(0, 0, 0, 0)
            kind = u.GetWindowRgn(uo.hwnd, hrgn)
            r = wt.RECT()
            gdi.GetRgnBox(hrgn, ctypes.byref(r))
            gdi.DeleteObject(hrgn)
            names = {0: 'NO_REGION', 1: 'NULL', 2: 'SIMPLE', 3: 'COMPLEX'}
            region_kind = names.get(kind, str(kind))
            print(f'host region kind={region_kind} box=({r.left},{r.top},'
                  f'{r.right},{r.bottom})', flush=True)
        except Exception as e:
            print(f'region probe error: {e!r}', flush=True)

        failures = []
        if not clicked:
            failures.append('click over EMPTY overlay space (no layer there) '
                            'did NOT reach the separate-process target window '
                            '— host.input_passthrough alone did not let it '
                            'through; SetWindowRgn must be excluding this area')
        if region_kind == 'NULL':
            failures.append('host region is NULL (full-screen, unclipped) — '
                            'the exact regression: this claims the whole '
                            'screen as the host\'s shape with nothing '
                            'excluded, which is what swallows cross-process '
                            'clicks regardless of WS_EX_TRANSPARENT')

        if failures:
            print('\nFAIL:', flush=True)
            for f in failures:
                print('  -', f, flush=True)
            return 1
        print('\nCross-process click passthrough works with the region fix.',
              flush=True)
        return 0
    finally:
        try:
            proc.terminate()
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


if __name__ == '__main__':
    rc = main()
    sys.stdout.flush()
    os._exit(rc)
