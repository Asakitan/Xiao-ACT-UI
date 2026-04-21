"""
Spike: GLFW borderless transparent click-through always-on-top overlay window
on Windows + moderngl context, render an animated SDF beam, and verify mouse
input passes through to underlying windows (game).

This is the proposed replacement for Tk Toplevel + UpdateLayeredWindow ULW
path used by SkillFX / HP / BOSSHP overlays.

Run:
    cd e:/VC/SAO-UI/sao_auto/tools && e:/Py/python.exe spike_overlay_window.py

Press ESC or close to exit. Move mouse over the window; clicks should reach
whatever is under it (NOT the overlay).
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes
import sys
import time

import glfw
import moderngl
import numpy as np


# ── Win32 constants ─────────────────────────────────────────────────────────
GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008
WS_EX_NOACTIVATE = 0x08000000

LWA_ALPHA = 0x00000002
LWA_COLORKEY = 0x00000001

HWND_TOPMOST = -1
SWP_NOMOVE = 0x0002
SWP_NOSIZE = 0x0001
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040

user32 = ctypes.WinDLL('user32', use_last_error=True)
user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
user32.SetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
user32.SetLayeredWindowAttributes.restype = wintypes.BOOL
user32.SetLayeredWindowAttributes.argtypes = [wintypes.HWND, wintypes.COLORREF,
                                              wintypes.BYTE, wintypes.DWORD]
user32.SetWindowPos.restype = wintypes.BOOL
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                wintypes.UINT]


def make_window_overlay(hwnd: int) -> None:
    """Apply layered + transparent + toolwindow ex-styles for click-through."""
    cur = user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
    new = (cur | WS_EX_LAYERED | WS_EX_TRANSPARENT
           | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE)
    user32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new)
    # Per-pixel alpha: pass alpha=255 so DWM uses framebuffer alpha channel
    user32.SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)
    user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)


# ── Shaders ─────────────────────────────────────────────────────────────────
VS = """
#version 330
in vec2 in_pos;
out vec2 v_uv;
void main() {
    v_uv = in_pos * 0.5 + 0.5;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
"""

# SDF beam — cyan→gold gradient with pulse, premultiplied alpha output
FS = """
#version 330
in vec2 v_uv;
out vec4 fragColor;
uniform float u_time;
uniform vec2 u_resolution;

float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

void main() {
    vec2 p = v_uv * u_resolution;
    vec2 a = vec2(u_resolution.x * 0.2, u_resolution.y * 0.5);
    vec2 b = vec2(u_resolution.x * 0.8, u_resolution.y * 0.5);
    float d = sdSegment(p, a, b);

    float pulse = 0.5 + 0.5 * sin(u_time * 4.0);
    float thickness = 6.0 + 4.0 * pulse;
    float aa = 2.0;
    float core  = 1.0 - smoothstep(thickness - aa, thickness + aa, d);
    float glow  = exp(-d * 0.04) * 0.6;

    float t = clamp(v_uv.x, 0.0, 1.0);
    vec3 cyan = vec3(0.0, 0.86, 1.0);
    vec3 gold = vec3(1.0, 0.85, 0.25);
    vec3 col  = mix(cyan, gold, t);

    float alpha = clamp(core + glow, 0.0, 1.0);
    // Premultiplied alpha for DWM correct compositing
    fragColor = vec4(col * alpha, alpha);
}
"""


def main() -> int:
    if not glfw.init():
        print('[spike] glfw init failed', file=sys.stderr)
        return 1

    glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 3)
    glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 3)
    glfw.window_hint(glfw.OPENGL_PROFILE, glfw.OPENGL_CORE_PROFILE)
    glfw.window_hint(glfw.DECORATED, glfw.FALSE)
    glfw.window_hint(glfw.TRANSPARENT_FRAMEBUFFER, glfw.TRUE)
    glfw.window_hint(glfw.FLOATING, glfw.TRUE)
    glfw.window_hint(glfw.RESIZABLE, glfw.FALSE)
    glfw.window_hint(glfw.SAMPLES, 0)
    glfw.window_hint(glfw.MOUSE_PASSTHROUGH, glfw.TRUE)  # GLFW 3.4 native click-through

    W, H = 800, 200
    win = glfw.create_window(W, H, 'SkillFX Spike', None, None)
    if not win:
        print('[spike] window create failed', file=sys.stderr)
        glfw.terminate()
        return 1
    glfw.set_window_pos(win, 200, 200)
    glfw.make_context_current(win)
    glfw.swap_interval(1)

    # Apply Win32 click-through belt-and-suspenders (in case MOUSE_PASSTHROUGH
    # not honored by some drivers/DWM configs)
    hwnd = glfw.get_win32_window(win)
    make_window_overlay(hwnd)

    ctx = moderngl.create_context()
    print(f'[spike] GL_VENDOR  : {ctx.info["GL_VENDOR"]}')
    print(f'[spike] GL_RENDERER: {ctx.info["GL_RENDERER"]}')
    print(f'[spike] GL_VERSION : {ctx.info["GL_VERSION"]}')

    prog = ctx.program(vertex_shader=VS, fragment_shader=FS)
    prog['u_resolution'].value = (W, H)
    quad = np.array([-1, -1, 1, -1, -1, 1, 1, 1], dtype='f4')
    vbo = ctx.buffer(quad.tobytes())
    vao = ctx.vertex_array(prog, [(vbo, '2f', 'in_pos')])

    ctx.enable(moderngl.BLEND)
    # Already-premultiplied alpha output → ONE, ONE_MINUS_SRC_ALPHA
    ctx.blend_func = (moderngl.ONE, moderngl.ONE_MINUS_SRC_ALPHA)

    t0 = time.perf_counter()
    frames = 0
    last_log = t0
    print('[spike] window up — move mouse over it; clicks should pass through.')
    print('[spike] press ESC to exit.')

    while not glfw.window_should_close(win):
        glfw.poll_events()
        if glfw.get_key(win, glfw.KEY_ESCAPE) == glfw.PRESS:
            break

        ctx.screen.use()
        ctx.clear(0.0, 0.0, 0.0, 0.0)
        prog['u_time'].value = time.perf_counter() - t0
        vao.render(moderngl.TRIANGLE_STRIP)
        glfw.swap_buffers(win)

        frames += 1
        now = time.perf_counter()
        if now - last_log >= 2.0:
            fps = frames / (now - last_log)
            print(f'[spike] {fps:5.1f} fps')
            frames = 0
            last_log = now

        if now - t0 > 12.0:  # auto-exit after 12s if not killed
            break

    vao.release()
    vbo.release()
    prog.release()
    ctx.release()
    glfw.destroy_window(win)
    glfw.terminate()
    print('[spike] OK — borderless transparent click-through overlay verified')
    return 0


if __name__ == '__main__':
    sys.exit(main())
