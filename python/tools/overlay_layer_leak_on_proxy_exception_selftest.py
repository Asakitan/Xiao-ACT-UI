# -*- coding: utf-8 -*-
"""Selftest: CompositorOverlayWindow.destroy() must remove the
compositor layer even when destroy_input_proxy() raises.

Context: destroy() used to call self._layer.destroy_input_proxy()
UNGUARDED, immediately before self._compositor.destroy_layer(self._name)
on the very next line. self._destroyed was already flipped True before
either call, so if destroy_input_proxy() ever threw, destroy_layer()
never ran, AND destroy()'s own early-return guard (`if self._destroyed:
return`) meant a second .destroy() call would silently no-op instead of
retrying — the layer (and its GPU FBO/texture) would leak for the rest
of the process's life. Every overlay window that uses this adapter
(fisheye, popup menu, panels — see overlay_adapter._gen_layer_name)
allocates a fresh, never-reused layer name each time it's constructed,
so nothing else would ever clean that specific name up either.

This is a pure logic test — UnifiedOverlay's layer dict/lock plumbing
needs no GPU context, GLFW window, or Tk root; only .start() (never
called here) touches any of that.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from render.overlay_compositor import UnifiedOverlay
from render.overlay_adapter import CompositorOverlayWindow


def main() -> int:
    failures = []

    uo = UnifiedOverlay()  # never .start()ed — no GL/window needed

    win = CompositorOverlayWindow(uo, w=64, h=64, x=0, y=0, title='leaktest')
    name = win._name
    print(f'[setup] layer {name!r} present: {name in uo._layers} '
          f'(expect True)', flush=True)
    if name not in uo._layers:
        failures.append('[setup] create_layer did not register the layer')

    # Simulate destroy_input_proxy() throwing — any Tk/Win32 edge case
    # inside it must not be able to block the actual layer cleanup.
    def _boom():
        raise RuntimeError('simulated destroy_input_proxy failure')
    win._layer.destroy_input_proxy = _boom

    win.destroy()

    still_present = name in uo._layers
    print(f'[1] layer {name!r} present after destroy() despite proxy '
          f'teardown raising: {still_present} (expect False)', flush=True)
    if still_present:
        failures.append('[1] destroy_layer() did not run when '
                        'destroy_input_proxy() raised — this is the leak')

    print(f'[2] destroyed flag set: {win._destroyed} (expect True — a '
          f'second .destroy() call correctly becomes a no-op, not a '
          f'retry vector, which is exactly why the guard above matters)',
          flush=True)
    if not win._destroyed:
        failures.append('[2] _destroyed flag not set after destroy()')

    # A second, unrelated layer must be completely unaffected — this
    # fix must not have broadened its blast radius to other layers.
    win2 = CompositorOverlayWindow(uo, w=32, h=32, x=0, y=0, title='other')
    name2 = win2._name
    win2.destroy()
    print(f'[3] unrelated second layer {name2!r} cleans up normally: '
          f'{name2 not in uo._layers} (expect True)', flush=True)
    if name2 in uo._layers:
        failures.append('[3] a normal destroy() (no exception) regressed')

    if failures:
        print('\nFAIL:', flush=True)
        for f in failures:
            print('  -', f, flush=True)
        return 1
    print('\nCompositorOverlayWindow.destroy() no longer leaks a layer '
          'when destroy_input_proxy() raises.', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
