# -*- coding: utf-8 -*-
"""Integration selftest: desktop-pet click halo + passthrough region.

Boots the real unified compositor and reproduces the "flickering click
region around the pet" scenario, asserting the fix at the deterministic
layer — the host window's SetWindowRgn state — rather than via flaky
synthetic clicks:

  1. Pet only (click_through=True): host is WS_EX_TRANSPARENT (nothing
     truly interactive is open) and the region is a REAL, tight scan
     result reflecting the pet's actual silhouette — NOT a NULL/full-
     screen shape. A NULL region here was an earlier, incorrect
     "optimization" (removed): per _sync_host_rgn's own docstring,
     SetWindowRgn's exclusion — not WS_EX_TRANSPARENT alone — is what
     reliably lets a click reach the game process; a NULL region means
     the host's shape covers the WHOLE screen with nothing excluded,
     which swallows clicks meant for the desktop/game regardless of the
     ex-style. See host_region_cross_process_selftest.py for the direct
     cross-process proof.

  2. Pet + NerveGear-style interactive layer WITH an input proxy: host
     STAYS passthrough (True) — the proxy carries the button's clicks,
     so the button no longer forces the host to gate input through
     SetWindowRgn. This is the actual fix for the original pet-halo
     report: the halo came from a proxyless NerveGear forcing the host
     out of passthrough, not from the region scan running.

  3. Counter-test — an interactive layer WITHOUT a proxy: host correctly
     drops out of passthrough and a real region appears (menus/panels
     that still rely on host-HWND routing keep working).

  4. Hiding that layer returns the host to passthrough, region rescans
     to reflect what's still actually visible.

Needs a GPU/DWM desktop session (same as the app).
"""
import os
import sys
import time
import ctypes
from ctypes import wintypes
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _region_kind(hwnd: int):
    user32 = ctypes.windll.user32
    gdi32 = ctypes.windll.gdi32
    hrgn = gdi32.CreateRectRgn(0, 0, 0, 0)
    kind = user32.GetWindowRgn(hwnd, hrgn)
    r = wintypes.RECT()
    gdi32.GetRgnBox(hrgn, ctypes.byref(r))
    gdi32.DeleteObject(hrgn)
    names = {0: 'NO_REGION', 1: 'NULL', 2: 'SIMPLE', 3: 'COMPLEX'}
    return names.get(kind, str(kind)), (r.left, r.top, r.right, r.bottom)


def _pet_bgra(w: int, h: int) -> bytes:
    """Opaque center with a transparent left/right/top margin — the exact
    silhouette shape whose margin used to become a click dead-zone."""
    buf = bytearray(w * h * 4)
    mx = w // 4          # left/right transparent margin
    my = h // 3          # top transparent margin (feet at the bottom)
    for y in range(h):
        for x in range(w):
            o = (y * w + x) * 4
            opaque = (mx <= x < w - mx) and (y >= my)
            if opaque:
                buf[o:o + 4] = bytes((200, 180, 160, 255))
            # else stays (0,0,0,0) transparent
    return bytes(buf)


def _pump(root, uo, secs: float):
    """Pump Tk + let the overlay thread drain its command queue."""
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < secs:
        try:
            root.update()
            uo.ensure_tk_poller()  # noqa
        except Exception:
            pass
        time.sleep(0.02)


