# -*- coding: utf-8 -*-
# DirectComposition presentation bridge — GL pixels → DComp → DWM.
#
# Replaces WGL SwapBuffers for overlay anti-capture. GL rendering stays
# on the existing WGL context; pixel data is read via moderngl read()
# and uploaded to a DXGI swap chain bound to a DirectComposition visual.
#
# DWM manages DirectComposition content so SetWindowDisplayAffinity
# (WDA) can exclude the overlay from screen capture.
#
# Setup chain:
# D3D11 device → IDXGIDevice → IDXGIAdapter → IDXGIFactory2
# → CreateSwapChainForComposition
# DCompositionCreateDevice → CreateTargetForHwnd → CreateVisual
# → SetContent(swapchain) → SetRoot → Commit
#
# Per-frame:
# glReadPixels → Map staging texture → flip+copy → Unmap
# → CopyResource(backbuf, staging) → Present → Commit
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
from ctypes import POINTER, Structure, byref, c_int, c_uint, c_void_p, sizeof
from typing import Any

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
IID_IDXGIKeyedMutex = _guid('9d8e1289-d7b3-465f-8126-250e349af85d')
IID_IDCompositionDevice = _guid('C37EA93A-E7AA-450D-B16F-9746CB0407F3')

# ── COM vtable call ─────────────────────────────────────────────

_PTR_SZ = sizeof(c_void_p)  # 8 on x64


def _vc(this, idx, ret, argtypes, *args):
    # Call COM vtable method *idx* on *this* (int or c_void_p).
    addr = this.value if isinstance(this, c_void_p) else int(this)
    vtbl = c_void_p.from_address(addr).value
    fp = c_void_p.from_address(vtbl + idx * _PTR_SZ).value
    return ctypes.WINFUNCTYPE(ret, c_void_p, *argtypes)(fp)(addr, *args)


def _qi(obj, iid):
    # QueryInterface → c_void_p on success, None on failure.
    out = c_void_p()
    hr = _vc(obj, 0, HRESULT, (POINTER(GUID), POINTER(c_void_p)),
             byref(iid), byref(out))
    return out if hr >= 0 else None


def _release(obj):
    # IUnknown::Release — safe for None / null pointer.
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
_D3D11_USAGE_DEFAULT = 0
_D3D11_USAGE_STAGING = 3
_D3D11_BIND_RENDER_TARGET = 0x20
_D3D11_CPU_ACCESS_WRITE = 0x10000
_D3D11_MAP_WRITE = 2

# DXGI present status / error codes (unsigned HRESULT form). A monitor
# powering off (DPMS) usually surfaces as the benign OCCLUDED *success*
# status — the present simply didn't reach the panel; the device is
# fine. A GPU TDR / driver re-init (which a display-mode switch or a
# power transition can trigger) surfaces as one of the DEVICE_* errors,
# after which every D3D11/DXGI COM pointer we hold is dangling.
_DXGI_STATUS_OCCLUDED = 0x087A0001
_DXGI_ERROR_DEVICE_REMOVED = 0x887A0005
_DXGI_ERROR_DEVICE_HUNG = 0x887A0006
_DXGI_ERROR_DEVICE_RESET = 0x887A0007
_DEVICE_LOST_CODES = frozenset((
    _DXGI_ERROR_DEVICE_REMOVED,
    _DXGI_ERROR_DEVICE_HUNG,
    _DXGI_ERROR_DEVICE_RESET,
))

# ── COM vtable indices ──────────────────────────────────────────
# Counted from the C++ header declaration order (IUnknown = 0‑2).

_IDXGIObject_GetParent = 6
_IDXGIDevice_GetAdapter = 7
_IDXGIFactory2_CreateSwapChainForComposition = 24
_IDXGISwapChain_Present = 8
_IDXGISwapChain_GetBuffer = 9
_IDXGISwapChain_ResizeBuffers = 13
_ID3D11Device_CreateTexture2D = 5
_ID3D11Device_OpenSharedResource = 28
_ID3D11Device_GetDeviceRemovedReason = 39
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


# Zero-copy pointer to bytes internal buffer. Deliberately NOT
# ctypes.pythonapi.PyBytes_AsString: with a statically linked CPython
# runtime (Nuitka standalone) the host EXE exports no Python C API
# symbols, so any pythonapi attribute lookup raises AttributeError at
# import time. c_char_p conversion of a bytes object references the
# same internal buffer (no copy) through plain ctypes machinery.
def _bytes_ptr(b: bytes) -> int:
    return ctypes.cast(ctypes.c_char_p(b), c_void_p).value

