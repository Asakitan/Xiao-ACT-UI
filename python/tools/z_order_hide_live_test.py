# -*- coding: utf-8 -*-
"""Live test: calibrate spwndPrev/spwndNext offsets + z-order unlink.

Requires rt_io driver infrastructure (R1/R3 paths).
Tests:
  1. _tw() resolves tagWND addresses
  2. _ensure_cal() finds spwnd offsets via auto-calibration
  3. hide_z_order() actually removes a window from GetWindow traversal
  4. SetWindowPos re-links the window (no permanent damage)
  5. No BSOD (the fact that this script finishes = success)

Run elevated:  python tools/z_order_hide_live_test.py
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

import ctypes
import ctypes.wintypes as wt
import struct
import time

u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32

u32.GetWindow.argtypes = [wt.HWND, wt.UINT]
u32.GetWindow.restype = wt.HWND
u32.IsWindow.argtypes = [wt.HWND]
u32.IsWindow.restype = wt.BOOL
u32.CreateWindowExW.argtypes = [
    wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    wt.HWND, wt.HMENU, wt.HINSTANCE, ctypes.c_void_p,
]
u32.CreateWindowExW.restype = wt.HWND
u32.DestroyWindow.argtypes = [wt.HWND]
u32.SetWindowPos.argtypes = [
    wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
    ctypes.c_int, ctypes.c_int, wt.UINT,
]
u32.SetWindowPos.restype = wt.BOOL
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetWindowRect.restype = wt.BOOL

GW_HWNDPREV = 3
GW_HWNDNEXT = 2
SWP = 0x0002 | 0x0001 | 0x0010


def find_above(target, start, max_steps=300):
    cur = start
    for _ in range(max_steps):
        cur = u32.GetWindow(cur, GW_HWNDPREV)
        if not cur:
            return False
        if cur == target:
            return True
    return False


def find_below(target, start, max_steps=300):
    cur = start
    for _ in range(max_steps):
        cur = u32.GetWindow(cur, GW_HWNDNEXT)
        if not cur:
            return False
        if cur == target:
            return True
    return False


print('=' * 60)
print('Z-ORDER HIDE LIVE TEST')
print('=' * 60)

# ── Step 0: check rt_io availability ──
print('\n[Step 0] Checking rt_io...')
try:
    from mem_probe import rt_io
    loaded = rt_io.ensure_loaded()
    tier = rt_io.memory_tier()
    print(f'  rt_io loaded: {loaded}, tier: {tier}')
    if not loaded:
        print('  ✗ Driver not loaded. Run elevated or check driver.')
        sys.exit(1)
except Exception as e:
    print(f'  ✗ rt_io import failed: {e}')
    sys.exit(1)

# ── Step 1: test _tw ──
print('\n[Step 1] Testing _tw (tagWND resolution)...')
hinst = k32.GetModuleHandleW(None)
test_hwnd = u32.CreateWindowExW(
    0x80, 'STATIC', 'tw_test',
    0x80000000 | 0x10000000,
    -500, -500, 1, 1, 0, 0, hinst, None)
assert test_hwnd, 'CreateWindow failed'

try:
    tw = rt_io._tw(test_hwnd)
    print(f'  HWND={test_hwnd:#x} → tagWND={tw:#x}' if tw else '  ✗ _tw returned 0')
    if not tw:
        print('  HMValidateHandle may be patched. Cannot proceed.')
        u32.DestroyWindow(test_hwnd)
        sys.exit(1)
    # sanity: read ExStyle from tagWND
    data = (ctypes.c_ubyte * 8)()
    ctypes.memmove(data, tw, 8)
    print(f'  tagWND first 8 bytes: {bytes(data).hex()}')
    print('  ✓ _tw works')
finally:
    u32.DestroyWindow(test_hwnd)

# ── Step 2: full calibration ──
print('\n[Step 2] Running full calibration (_ensure_cal)...')
from mem_probe._dc import _ensure_cal, _spwnd_prev_off, _spwnd_next_off
from mem_probe import _dc

_ensure_cal()

sp = _dc._spwnd_prev_off
sn = _dc._spwnd_next_off
exs = _dc._exs_off
rect = _dc._rect_off

print(f'  ExStyle   offset: {exs:#x}' if exs >= 0 else '  ExStyle:   FAILED')
print(f'  rcWindow  offset: {rect:#x}' if rect >= 0 else '  rcWindow:  FAILED')
print(f'  spwndPrev offset: {sp:#x}' if sp >= 0 else '  spwndPrev: FAILED')
print(f'  spwndNext offset: {sn:#x}' if sn >= 0 else '  spwndNext: FAILED')

if sp >= 0 and sn >= 0:
    print(f'  spwndNext = spwndPrev - 8: {sn == sp - 8}')
    print('  ✓ Calibration successful')
else:
    print('  ✗ spwnd calibration failed. Cannot test z-order hide.')
    sys.exit(1)

# ── Step 3: test hide_z_order ──
print('\n[Step 3] Testing hide_z_order...')

wC = u32.CreateWindowExW(0x80, 'STATIC', 'C', 0x80000000 | 0x10000000,
                          -500, -500, 1, 1, 0, 0, hinst, None)
wB = u32.CreateWindowExW(0x80, 'STATIC', 'B', 0x80000000 | 0x10000000,
                          -500, -500, 1, 1, 0, 0, hinst, None)
wA = u32.CreateWindowExW(0x80, 'STATIC', 'A', 0x80000000 | 0x10000000,
                          -500, -500, 1, 1, 0, 0, hinst, None)
assert wA and wB and wC, 'CreateWindow failed'

# Stack: A above B above C
# SetWindowPos(X, Y) puts X BELOW Y. So to get A>B>C:
u32.SetWindowPos(wC, wB, 0, 0, 0, 0, SWP)  # C below B
u32.SetWindowPos(wB, wA, 0, 0, 0, 0, SWP)  # B below A
# Now A is highest, C is lowest

b_below_a = find_below(wB, wA)
c_below_b = find_below(wC, wB)
print(f'  Before: A→down finds B: {b_below_a} (expect True)')
print(f'  Before: B→down finds C: {c_below_b} (expect True)')
assert b_below_a and c_below_b, 'Z-order setup failed'

# Unlink B
from mem_probe._dc import hide_z_order
ok = hide_z_order(wB)
print(f'  hide_z_order(B): {"OK" if ok else "FAILED"}')

if ok:
    b_vis_down = find_below(wB, wA)
    c_vis_down = find_below(wC, wA)
    b_vis_up = find_above(wB, wC)
    print(f'  After unlink: A→down finds B: {b_vis_down} (expect False)')
    print(f'  After unlink: A→down finds C: {c_vis_down} (expect True)')
    print(f'  After unlink: C→up finds B: {b_vis_up} (expect False)')

    if not b_vis_down and c_vis_down and not b_vis_up:
        print('  ✓ Z-ORDER HIDE WORKS!')
    else:
        print('  ✗ Z-order hide incomplete')

    # Step 4: verify SetWindowPos re-links
    print('\n[Step 4] Re-linking via SetWindowPos...')
    u32.SetWindowPos(wB, wA, 0, 0, 0, 0, SWP)  # B below A again
    b_back = find_below(wB, wA)
    print(f'  After re-SetWindowPos: A→down finds B: {b_back} (expect True)')
    if b_back:
        print('  ✓ Re-link works — SetWindowPos restores the chain')
    else:
        print('  ✗ Re-link failed')

    # Step 5: test hide_window_rect
    print('\n[Step 5] Testing hide_window_rect...')
    from mem_probe._dc import hide_window_rect
    u32.SetWindowPos(wB, 0, 200, 200, 300, 300, 0x0010)
    rc = wt.RECT()
    u32.GetWindowRect(wB, ctypes.byref(rc))
    print(f'  Before scrub: GetWindowRect = ({rc.left},{rc.top},{rc.right},{rc.bottom})')

    rok = hide_window_rect(wB)
    print(f'  hide_window_rect: {"OK" if rok else "FAILED"}')
    if rok:
        u32.GetWindowRect(wB, ctypes.byref(rc))
        print(f'  After scrub:  GetWindowRect = ({rc.left},{rc.top},{rc.right},{rc.bottom})')
        if rc.right - rc.left == 1 and rc.bottom - rc.top == 1:
            print('  ✓ RECT scrub works — window appears as 1×1')
        else:
            print('  ✗ RECT scrub did not take effect')

# Cleanup
u32.DestroyWindow(wA)
u32.DestroyWindow(wB)
u32.DestroyWindow(wC)

print('\n' + '=' * 60)
print('TEST COMPLETE — no BSOD')
print('=' * 60)
