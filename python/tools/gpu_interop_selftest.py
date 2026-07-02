# -*- coding: utf-8 -*-
"""GPU interop present-path selftest (Part A + Part B plumbing).

Verifies, on the real driver:
  1. WGL_NV_DX_interop2 availability + DCompBridge.enable_gl_interop().
  2. That GL draws issued the way overlay_compositor._render_frame
     issues them actually land in the interop D3D11 render target
     (regression test for moderngl framebuffer-state-tracking clashes
     with the raw glBindFramebuffer the bridge does).
  3. That a legacy shared-handle texture created on a *different* D3D11
     device (stand-in for an external producer process) can be opened +
     registered + sampled through the same code path CompositorLayer
     uses, and reports which way up it comes out.

Run:  python gpu_interop_selftest.py
Exit code 0 = all runnable checks passed (unsupported-driver skips are
not failures — the CPU fallback path covers those machines).
"""
from __future__ import annotations

import ctypes
import os
import sys
import struct

# runs from tools/ — the importable package root is python/ (parent)
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ctypes import POINTER, Structure, byref, c_uint, c_void_p

import moderngl

from render.overlay_host import OverlayHost
from render.dcomp_bridge import (
    DCompBridge, HRESULT, GUID, _guid, _vc, _release,
    _D3D11_TEXTURE2D_DESC, _D3D11_MAPPED_SUBRESOURCE, _DXGI_SAMPLE_DESC,
    _DXGI_FORMAT_R8G8B8A8_UNORM, _D3D11_USAGE_DEFAULT, _D3D11_USAGE_STAGING,
    _D3D_DRIVER_TYPE_HARDWARE, _D3D11_SDK_VERSION,
    _D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    _ID3D11Device_CreateTexture2D, _ID3D11DeviceContext_Map,
    _ID3D11DeviceContext_Unmap, _ID3D11DeviceContext_CopyResource,
    _d3d11, gl_gen_texture, gl_delete_texture, gl_bind_texture_unit0,
    release_com,
)

_D3D11_CPU_ACCESS_READ = 0x20000
_D3D11_MAP_READ = 1
_D3D11_BIND_SHADER_RESOURCE = 0x8
_D3D11_RESOURCE_MISC_SHARED = 0x2
_IDXGIResource_GetSharedHandle = 8
IID_IDXGIResource = _guid('035f3ab4-482e-4e50-b41f-8a7f8bd8960b')


class _D3D11_SUBRESOURCE_DATA(Structure):
    _fields_ = [
        ('pSysMem', c_void_p),
        ('SysMemPitch', c_uint),
        ('SysMemSlicePitch', c_uint),
    ]


def read_d3d_texture(dev, dctx, tex, w, h):
    """CopyResource → staging → Map(READ) → bytes (RGBA rows, top-down)."""
    td = _D3D11_TEXTURE2D_DESC()
    td.Width = w
    td.Height = h
    td.MipLevels = 1
    td.ArraySize = 1
    td.Format = _DXGI_FORMAT_R8G8B8A8_UNORM
    td.SampleDesc = _DXGI_SAMPLE_DESC(1, 0)
    td.Usage = _D3D11_USAGE_STAGING
    td.BindFlags = 0
    td.CPUAccessFlags = _D3D11_CPU_ACCESS_READ
    td.MiscFlags = 0
    stg = c_void_p()
    hr = _vc(dev, _ID3D11Device_CreateTexture2D, HRESULT,
             (POINTER(_D3D11_TEXTURE2D_DESC), c_void_p, POINTER(c_void_p)),
             byref(td), None, byref(stg))
    if hr < 0 or not stg.value:
        raise OSError(f'staging CreateTexture2D 0x{hr & 0xFFFFFFFF:08X}')
    try:
        _vc(dctx, _ID3D11DeviceContext_CopyResource, None,
            (c_void_p, c_void_p), stg.value, tex.value)
        m = _D3D11_MAPPED_SUBRESOURCE()
        hr = _vc(dctx, _ID3D11DeviceContext_Map, HRESULT,
                 (c_void_p, c_uint, c_uint, c_uint,
                  POINTER(_D3D11_MAPPED_SUBRESOURCE)),
                 stg.value, 0, _D3D11_MAP_READ, 0, byref(m))
        if hr < 0:
            raise OSError(f'staging Map 0x{hr & 0xFFFFFFFF:08X}')
        try:
            raw = ctypes.string_at(m.pData, m.RowPitch * h)
            pitch = m.RowPitch
        finally:
            _vc(dctx, _ID3D11DeviceContext_Unmap, None,
                (c_void_p, c_uint), stg.value, 0)
    finally:
        _release(stg)
    return raw, pitch


