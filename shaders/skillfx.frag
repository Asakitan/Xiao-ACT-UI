// sao_auto/shaders/skillfx.frag — v2.3.0 GUI 渲染链路重置
//
// Single-pass SDF renderer for SkillFX ring + beam + tail + glow.
// Replaces _render_ring_layer + _render_beam_layer + _draw_glfx PIL/numpy
// paths with one fragment shader that fills the entire overlay framebuffer.
//
// Outputs STRAIGHT (non-premultiplied) RGBA so it can be handed directly
// to PIL.Image.alpha_composite (Phase 1 interim consumer). For Phase 2
// (moderngl-window direct blit) we'll add an output mode flag and emit
// premultiplied RGBA for DWM blend (ONE, ONE_MINUS_SRC_ALPHA).
//
// All distances/sizes are in window pixels. Origin is top-left, y grows
// downward (matches PIL / Win32 coordinate space).
//
// Uniforms (set by SkillFXShaderPipeline.render):
//   u_resolution  : framebuffer (W, H)
//   u_time        : seconds since burst show (drives pulse/sweep/tail)
//   u_alpha_mul   : global enter*exit alpha multiplier  [0..1]
//   u_anchor      : ring center (px)
//   u_r_out       : ring outer radius (px)
//   u_r_in        : inner gold ring radius (px)  (= r_out - 16)
//   u_r_core      : white core ring radius (px)
//   u_pulse       : 0..1 pulse phase
//   u_beam_a      : beam start (px)
//   u_beam_b      : beam end (px)  (caption anchor)
//   u_beam_h      : beam thickness (px)
//   u_show_age    : seconds since show_t (for sweep/tail timing)
//   u_exiting     : >0 if exiting (suppresses sweep/tail)
//   u_glfx_intensity : 0..1 multiplier for energy-field glow (0 disables)
//   u_seed        : per-burst random seed for energy field

#version 330

uniform vec2  u_resolution;
uniform float u_time;
uniform float u_alpha_mul;
uniform vec2  u_anchor;
uniform float u_r_out;
uniform float u_r_in;
uniform float u_r_core;
uniform float u_pulse;
uniform vec2  u_beam_a;
uniform vec2  u_beam_b;
uniform float u_beam_h;
uniform float u_show_age;
uniform float u_exiting;
uniform float u_glfx_intensity;
uniform float u_seed;

in vec2 v_uv;
out vec4 fragColor;

const vec3 CYAN_HI   = vec3(0.443, 0.933, 1.000);  // (113,238,255)
const vec3 CYAN_MID  = vec3(0.380, 0.910, 1.000);  // (97,232,255)
const vec3 CYAN_SOFT = vec3(0.690, 0.969, 1.000);  // (176,247,255)
const vec3 GOLD_HI   = vec3(1.000, 0.741, 0.275);  // (255,189,70)
const vec3 GOLD_MID  = vec3(1.000, 0.737, 0.259);  // (255,188,66)
const vec3 WHITE_HI  = vec3(1.000, 1.000, 1.000);

// ── Helpers ─────────────────────────────────────────────────────────────────

float sdRing(vec2 p, float r) {
    return abs(length(p) - r);
}

float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 0.0001), 0.0, 1.0);
    return length(pa - ba * h);
}

// Returns t in [0..1] along segment, clamped to [0..1].
float segmentT(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    return clamp(dot(pa, ba) / max(dot(ba, ba), 0.0001), 0.0, 1.0);
}

// Signed perpendicular distance to infinite line a->b (positive on left)
float linePerpDist(vec2 p, vec2 a, vec2 b) {
    vec2 ba = b - a;
    float L = max(length(ba), 0.0001);
    vec2 n = vec2(-ba.y, ba.x) / L;
    return dot(p - a, n);
}

float smooth_aa(float d, float w) {
    // d = signed distance, w = anti-alias band width (~1 px)
    return 1.0 - smoothstep(-w, w, d);
}

