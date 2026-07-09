# -*- coding: utf-8 -*-
# DXGI Desktop Duplication — WDA-aware desktop capture.
#
# ImageGrab/mss 走 GDI BitBlt: 对 SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
# 排除的窗口(包括本进程 compositor host)整块表现为黑, 鱼眼因此看到全黑背景.
# IDXGIOutputDuplication 走 DWM 合成路径, 会真正跳过被排除的窗口, 让下方桌面/
# 游戏画面透出, 这是 WDA_EXCLUDEFROMCAPTURE 的设计语义.
#
# 采集链路:
#   D3D11CreateDevice → QI IDXGIDevice → GetAdapter → EnumOutputs(idx)
#   → QI IDXGIOutput1 → DuplicateOutput → IDXGIOutputDuplication
#
# 每帧:
#   AcquireNextFrame(timeout) → QI ID3D11Texture2D
#   → CopyResource(staging, desktop) → Map(staging, READ) → memmove
#   → Unmap → ReleaseFrame → BGRA→RGB tight-pack
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
from ctypes import POINTER, Structure, byref, c_int, c_uint, c_void_p, sizeof
from typing import Optional, Tuple

HRESULT = ctypes.HRESULT

# ── GUID helper (self-contained copy) ────────────────────────────

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
IID_IDXGIAdapter = _guid('2411e7e1-12ac-4ccf-bd14-9798e8534dc0')
IID_IDXGIOutput1 = _guid('00cddea8-939b-4b83-a340-a685226666cc')
IID_ID3D11Texture2D = _guid('6f15aaf2-d208-4e89-9ab4-489535d34f9c')

# ── COM vtable call ─────────────────────────────────────────────

_PTR_SZ = sizeof(c_void_p)


def _vc(this, idx, ret, argtypes, *args):
    addr = this.value if isinstance(this, c_void_p) else int(this)
    vtbl = c_void_p.from_address(addr).value
    fp = c_void_p.from_address(vtbl + idx * _PTR_SZ).value
    return ctypes.WINFUNCTYPE(ret, c_void_p, *argtypes)(fp)(addr, *args)


def _qi(obj, iid):
    out = c_void_p()
    hr = _vc(obj, 0, HRESULT, (POINTER(GUID), POINTER(c_void_p)),
             byref(iid), byref(out))
    return out if hr >= 0 else None


def _release(obj) -> None:
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


class _POINT(Structure):
    _fields_ = [('x', c_int), ('y', c_int)]


class _RECT(Structure):
    _fields_ = [('left', c_int), ('top', c_int),
                ('right', c_int), ('bottom', c_int)]


class _DXGI_RATIONAL(Structure):
    _fields_ = [('Numerator', c_uint), ('Denominator', c_uint)]


class _DXGI_MODE_DESC(Structure):
    _fields_ = [
        ('Width', c_uint), ('Height', c_uint),
        ('RefreshRate', _DXGI_RATIONAL),
        ('Format', c_uint),
        ('ScanlineOrdering', c_uint),
        ('Scaling', c_uint),
    ]


class _DXGI_OUTPUT_DESC(Structure):
    _fields_ = [
        ('DeviceName', wt.WCHAR * 32),
        ('DesktopCoordinates', _RECT),
        ('AttachedToDesktop', wt.BOOL),
        ('Rotation', c_uint),
        ('Monitor', c_void_p),
    ]


class _DXGI_OUTDUPL_POINTER_POSITION(Structure):
    _fields_ = [('Position', _POINT), ('Visible', wt.BOOL)]


class _DXGI_OUTDUPL_FRAME_INFO(Structure):
    _fields_ = [
        ('LastPresentTime', ctypes.c_int64),
        ('LastMouseUpdateTime', ctypes.c_int64),
        ('AccumulatedFrames', c_uint),
        ('RectsCoalesced', wt.BOOL),
        ('ProtectedContentMaskedOut', wt.BOOL),
        ('PointerPosition', _DXGI_OUTDUPL_POINTER_POSITION),
        ('TotalMetadataBufferSize', c_uint),
        ('PointerShapeBufferSize', c_uint),
    ]


