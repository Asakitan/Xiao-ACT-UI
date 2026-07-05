# -*- coding: utf-8 -*-
# Selftest: NerveGear opened WHILE the fisheye menu is up must not crash.
#
# Reproduces the user's crash scenario ("open the NerveGear button and the
# compositor freezes then crash-exits, no error"):
# - a proxyless interactive layer (stand-in for the fisheye menu) forces
# the host OUT of passthrough, so the per-pixel region scan is live;
# - then a real GpuNerveGearButton opens on top, arming its input proxy
# + WndProc activation shield while its glow animation drives frame
# changes every tick;
# - the 200ms shield/shape tick runs for several seconds.
#
# The freeze+crash was the shield re-subclassing the proxy WndProc every
# tick (a truncated 64-bit HWND broke the "already armed?" guard), each
# re-arm chaining CallWindowProc through the previous proc until the
# per-message call stack overflowed. Assert the shield arms exactly ONCE
# (WndProc ref count grows by 1, not per-tick) and the run stays alive.
import os
import sys
import time
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main() -> int:
    root = tk.Tk()
    root.geometry('80x40+0+0')
    root.withdraw()

    from render.gpu_overlay_window import (
        set_unified_overlay_mode, prestart_unified_overlay,
        _get_unified_overlay)
    from render.overlay_adapter import CompositorOverlayWindow
    from render import overlay_compositor as oc

    set_unified_overlay_mode(True)
    prestart_unified_overlay(root)
    uo = _get_unified_overlay(root)
    if not uo.wait_ready(timeout=15.0):
        print('COMPOSITOR_TIMEOUT', flush=True)
        return 3

    def _pump(secs):
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < secs:
            try:
                root.update()
                uo.ensure_tk_poller()
            except Exception:
                pass
            time.sleep(0.02)

    # Stand-in for the fisheye menu: proxyless interactive layer →
    # host leaves passthrough, region scan goes live.
    fisheye = CompositorOverlayWindow(uo, w=400, h=400, x=200, y=200,
                                      click_through=False, title='fisheye_test')
    fisheye.set_input_callbacks(mouse_button_fn=lambda *a: None)
    fisheye.show()
    _pump(0.8)
    print(f'fisheye up: host_passthrough={uo.host.input_passthrough} '
          f'(expect False)', flush=True)

    refs_before = len(oc._PROXY_WNDPROC_REFS)

    # Open the real NerveGear button on top and animate its glow so its
    # frame seq changes every tick (drives _apply_proxy_shape too).
    from gui_modules.sao_gui_nervegear_button import GpuNerveGearButton
    clicks = {'n': 0}
    btn = GpuNerveGearButton(root, 900, 500,
                             on_click=lambda: clicks.__setitem__('n', clicks['n'] + 1))
    btn.deiconify()

    layer = btn._win._delegate.layer if getattr(btn._win, '_delegate', None) else None

    phase = 0.0
    t0 = time.perf_counter()
    ref_samples = []
    while time.perf_counter() - t0 < 4.0:
        phase += 0.3
        try:
            btn.render(phase)          # animate glow → frame churn
        except Exception as e:
            print(f'render raised: {e!r}', flush=True)
            return 1
        root.update()
        uo.ensure_tk_poller()
        ref_samples.append(len(oc._PROXY_WNDPROC_REFS))
        time.sleep(0.05)

    refs_after = len(oc._PROXY_WNDPROC_REFS)
    added = refs_after - refs_before
    shielded = getattr(layer._input_proxy, '_sao_shielded', False) \
        if (layer and layer._input_proxy) else None
    proxy_alive = layer is not None and layer._input_proxy is not None

    print(f'proxy_created={proxy_alive} shielded={shielded}', flush=True)
    print(f'wndproc_refs added over 4s run = {added} '
          f'(max sample {max(ref_samples) - refs_before})', flush=True)
    print(f'still alive: compositor running={uo._running}', flush=True)

    failures = []
    if not proxy_alive:
        failures.append('NerveGear input proxy was not created')
    if added > 1:
        failures.append(f'WndProc subclassed {added} times — the chain-of-'
                        f'death that freezes+crashes is back (must be 1)')
    if not uo._running:
        failures.append('compositor thread died during the run')

    if failures:
        print('\nFAIL:', flush=True)
        for f in failures:
            print('  -', f, flush=True)
        return 1
    print('\nNerveGear + fisheye coexistence stayed alive, shield armed once.',
          flush=True)
    return 0


if __name__ == '__main__':
    rc = main()
    sys.stdout.flush()
    os._exit(rc)
