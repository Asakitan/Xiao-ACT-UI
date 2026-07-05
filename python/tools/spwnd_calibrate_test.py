# -*- coding: utf-8 -*-
"""Test spwndPrev/spwndNext offset calibration and z-order unlink.

Verifies:
1. Calibration finds valid offsets
2. hide_z_order actually removes a window from GetWindow traversal
3. DWM still renders the window (visual check - manual)
4. SetWindowPos re-links the window (GetWindow finds it again)
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

import ctypes
import ctypes.wintypes as wt
import time

u32 = ctypes.windll.user32
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

GW_HWNDPREV = 3
GW_HWNDNEXT = 2
SWP = 0x0002 | 0x0001 | 0x0010  # NOMOVE|NOSIZE|NOACTIVATE
HWND_TOPMOST = wt.HWND(-1)

def find_in_zorder(target_hwnd, start_hwnd, direction=GW_HWNDPREV, max_steps=200):
    """Walk z-order from start_hwnd. Return True if target_hwnd is found."""
    cur = start_hwnd
    for _ in range(max_steps):
        cur = u32.GetWindow(cur, direction)
        if not cur:
            break
        if cur == target_hwnd:
            return True
    return False

print('=== spwndPrev/spwndNext calibration test ===\n')

# Step 1: trigger calibration
from mem_probe._dc import _ensure_cal, _spwnd_prev_off, _spwnd_next_off
_ensure_cal()

# Re-import after calibration (module globals updated)
from mem_probe import _dc
sp = _dc._spwnd_prev_off
sn = _dc._spwnd_next_off

print(f'spwndPrev offset: {sp:#x}' if sp >= 0 else 'spwndPrev: FAILED')
print(f'spwndNext offset: {sn:#x}' if sn >= 0 else 'spwndNext: FAILED')
print(f'ExStyle   offset: {_dc._exs_off:#x}' if _dc._exs_off >= 0 else 'ExStyle: FAILED')
print(f'rcWindow  offset: {_dc._rect_off:#x}' if _dc._rect_off >= 0 else 'rcWindow: FAILED')

if sp < 0 or sn < 0:
    print('\nCALIBRATION FAILED — cannot test hide_z_order')
    sys.exit(1)

print(f'\nspwndNext is at spwndPrev - 8: {sn == sp - 8}')
print()

# Step 2: create 3 test windows — A above B above C
hinst = ctypes.windll.kernel32.GetModuleHandleW(None)
def mkwin(label):
    h = u32.CreateWindowExW(
        0x80, 'STATIC', label,
        0x80000000 | 0x10000000,
        -500, -500, 1, 1, 0, 0, hinst, None)
    assert h, f'CreateWindow {label} failed'
    return h

wC = mkwin('C')
wB = mkwin('B')
wA = mkwin('A')

# Stack: A above B above C
u32.SetWindowPos(wC, 0, 0, 0, 0, 0, SWP)
u32.SetWindowPos(wB, wC, 0, 0, 0, 0, SWP)
u32.SetWindowPos(wA, wB, 0, 0, 0, 0, SWP)

# Verify: from C, walking up should find B then A
assert find_in_zorder(wB, wC, GW_HWNDPREV), 'B not found above C'
assert find_in_zorder(wA, wC, GW_HWNDPREV), 'A not found above C'
print('Before unlink: C→up finds B ✓  C→up finds A ✓')

# Step 3: hide B from z-order
from mem_probe._dc import hide_z_order
ok = hide_z_order(wB)
print(f'hide_z_order(B): {"OK" if ok else "FAILED"}')

if ok:
    # From C walking up: should find A but NOT B
    b_visible = find_in_zorder(wB, wC, GW_HWNDPREV)
    a_visible = find_in_zorder(wA, wC, GW_HWNDPREV)
    print(f'After unlink:  C→up finds B: {b_visible} (should be False)')
    print(f'After unlink:  C→up finds A: {a_visible} (should be True)')

    if not b_visible and a_visible:
        print('\n✅ Z-ORDER UNLINK WORKS — B is invisible to GetWindow!')
    else:
        print('\n❌ Z-ORDER UNLINK FAILED')

    # Step 4: re-link by calling SetWindowPos again
    u32.SetWindowPos(wB, wC, 0, 0, 0, 0, SWP)
    b_back = find_in_zorder(wB, wC, GW_HWNDPREV)
    print(f'\nAfter re-SetWindowPos: C→up finds B: {b_back} (should be True)')
    if b_back:
        print('✅ SetWindowPos correctly re-links the window')
    else:
        print('❌ Re-link failed')

# Cleanup
u32.DestroyWindow(wA)
u32.DestroyWindow(wB)
u32.DestroyWindow(wC)
print('\nDone.')