# ── WGL_NV_DX_interop2 bridge (Part A: GPU-to-GPU present) ──────
#
# Lets a D3D11 texture be registered as a GL texture so the compositor
# can render directly into D3D11-visible memory and CopyResource it
# into the swapchain backbuffer, skipping glReadPixels + memmove
# entirely. Every function pointer here is resolved lazily via
# wglGetProcAddress (same pattern as overlay_host.py's
# wglChoosePixelFormatARB bootstrap) and cached at module scope —
# resolution only needs to happen once per process since these are
# process-wide DLL/driver entry points, not per-context state.

_opengl32 = ctypes.windll.opengl32

GLenum = c_uint
GLuint = c_uint
GLint = c_int
GLsizei = c_int

GL_TEXTURE_2D = 0x0DE1
GL_FRAMEBUFFER = 0x8D40
GL_COLOR_ATTACHMENT0 = 0x8CE0
GL_FRAMEBUFFER_COMPLETE = 0x8CD5

WGL_ACCESS_READ_ONLY_NV = 0x0000
WGL_ACCESS_READ_WRITE_NV = 0x0001
WGL_ACCESS_WRITE_DISCARD_NV = 0x0002

_opengl32.glGenTextures.restype = None
_opengl32.glGenTextures.argtypes = [GLsizei, POINTER(GLuint)]
_opengl32.glBindTexture.restype = None
_opengl32.glBindTexture.argtypes = [GLenum, GLuint]
_opengl32.glDeleteTextures.restype = None
_opengl32.glDeleteTextures.argtypes = [GLsizei, POINTER(GLuint)]

_opengl32.wglGetCurrentDC.restype = wt.HDC
_opengl32.wglGetCurrentDC.argtypes = []

_wglGetProcAddress = _opengl32.wglGetProcAddress
_wglGetProcAddress.restype = c_void_p
_wglGetProcAddress.argtypes = [ctypes.c_char_p]

_gl_proc_cache: dict[str, Any] = {}


def _gl_proc(name: str, restype, argtypes):
    # Resolve a WGL/GL extension entry point via wglGetProcAddress.
    #
    # Returns None if the current thread has no current WGL context, or
    # the driver doesn't export *name* — callers must treat None as
    # "interop unavailable" and fall back to the existing CPU path.
    cached = _gl_proc_cache.get(name)
    if cached is not None:
        return cached
    addr = _wglGetProcAddress(name.encode('ascii'))
    if not addr:
        return None
    fn = ctypes.WINFUNCTYPE(restype, *argtypes)(addr)
    _gl_proc_cache[name] = fn
    return fn


def _wgl_dx_interop2_supported(hdc: int) -> bool:
    # Query WGL_NV_DX_interop2 support on the current WGL context.
    get_ext = _gl_proc('wglGetExtensionsStringARB', ctypes.c_char_p, [wt.HDC])
    if get_ext is None:
        return False
    try:
        raw = get_ext(hdc)
        ext_str = raw.decode('ascii', 'ignore') if raw else ''
    except Exception:
        return False
    return 'WGL_NV_DX_interop2' in ext_str.split()


class WglDxInterop:
    # Thin wrapper around the WGL_NV_DX_interop2 entry points.
    #
    # One instance owns exactly one interop-open D3D11 device handle.
    # All methods must be called with the target WGL context current on
    # the calling thread (same requirement as the rest of this module).

    def __init__(self) -> None:
        self._hdevice: c_void_p | None = None
        self._fns: dict[str, Any] = {}

    def _resolve(self) -> bool:
        sigs = {
            'wglDXOpenDeviceNV': (c_void_p, [c_void_p]),
            'wglDXCloseDeviceNV': (wt.BOOL, [c_void_p]),
            'wglDXRegisterObjectNV': (
                c_void_p, [c_void_p, c_void_p, GLuint, GLenum, GLenum]),
            'wglDXUnregisterObjectNV': (wt.BOOL, [c_void_p, c_void_p]),
            'wglDXLockObjectsNV': (wt.BOOL, [c_void_p, GLint, POINTER(c_void_p)]),
            'wglDXUnlockObjectsNV': (wt.BOOL, [c_void_p, GLint, POINTER(c_void_p)]),
        }
        for name, (restype, argtypes) in sigs.items():
            fn = _gl_proc(name, restype, argtypes)
            if fn is None:
                return False
            self._fns[name] = fn
        return True

    def open(self, hdc: int, d3d_device_ptr: int) -> bool:
        if not _wgl_dx_interop2_supported(hdc):
            return False
        if not self._resolve():
            return False
        h = self._fns['wglDXOpenDeviceNV'](c_void_p(d3d_device_ptr))
        if not h:
            return False
        self._hdevice = c_void_p(h)
        return True

    def close(self) -> None:
        if self._hdevice is not None:
            try:
                self._fns['wglDXCloseDeviceNV'](self._hdevice)
            except Exception:
                pass
            self._hdevice = None

    def register_texture(self, d3d_tex_ptr: int, gl_tex_id: int,
                          access: int = WGL_ACCESS_READ_WRITE_NV):
        if self._hdevice is None:
            return None
        h = self._fns['wglDXRegisterObjectNV'](
            self._hdevice, c_void_p(d3d_tex_ptr), GLuint(gl_tex_id),
            GLenum(GL_TEXTURE_2D), GLenum(access))
        return c_void_p(h) if h else None

    def unregister(self, handle: c_void_p) -> None:
        if self._hdevice is None or not handle:
            return
        try:
            self._fns['wglDXUnregisterObjectNV'](self._hdevice, handle)
        except Exception:
            pass

    def lock(self, handle: c_void_p) -> bool:
        if self._hdevice is None or not handle:
            return False
        arr = (c_void_p * 1)(handle)
        return bool(self._fns['wglDXLockObjectsNV'](self._hdevice, 1, arr))

    def unlock(self, handle: c_void_p) -> bool:
        if self._hdevice is None or not handle:
            return False
        arr = (c_void_p * 1)(handle)
        return bool(self._fns['wglDXUnlockObjectsNV'](self._hdevice, 1, arr))

    @property
    def is_open(self) -> bool:
        return self._hdevice is not None


