# -*- coding: utf-8 -*-
# Selftest: does UnifiedOverlay._enforce_z_order() actually clear a
# STUCK WS_EX_TOPMOST bit on the host when the attached "game" window is
# NOT itself topmost?
#
# This is the load-bearing assumption behind the sao_gui_fisheye_mixin.py
# / ui_gpu/popup.py fix (this session): instead of unconditionally
# forcing HWND_NOTOPMOST (which fought _enforce_z_order's own decision
# and caused a NEW regression — "compositor doesn't stay in front,
# other windows cover it" — when the game IS legitimately topmost),
# those "demote" functions now just re-invoke _enforce_z_order() and
# trust it to pick the correct z-state. If _enforce_z_order's non-
# topmost branch (SetWindowPos with an explicit game HWND as
# hWndInsertAfter) does NOT clear a pre-existing WS_EX_TOPMOST bit on
# the host, that fix does nothing and the original NerveGear-buried-
# under-host bug comes back.
#
# Also verifies the OTHER branch: when the game IS topmost,
# _enforce_z_order puts the host back into the topmost band (so it
# doesn't fall behind).
import os
import sys
import time
import ctypes
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x00000008
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP = 0x0002 | 0x0001 | 0x0010  # NOMOVE | NOSIZE | NOACTIVATE


def _is_topmost(u, hwnd) -> bool:
    ex = u.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
    return bool(ex & WS_EX_TOPMOST)


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

    # Stand-in "game" window — an ordinary, NON-topmost top-level.
    game = tk.Toplevel(root)
    game.overrideredirect(True)
    game.geometry('400x300+300+300')
    game.configure(bg='#333')
    game.deiconify()
    _pump(0.3)
    game_hwnd = u.GetAncestor(game.winfo_id(), 2)
    uo._game_hwnd = game_hwnd

    failures = []

    # ── Case A: game NOT topmost, host STUCK topmost (simulating the
    # leaked-push state) — _enforce_z_order must clear it.
    u.SetWindowPos(host_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP)
    _pump(0.1)
    stuck = _is_topmost(u, host_hwnd)
    print(f'[A] host forced topmost before test: {stuck}', flush=True)

    uo._enforce_z_order()
    _pump(0.2)
    cleared = not _is_topmost(u, host_hwnd)
    print(f'[A] after _enforce_z_order (game NOT topmost): '
          f'host_topmost={_is_topmost(u, host_hwnd)} '
          f'(expect False — must clear the stuck bit)', flush=True)
    if not cleared:
        failures.append('[A] _enforce_z_order did NOT clear a stuck '
                        'WS_EX_TOPMOST bit when the game is not topmost — '
                        'the demote-via-_enforce_z_order fix does nothing, '
                        'the NerveGear-buried-under-host bug is back')

    # ── Case B: game IS topmost — _enforce_z_order must PUT the host
    # into the topmost band (not leave/force it NOTOPMOST), or the
    # compositor falls behind the game / other topmost overlays.
    u.SetWindowPos(game_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP)
    _pump(0.1)
    game_topmost = _is_topmost(u, game_hwnd)
    print(f'[B] game forced topmost: {game_topmost}', flush=True)

    # Start from a known NON-topmost host (as if a demote had just run).
    u.SetWindowPos(host_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP)
    _pump(0.1)
    print(f'[B] host forced non-topmost before test: '
          f'{_is_topmost(u, host_hwnd)}', flush=True)

    uo._enforce_z_order()
    _pump(0.2)
    host_topmost_now = _is_topmost(u, host_hwnd)
    print(f'[B] after _enforce_z_order (game IS topmost): '
          f'host_topmost={host_topmost_now} (expect True — host must '
          f'follow the topmost game, or it falls behind it / other '
          f'topmost overlays)', flush=True)
    if not host_topmost_now:
        failures.append('[B] _enforce_z_order did NOT re-topmost the host '
                        'when the game is topmost — "compositor gets '
                        'pushed behind other windows" regression')

    if failures:
        print('\nFAIL:', flush=True)
        for f in failures:
            print('  -', f, flush=True)
        return 1
    print('\n_enforce_z_order correctly tracks the game\'s topmost state '
          'in both directions.', flush=True)
    return 0


if __name__ == '__main__':
    rc = main()
    sys.stdout.flush()
    os._exit(rc)