// 1-px stroke, antialiased, alpha at signed-distance d
float strokeAA(float d) {
    return 1.0 - smoothstep(0.0, 1.0, abs(d));
}

float angleAt(vec2 p, vec2 c) {
    return atan(p.y - c.y, p.x - c.x);
}

// True if angle (radians, atan2 range -π..π) lies within [start, end]
// (degrees), allowing wrap-around.
bool inArc(float ang_deg, float start_deg, float end_deg) {
    if (start_deg <= end_deg) {
        return ang_deg >= start_deg && ang_deg <= end_deg;
    }
    return ang_deg >= start_deg || ang_deg <= end_deg;
}

// Internal accumulator uses PREMULTIPLIED form for correct "over"; we
// un-premultiply once at the end before writing fragColor. This keeps
// the per-layer math correct while letting PIL alpha_composite consume
// the result without further conversion.
vec4 over(vec4 src, vec4 dst) {
    float oa = src.a + dst.a * (1.0 - src.a);
    vec3 oc  = src.rgb + dst.rgb * (1.0 - src.a);
    return vec4(oc, oa);
}

vec4 prem(vec3 c, float a) {
    a = clamp(a, 0.0, 1.0);
    return vec4(c * a, a);
}

// ── Main ────────────────────────────────────────────────────────────────────

