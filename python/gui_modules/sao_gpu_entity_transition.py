# -*- coding: utf-8 -*-
# Direct-GPU Entity entry/exit transition overlay.

from __future__ import annotations

import time
from typing import Any, Optional, Tuple


_VS = """
#version 330
out vec2 v_uv;
vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() {
    vec2 p = pos[gl_VertexID];
    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
"""


_FS = """
#version 330
in vec2 v_uv;
out vec4 fragColor;
uniform vec2 u_resolution;
uniform vec2 u_center;
uniform vec2 u_target;
uniform float u_progress;
uniform float u_time;
uniform int u_kind;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float band(float x, float c, float w) {
    return exp(-pow((x - c) / max(0.0001, w), 2.0));
}

float lineGrid(vec2 p, vec2 dir, float scale, float width) {
    float v = abs(fract(dot(p, dir) * scale) - 0.5);
    return 1.0 - smoothstep(width, width + 0.018, v);
}

float easeOut(float x) {
    return 1.0 - pow(1.0 - clamp(x, 0.0, 1.0), 3.0);
}

float easeInOut(float x) {
    x = clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

vec4 entryFx(vec2 uv, vec2 p, vec2 c0, vec2 c1, float t) {
    float deploy = easeInOut(smoothstep(0.10, 0.88, t));
    vec2 center = mix(c0, c1, deploy);
    vec2 q = uv - center;
    q.x *= u_resolution.x / max(1.0, u_resolution.y);
    float r = length(q);
    float ang = atan(q.y, q.x);

    float ignition = 1.0 - smoothstep(0.0, 0.24, t);
    float reveal = smoothstep(0.0, 0.22, t);
    float exitFade = 1.0 - smoothstep(0.70, 1.0, t);
    float ringR = mix(0.66, 0.050, deploy);
    float ringW = mix(0.028, 0.010, deploy);
    float ring = band(r, ringR, ringW);
    float ring2 = band(r, ringR + 0.055, ringW * 1.45) * (1.0 - deploy * 0.35);
    float core = exp(-r * mix(4.2, 38.0, deploy));
    float spoke = pow(abs(sin(ang * 4.0 + t * 9.5)), 18.0) * band(r, ringR, ringW * 3.0);
    float sweep = band(uv.y, 0.20 + t * 0.58, 0.036) + band(uv.y, 0.62 - t * 0.20, 0.050) * 0.55;

    vec2 d0 = normalize(vec2(1.0, 0.0));
    vec2 d1 = normalize(vec2(0.5, 0.8660254));
    vec2 d2 = normalize(vec2(-0.5, 0.8660254));
    float gridMask = band(r, ringR, ringW * 5.5);
    float grid = max(lineGrid(q, d0, 28.0, 0.025),
                 max(lineGrid(q, d1, 28.0, 0.025), lineGrid(q, d2, 28.0, 0.025))) * gridMask;
    float scan = 0.92 + 0.08 * sin(uv.y * u_resolution.y * 2.8 + u_time * 18.0);
    float noise = hash21(gl_FragCoord.xy * 0.035 + u_time) * 0.040;

    vec3 cyan = vec3(0.50, 0.92, 1.0);
    vec3 blue = vec3(0.06, 0.48, 1.0);
    vec3 gold = vec3(1.0, 0.74, 0.22);
    vec3 white = vec3(1.0);
    vec3 color = vec3(0.0);
    color += white * ignition * exp(-length(p) * 2.4) * 1.25;
    color += mix(blue, cyan, 0.50) * ring * 1.10;
    color += gold * ring2 * 0.58;
    color += cyan * core * (0.40 + ignition * 0.75);
    color += white * spoke * 0.75;
    color += mix(cyan, gold, 0.22) * grid * 0.52;
    color += vec3(0.74, 0.95, 1.0) * sweep * reveal * 0.22;
    color += vec3(noise) * reveal;
    color *= scan;
    color = clamp(color, 0.0, 1.0);
    float alpha = clamp((ring * 0.95 + ring2 * 0.50 + core * 0.70 + spoke * 0.45 + grid * 0.30 + sweep * 0.10 + ignition * 0.55) * exitFade, 0.0, 0.96);
    return vec4(color * alpha, alpha);
}

vec4 exitFx(vec2 uv, vec2 p, vec2 center, float t) {
    vec2 q = uv - center;
    q.x *= u_resolution.x / max(1.0, u_resolution.y);
    float r = length(q);
    float ang = atan(q.y, q.x);
    float lock = easeOut(smoothstep(0.00, 0.30, t));
    float purge = easeInOut(smoothstep(0.16, 1.0, t));
    float ringR = mix(0.035, 0.92, purge);
    float ringW = mix(0.085, 0.014, purge);
    float ring = band(r, ringR, ringW);
    float echo = band(r, max(0.0, ringR - 0.10), ringW * 2.0) * (1.0 - smoothstep(0.30, 0.80, t));
    float core = exp(-r * mix(48.0, 9.0, purge)) * (1.0 - smoothstep(0.18, 0.70, t));
    float arc = smoothstep(0.12, 1.0, 0.5 + 0.5 * sin(ang * 12.0 - t * 20.0));
    float beam = band(uv.y, center.y + (t - 0.5) * 0.42, 0.030) + band(uv.x, center.x, mix(0.010, 0.030, purge)) * 0.48;
    float closeV = smoothstep(0.82, 1.0, t);
    float closeH = mix(0.50, 0.006, closeV);
    float closeMask = 1.0 - smoothstep(closeH, closeH + 0.018, abs(uv.y - 0.5));
    float scan = 0.90 + 0.10 * sin(uv.y * u_resolution.y * 3.0 + u_time * 28.0);

    vec2 d0 = normalize(vec2(1.0, 0.0));
    vec2 d1 = normalize(vec2(0.5, 0.8660254));
    vec2 d2 = normalize(vec2(-0.5, 0.8660254));
    float gridMask = band(r, ringR - 0.018, ringW * 4.0);
    float grid = max(lineGrid(q, d0, mix(22.0, 36.0, purge), 0.028),
                 max(lineGrid(q, d1, mix(22.0, 36.0, purge), 0.026), lineGrid(q, d2, mix(22.0, 36.0, purge), 0.026))) * gridMask;

    vec3 cyan = vec3(0.56, 0.94, 1.0);
    vec3 blue = vec3(0.06, 0.44, 1.0);
    vec3 gold = vec3(1.0, 0.78, 0.26);
    vec3 white = vec3(1.0);
    vec3 color = vec3(0.0);
    color += mix(blue, cyan, 0.45) * ring * (0.70 + 0.30 * arc);
    color += cyan * echo * 0.72;
    color += gold * grid * 0.55;
    color += white * core * (0.50 + 0.50 * lock);
    color += vec3(0.72, 0.95, 1.0) * beam * 0.25;
    float tailFade = 1.0 - smoothstep(0.88, 1.0, t);
    color *= scan * mix(1.0, closeMask, closeV) * tailFade;
    color = clamp(color, 0.0, 1.0);
    float alpha = clamp((ring * 0.95 + echo * 0.45 + grid * 0.32 + core * 0.80 + beam * 0.16) * mix(1.0, closeMask, closeV) * tailFade, 0.0, 0.98);
    return vec4(color * alpha, alpha);
}

void main() {
    vec2 p = v_uv - 0.5;
    p.x *= u_resolution.x / max(1.0, u_resolution.y);
    vec2 c0 = u_center / u_resolution;
    vec2 c1 = u_target / u_resolution;
    float t = clamp(u_progress, 0.0, 1.0);
    if (u_kind == 0) {
        fragColor = entryFx(v_uv, p, c0, c1, t);
    } else {
        fragColor = exitFx(v_uv, p, c0, t);
    }
}
"""


