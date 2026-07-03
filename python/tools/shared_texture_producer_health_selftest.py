# -*- coding: utf-8 -*-
"""Selftest: CompositorLayer._shared_producer_healthy() and its
heartbeat stamp in _poll_mmf().

Context: DCompBridge.lock_external_texture() (wglDXLockObjectsNV) has
no timeout parameter, unlike the keyed-mutex acquire beside it in the
same _draw_layers draw branch (which uses an explicit 8ms timeout
specifically because a stuck producer can hold the resource forever).
If the shared-texture producer process (the desktop pet) gets stuck
holding the D3D11 resource, that lock call can block the render thread
indefinitely — reproduced live as "开着桌宠跑一会就卡死, 鱼眼菜单打不开"
(running with the desktop pet, it freezes after a while, the fisheye
menu won't open), requiring Task Manager to end the whole process.

_shared_producer_healthy() uses the MMF side-channel (attached
alongside the shared texture for its alpha byte, polled once per tick
in _run()'s loop before _draw_layers runs) as an independent liveness
signal from the SAME producer process, so _draw_layers can skip the
un-timeout'd lock when the producer hasn't delivered a frame recently.

This is a pure logic test — CompositorLayer's constructor and these
two methods touch no GL/Tk/window state, so no compositor, GPU context,
or display is needed.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from render.overlay_compositor import CompositorLayer


class _FakeMMF:
    def __init__(self, fw=10, fh=10):
        self.fw = fw
        self.fh = fh
        self.next_frame = None

    def poll(self):
        return self.next_frame


def main() -> int:
    failures = []

    # ── 1. No MMF attached at all — can't health-check, must never
    # block drawing a layer this mechanism doesn't apply to.
    layer = CompositorLayer('no_mmf', 10, 10)
    ok = layer._shared_producer_healthy()
    print(f'[1] no _mmf_name attached: healthy={ok} (expect True)',
          flush=True)
    if not ok:
        failures.append('[1] a layer with no MMF side-channel was treated '
                        'as unhealthy — would refuse to ever draw it')

    # ── 2. MMF attached, but no heartbeat observed yet (fresh layer,
    # producer hasn't delivered its first frame). 0.0 must NOT be
    # treated as "infinitely stale" (time.perf_counter() is not
    # zero-based) — must default to healthy until proven otherwise.
    layer2 = CompositorLayer('never_polled', 10, 10)
    layer2._mmf_name = 'fake'
    ok2 = layer2._shared_producer_healthy()
    print(f'[2] MMF attached, never polled yet: healthy={ok2} '
          f'(expect True)', flush=True)
    if not ok2:
        failures.append('[2] a layer with no heartbeat yet was treated as '
                        'stale — likely the time.perf_counter()-is-not-'
                        'zero-based trap')

    # ── 3. _poll_mmf must stamp the heartbeat exactly when a genuinely
    # new frame arrives, not on every call.
    layer3 = CompositorLayer('pet', 10, 10)
    layer3._mmf_name = 'fake'
    layer3._shared_tex_handle = 999  # pretend a shared texture is active
    fake = _FakeMMF()
    layer3._mmf = fake

    fake.next_frame = None
    got_frame = layer3._poll_mmf(None)
    print(f'[3a] _poll_mmf with no frame available: returned={got_frame} '
          f'last_seen={layer3._shared_producer_last_seen} (expect False, '
          f'0.0)', flush=True)
    if got_frame or layer3._shared_producer_last_seen != 0.0:
        failures.append('[3a] _poll_mmf stamped a heartbeat despite no '
                        'frame being available')

    fake.next_frame = b'\x00' * (10 * 10 * 4)
    got_frame2 = layer3._poll_mmf(None)
    stamped = layer3._shared_producer_last_seen
    print(f'[3b] _poll_mmf with a real frame: returned={got_frame2} '
          f'last_seen={stamped} (expect True, > 0.0)', flush=True)
    if not got_frame2 or stamped == 0.0:
        failures.append('[3b] _poll_mmf did not stamp a heartbeat despite '
                        'a genuinely new frame arriving')

    ok3 = layer3._shared_producer_healthy()
    print(f'[3c] right after a fresh heartbeat: healthy={ok3} '
          f'(expect True)', flush=True)
    if not ok3:
        failures.append('[3c] layer treated as unhealthy immediately after '
                        'a fresh heartbeat')

    # ── 4. A heartbeat older than _SHARED_PRODUCER_STALE_S must flip
    # to unhealthy — this is the actual guard that skips the
    # un-timeout'd interop lock in _draw_layers.
    layer3._shared_producer_last_seen = (
        time.perf_counter() - CompositorLayer._SHARED_PRODUCER_STALE_S - 0.5)
    ok4 = layer3._shared_producer_healthy()
    print(f'[4] heartbeat older than _SHARED_PRODUCER_STALE_S '
          f'({CompositorLayer._SHARED_PRODUCER_STALE_S}s): healthy={ok4} '
          f'(expect False)', flush=True)
    if ok4:
        failures.append('[4] a stale producer was still treated as healthy '
                        '— the interop-lock guard would never engage')

    # ── 5. Recovery: a fresh heartbeat after a stale period must flip
    # back to healthy (this is not a one-way latch).
    layer3._shared_producer_last_seen = time.perf_counter()
    ok5 = layer3._shared_producer_healthy()
    print(f'[5] fresh heartbeat after being stale: healthy={ok5} '
          f'(expect True — must recover, not latch permanently unhealthy)',
          flush=True)
    if not ok5:
        failures.append('[5] producer health did not recover after a '
                        'fresh heartbeat — a real pet stall/recovery would '
                        'permanently stop rendering the layer')

    if failures:
        print('\nFAIL:', flush=True)
        for f in failures:
            print('  -', f, flush=True)
        return 1
    print('\nShared-texture producer health guard behaves correctly.',
          flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