void main() {
    vec2 p = vec2(v_uv.x, 1.0 - v_uv.y) * u_resolution;  // top-left origin
    vec4 acc = vec4(0.0);  // premultiplied accumulator

    if (u_alpha_mul <= 0.005) {
        fragColor = acc;
        return;
    }

    // ──────────────────────────────────────────────
    // 1) RING (centered at u_anchor)
    // ──────────────────────────────────────────────
    vec2 rp = p - u_anchor;
    float rd = length(rp);

    // Halo: gaussian band around r_out, cyan, breath-modulated by pulse
    // Sigma 40 to match the soft PIL halo (PIL post-blurs the halo+core
    // layer; widening the gaussian here gives a comparable soft falloff).
    float halo_breath = 0.52 + 0.40 * u_pulse;
    float halo_a = halo_breath * exp(-pow(rd - u_r_out, 2.0) / (40.0 * 40.0));
    halo_a *= 78.0 / 255.0;
    acc = over(prem(CYAN_MID, halo_a), acc);

    // Core fill (radial gradient inside r_core), cyan→soft white blend
    if (u_r_core > 2.0) {
        float core_t  = clamp(1.0 - rd / max(1.0, u_r_core), 0.0, 1.0);
        float core_a  = (0.6 + 0.4 * u_pulse) * core_t * (70.0 / 255.0);
        // soft mask edge
        float edge_aa = 1.0 - smoothstep(u_r_core - 1.0, u_r_core + 1.0, rd);
        core_a *= edge_aa;
        vec3 core_col = mix(CYAN_MID, CYAN_SOFT, core_t);
        acc = over(prem(core_col, core_a), acc);
    }

    // After-blur outer rings (drawn over halo). The original PIL pipeline
    // gaussian-blurs the halo+core *under* the rings; we approximate the
    // visual result by widening the stroke slightly (blur ≈ +0.5 px AA).
    {
        // Outer cyan stroke (1 px)
        float dso = abs(rd - u_r_out);
        float stroke_o = (1.0 - smoothstep(0.5, 1.5, dso));
        float a_o = (235.0 / 255.0) * (0.75 + 0.25 * u_pulse) * stroke_o;
        acc = over(prem(CYAN_HI, a_o), acc);

        // Inner gold ring (1 px) at r_in
        if (u_r_in > 2.0) {
            float dsi = abs(rd - u_r_in);
            float stroke_i = (1.0 - smoothstep(0.5, 1.5, dsi));
            acc = over(prem(GOLD_HI, (220.0 / 255.0) * stroke_i), acc);
        }

        // Core ring (white-ish, 1 px) at r_core
        if (u_r_core > 2.0) {
            float dsc = abs(rd - u_r_core);
            float stroke_c = (1.0 - smoothstep(0.5, 1.5, dsc));
            acc = over(prem(CYAN_SOFT, (200.0 / 255.0) * stroke_c), acc);
        }

        // Arcs at r_out + 12: top cyan 200..260 deg, bottom gold 20..80 deg
        float r_arc = u_r_out + 12.0;
        float dsa = abs(rd - r_arc);
        if (dsa < 1.5) {
            // PIL ImageDraw.arc uses degrees with 0 = +x, 90 = +y (downward)
            float ang = degrees(atan(rp.y, rp.x));
            if (ang < 0.0) ang += 360.0;
            float stroke_a = 1.0 - smoothstep(0.5, 1.5, dsa);
            if (inArc(ang, 200.0, 260.0)) {
                acc = over(prem(CYAN_HI, (220.0 / 255.0) * stroke_a), acc);
            } else if (inArc(ang, 20.0, 80.0)) {
                acc = over(prem(GOLD_MID, (210.0 / 255.0) * stroke_a), acc);
            }
        }
    }

    // Sweep band (only during enter, smoothstep 0.10..1.35 in show_age)
    if (u_exiting < 0.5) {
        float sweep_t = smoothstep(0.10, 1.35, u_show_age);
        if (sweep_t > 0.0 && sweep_t < 1.0) {
            float box = u_r_out + 28.0;
            float band_x = -box * 0.18 + (box * 1.30) * sweep_t;
            // band lies along local x axis of ring; project rp.x
            float bd = (rp.x - band_x);
            float band_a = exp(-pow(bd / 8.5, 2.0)) * (210.0 / 255.0);
            // Clip to circular region of radius (box/2 - 12)
            float clip_r = max(8.0, box * 0.5 - 12.0);
            float clip = 1.0 - smoothstep(clip_r - 1.0, clip_r + 1.0, rd);
            acc = over(prem(CYAN_HI, band_a * clip), acc);
        }
    }

    // ──────────────────────────────────────────────
    // 2) BEAM (a → b, thickness u_beam_h)
    // ──────────────────────────────────────────────
    {
        vec2 a = u_beam_a;
        vec2 b = u_beam_b;
        float t = segmentT(p, a, b);          // 0..1 along beam
        float side = linePerpDist(p, a, b);   // signed perpendicular dist
        float aside = abs(side);

        // Gradient cyan→gold along t (matches _fast_beam: t1 first 22%, t2 35%..100%)
        float t1 = clamp(t / 0.22, 0.0, 1.0);
        float t2 = clamp((t - 0.35) / 0.65, 0.0, 1.0);
        vec3 beam_rgb = mix(CYAN_MID, GOLD_MID, t2);

        // Vertical gaussian falloff — sigma widened (4.0 → 1.5) so the
        // beam halo spreads laterally to match PIL's post-blurred beam
        // layer instead of looking like a thin gradient strip.
        float ys = aside / max(1.0, u_beam_h * 0.5);
        float falloff = exp(-(ys * ys) * 1.5);
        // Add a second narrow-but-bright "hot core" gaussian to recover
        // the central density lost when we widened the main falloff.
        float core_falloff = exp(-(ys * ys) * 8.0);
        float A_base = (210.0 / 255.0) * (0.15 + 0.85 * t1);
        float A_glow = A_base * (falloff + 0.55 * core_falloff);
        // Only inside the beam segment range
        if (t > 0.0 && t < 1.0) {
            acc = over(prem(beam_rgb, A_glow), acc);

            // Core bright line (3 px tall, white-tinted with same gradient)
            float core_band = 1.0 - smoothstep(1.0, 2.0, aside);
            if (core_band > 0.0) {
                vec3 core_c = mix(CYAN_HI, GOLD_HI, t2);
                acc = over(prem(core_c, (245.0 / 255.0) * core_band), acc);
            }
        }

        // Two parallel trace lines (cyan offset -4, gold offset +4).
        // PIL draws these as 1-px lines then blurs the entire beam layer,
        // so they appear as soft ~5-px wide halos. Approximate by giving
        // them a wide AA falloff (smoothstep 0..5) instead of the sharp
        // 1-px stroke we had originally.
        float trace_wave = 0.5 + 0.5 * sin(max(0.0, u_show_age - 0.9) * (6.2832 / 2.45));
        float trace_alpha = (0.30 + 0.36 * trace_wave) * (130.0 / 255.0);
        if (t > 0.0 && t < 1.0 && trace_alpha > 0.005) {
            // cyan trace at side ≈ -4
            float dc = abs(side - (-4.0));
            float sc = 1.0 - smoothstep(0.0, 5.0, dc);
            acc = over(prem(CYAN_MID, trace_alpha * sc), acc);
            // gold trace at side ≈ +4
            float dg = abs(side - 4.0);
            float sg = 1.0 - smoothstep(0.0, 5.0, dg);
            acc = over(prem(GOLD_MID, trace_alpha * sg), acc);
        }

        // Tail capsule (sweeps along beam after show_age >= 0.52)
        if (u_exiting < 0.5) {
            float tail_age = u_show_age - 0.52;
            if (tail_age >= 0.0) {
                float tail_phase = mod(tail_age, 1.55) / 1.55;
                float tail_op;
                if (tail_phase <= 0.18) {
                    tail_op = tail_phase / 0.18;
                } else {
                    tail_op = max(0.0, 1.0 - (tail_phase - 0.18) / 0.82);
                }
                if (tail_op > 0.005) {
                    float frac = -0.10 + 1.14 * tail_phase;
                    vec2 tail_c = a + (b - a) * frac;
                    // 120×10 capsule ≈ gaussian on side, x-tent on along
                    float along = dot(p - tail_c, normalize(b - a));
                    float ax = clamp(1.0 - abs(along) / 60.0, 0.0, 1.0);
                    float ay = exp(-pow(side / (10.0 * 0.5), 2.0));
                    float tA = (235.0 / 255.0) * pow(ax, 1.2) * ay * tail_op;
                    acc = over(prem(WHITE_HI, tA), acc);
                }
            }
        }
    }

    // ──────────────────────────────────────────────
    // 3) GLFX energy field (mirrors original _draw_glfx shader)
    // ──────────────────────────────────────────────
    if (u_glfx_intensity > 0.005) {
        vec2 uv = p;
        float lineDist = sdSegment(uv, u_anchor, u_beam_b);
        float lineGlow = exp(-lineDist * 0.052);
        float radial = exp(-length(uv - u_anchor) * 0.030);
        float terminal = exp(-length(uv - u_beam_b) * 0.020);
        float scan = 0.5 + 0.5 * sin(u_time * 7.2 + lineDist * 0.15 + uv.x * 0.010 + u_seed);
        float wave = 0.5 + 0.5 * sin((uv.x + uv.y) * 0.016 - u_time * 2.6 + u_seed * 1.7);
        float pulse = 0.5 + 0.5 * sin(u_time * 4.6 + length(uv - u_anchor) * 0.032);
        float blend = clamp(0.32 + scan * 0.42 + wave * 0.24, 0.0, 1.0);
        vec3 beamColor = mix(CYAN_MID, GOLD_MID, blend);
        vec3 col = beamColor * lineGlow * (0.36 + 0.54 * pulse)
                 + CYAN_MID * radial * (1.05 + 0.28 * scan)
                 + GOLD_MID * terminal * (0.82 + 0.26 * wave);
        float alpha = clamp((lineGlow * 0.82 + radial * 0.96 + terminal * 0.72) * u_glfx_intensity, 0.0, 0.96);
        acc = over(vec4(col * alpha, alpha), acc);
    }

    // Apply global enter*exit fade (still premultiplied)
    acc *= u_alpha_mul;

    // Un-premultiply to STRAIGHT alpha before output so PIL.alpha_composite
    // can consume the result without postprocessing.
    if (acc.a > 0.0001) {
        fragColor = vec4(acc.rgb / acc.a, acc.a);
    } else {
        fragColor = vec4(0.0);
    }
}