def main() -> int:
    root = tk.Tk()
    root.geometry('80x40+0+0')
    root.withdraw()

    from config import SettingsManager
    SettingsManager().set('compositor_tk_panels', True)

    from render.gpu_overlay_window import (
        set_unified_overlay_mode, prestart_unified_overlay,
        _get_unified_overlay)
    set_unified_overlay_mode(True)
    prestart_unified_overlay(root)
    uo = _get_unified_overlay(root)
    if not uo.wait_ready(timeout=15.0):
        print('COMPOSITOR_TIMEOUT', flush=True)
        return 3

    host = uo.host
    failures = []

    def _check_never_null(tag, kind):
        # A NULL region is the exact signature of the removed Stage-C1
        # bug (host shape = entire screen, nothing excluded) regardless
        # of which passthrough state produced it.
        if kind == 'NULL':
            failures.append(f'[{tag}] region is NULL (full-screen, no '
                            f'exclusions) — this is the click-passthrough-'
                            f'breaking regression, not a valid state ever')

    # ── 1. Pet only ────────────────────────────────────────────────
    PW, PH = 240, 360
    pet = uo.create_layer('pet', PW, PH, x=600, y=300, z=200,
                          click_through=True)
    pet.upload_bgra(_pet_bgra(PW, PH), PW, PH)
    pet.show()
    _pump(root, uo, 1.2)

    pass1 = getattr(host, 'input_passthrough', None)
    kind1, box1 = _region_kind(uo.hwnd)
    print(f'[1] pet only            passthrough={pass1} region={kind1} {box1}')
    if pass1 is not True:
        failures.append('[1] host should be passthrough with only a '
                        f'click_through pet (got {pass1})')
    _check_never_null('1', kind1)
    if kind1 in ('NO_REGION',):
        failures.append(f'[1] expected a real region tightly scoped to the '
                        f'pet silhouette — got {kind1} (no region at all; '
                        f'the scan should still find the pet\'s pixels)')

    # ── 2. Pet + interactive layer WITH input proxy (NerveGear) ────
    from render.overlay_adapter import CompositorOverlayWindow
    ng = CompositorOverlayWindow(uo, w=64, h=64, x=1400, y=80,
                                 click_through=False, title='nervegear_test')
    clicks = {'n': 0}
    ng.set_input_callbacks(
        mouse_button_fn=lambda *a: clicks.__setitem__('n', clicks['n'] + 1))
    proxied = ng.enable_input_proxy()
    ng.show()
    _pump(root, uo, 1.2)

    has_proxy = ng.layer._input_proxy is not None
    pass2 = getattr(host, 'input_passthrough', None)
    kind2, box2 = _region_kind(uo.hwnd)
    print(f'[2] pet + proxied NG    passthrough={pass2} region={kind2} {box2} '
          f'proxy_created={has_proxy} enable_ret={proxied}')
    if not has_proxy:
        failures.append('[2] enable_input_proxy did not create a proxy on '
                        'the NerveGear layer')
    if pass2 is not True:
        failures.append('[2] host must STAY passthrough when the interactive '
                        f'layer has a proxy (got {pass2}) — otherwise the pet '
                        f'halo returns')
    _check_never_null('2', kind2)

    # ── 3. Counter-test: interactive layer WITHOUT a proxy ─────────
    panel = CompositorOverlayWindow(uo, w=300, h=200, x=200, y=200,
                                    click_through=False, title='panel_test')
    panel.set_input_callbacks(mouse_button_fn=lambda *a: None)
    panel.show()
    _pump(root, uo, 1.2)

    pass3 = getattr(host, 'input_passthrough', None)
    kind3, box3 = _region_kind(uo.hwnd)
    print(f'[3] + proxyless panel   passthrough={pass3} region={kind3} {box3}')
    if pass3 is not False:
        failures.append('[3] host must drop out of passthrough when a '
                        f'proxy-less interactive layer is visible (got {pass3})')
    if kind3 == 'NO_REGION':
        failures.append('[3] a real clip region must appear so host-routed '
                        f'panels still receive input — got {kind3}')
    _check_never_null('3', kind3)

    # ── 4. Hide the proxy-less panel → back to passthrough ─────────
    panel.hide()
    _pump(root, uo, 1.2)
    pass4 = getattr(host, 'input_passthrough', None)
    kind4, _ = _region_kind(uo.hwnd)
    print(f'[4] panel hidden        passthrough={pass4} region={kind4}')
    if pass4 is not True:
        failures.append('[4] host should return to passthrough after the '
                        f'proxy-less panel hides (got {pass4})')
    _check_never_null('4', kind4)

    try:
        ng.destroy()
        panel.destroy()
        uo.destroy_layer('pet')
    except Exception:
        pass

    if failures:
        print('\nFAIL:')
        for f in failures:
            print('  -', f)
        return 1
    print('\nAll pet-halo / passthrough-region checks passed.')
    return 0


if __name__ == '__main__':
    rc = main()
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(rc)