class _DXGI_OUTDUPL_DESC(Structure):
    _fields_ = [
        ('ModeDesc', _DXGI_MODE_DESC),
        ('Rotation', c_uint),
        ('DesktopImageInSystemMemory', wt.BOOL),
    ]

# ── Constants ───────────────────────────────────────────────────

_D3D_DRIVER_TYPE_HARDWARE = 1
_D3D11_SDK_VERSION = 7
_D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20

_D3D11_USAGE_STAGING = 3
_D3D11_CPU_ACCESS_READ = 0x20000
_D3D11_MAP_READ = 1

_DXGI_FORMAT_B8G8R8A8_UNORM = 87

# DXGI HRESULTs
_DXGI_ERROR_WAIT_TIMEOUT = 0x887A0027
_DXGI_ERROR_ACCESS_LOST = 0x887A0026
_DXGI_ERROR_INVALID_CALL = 0x887A0001
_DXGI_ERROR_NOT_FOUND = 0x887A0002
_DXGI_ERROR_DEVICE_REMOVED = 0x887A0005
_DXGI_ERROR_DEVICE_HUNG = 0x887A0006
_DXGI_ERROR_DEVICE_RESET = 0x887A0007
_DEVICE_LOST_CODES = frozenset((
    _DXGI_ERROR_DEVICE_REMOVED,
    _DXGI_ERROR_DEVICE_HUNG,
    _DXGI_ERROR_DEVICE_RESET,
))

# ── COM vtable indices ──────────────────────────────────────────

_IDXGIObject_GetParent = 6

_IDXGIDevice_GetAdapter = 7

_IDXGIAdapter_EnumOutputs = 7

_IDXGIOutput1_DuplicateOutput = 22

_IDXGIOutputDuplication_AcquireNextFrame = 8
_IDXGIOutputDuplication_ReleaseFrame = 14
_IDXGIOutputDuplication_GetDesc = 7

_ID3D11Device_CreateTexture2D = 5
_ID3D11Device_GetDeviceRemovedReason = 39

_ID3D11DeviceContext_Map = 14
_ID3D11DeviceContext_Unmap = 15
_ID3D11DeviceContext_CopyResource = 47

# ── DLL handle ──────────────────────────────────────────────────

_d3d11 = ctypes.WinDLL('d3d11')
_d3d11.D3D11CreateDevice.restype = HRESULT
_d3d11.D3D11CreateDevice.argtypes = [
    c_void_p, c_uint, c_void_p, c_uint,
    POINTER(c_int), c_uint, c_uint,
    POINTER(c_void_p), POINTER(c_int), POINTER(c_void_p),
]


def _hr_ok(hr: int, label: str) -> int:
    if hr < 0:
        raise OSError(f'{label} failed: HRESULT 0x{hr & 0xFFFFFFFF:08X}')
    return hr


# ── DXGIDuplicator ──────────────────────────────────────────────