def px(raw, pitch, x, y):
    off = y * pitch + x * 4
    return tuple(raw[off:off + 4])


def make_producer_device():
    dev = c_void_p()
    dctx = c_void_p()
    lvl = ctypes.c_int(0)
    hr = _d3d11.D3D11CreateDevice(
        None, _D3D_DRIVER_TYPE_HARDWARE, None,
        _D3D11_CREATE_DEVICE_BGRA_SUPPORT, None, 0, _D3D11_SDK_VERSION,
        byref(dev), byref(lvl), byref(dctx))
    if hr < 0:
        raise OSError(f'producer D3D11CreateDevice 0x{hr & 0xFFFFFFFF:08X}')
    return dev, dctx


def make_shared_pattern_texture(dev, w, h):
    """Create a MISC_SHARED texture: top-left quadrant red, rest blue
    (opaque). Row 0 of the initial data = D3D row 0 = TOP row."""
    buf = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            off = (y * w + x) * 4
            if x < w // 2 and y < h // 2:
                buf[off:off + 4] = b'\xff\x00\x00\xff'   # red RGBA
            else:
                buf[off:off + 4] = b'\x00\x00\xff\xff'   # blue
    cbuf = (ctypes.c_ubyte * len(buf)).from_buffer(buf)
    init = _D3D11_SUBRESOURCE_DATA(
        ctypes.addressof(cbuf), w * 4, 0)

    td = _D3D11_TEXTURE2D_DESC()
    td.Width = w
    td.Height = h
    td.MipLevels = 1
    td.ArraySize = 1
    td.Format = _DXGI_FORMAT_R8G8B8A8_UNORM
    td.SampleDesc = _DXGI_SAMPLE_DESC(1, 0)
    td.Usage = _D3D11_USAGE_DEFAULT
    td.BindFlags = _D3D11_BIND_SHADER_RESOURCE
    td.CPUAccessFlags = 0
    td.MiscFlags = _D3D11_RESOURCE_MISC_SHARED
    tex = c_void_p()
    hr = _vc(dev, _ID3D11Device_CreateTexture2D, HRESULT,
             (POINTER(_D3D11_TEXTURE2D_DESC),
              POINTER(_D3D11_SUBRESOURCE_DATA), POINTER(c_void_p)),
             byref(td), byref(init), byref(tex))
    if hr < 0 or not tex.value:
        raise OSError(f'shared CreateTexture2D 0x{hr & 0xFFFFFFFF:08X}')

    out = c_void_p()
    hr = _vc(tex, 0, HRESULT, (POINTER(GUID), POINTER(c_void_p)),
             byref(IID_IDXGIResource), byref(out))
    if hr < 0 or not out.value:
        _release(tex)
        raise OSError('QI IDXGIResource failed')
    handle = c_void_p()
    hr = _vc(out, _IDXGIResource_GetSharedHandle, HRESULT,
             (POINTER(c_void_p),), byref(handle))
    _release(out)
    if hr < 0 or not handle.value:
        _release(tex)
        raise OSError(f'GetSharedHandle 0x{hr & 0xFFFFFFFF:08X}')
    return tex, handle.value


VERT = '''
#version 330
in vec2 in_pos;
in vec2 in_uv;
out vec2 v_uv;
void main() {
    gl_Position = vec4(in_pos * 2.0 - 1.0, 0.0, 1.0);
    v_uv = in_uv;
}
'''
FRAG_SOLID = '''
#version 330
uniform vec4 u_color;
out vec4 fragColor;
void main() { fragColor = u_color; }
'''
FRAG_TEX = '''
#version 330
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 fragColor;
void main() { fragColor = texture(u_tex, v_uv); }
'''


