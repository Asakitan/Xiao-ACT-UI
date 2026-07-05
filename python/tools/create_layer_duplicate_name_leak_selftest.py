# -*- coding: utf-8 -*-
# Selftest: UnifiedOverlay.create_layer() called twice under the SAME
# name must release the old layer's resources, not silently overwrite it.
#
# Context: create_layer() used to do `self._layers[name] = layer`
# unconditionally, with no check for a pre-existing entry. Any caller that
# creates a layer under a fixed, reused name — e.g. SAOPlayerGUIFloatChrome
# Mixin._play_motion_blur's hardcoded '_motion_blur' layer, re-triggered by
# an ordinary fast menu-open-then-close click sequence well within its own
# ~0.4s animation lifetime — silently dropped the OLD CompositorLayer object
# out of both _layers and _z_sorted. The only place that releases a layer's
# GL FBO/texture (_release_gl, queued via destroy_layer's _cmd_q closure) was
# never reached for that orphan, leaking one screen-sized GPU texture+
# framebuffer per overlapping re-trigger, accumulating over a session. Any
# other caller re-using a fixed layer name (e.g. a plugin's
# create_compositor_layer bridge) hits the exact same gap.
#
# Pure logic test — UnifiedOverlay's layer dict/lock plumbing needs no GPU
# context, GLFW window, or Tk root; only .start() (never called here)
# touches any of that.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from render.overlay_compositor import UnifiedOverlay


def main() -> int:
    failures = []
    uo = UnifiedOverlay()  # never .start()ed — no GL/window needed

    released = {'count': 0, 'layers': []}

    def _make_layer(name, w=64, h=64):
        layer = uo.create_layer(name, width=w, height=h)
        orig_release = layer._release_gl

        def _tracked_release(dcomp):
            released['count'] += 1
            released['layers'].append(layer)
            return orig_release(dcomp)
        layer._release_gl = _tracked_release
        return layer

    first = _make_layer('_motion_blur')
    print(f"[setup] first layer registered: "
          f"{uo._layers.get('_motion_blur') is first} (expect True)",
          flush=True)

    # Simulate a fast re-trigger under the SAME name before the first
    # instance's own timer got a chance to call destroy_layer itself —
    # exactly what an ordinary fast menu open-then-close does to
    # '_motion_blur'.
    second = _make_layer('_motion_blur')
    print(f"[1] second create_layer call registers the NEW object: "
          f"{uo._layers.get('_motion_blur') is second} (expect True)",
          flush=True)
    if uo._layers.get('_motion_blur') is not second:
        failures.append('[1] the second create_layer call did not take '
                        'over the name slot')

    # Drain the cmd queue the way the render thread would, to actually
    # observe _release_gl firing.
    drained = 0
    import queue as _queue
    while True:
        try:
            cmd = uo._cmd_q.get_nowait()
        except _queue.Empty:
            break
        cmd()
        drained += 1

    print(f"[2] _release_gl fired for the orphaned FIRST layer: "
          f"{first in released['layers']} (expect True — this is the leak "
          f"fix: the old layer's GL FBO/texture must actually be queued "
          f"for release, not silently dropped)", flush=True)
    if first not in released['layers']:
        failures.append('[2] the orphaned first layer never had its GL '
                        'resources released — still leaking')

    print(f"[3] the SECOND (still-current) layer was NOT also released: "
          f"{second not in released['layers']} (expect True — only the "
          f"replaced instance should be torn down)", flush=True)
    if second in released['layers']:
        failures.append('[3] the second (still-live) layer was incorrectly '
                        'released too')

    print(f"[4] '_motion_blur' still resolves to the second layer after "
          f"drain: {uo._layers.get('_motion_blur') is second} (expect True)",
          flush=True)
    if uo._layers.get('_motion_blur') is not second:
        failures.append('[4] the live layer was removed from _layers by '
                        'the orphan cleanup — blast radius too wide')

    # A layer created under a name for the FIRST time must be completely
    # unaffected (no spurious destroy_layer side effects).
    fresh = _make_layer('unrelated')
    print(f"[5] a brand-new name registers cleanly: "
          f"{uo._layers.get('unrelated') is fresh} (expect True), "
          f"no release fired for it: {fresh not in released['layers']} "
          f"(expect True)", flush=True)
    if uo._layers.get('unrelated') is not fresh or fresh in released['layers']:
        failures.append('[5] a fresh, never-before-used name regressed')

    if failures:
        print('\nFAIL:', flush=True)
        for f in failures:
            print('  -', f, flush=True)
        return 1
    print('\ncreate_layer() correctly releases a prior layer registered '
          'under the same name instead of silently orphaning it.',
          flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
