# -*- coding: utf-8 -*-
"""
SAOPlayerGUILinkAnimationMixin — seventeenth mixin extracted from
SAOPlayerGUI (round 65 of the sao_gui split refactor). 12 methods,
~851 lines.

The full-screen SAO link-start / link-end overlay animations —
the entry boot sequence shown when the app first appears + the
exit pulse animation shown when the user clicks exit. Both use a
moderngl standalone context to render at full resolution + a Tk
Toplevel with WS_EX_LAYERED to composite over the desktop.

Methods (in source order):
  * _play_link_start (68) — top-level entry animation entry point.
    Suspends GPU overlay creation, creates the entry Toplevel,
    runs the boot animation, then resumes overlays + animates the
    float button into its final position.
  * _init_entry_boot_gl (101) — moderngl context + shader program
    setup for the entry boot effect (vertex + fragment GLSL).
  * _draw_entry_boot_gl (31) — per-frame GPU draw call.
  * _create_entry_overlay (30) — Tk Toplevel construction for the
    entry-animation overlay (with WS_EX_LAYERED + alpha).
  * _draw_entry_overlay (107) — main-thread animation loop that
    drives the GPU draw + alpha fade + text reveal.
  * _run_entry_animation (66) — bridges the entry overlay into the
    actual app startup (delays + completion callback).
  * _init_exit_pulse_gl (107) — exit pulse moderngl setup (mirrors
    the entry boot pattern).
  * _draw_exit_pulse_gl (31) — exit GPU draw call.
  * _get_exit_banner (17) — picks the exit-overlay text from
    settings ("EXIT" / "SWITCH" / custom).
  * _create_exit_overlay (42) — Tk Toplevel for the exit overlay.
  * _draw_exit_overlay (141) — exit animation loop (longest in
    this cluster).
  * _collect_exit_windows (110) — enumerates every visible Tk
    Toplevel + ULW window the exit animation needs to fade.

Required SAOPlayerGUI attrs:
  * self.root, self._float, self._fw, self._fh, self.settings
  * self._exit_overlay, self._entry_overlay,
    self._exit_overlay_gl_state, self._entry_overlay_gl_state
  * self._after_shutdown

Required SAOPlayerGUI methods (via MRO):
  * _animate_float_to (FloatHp mixin)
  * _create_floating_widget (FloatHandlers mixin)
  * _start_float_breath (FloatHp mixin)
  * _get_setting (SAOPlayerGUI)
"""

from __future__ import annotations

import math
import time
import tkinter as tk
from typing import Any, Optional

import numpy as np
from PIL import Image, ImageTk

from utils.sao_sound import get_sao_font
from sao_theme import SAOLinkStart, ease_out, ease_in_out, lerp
from gui_modules.sao_gpu_entity_transition import EntityTransitionGpuOverlay
from gui_modules.sao_panel_ui import _disable_native_window_shadow