def main() -> int:
    results = []

    def report(name, ok, detail=''):
        results.append((name, ok))
        print(f'  [{"PASS" if ok else "FAIL"}] {name}'
              + (f' — {detail}' if detail else ''), flush=True)

    print('[1] host + bridge init', flush=True)
    host = OverlayHost()
    host.create()
    ctx = host.ctx
    dc = DCompBridge(host.hwnd, host.width, host.height)
    sw, sh = host.width, host.height

    enabled = dc.enable_gl_interop(host.hdc)
    print(f'  interop enabled: {enabled}', flush=True)
    if not enabled:
        print('  driver has no WGL_NV_DX_interop2 — CPU fallback path '
              'covers this machine; nothing further to verify here.')
        dc.destroy()
        host.destroy()
        return 0

    prog = ctx.program(vertex_shader=VERT, fragment_shader=FRAG_SOLID)
    vbo = ctx.buffer(struct.pack(
        '16f',
        0.0, 0.0, 0.0, 0.0,
        1.0, 0.0, 1.0, 0.0,
        0.0, 1.0, 0.0, 1.0,
        1.0, 1.0, 1.0, 1.0,
    ))
    vao = ctx.vertex_array(prog, [(vbo, '2f 2x4', 'in_pos')])

    # ── check 2: raw-bind sequence must be assumed broken ──
    # (documents WHY _render_frame binds through a moderngl wrap: a raw
    # glBindFramebuffer behind moderngl's back is undone by the next
    # ctx.clear(), which rebinds moderngl's own tracked framebuffer.
    # This check is informational — either outcome is reported, only
    # the wrap-based checks below are correctness gates.)
    print('[2] raw-bind sequence (informational)', flush=True)
    assert dc.render_to_gpu_texture_begin()
    ctx.viewport = (0, 0, sw, sh)
    ctx.clear(0.0, 0.0, 0.0, 0.0)
    prog['u_color'].value = (1.0, 0.0, 0.0, 1.0)
    vao.render(moderngl.TRIANGLE_STRIP)
    dc.render_to_gpu_texture_end()
    ctypes.windll.opengl32.glFinish()
    raw, pitch = read_d3d_texture(dc._d3d_dev, dc._d3d_ctx, dc._gpu_tex,
                                  sw, sh)
    center = px(raw, pitch, sw // 2, sh // 2)
    print(f'  raw-bind draw landed in gpu_tex: {center[:3] == (255, 0, 0)}'
          f' (center={center}) — expected False on stock moderngl',
          flush=True)

    # ── check 3: the sequence _render_frame actually uses now ──
    # detect_framebuffer wrap + a mid-frame private-FBO pass (emulating
    # a render_fn layer) + re-use of the wrap.
    print('[3] wrap-based render, with mid-frame private FBO pass',
          flush=True)
    layer_tex = ctx.texture((64, 64), 4)
    layer_fbo = ctx.framebuffer(color_attachments=[layer_tex])
    assert dc.render_to_gpu_texture_begin()
    try:
        wrapped = ctx.detect_framebuffer(dc._gpu_fbo_id)
        wrapped.use()
        ctx.viewport = (0, 0, sw, sh)
        ctx.clear(0.0, 0.0, 0.0, 0.0)
        # render_fn-style detour into a private FBO...
        layer_fbo.use()
        ctx.viewport = (0, 0, 64, 64)
        ctx.clear(0.0, 0.0, 0.0, 0.0)
        # ...and back to the interop target, same as _render_frame
        wrapped.use()
        ctx.viewport = (0, 0, sw, sh)
        prog['u_color'].value = (0.0, 1.0, 0.0, 1.0)
        vao.render(moderngl.TRIANGLE_STRIP)
    finally:
        dc.render_to_gpu_texture_end()
    ctypes.windll.opengl32.glFinish()

    raw, pitch = read_d3d_texture(dc._d3d_dev, dc._d3d_ctx, dc._gpu_tex,
                                  sw, sh)
    center = px(raw, pitch, sw // 2, sh // 2)
    report('wrap-based draw (with FBO detour) lands in gpu_tex',
           center[:3] == (0, 255, 0), f'center={center}')
    layer_fbo.release()
    layer_tex.release()

    # ── check 4: cross-device legacy shared texture open+register ──
    print('[4] external shared texture (producer on a second device)',
          flush=True)
    pdev, pctx = make_producer_device()
    TW = TH = 64
    ptex, handle = make_shared_pattern_texture(pdev, TW, TH)
    # Writer-side Flush is mandatory with legacy shared handles — the
    # texture's initial-data upload sits in the producer's deferred
    # command stream until then, and the consumer device has no way to
    # wait on it (no keyed mutex on MISC_SHARED resources).
    _ID3D11DeviceContext_Flush = 111
    _vc(pctx, _ID3D11DeviceContext_Flush, None, ())
    print(f'  producer shared handle=0x{handle:X} (flushed)', flush=True)

    opened = dc.open_shared_texture(handle)
    report('OpenSharedResource on bridge device', opened is not None)
    hobj = None
    gl_id = 0
    if opened is not None:
        gl_id = gl_gen_texture()
        hobj = dc.register_external_texture(opened.value, gl_id)
        report('wglDXRegisterObjectNV on opened shared texture',
               hobj is not None)

    if hobj is not None:
        prog_tex = ctx.program(vertex_shader=VERT, fragment_shader=FRAG_TEX)
        vao_tex = ctx.vertex_array(
            prog_tex, [(vbo, '2f 2f', 'in_pos', 'in_uv')])
        # A raw glGenTextures id has mipmapping min-filter defaults; a
        # registered interop texture has exactly one level, so without
        # explicit filters it is INCOMPLETE and samples black.
        GL_TEXTURE_2D = 0x0DE1
        GL_TEXTURE_MIN_FILTER = 0x2801
        GL_TEXTURE_MAG_FILTER = 0x2800
        GL_LINEAR = 0x2601
        _gl = ctypes.windll.opengl32
        _gl.glTexParameteri.restype = None
        _gl.glTexParameteri.argtypes = [c_uint, c_uint, ctypes.c_int]
        assert dc.render_to_gpu_texture_begin()
        try:
            wrapped = ctx.detect_framebuffer(dc._gpu_fbo_id)
            wrapped.use()
            ctx.viewport = (0, 0, sw, sh)
            ctx.clear(0.0, 0.0, 0.0, 0.0)
            locked = dc.lock_external_texture(hobj)
            print(f'  lock_external_texture: {locked}', flush=True)
            if locked:
                try:
                    gl_bind_texture_unit0(gl_id)
                    _gl.glTexParameteri(
                        GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)
                    _gl.glTexParameteri(
                        GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR)
                    prog_tex['u_tex'].value = 0
                    vao_tex.render(moderngl.TRIANGLE_STRIP)
                finally:
                    dc.unlock_external_texture(hobj)
        finally:
            dc.render_to_gpu_texture_end()
        ctypes.windll.opengl32.glFinish()

        raw, pitch = read_d3d_texture(dc._d3d_dev, dc._d3d_ctx,
                                      dc._gpu_tex, sw, sh)
        # gpu_tex D3D row 0 = image top (DComp flip transform handles
        # display). The quad maps UV(0,0) to GL bottom-left. Sample the
        # four quadrant centers of the presented image:
        q_tl = px(raw, pitch, sw // 4, sh // 4)
        q_bl = px(raw, pitch, sw // 4, 3 * sh // 4)
        red_at_tl = q_tl[:3] == (255, 0, 0)
        red_at_bl = q_bl[:3] == (255, 0, 0)
        report('external texture sampled (some quadrant red)',
               red_at_tl or red_at_bl,
               f'tl={q_tl} bl={q_bl}')
        print(f'  orientation: producer top-left quadrant shows at '
              f'{"TOP-left (no flip needed w/ bgra-style UV)" if red_at_tl else ""}'
              f'{"BOTTOM-left (needs flipped UV)" if red_at_bl else ""}',
              flush=True)

        dc.unregister_external_texture(hobj)
    if gl_id:
        gl_delete_texture(gl_id)
    if opened is not None:
        release_com(opened)
    _release(ptex)
    _release(pctx)
    _release(pdev)

    dc.destroy()
    host.destroy()

    failed = [n for n, ok in results if not ok]
    print(f'\n{len(results) - len(failed)}/{len(results)} checks passed',
          flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
