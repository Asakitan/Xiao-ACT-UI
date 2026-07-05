# -*- coding: utf-8 -*-
# Proves the host RGN can no longer swallow thin fast-moving features
# (a heel mid-swing, a 1px hair strand) on content that changes every
# frame.
#
# The clip region (SetWindowRgn, USER32/DWM pipeline) and the presented
# pixels (DComp commit pipeline) are applied by two unsynchronized paths,
# so the region built from frame N is routinely paired on screen with
# frame N-1's or N+1's pixels for one composition. An exact-fit region
# then clips whatever moved between the frames — thick shapes lose an
# invisible 1-3px sliver, but a feature thinner than its own per-frame
# travel has ZERO overlap with the stale region and vanishes outright.
#
# The fix under test (overlay_compositor._sync_host_rgn):
# * temporal union — the region also contains the PREVIOUS frame's
# spans, covering backward pairing exactly;
# * predictive pad — sized to the measured silhouette motion between
# the two frames (x2 headroom), covering forward pairing;
# * settle — after _RGN_STATIC_SETTLE unchanged ticks the region
# returns to exact-fit spans (no permanent click-swallow halo).
#
# Drives the REAL UnifiedOverlay._sync_host_rgn with synthetic BGRA
# frames; only _build_region_from_rects and the host handle are faked
# (no Win32 window or GL needed).
#
# Run: python rgn_small_feature_coverage_test.py
from __future__ import annotations

import os
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import render.overlay_compositor as oc

W = H = 96
BODY = (30, 20, 60, 80)          # static torso: x0, y0, x1, y1
HEEL_W, HEEL_STEP = 2, 8         # 2px feature moving 8px/frame
HAIR_W, HAIR_STEP = 1, 6         # 1px feature moving 6px/frame


def make_frame(i: int) -> bytes:
    # Synthetic BGRA frame: static body + two thin features whose
    # per-frame travel exceeds their own width (the swallow scenario).
    buf = bytearray(W * H * 4)

    def fill(x0, y0, x1, y1):
        for y in range(y0, y1):
            row = y * W * 4
            for x in range(x0, x1):
                buf[row + x * 4 + 3] = 255

    fill(*BODY)
    hx = 6 + HEEL_STEP * i
    fill(hx, 70, hx + HEEL_W, 90)          # "heel"
    gx = 80 - HAIR_STEP * i
    fill(gx, 6, gx + HAIR_W, 30)           # "hair strand"
    return bytes(buf)


def opaque_pixels(frame: bytes) -> set:
    pts = set()
    for y in range(H):
        row = y * W * 4
        for x in range(W):
            if frame[row + x * 4 + 3] > 0:
                pts.add((x, y))
    return pts


def feature_pixels(frame: bytes) -> set:
    # Only the thin-feature pixels (everything outside the body box).
    return {(x, y) for (x, y) in opaque_pixels(frame)
            if not (BODY[0] <= x < BODY[2] and BODY[1] <= y < BODY[3])}


def rasterize(spans: list) -> set:
    pts = set()
    for x0, y0, x1, y1 in spans:
        for y in range(y0, y1):
            for x in range(x0, x1):
                pts.add((x, y))
    return pts


def exact_spans(frame: bytes) -> list:
    if oc._cy_alpha_spans is not None:
        return list(oc._cy_alpha_spans(frame, W, H, 0, 0, 1.0, 1.0, 0))
    spans = []
    for y in range(H):
        row = y * W * 4
        x = 0
        while x < W:
            if frame[row + x * 4 + 3] > 0:
                x0 = x
                while x < W and frame[row + x * 4 + 3] > 0:
                    x += 1
                spans.append((x0, y, x, y + 1))
            else:
                x += 1
    return spans


