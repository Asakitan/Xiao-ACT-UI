# -*- coding: utf-8 -*-
# Read the compositor host window's actual SetWindowRgn box from
# outside the process. Usage: host_rgn_probe.py <hwnd-hex>
import sys
import ctypes
from ctypes import wintypes

hwnd = int(sys.argv[1], 16)
user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

hrgn = gdi32.CreateRectRgn(0, 0, 0, 0)
# GetWindowRgn: 1=NULLREGION 2=SIMPLEREGION 3=COMPLEXREGION 0=ERROR(no rgn)
kind = user32.GetWindowRgn(hwnd, hrgn)
r = wintypes.RECT()
gdi32.GetRgnBox(hrgn, ctypes.byref(r))
gdi32.DeleteObject(hrgn)
names = {0: 'NO_REGION(full window)', 1: 'NULL', 2: 'SIMPLE', 3: 'COMPLEX'}
print(f'region kind={names.get(kind, kind)} box=({r.left},{r.top})-({r.right},{r.bottom})')

# What ex-styles does the host carry right now?
GWL_EXSTYLE = -20
ex = user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
print(f'exstyle=0x{ex:08X} WS_EX_TRANSPARENT={"ON" if ex & 0x20 else "OFF"}')