def _resolve_gl_fbo_fns() -> dict[str, Any] | None:
    # Resolve the handful of GL_ARB_framebuffer_object entry points
    # needed to attach an interop-registered texture as a render target.
    # ``glGenTextures``/``glBindTexture``/``glDeleteTextures`` are GL 1.1
    # core and already available directly on ``_opengl32`` — only the
    # FBO functions need wglGetProcAddress (GL 3.0+ / ARB extension).
    sigs = {
        'glGenFramebuffers': (None, [GLsizei, POINTER(GLuint)]),
        'glDeleteFramebuffers': (None, [GLsizei, POINTER(GLuint)]),
        'glBindFramebuffer': (None, [GLenum, GLuint]),
        'glFramebufferTexture2D': (None, [GLenum, GLenum, GLenum, GLuint, GLint]),
        'glCheckFramebufferStatus': (GLenum, [GLenum]),
    }
    out: dict[str, Any] = {}
    for name, (restype, argtypes) in sigs.items():
        fn = _gl_proc(name, restype, argtypes)
        if fn is None:
            return None
        out[name] = fn
    return out


# ── Small public GL helpers (used by Part B external-texture layers) ──
#
# These wrap the same already-configured ``_opengl32`` entry points
# used internally above, exposed for ``overlay_compositor.py`` so it
# doesn't need to redeclare GL 1.1 signatures or re-resolve
# ``glActiveTexture`` itself.

GL_TEXTURE0 = 0x84C0
_glActiveTexture_fn: Any = None


def gl_gen_texture() -> int:
    # Allocate one GL texture name (glGenTextures). Returns 0 on failure.
    tid = GLuint(0)
    try:
        _opengl32.glGenTextures(1, byref(tid))
    except Exception:
        return 0
    return tid.value


def gl_delete_texture(tex_id: int) -> None:
    if not tex_id:
        return
    tid = GLuint(tex_id)
    try:
        _opengl32.glDeleteTextures(1, byref(tid))
    except Exception:
        pass


def gl_bind_texture_unit0(tex_id: int) -> None:
    # Bind *tex_id* as GL_TEXTURE_2D on texture unit 0 — matches the
    # ``location=0`` convention moderngl's ``Texture.use()`` follows, so
    # an externally-registered interop texture can be sampled by a
    # moderngl ``Program``/``VertexArray`` exactly like a moderngl-owned
    # texture, with no other state changes needed.
    global _glActiveTexture_fn
    if _glActiveTexture_fn is None:
        _glActiveTexture_fn = _gl_proc('glActiveTexture', None, [GLenum])
        if _glActiveTexture_fn is None:
            return
    _glActiveTexture_fn(GL_TEXTURE0)
    _opengl32.glBindTexture(GL_TEXTURE_2D, tex_id)


GL_TEXTURE_MIN_FILTER = 0x2801
GL_TEXTURE_MAG_FILTER = 0x2800
GL_LINEAR = 0x2601

_opengl32.glTexParameteri.restype = None
_opengl32.glTexParameteri.argtypes = [GLenum, GLenum, GLint]


def gl_set_bound_texture_linear() -> None:
    # Set non-mipmapped LINEAR filtering on the currently bound
    # GL_TEXTURE_2D. A raw ``glGenTextures`` name defaults to a
    # mipmapping min-filter; an interop-registered texture has exactly
    # one level, so without this the texture is INCOMPLETE and every
    # sample silently returns black.
    try:
        _opengl32.glTexParameteri(
            GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)
        _opengl32.glTexParameteri(
            GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR)
    except Exception:
        pass