def main() -> int:
    ok = True

    def report(name, cond, detail=''):
        nonlocal ok
        ok = ok and bool(cond)
        print(f'  [{"PASS" if cond else "FAIL"}] {name}'
              + (f' — {detail}' if detail else ''), flush=True)

    frames = [make_frame(i) for i in range(8)]

    # ── check 1: the OLD exact-fit region really swallows the features
    # (bug reproduced analytically: frame N's exact spans vs the thin
    # features of frames N-1 / N+1 — zero overlap).
    print('[1] old behavior: exact-fit region vs adjacent-frame features',
          flush=True)
    exact_n = rasterize(exact_spans(frames[3]))
    prev_feat = feature_pixels(frames[2])
    next_feat = feature_pixels(frames[4])
    report('old: previous frame features fully clipped (bug reproduced)',
           not (prev_feat & exact_n),
           f'{len(prev_feat & exact_n)}/{len(prev_feat)} px survive')
    report('old: next frame features fully clipped (bug reproduced)',
           not (next_feat & exact_n),
           f'{len(next_feat & exact_n)}/{len(next_feat)} px survive')

    # ── check 2: drive the REAL _sync_host_rgn tick-by-tick and assert
    # the emitted region covers current, previous AND (steady-motion)
    # next frame every tick.
    print('[2] new behavior: real _sync_host_rgn coverage per tick',
          flush=True)
    captured: list = []
    orig_build = oc._build_region_from_rects
    oc._build_region_from_rects = lambda spans: (captured.append(list(spans)), 0)[1]
    try:
        overlay = oc.UnifiedOverlay(None)
        overlay._host = types.SimpleNamespace(origin_x=0, origin_y=0, hwnd=0)
        layer = overlay.create_layer('pet', W, H, click_through=True)
        layer.visible = True

        snap = {'pet': (0, 0, W, H)}
        union_ok = True
        predict_ok = True
        current_ok = True
        for i, frame in enumerate(frames):
            layer._frame_bytes = frame
            layer._frame_w = W
            layer._frame_h = H
            layer._frame_seq += 1
            overlay._sync_host_rgn(True, snap)
            if not captured:
                current_ok = False
                break
            region = rasterize(captured[-1])
            if not opaque_pixels(frame) <= region:
                current_ok = False
            if i > 0 and not opaque_pixels(frames[i - 1]) <= region:
                union_ok = False
            if 0 < i < len(frames) - 1 \
                    and not opaque_pixels(frames[i + 1]) <= region:
                predict_ok = False
        report('current frame always fully covered', current_ok)
        report('previous frame always fully covered (temporal union)',
               union_ok)
        report('next frame covered under steady motion (predictive pad)',
               predict_ok)

        # ── check 3: settle — after _RGN_STATIC_SETTLE unchanged ticks
        # the region returns to the exact-fit spans (no permanent halo).
        print('[3] settle back to exact-fit after quiescence', flush=True)
        for _ in range(oc.UnifiedOverlay._RGN_STATIC_SETTLE + 2):
            overlay._sync_host_rgn(True, snap)
        settled = rasterize(captured[-1])
        exact = rasterize(exact_spans(frames[-1]))
        report('settled region == exact alpha silhouette',
               settled == exact,
               f'settled={len(settled)}px exact={len(exact)}px')
    finally:
        oc._build_region_from_rects = orig_build

    # ── check 4: Cython vs pure-Python motion metric equivalence.
    print('[4] span_row_extent_step: cython vs python fallback', flush=True)
    pairs = []
    for a, b in ((0, 1), (2, 3), (0, 5), (6, 7)):
        pairs.append((exact_spans(frames[a]), exact_spans(frames[b])))
    pairs.append(([], exact_spans(frames[0])))
    pairs.append((exact_spans(frames[0]), exact_spans(frames[0])))
    if oc._cy_span_step is None:
        report('cython kernel importable', False, '_cy_span_step is None')
    else:
        mismatch = []
        for cur, prev in pairs:
            c = oc._cy_span_step(cur, prev)
            p = oc._span_row_extent_step_py(cur, prev)
            if c != p:
                mismatch.append((c, p))
        report('cython == python on all pairs', not mismatch,
               f'{len(pairs)} pairs' + (f' mismatches={mismatch}'
                                        if mismatch else ''))
        same = oc._cy_span_step(exact_spans(frames[0]),
                                exact_spans(frames[0]))
        step = oc._cy_span_step(exact_spans(frames[1]),
                                exact_spans(frames[0]))
        report('identical frames measure 0 motion', same == 0, f'{same}')
        report(f'moving features measure >= {HEEL_STEP}px motion',
               step >= HEEL_STEP, f'{step}')

    print(f'\n{"ALL PASS" if ok else "SOME CHECKS FAILED"}', flush=True)
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
