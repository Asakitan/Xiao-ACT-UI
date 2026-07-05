# -*- coding: utf-8 -*-
"""Verify whether DComp visual content is clipped to the host window rect.

Creates a SMALL (50x50) WS_EX_NOREDIRECTIONBITMAP window, then attaches a
LARGE (400x400) DComp swap chain filled with solid red.  If the red area
extends beyond the 50x50 window boundary → DComp does NOT clip to rcWindow
→ route A (1x1 window + full-screen DComp) is viable.

Run:  python tools/dcomp_clip_test.py
Visual result: look at the screen near top-left (100,100).
  - 50x50 red square  → DComp IS clipped → route A fails
  - 400x400 red square → DComp NOT clipped → route A works
Press Enter to exit.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time
from ctypes import (POINTER, WINFUNCTYPE, Structure, byref, c_float, c_int,
                    c_uint, c_void_p, sizeof)

if sys.platform != 'win32':
    raise SystemExit('Windows only')

HRESULT = ctypes.HRESULT
u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32
dwm = ctypes.windll.dwmapi
d3d11 = ctypes.WinDLL('d3d11')
dcomp = ctypes.WinDLL('dcomp')

# ── constants ──
WS_POPUP = 0x80000000
WS_VISIBLE = 0x10000000
WS_EX_TOOLWINDOW = 0x80
WS_EX_NOREDIRECTIONBITMAP = 0x00200000
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TOPMOST = 0x8
CS_OWNDC = 0x20

# ── structs ──
class WNDCLASSEXW(Structure):
    _fields_ = [
        ('cbSize', wt.UINT), ('style', wt.UINT),
        ('lpfnWndProc', c_void_p),
        ('cbClsExtra', c_int), ('cbWndExtra', c_int),
        ('hInstance', wt.HINSTANCE), ('hIcon', wt.HICON),
        ('hCursor', wt.HANDLE), ('hbrBackground', wt.HBRUSH),
        ('lpszMenuName', wt.LPCWSTR), ('lpszClassName', wt.LPCWSTR),
        ('hIconSm', wt.HICON),
    ]

class MARGINS(Structure):
    _fields_ = [('l', c_int), ('r', c_int), ('t', c_int), ('b', c_int)]

class GUID(Structure):
    _fields_ = [('d1', c_uint), ('d2', ctypes.c_ushort),
                ('d3', ctypes.c_ushort), ('d4', ctypes.c_ubyte * 8)]

def guid(s):
    p = s.replace('-', '')
    return GUID(int(p[:8],16), int(p[8:12],16), int(p[12:16],16),
                (ctypes.c_ubyte*8)(*[int(p[i:i+2],16) for i in range(16,32,2)]))

class DXGI_SAMPLE_DESC(Structure):
    _fields_ = [('Count', c_uint), ('Quality', c_uint)]

class DXGI_SWAP_CHAIN_DESC1(Structure):
    _fields_ = [
        ('Width', c_uint), ('Height', c_uint),
        ('Format', c_uint), ('Stereo', wt.BOOL),
        ('SampleDesc', DXGI_SAMPLE_DESC),
        ('BufferUsage', c_uint), ('BufferCount', c_uint),
        ('Scaling', c_uint), ('SwapEffect', c_uint),
        ('AlphaMode', c_uint), ('Flags', c_uint),
    ]

class D3D11_TEXTURE2D_DESC(Structure):
    _fields_ = [
        ('Width', c_uint), ('Height', c_uint),
        ('MipLevels', c_uint), ('ArraySize', c_uint),
        ('Format', c_uint), ('SampleDesc', DXGI_SAMPLE_DESC),
        ('Usage', c_uint), ('BindFlags', c_uint),
        ('CPUAccessFlags', c_uint), ('MiscFlags', c_uint),
    ]

class D3D11_MAPPED_SUBRESOURCE(Structure):
    _fields_ = [('pData', c_void_p), ('RowPitch', c_uint), ('DepthPitch', c_uint)]

PSZ = sizeof(c_void_p)

def vc(this, idx, ret, at, *args):
    addr = this.value if isinstance(this, c_void_p) else int(this)
    vtbl = c_void_p.from_address(addr).value
    fp = c_void_p.from_address(vtbl + idx * PSZ).value
    return WINFUNCTYPE(ret, c_void_p, *at)(fp)(addr, *args)

def qi(obj, iid):
    out = c_void_p()
    hr = vc(obj, 0, HRESULT, (POINTER(GUID), POINTER(c_void_p)), byref(iid), byref(out))
    return out if hr >= 0 else None

# ── window dimensions ──
WIN_W, WIN_H = 400, 400     # create FULL SIZE first
SC_W, SC_H   = 400, 400     # swap chain matches

# ── create window ──
WNDPROC = WINFUNCTYPE(ctypes.c_long, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
u32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.DefWindowProcW.restype = ctypes.c_long

def wndproc(h, m, w, l):
    return u32.DefWindowProcW(h, m, w, l)
_proc_ref = WNDPROC(wndproc)

hinst = k32.GetModuleHandleW(None)
cls_name = 'DCompClipTest'
wc = WNDCLASSEXW()
wc.cbSize = sizeof(WNDCLASSEXW)
wc.style = CS_OWNDC
wc.lpfnWndProc = ctypes.cast(_proc_ref, c_void_p)
wc.hInstance = hinst
wc.lpszClassName = cls_name

u32.RegisterClassExW.argtypes = [POINTER(WNDCLASSEXW)]
u32.RegisterClassExW.restype = wt.ATOM
u32.RegisterClassExW(byref(wc))

u32.CreateWindowExW.argtypes = [
    wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
    c_int, c_int, c_int, c_int,
    wt.HWND, wt.HMENU, wt.HINSTANCE, c_void_p,
]
u32.CreateWindowExW.restype = wt.HWND

hwnd = u32.CreateWindowExW(
    WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
    cls_name, '',
    WS_POPUP | WS_VISIBLE,
    100, 100, WIN_W, WIN_H,
    0, 0, hinst, None,
)
assert hwnd, f'CreateWindowExW failed: {ctypes.GetLastError()}'
print(f'Window HWND={hwnd:#x}  rect=({100},{100},{100+WIN_W},{100+WIN_H})')

# DWM glass for transparency
m = MARGINS(-1, -1, -1, -1)
dwm.DwmExtendFrameIntoClientArea(hwnd, byref(m))

# ── D3D11 device ──
d3d11.D3D11CreateDevice.restype = HRESULT
d3d11.D3D11CreateDevice.argtypes = [
    c_void_p, c_uint, c_void_p, c_uint,
    POINTER(c_int), c_uint, c_uint,
    POINTER(c_void_p), POINTER(c_int), POINTER(c_void_p),
]
dev = c_void_p()
ctx = c_void_p()
feat = c_int()
hr = d3d11.D3D11CreateDevice(
    None, 1, None, 0x20, None, 0, 7,
    byref(dev), byref(feat), byref(ctx))
assert hr >= 0, f'D3D11CreateDevice failed: {hr:#x}'

# ── DXGI factory ──
IID_IDXGIDevice = guid('54ec77fa-1377-44e6-8c32-88fd5f44c84c')
IID_IDXGIFactory2 = guid('50c83a1c-e072-4c48-87b0-3630fa36a6d0')
IID_IDCompositionDevice = guid('C37EA93A-E7AA-450D-B16F-9746CB0407F3')

dxgi_dev = qi(dev, IID_IDXGIDevice)
assert dxgi_dev, 'QueryInterface IDXGIDevice failed'
adapter = c_void_p()
vc(dxgi_dev, 7, HRESULT, (POINTER(c_void_p),), byref(adapter))
fac = c_void_p()
vc(adapter, 6, HRESULT, (POINTER(GUID), POINTER(c_void_p)), byref(IID_IDXGIFactory2), byref(fac))

# ── swap chain ──
desc = DXGI_SWAP_CHAIN_DESC1()
desc.Width = SC_W
desc.Height = SC_H
desc.Format = 28  # R8G8B8A8_UNORM
desc.SampleDesc.Count = 1
desc.BufferUsage = 0x20
desc.BufferCount = 2
desc.Scaling = 0
desc.SwapEffect = 3  # FLIP_SEQUENTIAL
desc.AlphaMode = 1    # PREMULTIPLIED
swap = c_void_p()
hr = vc(fac, 24, HRESULT,
        (c_void_p, POINTER(DXGI_SWAP_CHAIN_DESC1), c_void_p, POINTER(c_void_p)),
        dev.value, byref(desc), None, byref(swap))
assert hr >= 0, f'CreateSwapChainForComposition failed: {hr:#x}'

# ── fill swap chain with solid RED ──
IID_ID3D11Texture2D = guid('6f15aaf2-d208-4e89-9ab4-489535d34f9c')
backbuf = c_void_p()
vc(swap, 9, HRESULT, (c_uint, POINTER(GUID), POINTER(c_void_p)),
   0, byref(IID_ID3D11Texture2D), byref(backbuf))

# Create staging texture, fill with red, copy to backbuf
td = D3D11_TEXTURE2D_DESC()
td.Width = SC_W; td.Height = SC_H
td.MipLevels = 1; td.ArraySize = 1
td.Format = 28; td.SampleDesc.Count = 1
td.Usage = 3  # STAGING
td.CPUAccessFlags = 0x10000  # WRITE
staging = c_void_p()
vc(dev, 5, HRESULT, (POINTER(D3D11_TEXTURE2D_DESC), c_void_p, POINTER(c_void_p)),
   byref(td), None, byref(staging))

mapped = D3D11_MAPPED_SUBRESOURCE()
vc(ctx, 14, HRESULT, (c_void_p, c_uint, c_uint, c_uint, POINTER(D3D11_MAPPED_SUBRESOURCE)),
   staging.value, 0, 2, 0, byref(mapped))

# Fill with RGBA(255, 0, 0, 200) — semi-transparent red
pixel = struct.pack('BBBB', 200, 0, 0, 200)  # premultiplied: R*A/255
for row in range(SC_H):
    dst = mapped.pData + row * mapped.RowPitch
    ctypes.memmove(dst, pixel * SC_W, 4 * SC_W)
vc(ctx, 15, None, (c_void_p, c_uint), staging.value, 0)
vc(ctx, 47, None, (c_void_p, c_void_p), backbuf.value, staging.value)

# ── DComp ──
dcomp.DCompositionCreateDevice.restype = HRESULT
dcomp.DCompositionCreateDevice.argtypes = [c_void_p, POINTER(GUID), POINTER(c_void_p)]
dc_dev = c_void_p()
hr = dcomp.DCompositionCreateDevice(dxgi_dev.value, byref(IID_IDCompositionDevice), byref(dc_dev))
assert hr >= 0, f'DCompositionCreateDevice failed: {hr:#x}'

dc_tgt = c_void_p()
vc(dc_dev, 6, HRESULT, (c_void_p, c_int, POINTER(c_void_p)),
   hwnd, 1, byref(dc_tgt))

dc_vis = c_void_p()
vc(dc_dev, 7, HRESULT, (POINTER(c_void_p),), byref(dc_vis))
vc(dc_vis, 15, HRESULT, (c_void_p,), swap.value)  # SetContent
vc(dc_tgt, 3, HRESULT, (c_void_p,), dc_vis.value)  # SetRoot

# Present + Commit
vc(swap, 8, HRESULT, (c_uint, c_uint), 0, 0)
vc(dc_dev, 3, HRESULT, ())

import time
time.sleep(0.3)

hdc_screen = u32.GetDC(0)
u32.GetDC.restype = wt.HDC
_gdi32 = ctypes.windll.gdi32
_gdi32.GetPixel.argtypes = [wt.HDC, c_int, c_int]
_gdi32.GetPixel.restype = wt.DWORD

def sample(x, y):
    c = _gdi32.GetPixel(hdc_screen, x, y)
    r, g, b = c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF
    return r, g, b, r > 100 and g < 50 and b < 50

# Phase 1: window is 400x400 — should see red everywhere in the area
print(f'Window created at 400x400, swap chain 400x400')
pts = [(125,125), (300,300), (450,450)]
print('\n--- Phase 1: before scrub (400x400 window) ---')
for x, y in pts:
    r, g, b, red = sample(x, y)
    print(f'  ({x},{y}) RGB({r},{g},{b}) {"RED" if red else "not red"}')

# Phase 2: scrub rcWindow to 1x1 via SetWindowPos (user-mode, kernel sees it)
# This changes the REAL rcWindow — both user-mode and kernel
u32.SetWindowPos.argtypes = [
    wt.HWND, wt.HWND, c_int, c_int, c_int, c_int, wt.UINT]
u32.SetWindowPos.restype = wt.BOOL
u32.SetWindowPos(hwnd, 0, 100, 100, 1, 1, 0x0010)
time.sleep(0.3)

print('\n--- Phase 2: after SetWindowPos to 1x1 (real resize) ---')
for x, y in pts:
    r, g, b, red = sample(x, y)
    print(f'  ({x},{y}) RGB({r},{g},{b}) {"RED" if red else "not red"}')

# Phase 3: resize BACK to 400x400 then scrub tagWND only
u32.SetWindowPos(hwnd, 0, 100, 100, 400, 400, 0x0010)
# re-present the swap chain
vc(swap, 8, HRESULT, (c_uint, c_uint), 0, 0)
vc(dc_dev, 3, HRESULT, ())
time.sleep(0.3)

# Now scrub tagWND.rcWindow to 1x1 via DIRECT MEMORY WRITE
# (simulating what _dc.hide_window_rect does via physical write)
# Since we can't use physical write in test, use SetWindowPos to (1,1)
# BUT with SWP_NOSENDCHANGING to avoid DWM notification... no that
# doesn't exist. Instead, try writing via ctypes directly to the
# desktop heap mapping (read-only, will fail). So we resort to the
# real API and check DWM behavior.
#
# Real test: SetWindowPos to 400x400 (DWM caches 400x400), then
# SetWindowPos to 1x1 but see if DWM IMMEDIATELY stops rendering
# or keeps the old area for a bit.
u32.SetWindowPos(hwnd, 0, 100, 100, 1, 1, 0x0010)
# DON'T sleep — check immediately
print('\n--- Phase 3: SetWindowPos to 1x1, check IMMEDIATELY ---')
for x, y in pts:
    r, g, b, red = sample(x, y)
    print(f'  ({x},{y}) RGB({r},{g},{b}) {"RED" if red else "not red"}')

time.sleep(0.3)
print('\n--- Phase 3b: same check after 300ms ---')
for x, y in pts:
    r, g, b, red = sample(x, y)
    print(f'  ({x},{y}) RGB({r},{g},{b}) {"RED" if red else "not red"}')

u32.ReleaseDC(0, hdc_screen)
u32.DestroyWindow(hwnd)

print()
print('If Phase 2/3 show red ONLY at (125,125) → DWM respects rcWindow')
print('If Phase 3 shows red at all points → DWM uses cached value (scrub works!)')