def release_com(obj) -> None:
    # Public alias for the module's IUnknown::Release helper.
    _release(obj)


# ── IDXGIKeyedMutex (cross-process read/write exclusion) ────────
#
# Shared textures created with D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
# carry a GPU-side mutex: the producer holds key 0 while writing, the
# consumer holds it while sampling, so a reader can never observe a
# half-written texture (plain MISC_SHARED has no synchronization at
# all — the mid-copy overlap shows as horizontal tearing on fast-moving
# content). Whether a given texture has one is detected by QI, so
# plain-shared producers keep working unchanged.

_IDXGIKeyedMutex_AcquireSync = 8
_IDXGIKeyedMutex_ReleaseSync = 9


def open_keyed_mutex(tex):
    # QI IDXGIKeyedMutex from an opened shared texture.
    #
    # Returns a ``c_void_p`` the caller must ``release_com()``, or None
    # when the texture was created without the keyed-mutex flag.
    if tex is None or not tex.value:
        return None
    try:
        return _qi(tex, IID_IDXGIKeyedMutex)
    except Exception:
        return None


def keyed_mutex_acquire(km, key: int = 0, timeout_ms: int = 8) -> bool:
    # AcquireSync — True only on S_OK (WAIT_TIMEOUT is a positive
    # HRESULT). The producer holds the key only for one CopyResource, so
    # a short timeout only fires if the other side died mid-hold.
    try:
        hr = _vc(km, _IDXGIKeyedMutex_AcquireSync, HRESULT,
                 (ctypes.c_uint64, c_uint), key, timeout_ms)
        return hr == 0
    except Exception:
        return False


def keyed_mutex_release(km, key: int = 0) -> None:
    try:
        _vc(km, _IDXGIKeyedMutex_ReleaseSync, HRESULT,
            (ctypes.c_uint64,), key)
    except Exception:
        pass


# ── DCompBridge ─────────────────────────────────────────────────