class DXGIDuplicator:
    # Per-instance state owns exactly one D3D11 device + one output
    # duplication. All methods must be called from the same thread that
    # created the instance (D3D11 device is thread-affine unless
    # created with MULTITHREAD flag, which we deliberately skip — the
    # fisheye worker is single-threaded).

    def __init__(self, output_index: int = 0) -> None:
        self._output_index = int(output_index)
        self._alive = False

        self._d3d_dev: Optional[c_void_p] = None
        self._d3d_ctx: Optional[c_void_p] = None
        self._dxgi_dev: Optional[c_void_p] = None
        self._dxgi_adp: Optional[c_void_p] = None
        self._output1: Optional[c_void_p] = None
        self._dup: Optional[c_void_p] = None
        self._staging: Optional[c_void_p] = None

        self._width = 0
        self._height = 0
        self._rotation = 0
        # BGRA row is width*4; staging RowPitch may be larger (GPU pitch
        # alignment). Cached on first successful Map to avoid re-reading
        # every frame.
        self._row_pitch = 0

        try:
            self._init()
        except Exception:
            self._destroy_all()
            raise

    # ── setup ────────────────────────────────────────────────────

    def _init(self) -> None:
        # D3D11 device (no MULTITHREAD — see class docstring).
        d3d_dev = c_void_p()
        d3d_ctx = c_void_p()
        feat = c_int()
        _hr_ok(
            _d3d11.D3D11CreateDevice(
                None, _D3D_DRIVER_TYPE_HARDWARE, None,
                _D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                None, 0, _D3D11_SDK_VERSION,
                byref(d3d_dev), byref(feat), byref(d3d_ctx),
            ), 'D3D11CreateDevice')
        self._d3d_dev = d3d_dev
        self._d3d_ctx = d3d_ctx

        self._dxgi_dev = _qi(self._d3d_dev, IID_IDXGIDevice)
        if not self._dxgi_dev or not self._dxgi_dev.value:
            raise OSError('QI IDXGIDevice failed')

        self._dxgi_adp = c_void_p()
        _hr_ok(
            _vc(self._dxgi_dev, _IDXGIDevice_GetAdapter, HRESULT,
                (POINTER(c_void_p),), byref(self._dxgi_adp)),
            'IDXGIDevice::GetAdapter')

        # EnumOutputs → IDXGIOutput → QI IDXGIOutput1.
        output = c_void_p()
        hr = _vc(self._dxgi_adp, _IDXGIAdapter_EnumOutputs, HRESULT,
                 (c_uint, POINTER(c_void_p)),
                 self._output_index, byref(output))
        if hr < 0 or not output.value:
            raise OSError(
                f'EnumOutputs({self._output_index}) failed: '
                f'0x{hr & 0xFFFFFFFF:08X}')

        self._output1 = _qi(output, IID_IDXGIOutput1)
        _release(output)
        if not self._output1 or not self._output1.value:
            raise OSError('QI IDXGIOutput1 failed (Win7 or older?)')

        # DuplicateOutput — the actual desktop-duplication object.
        dup = c_void_p()
        hr = _vc(self._output1, _IDXGIOutput1_DuplicateOutput, HRESULT,
                 (c_void_p, POINTER(c_void_p)),
                 self._d3d_dev.value, byref(dup))
        if hr < 0 or not dup.value:
            raise OSError(
                f'IDXGIOutput1::DuplicateOutput failed: '
                f'0x{hr & 0xFFFFFFFF:08X}')
        self._dup = dup

        # GetDesc → ModeDesc.Width/Height so we can pre-create staging.
        desc = _DXGI_OUTDUPL_DESC()
        hr = _vc(self._dup, _IDXGIOutputDuplication_GetDesc, None,
                 (POINTER(_DXGI_OUTDUPL_DESC),), byref(desc))
        self._width = int(desc.ModeDesc.Width)
        self._height = int(desc.ModeDesc.Height)
        self._rotation = int(desc.Rotation)

        if self._width <= 0 or self._height <= 0:
            raise OSError('DuplicateOutput reported zero size')

        self._create_staging(self._width, self._height)
        self._alive = True

    def _create_staging(self, width: int, height: int) -> None:
        td = _D3D11_TEXTURE2D_DESC()
        td.Width = width
        td.Height = height
        td.MipLevels = 1
        td.ArraySize = 1
        td.Format = _DXGI_FORMAT_B8G8R8A8_UNORM
        td.SampleDesc.Count = 1
        td.SampleDesc.Quality = 0
        td.Usage = _D3D11_USAGE_STAGING
        td.BindFlags = 0
        td.CPUAccessFlags = _D3D11_CPU_ACCESS_READ
        td.MiscFlags = 0

        staging = c_void_p()
        _hr_ok(
            _vc(self._d3d_dev, _ID3D11Device_CreateTexture2D, HRESULT,
                (POINTER(_D3D11_TEXTURE2D_DESC), c_void_p, POINTER(c_void_p)),
                byref(td), None, byref(staging)),
            'CreateTexture2D(staging)')
        self._staging = staging
        self._row_pitch = 0  # invalidate cached pitch

    # ── per-frame ────────────────────────────────────────────────

    def try_acquire_rgb(self, timeout_ms: int = 8
                        ) -> Optional[Tuple[bytes, int, int]]:
        # Returns (rgb_bytes, width, height) on success, None on timeout /
        # transient failure. Fisheye worker retries on None. AccessLost /
        # device-loss triggers a one-shot rebuild attempt inside; caller
        # sees None during that window and re-tries next frame.
        if not self._alive:
            return None

        info = _DXGI_OUTDUPL_FRAME_INFO()
        resource = c_void_p()

        hr = _vc(self._dup, _IDXGIOutputDuplication_AcquireNextFrame, HRESULT,
                 (c_uint, POINTER(_DXGI_OUTDUPL_FRAME_INFO), POINTER(c_void_p)),
                 int(timeout_ms), byref(info), byref(resource))
        code = hr & 0xFFFFFFFF

        if hr < 0:
            if code == _DXGI_ERROR_WAIT_TIMEOUT:
                return None
            if code == _DXGI_ERROR_ACCESS_LOST:
                # Screen resolution change, session lock, UAC prompt —
                # the current duplication is invalid, rebuild on next
                # call. Callers see None this frame.
                self._rebuild_duplication()
                return None
            if code in _DEVICE_LOST_CODES:
                self._alive = False
                return None
            # Any other error: log once and go dead.
            print(f'[DDA] AcquireNextFrame failed: 0x{code:08X}', flush=True)
            self._alive = False
            return None

        # First-acquire trap: right after DuplicateOutput the driver
        # sometimes returns success with LastPresentTime=0 and an
        # uninitialised (all-zero) back buffer. Skip that frame and let
        # the caller retry — the very next Acquire returns real desktop
        # pixels. Must still ReleaseFrame + Release resource.
        if info.LastPresentTime == 0:
            try:
                _vc(self._dup, _IDXGIOutputDuplication_ReleaseFrame,
                    HRESULT, ())
            except Exception:
                pass
            _release(resource)
            return None

        # ── happy path ── AcquireNextFrame succeeded; MUST ReleaseFrame
        # before the next Acquire, even on our own failures below.
        rgb: Optional[bytes] = None
        try:
            tex = _qi(resource, IID_ID3D11Texture2D)
            if tex is None or not tex.value:
                return None
            try:
                # CopyResource desktop → staging (GPU-side, no CPU touch).
                _vc(self._d3d_ctx, _ID3D11DeviceContext_CopyResource, None,
                    (c_void_p, c_void_p),
                    self._staging.value, tex.value)
            finally:
                _release(tex)

            mapped = _D3D11_MAPPED_SUBRESOURCE()
            hr = _vc(self._d3d_ctx, _ID3D11DeviceContext_Map, HRESULT,
                     (c_void_p, c_uint, c_uint, c_uint,
                      POINTER(_D3D11_MAPPED_SUBRESOURCE)),
                     self._staging.value, 0, _D3D11_MAP_READ, 0,
                     byref(mapped))
            if hr < 0:
                return None
            try:
                w = self._width
                h = self._height
                pitch = int(mapped.RowPitch)
                self._row_pitch = pitch
                rgb = _bgra_pitched_to_rgb_bytes(mapped.pData, w, h, pitch)
            finally:
                _vc(self._d3d_ctx, _ID3D11DeviceContext_Unmap, None,
                    (c_void_p, c_uint), self._staging.value, 0)
        finally:
            # ReleaseFrame is REQUIRED after every successful Acquire.
            try:
                _vc(self._dup, _IDXGIOutputDuplication_ReleaseFrame,
                    HRESULT, ())
            except Exception:
                pass
            _release(resource)

        if rgb is None:
            return None
        return rgb, self._width, self._height

    # ── recovery ─────────────────────────────────────────────────

    def _rebuild_duplication(self) -> None:
        # Tear down current duplication + staging + Output1 and reopen.
        # Leaves D3D11 device/adapter/DXGI device alone — those survive
        # AccessLost. If reopening fails, self._alive stays False and the
        # caller falls back to the next capture chain link (mss / GDI).
        try:
            if self._dup is not None:
                _release(self._dup)
                self._dup = None
            if self._staging is not None:
                _release(self._staging)
                self._staging = None
            if self._output1 is not None:
                _release(self._output1)
                self._output1 = None
        except Exception:
            pass

        try:
            output = c_void_p()
            hr = _vc(self._dxgi_adp, _IDXGIAdapter_EnumOutputs, HRESULT,
                     (c_uint, POINTER(c_void_p)),
                     self._output_index, byref(output))
            if hr < 0 or not output.value:
                self._alive = False
                return
            self._output1 = _qi(output, IID_IDXGIOutput1)
            _release(output)
            if not self._output1 or not self._output1.value:
                self._alive = False
                return

            dup = c_void_p()
            hr = _vc(self._output1, _IDXGIOutput1_DuplicateOutput, HRESULT,
                     (c_void_p, POINTER(c_void_p)),
                     self._d3d_dev.value, byref(dup))
            if hr < 0 or not dup.value:
                self._alive = False
                return
            self._dup = dup

            desc = _DXGI_OUTDUPL_DESC()
            _vc(self._dup, _IDXGIOutputDuplication_GetDesc, None,
                (POINTER(_DXGI_OUTDUPL_DESC),), byref(desc))
            new_w = int(desc.ModeDesc.Width)
            new_h = int(desc.ModeDesc.Height)
            if new_w <= 0 or new_h <= 0:
                self._alive = False
                return
            self._width = new_w
            self._height = new_h
            self._rotation = int(desc.Rotation)
            self._create_staging(new_w, new_h)
            self._alive = True
        except Exception as exc:
            print(f'[DDA] rebuild failed: {exc}', flush=True)
            self._alive = False

    # ── teardown ─────────────────────────────────────────────────

    def _destroy_all(self) -> None:
        for obj in (self._staging, self._dup, self._output1,
                    self._dxgi_adp, self._dxgi_dev,
                    self._d3d_ctx, self._d3d_dev):
            _release(obj)
        self._staging = self._dup = self._output1 = None
        self._dxgi_adp = self._dxgi_dev = None
        self._d3d_ctx = self._d3d_dev = None
        self._alive = False

    def destroy(self) -> None:
        if not self._alive and self._d3d_dev is None:
            return
        self._destroy_all()

    def __del__(self):
        try:
            self.destroy()
        except Exception:
            pass

    @property
    def alive(self) -> bool:
        return self._alive

    @property
    def size(self) -> Tuple[int, int]:
        return (self._width, self._height)