class EntityTransitionGpuOverlay:
    # Small direct-present GPU transition used between LinkStart and Entity UI.

    def __init__(
        self,
        root: Any,
        *,
        kind: str,
        center: Tuple[float, float],
        target: Optional[Tuple[float, float]] = None,
        title: str = "sao_entity_transition_gpu",
    ) -> None:
        self.root = root
        self.kind = "exit" if kind == "exit" else "entry"
        self.center = (float(center[0]), float(center[1]))
        self.target = (float((target or center)[0]), float((target or center)[1]))
        self.title = title
        self.progress = 0.0
        self._win: Any = None
        self._prog: Any = None
        self._vao: Any = None
        self._started_at = time.perf_counter()
        self._w = 1
        self._h = 1

    def _to_gl_point(self, point: Tuple[float, float]) -> Tuple[float, float]:
        # Convert top-left screen coordinates to bottom-left GL pixels.
        return (float(point[0]), float(self._h) - float(point[1]))

    def start(self) -> bool:
        try:
            from render import gpu_overlay_window as _gow
            if not _gow.glfw_supported():
                return False
            self._w = int(self.root.winfo_screenwidth())
            self._h = int(self.root.winfo_screenheight())
            pump = _gow.get_glfw_pump(self.root)
            self._win = _gow.GpuOverlayWindow(
                pump,
                w=self._w,
                h=self._h,
                x=0,
                y=0,
                render_fn=self.render,
                click_through=True,
                title=self.title,
                vsync=True,
            )
            self._win.show()
            self._win.request_redraw()
            return True
        except Exception:
            self.destroy()
            return False

    def set_progress(self, progress: float) -> None:
        self.progress = max(0.0, min(1.0, float(progress)))
        try:
            if self._win is not None:
                self._win.request_redraw()
        except Exception:
            pass

    def render(self, ctx: Any, _pump_t: float) -> None:
        try:
            from render import gpu_overlay_window as _gow
            if self._prog is None:
                self._prog = ctx.program(vertex_shader=_VS, fragment_shader=_FS)
                self._vao = ctx.vertex_array(self._prog, [])
            self._prog["u_resolution"].value = (float(self._w), float(self._h))
            self._prog["u_center"].value = self._to_gl_point(self.center)
            self._prog["u_target"].value = self._to_gl_point(self.target)
            self._prog["u_progress"].value = float(self.progress)
            self._prog["u_time"].value = float(time.perf_counter() - self._started_at)
            self._prog["u_kind"].value = 1 if self.kind == "exit" else 0
            self._vao.render(mode=_gow._moderngl.TRIANGLES, vertices=3)
        except Exception:
            return

    def destroy(self) -> None:
        win = self._win
        self._win = None
        # GpuOverlayWindow owns the GL context and destroys it on the pump
        # thread. Do not release VAO/program objects here from Tk after that
        # context is gone; some drivers terminate the process instead of
        # raising a Python exception.
        self._vao = None
        self._prog = None
        if win is not None:
            try:
                win.destroy()
            except Exception:
                pass