class DCompBridge:
    # Present GL-rendered pixels via DirectComposition.
    #
    # All methods must be called from the compositor thread (same
    # thread that owns the WGL context).

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

        # ── GPU interop state (Part A) — all None/False until
        # enable_gl_interop() succeeds; every consumer must treat that
        # as "use the CPU present() path instead". ──
        self._interop: WglDxInterop | None = None
        self._gl_fbo_fns: dict[str, Any] | None = None
        self._gpu_tex: c_void_p | None = None        # D3D11 render target
        self._gpu_tex_gl_id: int = 0                  # GL texture name
        self._gpu_tex_dx_handle: c_void_p | None = None  # interop object handle
        self._gpu_fbo_id: int = 0                      # GL FBO wrapping the texture
        self._gpu_target_gen: int = 0  # bumped every render-target (re)create
        self._gl_interop_active = False

        try:
            self._init()
        except Exception:
            # _init() raising partway through still leaves every COM
            # object acquired UP TO that point holding a real refcount.
            # Nothing else will ever call destroy() on this now-half-
            # built, about-to-be-discarded instance (the caller catches
            # this same exception and drops the only reference — see
            # overlay_compositor._run()'s DComp init try/except setting
            # self._dcomp = None), so those COM objects (D3D11 device/
            # context, DXGI device/adapter/factory, possibly the
            # swapchain/DComp device/target/visual) would otherwise leak
            # for the rest of the process's life — destroy() itself
            # early-returns on `not self._alive`, which is exactly this
            # state, so it can't be relied on here. Release everything
            # directly instead; _release() already tolerates None/null.
            for obj in (self._staging, self._dc_vis, self._dc_tgt,
                        self._dc_dev, self._swap, self._dxgi_fac,
                        self._dxgi_adp, self._dxgi_dev,
                        self._d3d_ctx, self._d3d_dev):
                _release(obj)
            raise

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
        # Set Y-flip on the DComp visual so GL bottom-up rows display top-down.
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

    # ── GPU interop (Part A) ────────────────────────────────────────
    #
    # Registers a persistent D3D11 render-target texture as a GL
    # texture via WGL_NV_DX_interop2, so the compositor can render
    # directly into D3D11-visible memory and CopyResource it into the
    # swapchain backbuffer — no glReadPixels, no memmove. Every step
    # here is best-effort: any failure leaves gl_interop_active False
    # and the existing present()/present_partial() CPU path keeps
    # working unchanged.

    def enable_gl_interop(self, hdc: int) -> bool:
        if self._gl_interop_active:
            return True
        if not self._alive:
            return False
        try:
            fbo_fns = _resolve_gl_fbo_fns()
            if fbo_fns is None:
                return False
            interop = WglDxInterop()
            if not interop.open(hdc, self._d3d_dev.value):
                return False
            self._interop = interop
            self._gl_fbo_fns = fbo_fns
            if not self._create_gpu_render_target():
                self._teardown_gl_interop_state()
                return False
            self._gl_interop_active = True
            print(f'[DComp] GL interop enabled (WGL_NV_DX_interop2)', flush=True)
            return True
        except Exception as exc:
            print(f'[DComp] enable_gl_interop failed: {exc}', flush=True)
            self._teardown_gl_interop_state()
            return False

    def disable_gl_interop(self) -> None:
        if not self._gl_interop_active and self._interop is None:
            return
        self._teardown_gl_interop_state()
        print('[DComp] GL interop disabled — reverted to CPU present path',
              flush=True)

    def _create_gpu_render_target(self) -> bool:
        td = _D3D11_TEXTURE2D_DESC()
        td.Width = self._width
        td.Height = self._height
        td.MipLevels = 1
        td.ArraySize = 1
        td.Format = _DXGI_FORMAT_R8G8B8A8_UNORM
        td.SampleDesc.Count = 1
        td.SampleDesc.Quality = 0
        td.Usage = _D3D11_USAGE_DEFAULT
        td.BindFlags = _D3D11_BIND_RENDER_TARGET
        td.CPUAccessFlags = 0
        td.MiscFlags = 0
        tex = c_void_p()
        hr = _vc(self._d3d_dev, _ID3D11Device_CreateTexture2D, HRESULT,
                 (POINTER(_D3D11_TEXTURE2D_DESC), c_void_p, POINTER(c_void_p)),
                 byref(td), None, byref(tex))
        if hr < 0 or not tex.value:
            return False

        gl_id = GLuint(0)
        _opengl32.glGenTextures(1, byref(gl_id))
        if not gl_id.value:
            _release(tex)
            return False

        dx_handle = self._interop.register_texture(
            tex.value, gl_id.value, WGL_ACCESS_READ_WRITE_NV)
        if dx_handle is None:
            _opengl32.glDeleteTextures(1, byref(gl_id))
            _release(tex)
            return False

        fbo_id = GLuint(0)
        self._gl_fbo_fns['glGenFramebuffers'](1, byref(fbo_id))
        if not fbo_id.value:
            self._interop.unregister(dx_handle)
            _opengl32.glDeleteTextures(1, byref(gl_id))
            _release(tex)
            return False

        # Attach + verify completeness while the object is locked (the
        # interop object must be locked for GL to safely touch it).
        ok = False
        if self._interop.lock(dx_handle):
            try:
                self._gl_fbo_fns['glBindFramebuffer'](GL_FRAMEBUFFER, fbo_id.value)
                self._gl_fbo_fns['glFramebufferTexture2D'](
                    GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                    gl_id.value, 0)
                status = self._gl_fbo_fns['glCheckFramebufferStatus'](GL_FRAMEBUFFER)
                ok = (status == GL_FRAMEBUFFER_COMPLETE)
                self._gl_fbo_fns['glBindFramebuffer'](GL_FRAMEBUFFER, 0)
            finally:
                self._interop.unlock(dx_handle)

        if not ok:
            self._gl_fbo_fns['glDeleteFramebuffers'](1, byref(fbo_id))
            self._interop.unregister(dx_handle)
            _opengl32.glDeleteTextures(1, byref(gl_id))
            _release(tex)
            return False

        self._gpu_tex = tex
        self._gpu_tex_gl_id = gl_id.value
        self._gpu_tex_dx_handle = dx_handle
        self._gpu_fbo_id = fbo_id.value
        # GL id numbers get recycled by the driver, so a bare id compare
        # can't tell "same FBO" from "new FBO that reused the old id" —
        # consumers caching a moderngl detect_framebuffer() wrap (which
        # snapshots attachment size at detect time) key off this
        # generation instead.
        self._gpu_target_gen += 1
        return True

    def _destroy_gpu_render_target(self) -> None:
        if self._gpu_fbo_id and self._gl_fbo_fns is not None:
            fbo_id = GLuint(self._gpu_fbo_id)
            try:
                self._gl_fbo_fns['glDeleteFramebuffers'](1, byref(fbo_id))
            except Exception:
                pass
            self._gpu_fbo_id = 0
        if self._gpu_tex_dx_handle is not None and self._interop is not None:
            self._interop.unregister(self._gpu_tex_dx_handle)
            self._gpu_tex_dx_handle = None
        if self._gpu_tex_gl_id:
            gl_id = GLuint(self._gpu_tex_gl_id)
            try:
                _opengl32.glDeleteTextures(1, byref(gl_id))
            except Exception:
                pass
            self._gpu_tex_gl_id = 0
        if self._gpu_tex is not None:
            _release(self._gpu_tex)
            self._gpu_tex = None

    def _teardown_gl_interop_state(self) -> None:
        self._gl_interop_active = False
        self._destroy_gpu_render_target()
        if self._interop is not None:
            self._interop.close()
            self._interop = None
        self._gl_fbo_fns = None

    def render_to_gpu_texture_begin(self) -> bool:
        # Bind the interop GL FBO as the current render target.
        #
        # Caller must issue its GL draw calls, then call
        # ``render_to_gpu_texture_end()`` before touching D3D11 again.
        # Returns False (and leaves nothing bound) on any failure — the
        # caller must fall back to rendering to ``ctx.screen`` instead.
        if not self._gl_interop_active:
            return False
        # Proactive device-loss check BEFORE the interop lock, not
        # after — see device_removed()'s docstring for why the lock
        # itself can't be trusted to fail cleanly/promptly against an
        # already-dead device.
        if self.device_removed():
            return False
        if not self._interop.lock(self._gpu_tex_dx_handle):
            return False
        self._gl_fbo_fns['glBindFramebuffer'](GL_FRAMEBUFFER, self._gpu_fbo_id)
        return True

    def render_to_gpu_texture_end(self) -> None:
        if not self._gl_interop_active:
            return
        try:
            self._gl_fbo_fns['glBindFramebuffer'](GL_FRAMEBUFFER, 0)
        finally:
            self._interop.unlock(self._gpu_tex_dx_handle)

    def rebind_gpu_texture_target(self) -> None:
        # Re-bind the interop FBO without touching the lock.
        #
        # For callers that must temporarily bind a *different* FBO
        # in-between (e.g. a layer's own render-to-texture pass) while
        # still inside a ``render_to_gpu_texture_begin/end`` bracket —
        # the interop object stays locked for the whole bracket, only
        # the GL_FRAMEBUFFER binding changes.
        if not self._gl_interop_active:
            return
        self._gl_fbo_fns['glBindFramebuffer'](GL_FRAMEBUFFER, self._gpu_fbo_id)

    @property
    def gl_interop_active(self) -> bool:
        return self._gl_interop_active

    @property
    def gpu_fbo_id(self) -> int:
        # GL FBO name of the interop render target (0 if inactive).
        # Valid only between ``render_to_gpu_texture_begin/end`` for GL
        # use — the interop object must be locked while GL touches it.
        return self._gpu_fbo_id

    @property
    def gpu_target_generation(self) -> int:
        # Bumped whenever the interop render target is (re)created —
        # cache keys derived from ``gpu_fbo_id`` must include this.
        return self._gpu_target_gen

    # ── external (cross-process) shared textures — Part B ──────────
    #
    # Generic support for registering an *arbitrary* D3D11 legacy
    # shared-handle texture (created by another process, e.g. a
    # plugin's own worker) as a GL texture, reusing the same interop
    # device opened by enable_gl_interop(). This has no knowledge of
    # any particular plugin — it just opens/registers/locks whatever
    # handle it's given.

    def open_shared_texture(self, handle: int):
        # OpenSharedResource(handle) on this bridge's D3D11 device.
        #
        # *handle* is a legacy D3D11 shared handle value (from another
        # process's ``IDXGIResource::GetSharedHandle()``) — same-session
        # processes can open it directly, no DuplicateHandle needed.
        # Returns a ``c_void_p`` to the opened ``ID3D11Texture2D`` on
        # success, or ``None`` on failure. Caller owns the returned
        # reference and must ``release_com()`` it when done.
        if not self._alive or not handle:
            return None
        tex = c_void_p()
        hr = _vc(self._d3d_dev, _ID3D11Device_OpenSharedResource, HRESULT,
                 (c_void_p, POINTER(GUID), POINTER(c_void_p)),
                 c_void_p(handle), byref(IID_ID3D11Texture2D), byref(tex))
        if hr < 0 or not tex.value:
            return None
        return tex

    def register_external_texture(self, d3d_tex_ptr: int, gl_tex_id: int,
                                   access: int = WGL_ACCESS_READ_ONLY_NV):
        # Register an externally-opened D3D11 texture as a GL texture.
        #
        # Requires ``gl_interop_active`` (the interop device from
        # ``enable_gl_interop()`` is reused — legacy shared handles can
        # only be opened/registered against a device that is itself
        # interop-open). Returns the interop object handle for
        # lock/unlock, or ``None`` on failure.
        if not self._gl_interop_active or self._interop is None:
            return None
        return self._interop.register_texture(d3d_tex_ptr, gl_tex_id, access)

    def unregister_external_texture(self, dx_handle) -> None:
        if self._interop is not None:
            self._interop.unregister(dx_handle)

    def lock_external_texture(self, dx_handle) -> bool:
        if self._interop is None:
            return False
        return self._interop.lock(dx_handle)

    def unlock_external_texture(self, dx_handle) -> None:
        if self._interop is not None:
            self._interop.unlock(dx_handle)

    # ── per-frame ────────────────────────────────────────────────

    def _handle_device_loss(self, reason: str) -> None:
        # Put the bridge into a safe dead state after the D3D11 device
        # is confirmed gone. Shared by ``_note_present_hr`` (reactive —
        # after a Present() already failed) and ``device_removed()``
        # (proactive — checked before risking a call that has no failure
        # path at all, see that method's docstring).
        if self._alive:
            print(f'[DComp] device lost ({reason}) — reverting to '
                  f'SwapBuffers present path', flush=True)
        self._teardown_gl_interop_state()
        self._alive = False

    def device_removed(self) -> bool:
        # Cheap, always-non-blocking D3D11 device health probe.
        #
        # ``ID3D11Device::GetDeviceRemovedReason`` just reads a stored
        # flag — unlike a ``Present()`` call (which only surfaces a lost
        # device reactively, after already trying to submit a frame) or
        # ``wglDXLockObjectsNV`` (``WglDxInterop.lock``/``render_to_gpu_
        # texture_begin``/``lock_external_texture``): that call has NO
        # timeout parameter in the extension spec at all, and its
        # behavior against an already-lost D3D device is driver-defined
        # — observed live as a sustained CPU+GPU usage spike that reads
        # as a full hang ("开着桌宠挂机久了会突然卡死, 不用开菜单也会",
        # both on display sleep/wake AND with the display continuously
        # on — i.e. any GPU TDR, not just a power-state transition).
        # Call this BEFORE attempting the interop lock each frame so a
        # confirmed-dead device tears down and falls back to plain GL
        # rendering (``ctx.screen``, matching the present path's existing
        # ``swap_buffers`` fallback) instead of the render thread ever
        # reaching that unbounded call again this session.
        if self._d3d_dev is None:
            return False
        try:
            hr = _vc(self._d3d_dev, _ID3D11Device_GetDeviceRemovedReason,
                     HRESULT, ())
        except Exception:
            return False
        if (hr & 0xFFFFFFFF) in _DEVICE_LOST_CODES:
            self._handle_device_loss(f'0x{hr & 0xFFFFFFFF:08X}')
            return True
        return False

    def _note_present_hr(self, hr: int) -> bool:
        # Inspect a ``Present`` HRESULT and, on a lost device, put the
        # bridge into a safe dead state.
        #
        # A removed/reset/hung device means the display adapter re-
        # initialised — the D3D11 device, swapchain, and every COM
        # pointer derived from them are now dangling. Continuing to call
        # ``Map``/``GetBuffer``/``CopyResource``/``Present`` into them is
        # an access violation in native code that the Python ``try`` here
        # cannot catch (observed as the whole app crash-exiting when the
        # screen powers off / the GPU resets). Marking ``_alive = False``
        # makes every present method short-circuit at its opening guard,
        # so ``_present_frame`` falls back to plain ``SwapBuffers`` (which
        # stays valid — the WGL window context survives a mere monitor
        # power event) and the overlay keeps running instead of dying.
        #
        # ``DXGI_STATUS_OCCLUDED`` (a *positive* success code, so the
        # ``hr < 0`` guards elsewhere never see it) is NOT a loss — the
        # monitor is just off; the present was accepted but not shown. We
        # return False and keep presenting so the overlay is already
        # correct the instant the display comes back.
        code = hr & 0xFFFFFFFF
        if code in _DEVICE_LOST_CODES:
            self._handle_device_loss(f'0x{code:08X}')
            return True
        return False

    def present(self, pixels, width: int, height: int) -> bool:
        # Upload RGBA pixel data and present via DComp.
        #
        # *pixels*: ``bytes``, ``bytearray``, or ``int`` (raw pointer).
        # The DComp visual has a Y-flip transform, so rows are copied
        # straight (no reversal needed).  Returns True on success.
        if not self._alive:
            return False
        if width != self._width or height != self._height:
            self.resize(width, height)

        row_pitch = width * 4
        total = height * row_pitch

        if isinstance(pixels, bytes):
            if len(pixels) < total:
                return False
            src_ptr = _bytes_ptr(pixels)
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

            phr = _vc(self._swap, _IDXGISwapChain_Present, HRESULT,
                      (c_uint, c_uint), 0, 0)
            if self._note_present_hr(phr):
                return False
            _vc(self._dc_dev, _IDCompositionDevice_Commit, HRESULT, ())
            return True

        except Exception as exc:
            print(f'[DComp] present error: {exc}', flush=True)
            return False

    def present_partial(self, pixels, full_width: int, full_height: int,
                         gl_x: int, gl_y: int, w: int, h: int) -> bool:
        # Update only a sub-rectangle of the swapchain, then present.
        #
        # *pixels* is a tightly-packed ``w*h*4`` RGBA buffer in the same
        # bottom-up GL row order ``Framebuffer.read_into(viewport=...)``
        # produces — row 0 of *pixels* is GL row *gl_y*. ``(gl_x, gl_y)``
        # are GL viewport coordinates (bottom-left origin), matching the
        # viewport passed to that ``read_into`` call.
        #
        # The staging texture is never recreated between calls (only on
        # ``resize()``), so rows/columns outside this sub-rect keep
        # whatever was correctly written on their last actual update — a
        # D3D11 ``STAGING`` resource's CPU-visible memory persists across
        # ``Map``/``Unmap`` cycles (unlike a ``DYNAMIC`` resource mapped
        # with ``WRITE_DISCARD``, which may rename the allocation). This
        # is only safe if *full_width*/*full_height* still match the
        # current swapchain size — on any real resize the caller must use
        # ``present()`` for a full refresh first.
        if not self._alive:
            return False
        if full_width != self._width or full_height != self._height:
            return False
        if w <= 0 or h <= 0:
            return False
        if gl_x < 0 or gl_y < 0 or gl_x + w > full_width or gl_y + h > full_height:
            return False

        row_pitch = w * 4
        total = h * row_pitch

        if isinstance(pixels, bytes):
            if len(pixels) < total:
                return False
            src_ptr = _bytes_ptr(pixels)
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

            dp = mapped.RowPitch
            base = mapped.pData + gl_y * dp + gl_x * 4
            for row in range(h):
                ctypes.memmove(
                    base + row * dp,
                    src_ptr + row * row_pitch,
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

            phr = _vc(self._swap, _IDXGISwapChain_Present, HRESULT,
                      (c_uint, c_uint), 0, 0)
            if self._note_present_hr(phr):
                return False
            _vc(self._dc_dev, _IDCompositionDevice_Commit, HRESULT, ())
            return True

        except Exception as exc:
            print(f'[DComp] present_partial error: {exc}', flush=True)
            return False

    def present_gpu(self) -> bool:
        # Present the GPU render target (filled between a
        # ``render_to_gpu_texture_begin/end`` pair) with no CPU touch:
        # pure D3D11 ``CopyResource`` into the backbuffer + ``Present``.
        # Caller must have already unlocked the interop object (i.e.
        # call this after ``render_to_gpu_texture_end()``, not inside
        # the begin/end bracket).
        if not self._gl_interop_active or not self._alive:
            return False
        try:
            bb = c_void_p()
            hr = _vc(self._swap, _IDXGISwapChain_GetBuffer, HRESULT,
                     (c_uint, POINTER(GUID), POINTER(c_void_p)),
                     0, byref(IID_ID3D11Texture2D), byref(bb))
            if hr < 0:
                return False
            _vc(self._d3d_ctx, _ID3D11DeviceContext_CopyResource, None,
                (c_void_p, c_void_p), bb.value, self._gpu_tex.value)
            _release(bb)

            phr = _vc(self._swap, _IDXGISwapChain_Present, HRESULT,
                      (c_uint, c_uint), 0, 0)
            if self._note_present_hr(phr):
                return False
            _vc(self._dc_dev, _IDCompositionDevice_Commit, HRESULT, ())
            return True
        except Exception as exc:
            print(f'[DComp] present_gpu error: {exc}', flush=True)
            return False

    # ── resize ───────────────────────────────────────────────────

    def resize(self, width: int, height: int) -> None:
        if not self._alive or (width == self._width and height == self._height):
            return
        try:
            _release(self._staging)
            self._staging = None
            was_interop_active = self._gl_interop_active
            if was_interop_active:
                self._destroy_gpu_render_target()

            hr = _vc(self._swap, _IDXGISwapChain_ResizeBuffers, HRESULT,
                     (c_uint, c_uint, c_uint, c_uint, c_uint),
                     0, width, height, 0, 0)
            if hr < 0:
                print(f'[DComp] ResizeBuffers 0x{hr & 0xFFFFFFFF:08X}',
                      flush=True)
                self._alive = False
                self._gl_interop_active = False
                return

            self._width = width
            self._height = height
            self._create_staging()
            self._set_flip_transform()
            if was_interop_active:
                # Recreate the GPU render target at the new size; if this
                # fails, fall back to the CPU path rather than leaving a
                # stale/wrong-sized interop texture around.
                if not self._create_gpu_render_target():
                    self._teardown_gl_interop_state()
            _vc(self._dc_dev, _IDCompositionDevice_Commit, HRESULT, ())
            print(f'[DComp] resized → {width}x{height}', flush=True)
        except Exception as exc:
            print(f'[DComp] resize error: {exc}', flush=True)
            self._teardown_gl_interop_state()

    # ── teardown ─────────────────────────────────────────────────

    def destroy(self) -> None:
        if not self._alive:
            return
        self._teardown_gl_interop_state()
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
