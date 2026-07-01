# -*- coding: utf-8 -*-
"""DirectComposition presentation bridge — GL pixels → DComp → DWM.

Replaces WGL SwapBuffers for overlay anti-capture. GL rendering stays
on the existing WGL context; pixel data is read via moderngl read()
and uploaded to a DXGI swap chain bound to a DirectComposition visual.

DWM manages DirectComposition content so SetWindowDisplayAffinity
(WDA) can exclude the overlay from screen capture.

Setup chain:
  D3D11 device → IDXGIDevice → IDXGIAdapter → IDXGIFactory2
    → CreateSwapChainForComposition
  DCompositionCreateDevice → CreateTargetForHwnd → CreateVisual
    → SetContent(swapchain) → SetRoot → Commit

Per-frame:
  glReadPixels → Map staging texture → flip+copy → Unmap
    → CopyResource(backbuf, staging) → Present → Commit
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
from ctypes import POINTER, Structure, byref, c_int, c_uint, c_void_p, sizeof

HRESULT = ctypes.HRESULT

# ── GUID helper ─────────────────────────────────────────────────

class GUID(Structure):
    _fields_ = [
        ('Data1', c_uint),
        ('Data2', ctypes.c_ushort),
        ('Data3', ctypes.c_ushort),
        ('Data4', ctypes.c_ubyte * 8),
    ]


def _guid(s: str) -> GUID:
    p = s.replace('-', '')
    return GUID(
        int(p[0:8], 16), int(p[8:12], 16), int(p[12:16], 16),
        (ctypes.c_ubyte * 8)(*[int(p[i:i + 2], 16) for i in range(16, 32, 2)]),
    )


IID_IDXGIDevice = _guid('54ec77fa-1377-44e6-8c32-88fd5f44c84c')
IID_IDXGIFactory2 = _guid('50c83a1c-e072-4c48-87b0-3630fa36a6d0')
IID_ID3D11Texture2D = _guid('6f15aaf2-d208-4e89-9ab4-489535d34f9c')
IID_IDCompositionDevice = _guid('C37EA93A-E7AA-450D-B16F-9746CB0407F3')

# ── COM vtable call ─────────────────────────────────────────────

_PTR_SZ = sizeof(c_void_p)  # 8 on x64


def _vc(this, idx, ret, argtypes, *args):
    """Call COM vtable method *idx* on *this* (int or c_void_p)."""
    addr = this.value if isinstance(this, c_void_p) else int(this)
    vtbl = c_void_p.from_address(addr).value
    fp = c_void_p.from_address(vtbl + idx * _PTR_SZ).value
    return ctypes.WINFUNCTYPE(ret, c_void_p, *argtypes)(fp)(addr, *args)


def _qi(obj, iid):
    """QueryInterface → c_void_p on success, None on failure."""
    out = c_void_p()
    hr = _vc(obj, 0, HRESULT, (POINTER(GUID), POINTER(c_void_p)),
             byref(iid), byref(out))
    return out if hr >= 0 else None


def _release(obj):
    """IUnknown::Release — safe for None / null pointer."""
    if obj is None:
        return
    addr = obj.value if isinstance(obj, c_void_p) else obj
    if not addr:
        return
    try:
        _vc(obj, 2, c_uint, ())
    except Exception:
        pass

# ── Structures ──────────────────────────────────────────────────

class _DXGI_SAMPLE_DESC(Structure):
    _fields_ = [('Count', c_uint), ('Quality', c_uint)]


class _DXGI_SWAP_CHAIN_DESC1(Structure):
    _fields_ = [
        ('Width', c_uint), ('Height', c_uint),
        ('Format', c_uint), ('Stereo', wt.BOOL),
        ('SampleDesc', _DXGI_SAMPLE_DESC),
        ('BufferUsage', c_uint), ('BufferCount', c_uint),
        ('Scaling', c_uint), ('SwapEffect', c_uint),
        ('AlphaMode', c_uint), ('Flags', c_uint),
    ]


class _D3D11_TEXTURE2D_DESC(Structure):
    _fields_ = [
        ('Width', c_uint), ('Height', c_uint),
        ('MipLevels', c_uint), ('ArraySize', c_uint),
        ('Format', c_uint), ('SampleDesc', _DXGI_SAMPLE_DESC),
        ('Usage', c_uint), ('BindFlags', c_uint),
        ('CPUAccessFlags', c_uint), ('MiscFlags', c_uint),
    ]


class _D3D11_MAPPED_SUBRESOURCE(Structure):
    _fields_ = [
        ('pData', c_void_p),
        ('RowPitch', c_uint),
        ('DepthPitch', c_uint),
    ]


class _D2D_MATRIX_3X2_F(Structure):
    _fields_ = [
        ('_11', ctypes.c_float), ('_12', ctypes.c_float),
        ('_21', ctypes.c_float), ('_22', ctypes.c_float),
        ('_31', ctypes.c_float), ('_32', ctypes.c_float),
    ]

# ── Constants ───────────────────────────────────────────────────

_DXGI_FORMAT_R8G8B8A8_UNORM = 28
_DXGI_USAGE_RENDER_TARGET_OUTPUT = 0x20
_DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3
_DXGI_SCALING_STRETCH = 0
_DXGI_ALPHA_MODE_PREMULTIPLIED = 1

_D3D_DRIVER_TYPE_HARDWARE = 1
_D3D11_SDK_VERSION = 7
_D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20
_D3D11_USAGE_STAGING = 3
_D3D11_CPU_ACCESS_WRITE = 0x10000
_D3D11_MAP_WRITE = 2

# ── COM vtable indices ──────────────────────────────────────────
# Counted from the C++ header declaration order (IUnknown = 0‑2).

_IDXGIObject_GetParent = 6
_IDXGIDevice_GetAdapter = 7
_IDXGIFactory2_CreateSwapChainForComposition = 24
_IDXGISwapChain_Present = 8
_IDXGISwapChain_GetBuffer = 9
_IDXGISwapChain_ResizeBuffers = 13
_ID3D11Device_CreateTexture2D = 5
_ID3D11DeviceContext_Map = 14
_ID3D11DeviceContext_Unmap = 15
_ID3D11DeviceContext_CopyResource = 47
_IDCompositionDevice_Commit = 3
_IDCompositionDevice_CreateTargetForHwnd = 6
_IDCompositionDevice_CreateVisual = 7
_IDCompositionTarget_SetRoot = 3
_IDCompositionVisual_SetContent = 15
_IDCompositionVisual_SetTransform_Matrix = 8

# ── DLL handles ─────────────────────────────────────────────────

_d3d11 = ctypes.WinDLL('d3d11')
_d3d11.D3D11CreateDevice.restype = HRESULT
_d3d11.D3D11CreateDevice.argtypes = [
    c_void_p, c_uint, c_void_p, c_uint,
    POINTER(c_int), c_uint, c_uint,
    POINTER(c_void_p), POINTER(c_int), POINTER(c_void_p),
]

_dcomp = ctypes.WinDLL('dcomp')
_dcomp.DCompositionCreateDevice.restype = HRESULT
_dcomp.DCompositionCreateDevice.argtypes = [
    c_void_p, POINTER(GUID), POINTER(c_void_p),
]


def _hr_ok(hr: int, label: str) -> int:
    if hr < 0:
        raise OSError(f'{label} failed: HRESULT 0x{hr & 0xFFFFFFFF:08X}')
    return hr


# Zero-copy pointer to bytes internal buffer
_PyBytes_AsString = ctypes.pythonapi.PyBytes_AsString
_PyBytes_AsString.restype = c_void_p
_PyBytes_AsString.argtypes = [ctypes.py_object]

# ── DCompBridge ─────────────────────────────────────────────────


class DCompBridge:
    """Present GL-rendered pixels via DirectComposition.

    All methods must be called from the compositor thread (same
    thread that owns the WGL context).
    """

    def __init__(self, hwnd: int, width: int, height: int) -> None:
        self._hwnd = hwnd
        self._width = width
        self._height = height
        self._alive = False

        self._d3d_dev = c_void_p()
        self._d3d_ctx = c_void_p()
        self._dxgi_dev: c_void_p | None = None
        self._dxgi_adp: c_void_p | None = None
        self._dxgi_fac: c_void_p | None = None
        self._swap: c_void_p | None = None
        self._dc_dev: c_void_p | None = None
        self._dc_tgt: c_void_p | None = None
        self._dc_vis: c_void_p | None = None
        self._staging: c_void_p | None = None

        self._init()

    # ── setup ────────────────────────────────────────────────────

    def _init(self) -> None:
        feat = c_int()
        _hr_ok(
            _d3d11.D3D11CreateDevice(
                None, _D3D_DRIVER_TYPE_HARDWARE, None,
                _D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                None, 0, _D3D11_SDK_VERSION,
                byref(self._d3d_dev), byref(feat), byref(self._d3d_ctx),
            ), 'D3D11CreateDevice')

        self._dxgi_dev = _qi(self._d3d_dev, IID_IDXGIDevice)
        if not self._dxgi_dev or not self._dxgi_dev.value:
            raise OSError('QI IDXGIDevice failed')

        self._dxgi_adp = c_void_p()
        _hr_ok(
            _vc(self._dxgi_dev, _IDXGIDevice_GetAdapter, HRESULT,
                (POINTER(c_void_p),), byref(self._dxgi_adp)),
            'GetAdapter')

        self._dxgi_fac = c_void_p()
        _hr_ok(
            _vc(self._dxgi_adp, _IDXGIObject_GetParent, HRESULT,
                (POINTER(GUID), POINTER(c_void_p)),
                byref(IID_IDXGIFactory2), byref(self._dxgi_fac)),
            'GetParent→IDXGIFactory2')

        desc = _DXGI_SWAP_CHAIN_DESC1()
        desc.Width = self._width
        desc.Height = self._height
        desc.Format = _DXGI_FORMAT_R8G8B8A8_UNORM
        desc.Stereo = False
        desc.SampleDesc.Count = 1
        desc.SampleDesc.Quality = 0
        desc.BufferUsage = _DXGI_USAGE_RENDER_TARGET_OUTPUT
        desc.BufferCount = 2
        desc.Scaling = _DXGI_SCALING_STRETCH
        desc.SwapEffect = _DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
        desc.AlphaMode = _DXGI_ALPHA_MODE_PREMULTIPLIED
        desc.Flags = 0

        self._swap = c_void_p()
        _hr_ok(
            _vc(self._dxgi_fac,
                _IDXGIFactory2_CreateSwapChainForComposition, HRESULT,
                (c_void_p, POINTER(_DXGI_SWAP_CHAIN_DESC1),
                 c_void_p, POINTER(c_void_p)),
                self._d3d_dev.value, byref(desc), None, byref(self._swap)),
            'CreateSwapChainForComposition')

        self._dc_dev = c_void_p()
        _hr_ok(
            _dcomp.DCompositionCreateDevice(
                self._dxgi_dev.value,
                byref(IID_IDCompositionDevice),
                byref(self._dc_dev)),
            'DCompositionCreateDevice')

        self._dc_tgt = c_void_p()
        _hr_ok(
            _vc(self._dc_dev,
                _IDCompositionDevice_CreateTargetForHwnd, HRESULT,
                (c_void_p, c_int, POINTER(c_void_p)),
                self._hwnd, 1, byref(self._dc_tgt)),
            'CreateTargetForHwnd')

        self._dc_vis = c_void_p()
        _hr_ok(
            _vc(self._dc_dev,
                _IDCompositionDevice_CreateVisual, HRESULT,
                (POINTER(c_void_p),), byref(self._dc_vis)),
            'CreateVisual')

        _hr_ok(
            _vc(self._dc_vis,
                _IDCompositionVisual_SetContent, HRESULT,
                (c_void_p,), self._swap.value),
            'SetContent')

        _hr_ok(
            _vc(self._dc_tgt,
                _IDCompositionTarget_SetRoot, HRESULT,
                (c_void_p,), self._dc_vis.value),
            'SetRoot')

        self._set_flip_transform()

        _hr_ok(
            _vc(self._dc_dev, _IDCompositionDevice_Commit, HRESULT, ()),
            'initial Commit')

        self._create_staging()
        self._alive = True
        print(f'[DComp] bridge ready  {self._width}x{self._height}  '
              f'D3D FL={feat.value:#x}', flush=True)

    def _set_flip_transform(self) -> None:
        """Set Y-flip on the DComp visual so GL bottom-up rows display top-down."""
        flip = _D2D_MATRIX_3X2_F(1.0, 0.0, 0.0, -1.0, 0.0, float(self._height))
        _vc(self._dc_vis, _IDCompositionVisual_SetTransform_Matrix, HRESULT,
            (POINTER(_D2D_MATRIX_3X2_F),), byref(flip))

    def _create_staging(self) -> None:
        td = _D3D11_TEXTURE2D_DESC()
        td.Width = self._width
        td.Height = self._height
        td.MipLevels = 1
        td.ArraySize = 1
        td.Format = _DXGI_FORMAT_R8G8B8A8_UNORM
        td.SampleDesc.Count = 1
        td.SampleDesc.Quality = 0
        td.Usage = _D3D11_USAGE_STAGING
        td.BindFlags = 0
        td.CPUAccessFlags = _D3D11_CPU_ACCESS_WRITE
        td.MiscFlags = 0
        self._staging = c_void_p()
        _hr_ok(
            _vc(self._d3d_dev, _ID3D11Device_CreateTexture2D, HRESULT,
                (POINTER(_D3D11_TEXTURE2D_DESC), c_void_p, POINTER(c_void_p)),
                byref(td), None, byref(self._staging)),
            'CreateTexture2D(staging)')

    # ── per-frame ────────────────────────────────────────────────

    def present(self, pixels, width: int, height: int) -> bool:
        """Upload RGBA pixel data and present via DComp.

        *pixels*: ``bytes``, ``bytearray``, or ``int`` (raw pointer).
        The DComp visual has a Y-flip transform, so rows are copied
        straight (no reversal needed).  Returns True on success.
        """
        if not self._alive:
            return False
        if width != self._width or height != self._height:
            self.resize(width, height)

        row_pitch = width * 4
        total = height * row_pitch

        if isinstance(pixels, bytes):
            if len(pixels) < total:
                return False
            src_ptr = _PyBytes_AsString(pixels)
        elif isinstance(pixels, bytearray):
            if len(pixels) < total:
                return False
            _view = (ctypes.c_char * total).from_buffer(pixels)
            src_ptr = ctypes.addressof(_view)
        else:
            src_ptr = int(pixels)

        try:
            mapped = _D3D11_MAPPED_SUBRESOURCE()
            hr = _vc(self._d3d_ctx, _ID3D11DeviceContext_Map, HRESULT,
                     (c_void_p, c_uint, c_uint, c_uint,
                      POINTER(_D3D11_MAPPED_SUBRESOURCE)),
                     self._staging.value, 0, _D3D11_MAP_WRITE, 0,
                     byref(mapped))
            if hr < 0:
                return False

            if mapped.RowPitch == row_pitch:
                ctypes.memmove(mapped.pData, src_ptr, total)
            else:
                dp = mapped.RowPitch
                for y in range(height):
                    ctypes.memmove(
                        mapped.pData + y * dp,
                        src_ptr + y * row_pitch,
                        row_pitch)

            _vc(self._d3d_ctx, _ID3D11DeviceContext_Unmap, None,
                (c_void_p, c_uint), self._staging.value, 0)

            bb = c_void_p()
            hr = _vc(self._swap, _IDXGISwapChain_GetBuffer, HRESULT,
                     (c_uint, POINTER(GUID), POINTER(c_void_p)),
                     0, byref(IID_ID3D11Texture2D), byref(bb))
            if hr < 0:
                return False
            _vc(self._d3d_ctx, _ID3D11DeviceContext_CopyResource, None,
                (c_void_p, c_void_p), bb.value, self._staging.value)
            _release(bb)

            _vc(self._swap, _IDXGISwapChain_Present, HRESULT,
                (c_uint, c_uint), 0, 0)
            _vc(self._dc_dev, _IDCompositionDevice_Commit, HRESULT, ())
            return True

        except Exception as exc:
            print(f'[DComp] present error: {exc}', flush=True)
            return False

    # ── resize ───────────────────────────────────────────────────

    def resize(self, width: int, height: int) -> None:
        if not self._alive or (width == self._width and height == self._height):
            return
        try:
            _release(self._staging)
            self._staging = None

            hr = _vc(self._swap, _IDXGISwapChain_ResizeBuffers, HRESULT,
                     (c_uint, c_uint, c_uint, c_uint, c_uint),
                     0, width, height, 0, 0)
            if hr < 0:
                print(f'[DComp] ResizeBuffers 0x{hr & 0xFFFFFFFF:08X}',
                      flush=True)
                self._alive = False
                return

            self._width = width
            self._height = height
            self._create_staging()
            self._set_flip_transform()
            _vc(self._dc_dev, _IDCompositionDevice_Commit, HRESULT, ())
            print(f'[DComp] resized → {width}x{height}', flush=True)
        except Exception as exc:
            print(f'[DComp] resize error: {exc}', flush=True)

    # ── teardown ─────────────────────────────────────────────────

    def destroy(self) -> None:
        if not self._alive:
            return
        self._alive = False
        for obj in (self._staging, self._dc_vis, self._dc_tgt,
                    self._dc_dev, self._swap, self._dxgi_fac,
                    self._dxgi_adp, self._dxgi_dev,
                    self._d3d_ctx, self._d3d_dev):
            _release(obj)
        print('[DComp] bridge destroyed', flush=True)

    @property
    def alive(self) -> bool:
        return self._alive