class SAOPlayerGUILinkAnimationMixin:
    """Mixin bundling SAO link-start / link-end full-screen animations."""

    def _play_link_start(self):
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        # 目标位置: 上次保存的位置, 否则右下角
        saved_x = self.settings.get('float_x', None)
        saved_y = self.settings.get('float_y', None)
        # 固定位置: 左下角覆盖整个底部区域 (统一 HUD)
        # 向右偏移 4% 屏宽以覆盖游戏原生 HP/STA 条
        _offset_pct = 0.04
        try:
            if hasattr(self, 'settings') and self.settings:
                _offset_pct = self.settings.get('hud_offset_x', 0.04)
        except Exception:
            pass
        _hp_x = int(sw * _offset_pct)
        _hp_y = sh - self._fh
        if saved_x is not None and saved_y is not None:
            fx_final = max(0, min(int(saved_x), sw - self._fw))
            fy_final = max(0, min(int(saved_y), sh - self._fh))
        else:
            fx_final = _hp_x
            fy_final = _hp_y
        # 起始位置: 屏幕正中央 (LinkStart 动画中心)
        fx_start = sw // 2 - self._fw // 2
        fy_start = sh // 2 + 80   # 略低于中心 (文字下方)

        try:
            from render.gpu_overlay_window import (
                suspend_gpu_overlay_creation as _suspend_gpu_overlays,
                resume_gpu_overlay_creation as _resume_gpu_overlays,
            )
        except Exception:
            _suspend_gpu_overlays = None  # type: ignore[assignment]
            _resume_gpu_overlays = None  # type: ignore[assignment]
        _gpu_overlays_suspended = False
        if _suspend_gpu_overlays is not None:
            try:
                # LinkStart now owns a direct GLFW/ModernGL presentation
                # window. Suspending new GPU overlays here prevents that
                # window from being created and causes the animation to be
                # skipped instead of rendered.
                _gpu_overlays_suspended = False
            except Exception:
                _gpu_overlays_suspended = False

        def _resume_overlay_creation():
            nonlocal _gpu_overlays_suspended
            if not _gpu_overlays_suspended or _resume_gpu_overlays is None:
                return
            _gpu_overlays_suspended = False
            try:
                _resume_gpu_overlays()
            except Exception:
                pass

        def on_done():
            _resume_overlay_creation()
            self._float.geometry(f'{self._fw}x{self._fh}+{fx_start}+{fy_start}')
            self._float.deiconify()
            self._float.lift()
            self._play_motion_blur(closing=False)
            self._run_entry_animation(fx_start, fy_start, fx_final, fy_final)

        # Canvas 渲染 (SAO-UI 隧道模型)
        try:
            ls = SAOLinkStart(self.root, on_done=on_done)
            ls.play()
        except Exception:
            _resume_overlay_creation()
            raise

    def _init_entry_boot_gl(self, width, height):
        try:
            import moderngl
        except Exception:
            return None
        try:
            ctx = moderngl.create_standalone_context()
            prog = ctx.program(
                vertex_shader='''
#version 330
out vec2 uv;
vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() {
    vec2 p = pos[gl_VertexID];
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
''',
                fragment_shader='''
#version 330
in vec2 uv;
uniform vec2 u_resolution;
uniform float u_progress;
out vec4 fragColor;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float band(float x, float center, float width) {
    return exp(-pow((x - center) / max(0.0001, width), 2.0));
}

void main() {
    vec2 p = uv - 0.5;
    p.x *= u_resolution.x / max(1.0, u_resolution.y);
    float r = length(p);
    float progress = clamp(u_progress, 0.0, 1.0);

    float openA = smoothstep(0.00, 0.22, progress);
    float openB = smoothstep(0.12, 0.56, progress);
    float settle = smoothstep(0.40, 1.00, progress);
    // barrel distortion strongest at ignition, flattens as screen opens
    float barrelK = mix(0.44, 0.0, smoothstep(0.0, 0.34, progress));
    vec2 bv = uv - 0.5;
    vec2 distUv = uv + bv * barrelK * dot(bv, bv);

    float halfH = mix(0.003, 0.50, openA);
    float halfW = mix(0.030, 0.62, openB);
    float maskY = 1.0 - smoothstep(halfH, halfH + 0.030, abs(distUv.y - 0.5));
    float maskX = 1.0 - smoothstep(halfW, halfW + 0.045, abs(distUv.x - 0.5));
    float screenMask = clamp(maskX * maskY, 0.0, 1.0);

    // overexposure flash: floods full frame at the moment the screen fires on
    float overexpose = smoothstep(0.0, 0.06, progress) * (1.0 - smoothstep(0.14, 0.40, progress));

    float ignition = band(uv.y, 0.5, mix(0.0016, 0.020, openA)) * (1.0 - smoothstep(0.18, 0.46, progress));
    float flare = exp(-r * mix(24.0, 6.5, openB)) * (0.45 + 0.55 * (1.0 - settle));
    float scan = 0.92 + 0.08 * sin((uv.y * u_resolution.y) * 2.8 + progress * 1100.0);
    float noise = hash21(gl_FragCoord.xy * 0.03 + progress * 17.0) * 0.05;
    float bezel = smoothstep(0.90, 0.18, max(abs(p.x) * 0.92, abs(p.y) * 1.25));
    float sweep = band(uv.y, 0.26 + progress * 0.48, 0.045) + band(uv.y, 0.60 + progress * 0.12, 0.060) * 0.55;

    vec3 cyan = vec3(0.52, 0.92, 1.0);
    vec3 blue = vec3(0.08, 0.46, 1.0);
    vec3 white = vec3(1.0, 1.0, 1.0);
    vec3 color = vec3(0.0);
    // full-frame overexposure bloom + cyan tint bleed at ignition
    color += white * overexpose * 2.60;
    color += vec3(0.70, 0.94, 1.0) * overexpose * exp(-r * 3.0) * 1.40;
    color += white * ignition * 1.6;
    color += mix(blue, cyan, 0.50) * flare * (0.65 + 0.35 * openB);
    color += cyan * screenMask * (0.18 + 0.24 * sweep + 0.18 * settle);
    color += white * screenMask * 0.10 * (1.0 - smoothstep(0.0, 0.4, r));
    color += vec3(0.78, 0.96, 1.0) * sweep * screenMask * 0.24;
    color *= scan * bezel;
    color += vec3(noise) * screenMask * 0.12;
    color = clamp(color, 0.0, 1.0);
    fragColor = vec4(color, 1.0);
}
''')
            vao = ctx.vertex_array(prog, [])
            tex = ctx.texture((width, height), 4)
            fbo = ctx.framebuffer(color_attachments=[tex])
            prog['u_resolution'].value = (float(width), float(height))
            return {
                'ctx': ctx,
                'boot_prog': prog,
                'boot_vao': vao,
                'boot_tex': tex,
                'boot_fbo': fbo,
            }
        except Exception:
            try:
                ctx.release()
            except Exception:
                pass
            return None

    def _draw_entry_boot_gl(self, cv, ov, progress):
        gl = ov.get('gl')
        if not gl:
            return False
        try:
            prog = gl['boot_prog']
            fbo = gl['boot_fbo']
            vao = gl['boot_vao']
            fbo.use()
            gl['ctx'].clear(0.0, 0.0, 0.0, 1.0)
            prog['u_progress'].value = float(max(0.0, min(1.0, progress)))
            vao.render()
            raw = fbo.read(components=4, alignment=1)
            _h, _w = ov['sh'], ov['sw']
            arr = np.frombuffer(raw, dtype=np.uint8).reshape(_h, _w, 4)
            pil_img = ov.get('_gl_pil_buf')
            if pil_img is None or pil_img.size != (_w, _h):
                pil_img = Image.fromarray(arr[::-1], 'RGBA')
                ov['_gl_pil_buf'] = pil_img
                photo = ImageTk.PhotoImage(pil_img)
                ov['gl_photo'] = photo
                ov['_gl_canvas_id'] = cv.create_image(0, 0, image=photo, anchor='nw')
            else:
                pil_img.frombytes(arr[::-1].tobytes())
                photo = ov['gl_photo']
                photo.paste(pil_img)
                # canvas item already exists — just update reference
            return True
        except Exception:
            return False

    def _create_entry_overlay(self, start_x, start_y, end_x, end_y):
        self._cleanup_entry_overlay()
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        start_center = (start_x + self._fw // 2, start_y + self._fh // 2)
        end_center = (end_x + self._fw // 2, end_y + self._fh // 2)
        try:
            gpu = EntityTransitionGpuOverlay(
                self.root,
                kind='entry',
                center=start_center,
                target=end_center,
                title='sao_entity_entry_gpu',
            )
            if gpu.start():
                self._entry_overlay = {
                    'gpu_transition': gpu,
                    'sw': sw,
                    'sh': sh,
                    'start_x': start_center[0],
                    'start_y': start_center[1],
                    'end_x': end_center[0],
                    'end_y': end_center[1],
                }
                return self._entry_overlay
        except Exception:
            pass
        ov = tk.Toplevel(self.root)
        ov.overrideredirect(True)
        ov.attributes('-topmost', True)
        ov.geometry(f'{sw}x{sh}+0+0')
        ov.configure(bg='#060a10')
        ov.attributes('-alpha', 0.0)
        try:
            _disable_native_window_shadow(ov)
        except Exception:
            pass
        cv = tk.Canvas(ov, width=sw, height=sh, bg='#060a10', highlightthickness=0, bd=0)
        cv.pack(fill=tk.BOTH, expand=True)
        self._entry_overlay = {
            'win': ov,
            'cv': cv,
            'sw': sw,
            'sh': sh,
            'gl': self._init_entry_boot_gl(sw, sh),
            'gl_photo': None,
            'start_x': start_center[0],
            'start_y': start_center[1],
            'end_x': end_center[0],
            'end_y': end_center[1],
        }
        return self._entry_overlay

    def _draw_entry_overlay(self, progress):
        ov = getattr(self, '_entry_overlay', None)
        if not ov:
            return
        gpu = ov.get('gpu_transition')
        if gpu is not None:
            try:
                gpu.set_progress(progress)
            except Exception:
                pass
            return
        try:
            win = ov['win']
            cv = ov['cv']
            if not win.winfo_exists() or not cv.winfo_exists():
                return
        except Exception:
            return

        sw, sh = ov['sw'], ov['sh']
        ignite_t = min(1.0, progress / 0.30)
        deploy_t = max(0.0, min(1.0, (progress - 0.10) / 0.70))
        settle_t = max(0.0, min(1.0, (progress - 0.80) / 0.20))
        bloom = ease_out(ignite_t)
        deploy = ease_in_out(deploy_t)
        settle = ease_in_out(settle_t)
        cx = int(lerp(ov['start_x'], ov['end_x'], deploy))
        cy = int(lerp(ov['start_y'], ov['end_y'], deploy))
        cyan = '#86dfff'
        gold = '#f3af12'
        white = '#edf7ff'
        dim_cyan = '#173746'
        dim_gold = '#5e4211'
        if progress <= 0.68:
            boot_t = progress / 0.68
        else:
            boot_t = 1.0

        try:
            if progress < 0.14:
                overlay_alpha = lerp(0.18, 0.94, ease_out(progress / 0.14))
            elif progress < 0.80:
                overlay_alpha = lerp(0.94, 0.74, ease_in_out((progress - 0.14) / 0.66))
            else:
                overlay_alpha = 0.74 * (1.0 - settle)
            win.attributes('-alpha', max(0.0, min(0.94, overlay_alpha)))
        except Exception:
            pass

        cv.delete('all')
        boot_gl_drawn = self._draw_entry_boot_gl(cv, ov, boot_t)
        if not boot_gl_drawn:
            scan_pitch = 24
            scan_shift = int((progress * 240) % scan_pitch)
            for y in range(-scan_pitch, sh + scan_pitch, scan_pitch):
                yy = y + scan_shift
                col = dim_cyan if ((y // scan_pitch) % 2 == 0) else '#101823'
                cv.create_line(0, yy, sw, yy, fill=col, width=1)

        if settle > 0.82:
            return

        span = int(lerp(min(sw * 0.42, 520), min(sw * 0.22, 260), deploy))
        aperture = int(lerp(172, 28, deploy))
        for off, col in [(-54, cyan), (-24, dim_cyan), (24, dim_gold), (54, gold)]:
            cv.create_line(cx - span, cy + off, cx - aperture, cy + off, fill=col, width=1)
            cv.create_line(cx + aperture, cy + off, cx + span, cy + off, fill=col, width=1)

        ring_r = int(lerp(220, 64, deploy))
        for extra, col in [(0, cyan), (24, gold)]:
            r = ring_r + extra
            arm = 22 + extra // 4
            for sx in (-1, 1):
                for sy in (-1, 1):
                    px = cx + sx * r
                    py = cy + sy * r
                    cv.create_line(px, py, px - sx * arm, py, fill=col, width=1)
                    cv.create_line(px, py, px, py - sy * arm, fill=col, width=1)

        diamond = int(lerp(28, 10, deploy))
        cv.create_polygon(cx, cy - diamond, cx + diamond, cy,
                          cx, cy + diamond, cx - diamond, cy,
                          outline=white, fill='')
        cv.create_line(cx - 46, cy, cx + 46, cy, fill=white, width=1)
        cv.create_line(cx, cy - 22, cx, cy + 22, fill=white, width=1)

        if not boot_gl_drawn or progress > 0.28:
            pulse_y = int(lerp(cy - 160, cy + 88, bloom))
            cv.create_line(max(0, cx - span - 150), pulse_y,
                           min(sw, cx + span + 150), pulse_y,
                           fill=cyan, width=1)
            cv.create_line(max(0, cx - span - 110), pulse_y + 3,
                           min(sw, cx + span + 110), pulse_y + 3,
                           fill=dim_cyan, width=1)

        label_x1 = max(30, cx - span - 70)
        label_x2 = min(sw - 30, cx + span + 70)
        cv.create_text(label_x1, max(24, cy - 164), text='SYS:ENTITY',
                       anchor='w', fill=cyan, font=('Consolas', 9))
        cv.create_text(label_x2, max(24, cy - 164), text='SEQ:ENTRY',
                       anchor='e', fill=gold, font=('Consolas', 9))
        cv.create_text(label_x1, min(sh - 24, cy + 174), text='STATUS:DEPLOY',
                       anchor='w', fill=dim_cyan, font=('Consolas', 9))
        cv.create_text(label_x2, min(sh - 24, cy + 174), text=time.strftime('%H:%M:%S'),
                       anchor='e', fill=dim_gold, font=('Consolas', 9))

        text_y = cy + 92
        cv.create_text(cx, text_y, text='LINK START', fill=white,
                       font=get_sao_font(16, True))
        cv.create_text(cx, text_y + 26, text='ENTITY DEPLOYMENT', fill=gold,
                       font=('Consolas', 11, 'bold'))
        cv.create_text(cx, text_y + 48, text='INITIALIZING VISUAL SHELL',
                       fill='#8aaec0', font=('Consolas', 9))

    def _run_entry_animation(self, fx_start, fy_start, fx_final, fy_final):
        self._create_entry_overlay(fx_start, fy_start, fx_final, fy_final)
        anim_start = time.time()
        total = 1.16
        phase1 = 0.34

        def _done():
            self._cleanup_entry_overlay()
            self._breath_base_x = fx_final
            self._breath_base_y = fy_final
            # self.root.after(120, self._start_float_breath)  # 禁用浮动
            self.root.after(160, self._animate_float_hud)
            # 启动识别循环
            self.root.after(200, self._start_recognition)
            self.root.after(600, self._recognition_loop)
            if not self._username:
                self.root.after(420, self._show_welcome_then_menu)
            else:
                self.root.after(420, self._toggle_sao_menu)
            self.root.after(900, self._restore_panels)
            self.root.after(220, self._mark_update_popup_ready)

        def _tick():
            if self._destroyed:
                self._cleanup_entry_overlay()
                return
            try:
                if not self._float.winfo_exists():
                    self._cleanup_entry_overlay()
                    return
            except Exception:
                self._cleanup_entry_overlay()
                return

            elapsed = time.time() - anim_start
            t = min(1.0, elapsed / total)
            self._draw_entry_overlay(t)

            if elapsed < phase1:
                hold = ease_out(elapsed / phase1)
                self._set_float_alpha(0.12 * hold)
                try:
                    self.root.after(16, _tick)
                except Exception:
                    self._cleanup_entry_overlay()
                return

            deploy = min(1.0, (elapsed - phase1) / max(0.001, total - phase1))
            deploy_e = ease_out(deploy)
            fx = int(lerp(fx_start, fx_final, deploy_e))
            fy = int(lerp(fy_start, fy_final, deploy_e))
            self._float.geometry(f'+{fx}+{fy}')
            self._set_float_alpha(0.95 * ease_in_out(deploy))

            if elapsed < total:
                try:
                    self.root.after(16, _tick)
                except Exception:
                    self._cleanup_entry_overlay()
            else:
                self._float.geometry(f'+{fx_final}+{fy_final}')
                self._set_float_alpha(0.95)
                _done()

        _tick()

    def _get_exit_banner(self, mode='exit', target_label=None):
        if mode == 'switch':
            return {
                'primary': 'INTERFACE SHIFT',
                'secondary': (target_label or 'NEXT UI').upper(),
                'tertiary': 'TRANSFERRING CONTROL TO NEXT LAYER',
                'accent': '#f3af12',
                'accent_dim': '#5e4211',
            }
        return {
            'primary': 'SYSTEM LOG OUT',
            'secondary': 'SAO ENTITY',
            'tertiary': 'PERSISTING SESSION STATE',
            'accent': '#86dfff',
            'accent_dim': '#173746',
        }

    def _init_exit_pulse_gl(self, width, height):
        try:
            import moderngl
        except Exception:
            return None
        try:
            ctx = moderngl.create_standalone_context()
            prog = ctx.program(
                vertex_shader='''
#version 330
out vec2 uv;
vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() {
    vec2 p = pos[gl_VertexID];
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
''',
                fragment_shader='''
#version 330
in vec2 uv;
uniform vec2 u_resolution;
uniform vec2 u_center;
uniform float u_progress;
out vec4 fragColor;

float band(float x, float center, float width) {
    return exp(-pow((x - center) / max(0.0001, width), 2.0));
}

float gridLine(vec2 q, vec2 dir, float scale, float width) {
    float v = abs(fract(dot(q, dir) * scale) - 0.5);
    return 1.0 - smoothstep(width, width + 0.018, v);
}

void main() {
    vec2 center = u_center / u_resolution;
    vec2 p = uv - center;
    p.x *= u_resolution.x / max(1.0, u_resolution.y);
    float r = length(p);
    float ang = atan(p.y, p.x);
    float progress = clamp(u_progress, 0.0, 1.0);
    vec2 dirA = normalize(vec2(1.0, 0.0));
    vec2 dirB = normalize(vec2(0.5, 0.8660254));
    vec2 dirC = normalize(vec2(-0.5, 0.8660254));
    float hexScale = mix(20.0, 34.0, smoothstep(0.14, 0.86, progress));
    float lineA = gridLine(p, dirA, hexScale, 0.030);
    float lineB = gridLine(p, dirB, hexScale, 0.028);
    float lineC = gridLine(p, dirC, hexScale, 0.028);
    float ringRadius = mix(0.02, 0.92, smoothstep(0.0, 0.80, progress));
    float ringWidth = mix(0.090, 0.014, progress);
    float ring = band(r, ringRadius, ringWidth);
    float echoInner = band(r, max(0.0, ringRadius - 0.085), ringWidth * 2.2) * (1.0 - smoothstep(0.26, 0.84, progress));
    float echoOuter = band(r, ringRadius + 0.055, ringWidth * 1.5) * (1.0 - smoothstep(0.48, 0.94, progress));
    float hexMask = band(r, ringRadius - 0.018, ringWidth * 3.8);
    float grid = max(lineA, max(lineB, lineC)) * hexMask;
    float arc = smoothstep(0.18, 1.0, 0.5 + 0.5 * sin(ang * 12.0 - progress * 18.0));
    float core = exp(-r * mix(58.0, 22.0, progress));
    float halo = exp(-r * 3.4) * smoothstep(0.00, 0.18, progress) * (1.0 - smoothstep(0.62, 1.0, progress));
    float sweep = band(uv.y, 0.18 + progress * 0.60, 0.035) + band(uv.y, 0.58 - progress * 0.08, 0.055) * 0.55;
    float closeV = smoothstep(0.82, 1.0, progress);
    float closeH = mix(0.50, 0.006, closeV);
    float closeW = mix(0.50, 0.018, closeV);
    float tvMaskY = 1.0 - smoothstep(closeH, closeH + 0.02, abs(uv.y - 0.5));
    float tvMaskX = 1.0 - smoothstep(closeW, closeW + 0.02, abs(uv.x - 0.5));
    float tvMask = mix(1.0, tvMaskY, smoothstep(0.82, 0.94, progress));
    tvMask *= mix(1.0, tvMaskX, smoothstep(0.92, 1.0, progress));
    float scanlines = 0.92 + 0.08 * sin((uv.y * u_resolution.y) * 2.4 + progress * 1500.0);
    float vignette = smoothstep(1.34, 0.28, r);

    vec3 cyan = vec3(0.60, 0.95, 1.0);
    vec3 blue = vec3(0.06, 0.48, 1.0);
    vec3 white = vec3(1.0, 1.0, 1.0);
    vec3 gold = vec3(1.0, 0.80, 0.28);
    vec3 color = vec3(0.0);

    color += mix(blue, cyan, 0.42) * ring * (0.58 + 0.42 * arc);
    color += cyan * echoInner * 0.82;
    color += gold * echoOuter * 0.44;
    color += mix(cyan, gold, 0.34) * grid * 0.58;
    color += white * core * (0.26 + 0.74 * (1.0 - smoothstep(0.16, 0.60, progress)));
    color += cyan * halo * 0.40;
    color += vec3(0.82, 0.96, 1.0) * sweep * (0.08 + ring * 0.24);
    color *= scanlines * vignette * tvMask;
    color = clamp(color, 0.0, 1.0);
    float alpha = clamp((ring * 0.88 + echoInner * 0.52 + grid * 0.30 + core * 0.82 + sweep * 0.16) * tvMask, 0.0, 1.0);
    fragColor = vec4(color, alpha);
}
''')
            vao = ctx.vertex_array(prog, [])
            tex = ctx.texture((width, height), 4)
            fbo = ctx.framebuffer(color_attachments=[tex])
            prog['u_resolution'].value = (float(width), float(height))
            return {
                'ctx': ctx,
                'pulse_prog': prog,
                'pulse_vao': vao,
                'pulse_tex': tex,
                'pulse_fbo': fbo,
            }
        except Exception:
            try:
                ctx.release()
            except Exception:
                pass
            return None

    def _draw_exit_pulse_gl(self, cv, ov, cx, cy, purge_t):
        gl = ov.get('gl')
        if not gl:
            return False
        try:
            prog = gl['pulse_prog']
            fbo = gl['pulse_fbo']
            vao = gl['pulse_vao']
            fbo.use()
            gl['ctx'].clear(0.0, 0.0, 0.0, 0.0)
            prog['u_center'].value = (float(cx), float(cy))
            prog['u_progress'].value = float(max(0.0, min(1.0, purge_t)))
            vao.render()
            raw = fbo.read(components=4, alignment=1)
            _h, _w = ov['sh'], ov['sw']
            arr = np.frombuffer(raw, dtype=np.uint8).reshape(_h, _w, 4)
            pil_img = ov.get('_gl_pil_buf')
            if pil_img is None or pil_img.size != (_w, _h):
                pil_img = Image.fromarray(arr[::-1], 'RGBA')
                ov['_gl_pil_buf'] = pil_img
                photo = ImageTk.PhotoImage(pil_img)
                ov['gl_photo'] = photo
                ov['_gl_canvas_id'] = cv.create_image(0, 0, image=photo, anchor='nw')
            else:
                pil_img.frombytes(arr[::-1].tobytes())
                photo = ov['gl_photo']
                photo.paste(pil_img)
            return True
        except Exception:
            return False

    def _create_exit_overlay(self, mode='exit', target_label=None):
        self._cleanup_exit_overlay()
        try:
            sw = self.root.winfo_screenwidth()
            sh = self.root.winfo_screenheight()
        except Exception:
            sw, sh = 1920, 1080
        try:
            fx = self._float.winfo_rootx() + self._fw // 2
            fy = self._float.winfo_rooty() + self._fh // 2
        except Exception:
            fx, fy = sw // 2, sh // 2
        try:
            gpu = EntityTransitionGpuOverlay(
                self.root,
                kind='exit',
                center=(fx, fy),
                target=(fx, fy),
                title='sao_entity_exit_gpu',
            )
            if gpu.start():
                self._exit_overlay = {
                    'gpu_transition': gpu,
                    'sw': sw,
                    'sh': sh,
                    'fx': fx,
                    'fy': fy,
                    'banner': self._get_exit_banner(mode, target_label),
                    'mode': mode,
                }
                return self._exit_overlay
        except Exception:
            pass
        try:
            ov = tk.Toplevel(self.root)
            ov.overrideredirect(True)
        except Exception:
            self._finalize_close()
            return None
        ov.attributes('-topmost', True)
        ov.geometry(f'{sw}x{sh}+0+0')
        ov.configure(bg='#060a10')
        ov.attributes('-alpha', 0.0)
        try:
            _disable_native_window_shadow(ov)
        except Exception:
            pass
        cv = tk.Canvas(ov, width=sw, height=sh, bg='#060a10', highlightthickness=0, bd=0)
        cv.pack(fill=tk.BOTH, expand=True)
        self._exit_overlay = {
            'win': ov,
            'cv': cv,
            'sw': sw,
            'sh': sh,
            'fx': fx,
            'fy': fy,
            'banner': self._get_exit_banner(mode, target_label),
            'mode': mode,
            'gl': self._init_exit_pulse_gl(sw, sh),
            'gl_photo': None,
        }
        return self._exit_overlay

    def _draw_exit_overlay(self, progress):
        ov = getattr(self, '_exit_overlay', None)
        if not ov:
            return
        gpu = ov.get('gpu_transition')
        if gpu is not None:
            try:
                gpu.set_progress(progress)
            except Exception:
                pass
            return
        try:
            win = ov['win']
            cv = ov['cv']
            if not win.winfo_exists() or not cv.winfo_exists():
                return
        except Exception:
            return

        sw, sh = ov['sw'], ov['sh']
        cx, cy = ov['fx'], ov['fy']
        lock_t = min(1.0, progress / 0.30)
        purge_t = max(0.0, min(1.0, (progress - 0.18) / 0.82))
        lock_e = ease_out(lock_t)
        purge_e = ease_in_out(purge_t)
        cyan = '#86dfff'
        gold = ov['banner']['accent']
        dim_cyan = '#173746'
        dim_gold = ov['banner']['accent_dim']
        white = '#edf7ff'

        wash = 0.22 + 0.78 * lock_e
        sweep = ((lock_t * 0.45) + purge_t * 1.2) % 1.0
        tv_fade = 0.0 if progress <= 0.84 else min(1.0, max(0.0, (progress - 0.84) / 0.16))
        tv_fade = ease_in_out(tv_fade)

        try:
            peak_alpha = min(0.96, 0.16 + 0.50 * lock_e + 0.18 * purge_e)
            win.attributes('-alpha', max(0.0, peak_alpha * (1.0 - tv_fade * 0.97)))
        except Exception:
            pass

        cv.delete('all')
        if purge_t < 0.06 or not ov.get('gl'):
            scan_pitch = 26
            scan_shift = int((progress * 280) % scan_pitch)
            for y in range(-scan_pitch, sh + scan_pitch, scan_pitch):
                yy = y + scan_shift
                col = dim_cyan if ((y // scan_pitch) % 2 == 0) else '#101823'
                cv.create_line(0, yy, sw, yy, fill=col, width=1)

        pulse_gl_drawn = False
        if purge_t > 0.0:
            pulse_gl_drawn = self._draw_exit_pulse_gl(cv, ov, cx, cy, purge_t)
            if not pulse_gl_drawn:
                pulse = max(0.0, 1.0 - abs(purge_t - 0.18) / 0.18)
                if pulse > 0.01:
                    if pulse > 0.72:
                        flash_fill = '#eefbff'
                        flash_stipple = 'gray25'
                    elif pulse > 0.38:
                        flash_fill = '#c8efff'
                        flash_stipple = 'gray25'
                    else:
                        flash_fill = '#8edfff'
                        flash_stipple = 'gray50'
                    cv.create_rectangle(0, 0, sw, sh, fill=flash_fill, outline='', stipple=flash_stipple)
                    bloom_r = int(lerp(40, min(sw, sh) * 0.32, pulse))
                    core_r = max(10, int(bloom_r * 0.26))
                    cv.create_oval(cx - bloom_r, cy - bloom_r,
                                   cx + bloom_r, cy + bloom_r,
                                   outline='#dff8ff', width=max(1, int(2 + pulse * 3)),
                                   stipple='gray25')
                    cv.create_oval(cx - core_r, cy - core_r,
                                   cx + core_r, cy + core_r,
                                   fill='#f8feff', outline='', stipple='gray25')

                if tv_fade > 0.0:
                    fade = int(255 * tv_fade)
                    fill = f'#{fade:02x}{fade:02x}{fade:02x}'
                    cv.create_rectangle(0, 0, sw, sh, fill=fill, outline='',
                                        stipple='gray50' if tv_fade < 0.7 else '')

        if tv_fade > 0.18:
            return

        span = int(lerp(min(sw * 0.30, 360), min(sw * 0.38, 460), lock_e))
        aperture = int(lerp(146, 22, purge_e))
        for off, col in [(-60, cyan), (-28, dim_cyan), (28, dim_gold), (60, gold)]:
            cv.create_line(cx - span, cy + off, cx - aperture, cy + off, fill=col, width=1)
            cv.create_line(cx + aperture, cy + off, cx + span, cy + off, fill=col, width=1)

        base_r = int(lerp(34, 194, lock_e * (1.0 - purge_e * 0.20)))
        for extra, col in [(0, cyan), (20, gold)]:
            r = max(22, int((base_r + extra) * (1.0 - 0.58 * purge_e)))
            arm = 18 + extra // 3
            for sx in (-1, 1):
                for sy in (-1, 1):
                    px = cx + sx * r
                    py = cy + sy * r
                    cv.create_line(px, py, px - sx * arm, py, fill=col, width=1)
                    cv.create_line(px, py, px, py - sy * arm, fill=col, width=1)

        diamond = int(lerp(24, 9, purge_e))
        cv.create_polygon(cx, cy - diamond, cx + diamond, cy,
                          cx, cy + diamond, cx - diamond, cy,
                          outline=white, fill='')
        cv.create_line(cx - 38, cy, cx + 38, cy, fill=white, width=1)
        cv.create_line(cx, cy - 18, cx, cy + 18, fill=white, width=1)

        if purge_t > 0.0 and not pulse_gl_drawn:
            burst = int(lerp(18, 220, purge_e))
            flash = '#d7f7ff' if purge_t < 0.7 else gold
            cv.create_line(cx - burst, cy, cx + burst, cy, fill=flash, width=2)
            cv.create_line(cx, cy - int(burst * 0.42), cx, cy + int(burst * 0.42), fill=flash, width=1)

        if tv_fade < 0.92:
            scan_y = int(lerp(cy - 140, cy + 120, sweep))
            cv.create_line(max(0, cx - span - 140), scan_y,
                           min(sw, cx + span + 140), scan_y,
                           fill=cyan, width=1)
            cv.create_line(max(0, cx - span - 120), scan_y + 3,
                           min(sw, cx + span + 120), scan_y + 3,
                           fill=dim_cyan, width=1)

        banner_x1 = max(30, cx - span - 60)
        banner_x2 = min(sw - 30, cx + span + 60)
        seq_label = 'SEQ:SHIFT' if ov.get('mode') == 'switch' else 'SEQ:EXIT'
        status_label = 'STATUS:LOCK' if purge_t < 0.08 else ('STATUS:TRANSFER' if ov.get('mode') == 'switch' else 'STATUS:PURGE')
        cv.create_text(banner_x1, max(24, cy - 150), text='SYS:ENTITY',
                       anchor='w', fill=cyan, font=('Consolas', 9))
        cv.create_text(banner_x2, max(24, cy - 150), text=seq_label,
                       anchor='e', fill=gold, font=('Consolas', 9))
        cv.create_text(banner_x1, min(sh - 24, cy + 164), text=status_label,
                       anchor='w', fill=dim_cyan, font=('Consolas', 9))
        cv.create_text(banner_x2, min(sh - 24, cy + 164), text=time.strftime('%H:%M:%S'),
                       anchor='e', fill=dim_gold, font=('Consolas', 9))

        text_y = cy + 86
        primary = 'ENTITY LOCK' if purge_t < 0.12 else ov['banner']['primary']
        tertiary = 'FREEZING UI STATE' if purge_t < 0.12 else ov['banner']['tertiary']
        cv.create_text(cx, text_y, text=primary,
                       fill=white, font=get_sao_font(16, True))
        cv.create_text(cx, text_y + 26, text=ov['banner']['secondary'],
                       fill=gold, font=('Consolas', 11, 'bold'))
        cv.create_text(cx, text_y + 48, text=tertiary,
                       fill='#8aaec0', font=('Consolas', 9))

    def _collect_exit_windows(self):
        wins = []
        seen = set()

        try:
            focus_x = self._float.winfo_x() + self._fw // 2
            focus_y = self._float.winfo_y() + self._fh // 2
        except Exception:
            focus_x = self.root.winfo_screenwidth() // 2
            focus_y = self.root.winfo_screenheight() // 2

        def _profile(x, y, role, order):
            dx = x - focus_x
            dy = y - focus_y
            dist = max(1.0, math.hypot(dx, dy))
            ux, uy = dx / dist, dy / dist
            if role == 'float':
                return {'delay': 0.28, 'duration': 0.52, 'travel': 86,
                        'ux': 1.0, 'uy': -0.25, 'movable': True}
            if role == 'panel':
                return {'delay': 0.12 + order * 0.085, 'duration': 0.40,
                        'travel': 48 + order * 12, 'ux': ux, 'uy': uy + 0.24, 'movable': True}
            if role == 'menu':
                return {'delay': 0.00, 'duration': 0.32, 'travel': 0,
                        'ux': 0.0, 'uy': 0.0, 'movable': False}
            if role == 'fisheye':
                return {'delay': 0.00, 'duration': 0.24, 'travel': 0,
                        'ux': 0.0, 'uy': 0.0, 'movable': False}
            return {'delay': 0.06, 'duration': 0.32, 'travel': 22,
                    'ux': ux, 'uy': uy, 'movable': True}

        def _add(win, role, order=0, ulw=False):
            if not win:
                return
            try:
                if not win.winfo_exists():
                    return
                wid = win.winfo_id()
                if wid in seen:
                    return
                seen.add(wid)
                try:
                    alpha = float(win.attributes('-alpha'))
                except Exception:
                    alpha = 1.0
                profile = _profile(win.winfo_x(), win.winfo_y(), role, order)
                wins.append({
                    'win': win,
                    'alpha': max(0.0, min(1.0, alpha)),
                    'x': win.winfo_x(),
                    'y': win.winfo_y(),
                    'role': role,
                    'ulw': ulw,
                    **profile,
                })
            except Exception:
                pass

        def _add_panel_owner(panel, order=0):
            if not panel:
                return
            try:
                if hasattr(panel, 'is_visible') and not panel.is_visible():
                    return
            except Exception:
                pass
            win = getattr(panel, '_win', None)
            if not win:
                return
            try:
                if hasattr(win, 'state') and str(win.state()) == 'withdrawn':
                    return
            except Exception:
                pass
            _add(win, 'panel', order=order)

        # HP float 使用 ULW，不能用 attributes('-alpha') 读写
        _float = getattr(self, '_float', None)
        if _float:
            try:
                if _float.winfo_exists():
                    wid = _float.winfo_id()
                    if wid not in seen:
                        seen.add(wid)
                        profile = _profile(_float.winfo_x(), _float.winfo_y(), 'float', 0)
                        wins.append({
                            'win': _float,
                            'alpha': getattr(self, '_float_alpha', 1.0),
                            'x': _float.winfo_x(),
                            'y': _float.winfo_y(),
                            'role': 'float',
                            'ulw': True,
                            **profile,
                        })
            except Exception:
                pass
        for idx, panel in enumerate([
            self._status_panel,
            getattr(self._autokey_panel, '_win', None),
            getattr(self._bossraid_panel, '_win', None),
            getattr(self._autokey_detail_panel, '_win', None),
            getattr(self._bossraid_detail_panel, '_win', None),
        ]):
            _add(panel, 'panel', order=idx)
        _add_panel_owner(self._commander_panel, order=5)
        _add(getattr(getattr(self, '_sao_menu', None), '_overlay', None), 'menu')
        _add(getattr(self, '_fisheye_ov', None), 'fisheye')
        # _hp_alpha_windows 已废弃 (ULW 内部渲染)
        return wins
