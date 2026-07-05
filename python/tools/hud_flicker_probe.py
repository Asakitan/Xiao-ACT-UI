# -*- coding: utf-8 -*-
# Burst-capture the open popup menu and verify the dynamic HUD
# elements (cyan scan line / dots, gold timestamp) are present in every
# frame — before the scratch-order fix roughly every other frame lost
# them (visible flicker).
import time

import numpy as np
from PIL import ImageGrab

N = 10
INTERVAL = 0.09

shots = []
for i in range(N):
    shots.append(ImageGrab.grab())
    time.sleep(INTERVAL)

for i, img in enumerate(shots):
    arr = np.asarray(img.convert('RGB')).astype(int)
    r, g, b = arr[..., 0], arr[..., 1], arr[..., 2]
    # HUD cyan (94,184,202): scan line + trails + left rail dot.
    cyan = ((abs(r - 94) < 50) & (abs(g - 184) < 45) & (abs(b - 202) < 45))
    # HUD gold (222,178,96)-ish: right dot + timestamp + right rail.
    gold = ((r > 170) & (abs(g - 165) < 55) & (b < 140))
    print(f'frame {i}: cyan_px={int(cyan.sum())} gold_px={int(gold.sum())}')
