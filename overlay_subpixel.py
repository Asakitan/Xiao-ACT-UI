"""overlay_subpixel.py - sub-pixel paste / bar-width helpers for ULW overlays.

PIL `Image.alpha_composite` only accepts integer offsets and `int(round(...))`
on a width snaps every animation to a 1-pixel grid. Slow tweens (HP drains,
caption drift, fisheye breathing) are then visibly stair-stepped because the
on-screen position only updates 1 px every several frames. These helpers let
those animations move smoothly between integer pixels by either:

* `subpixel_alpha_composite(dst, src, x, y)` - shifts `src` by the
  fractional remainder of (x, y) using PIL's bilinear AFFINE transform,
  then composites at the integer base. Single transform + composite, ~0.3-1 ms
  for typical 100-500 px sprites.
* `subpixel_bar_width(bar_img, frac_w)` - returns `bar_img` cropped to
  ceil(frac_w) px wide with the trailing column's alpha modulated by the
  fractional part, so a bar growing from 100.0 -> 100.99 px visibly fades the
  101st column in instead of jumping at 100.5.

Both are pure helpers - they never mutate inputs and can be safely called
from the render worker thread.
"""
from __future__ import annotations

import math
from typing import Optional

import numpy as np
from PIL import Image


_EPS = 1.0 / 512.0


def subpixel_alpha_composite(dst: Image.Image, src: Image.Image,
                             x: float, y: float) -> None:
    """Composite `src` into `dst` at fractional position (x, y).

    Falls back to a plain integer composite when the fractional part is
    negligible (< 1/512 px) so cached layouts that happen to land on an
    integer don't pay the transform cost.
    """
    ix = int(math.floor(x))
    iy = int(math.floor(y))
    fx = x - ix
    fy = y - iy
    if fx < _EPS and fy < _EPS:
        dst.alpha_composite(src, (ix, iy))
        return
    w, h = src.size
    if w <= 0 or h <= 0:
        return
    # PIL's BILINEAR transform clamps to the edge instead of blending with
    # the fillcolor, so a fractional shift on a solid sprite would leave its
    # leading edge fully opaque (no anti-aliasing). Pad the source with a
    # 1 px transparent border first; the bilinear sampler then blends the
    # outermost row/column down to (1 - frac) * alpha as we want.
    padded = Image.new('RGBA', (w + 2, h + 2), (0, 0, 0, 0))
    padded.alpha_composite(src, (1, 1))
    shifted = padded.transform(
        (w + 2, h + 2),
        Image.AFFINE,
        (1, 0, -fx, 0, 1, -fy),
        resample=Image.BILINEAR,
        fillcolor=(0, 0, 0, 0),
    )
    dst.alpha_composite(shifted, (ix - 1, iy - 1))


def subpixel_bar_width(bar_img: Image.Image,
                       frac_w: float) -> Optional[Image.Image]:
    """Trim `bar_img` to a fractional width.

    Returns ``None`` when ``frac_w <= 0``. Otherwise returns an image of
    ``ceil(frac_w)`` px width whose final column has its alpha multiplied by
    the fractional remainder. Caller is expected to paste at the regular
    integer left edge.
    """
    if frac_w <= 0.0:
        return None
    fw_int = int(math.ceil(frac_w))
    frac = frac_w - math.floor(frac_w)
    if fw_int > bar_img.width:
        fw_int = bar_img.width
        frac = 0.0
    cropped = bar_img.crop((0, 0, fw_int, bar_img.height))
    if frac < _EPS or frac > (1.0 - _EPS):
        return cropped
    arr = np.array(cropped)
    last = arr[:, fw_int - 1, 3].astype(np.float32) * frac
    arr[:, fw_int - 1, 3] = np.clip(last, 0, 255).astype(np.uint8)
    return Image.fromarray(arr, 'RGBA')
