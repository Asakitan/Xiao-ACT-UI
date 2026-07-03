# -*- coding: utf-8 -*-
"""Measure the actual pixel bounds of the menu HUD glass shell.

Renders MenuHudSpriteRenderer's static GPU layer offline and reports,
for each edge, where opaque plate pixels start — expected symmetric at
_shell_body_pad from every canvas edge.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np

from gui_modules.sao_menu_hud import MenuHudSpriteRenderer


def bounds(arr: np.ndarray, thresh: int = 60):
    a = arr[..., 3]
    ys, xs = np.where(a >= thresh)
    if len(xs) == 0:
        return None
    return int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())


def main() -> None:
    r = MenuHudSpriteRenderer()
    cw, ch = 400, 600
    img = r._get_static_layer_gpu(cw, ch, 1920, 1080)
    w, h = img.size
    pad = r.gpu_pad
    body = r._shell_body_pad
    print(f'canvas={w}x{h} gpu_pad={pad} shell_body_pad={body}')
    arr = np.asarray(img)

    # Plate fill alpha is ~160-200; brackets/rails are thin 1px lines.
    # Use a mid threshold to catch the plate body.
    b = bounds(arr, 60)
    print(f'alpha>=60 bounds: left={b[0]} top={b[1]} right={w-1-b[2]} bottom={h-1-b[3]}'
          f'  (expected ~{body} on every edge)')

    # Also scan the horizontal center row and vertical center column for
    # where the plate's fill starts/ends, ignoring bracket lines.
    row = arr[h // 2, :, 3]
    col = arr[:, w // 2, 3]
    row_on = np.where(row >= 60)[0]
    col_on = np.where(col >= 60)[0]
    print(f'center row plate: x {row_on.min()}..{row_on.max()} '
          f'(left gap {row_on.min()}, right gap {w-1-row_on.max()})')
    print(f'center col plate: y {col_on.min()}..{col_on.max()} '
          f'(top gap {col_on.min()}, bottom gap {h-1-col_on.max()})')

    img.save(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          'shell_probe.png'))
    print('saved shell_probe.png')

    # Compare with the CPU fallback (PIL) shell for reference.
    import gui_modules.sao_menu_hud as m
    saved = m._gpu_shell
    m._gpu_shell = None
    try:
        r2 = MenuHudSpriteRenderer()
        img2 = r2._get_static_layer_gpu(cw, ch, 1920, 1080)
        arr2 = np.asarray(img2)
        row2 = arr2[h // 2, :, 3]
        col2 = arr2[:, w // 2, 3]
        row_on2 = np.where(row2 >= 60)[0]
        col_on2 = np.where(col2 >= 60)[0]
        print(f'CPU fallback row: left gap {row_on2.min()}, right gap {w-1-row_on2.max()}')
        print(f'CPU fallback col: top gap {col_on2.min()}, bottom gap {h-1-col_on2.max()}')
    finally:
        m._gpu_shell = saved


if __name__ == '__main__':
    main()