# ── BGRA (pitched) → RGB (tight) conversion ─────────────────────
#
# Staging Map returns rows padded to GPU pitch alignment; fisheye
# presenter uploads tight-packed rgb (w*h*3). Prefer numpy when
# available (single vectorised slice+swap ~2ms for 1920x1080); fall
# back to a pure-ctypes memmove+swap loop if numpy is unavailable
# (about 3× slower but still <10ms).

def _bgra_pitched_to_rgb_bytes(src_ptr: int, w: int, h: int,
                                pitch: int) -> bytes:
    try:
        import numpy as _np
    except Exception:
        _np = None

    row_bytes = w * 4
    total_src = h * pitch

    if _np is not None:
        # Wrap the mapped memory as a read-only numpy array. from_address
        # doesn't take ownership — Unmap() happens right after we return
        # so we must materialise the bytes before yielding control.
        buf = (ctypes.c_ubyte * total_src).from_address(int(src_ptr))
        arr = _np.frombuffer(buf, dtype=_np.uint8).reshape(h, pitch)
        tight = arr[:, :row_bytes].reshape(h, w, 4)
        rgb = tight[:, :, [2, 1, 0]]  # BGRA → RGB (drops alpha)
        return rgb.tobytes()

    # Pure-ctypes fallback.
    row = (ctypes.c_ubyte * row_bytes)()
    out = bytearray(w * h * 3)
    out_off = 0
    for y in range(h):
        ctypes.memmove(row, int(src_ptr) + y * pitch, row_bytes)
        # BGRA → RGB swap in Python.
        for x in range(w):
            b = row[x * 4]
            g = row[x * 4 + 1]
            r = row[x * 4 + 2]
            out[out_off] = r
            out[out_off + 1] = g
            out[out_off + 2] = b
            out_off += 3
    return bytes(out)
