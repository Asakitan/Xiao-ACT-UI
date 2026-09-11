Texture2D fieldTexture : register(t0);
SamplerState fieldSampler : register(s0);
cbuffer Backdrop : register(b0) {
    float2 resolution; float time; float openness;
    float reducedMotion; float renderPass; float2 padding;
};

float hash21(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float noise2(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash21(i), hash21(i + float2(1, 0)), f.x),
                lerp(hash21(i + float2(0, 1)), hash21(i + 1.0), f.x), f.y);
}
float frost(float2 p) {
    float value = 0.0, amplitude = 0.5;
    [unroll] for (int i = 0; i < 5; ++i) {
        value += amplitude * noise2(p);
        p = float2(p.x * 0.8 - p.y * 0.6, p.x * 0.6 + p.y * 0.8) * 2.0;
        amplitude *= 0.5;
    }
    return value;
}
float band(float value, float width) { return 1.0 - smoothstep(0.0, width, abs(value)); }
float angularDistance(float angle) { return abs(frac(angle / 6.2831853 + 0.5) - 0.5) * 6.2831853; }

float3 procedural(float2 uv, float t) {
    float aspect = resolution.x / max(1.0, resolution.y);
    float2 p = (uv - 0.5) * float2(aspect, 1.0);
    float r = length(p), angle = atan2(p.y, p.x);
    const float3 white = float3(0.95, 0.95, 0.96);
    const float3 silver = float3(0.75, 0.76, 0.78);
    const float3 grey = float3(0.55, 0.56, 0.58);
    const float3 amber = float3(0.83, 0.66, 0.33);
    const float3 teal = float3(0.30, 0.72, 0.72);
    float3 color = lerp(float3(0.96, 0.96, 0.97), float3(0.80, 0.81, 0.83), saturate(r));
    color += 0.03 * (frost(uv * 6.0 + float2(t * 0.03, -t * 0.02)) - 0.5);
    color += float3(0.06, 0.06, 0.07) * exp(-r * r * 8.0);
    [unroll] for (int rayIndex = 0; rayIndex < 5; ++rayIndex) {
        float a = float(rayIndex) * 1.256637 + t * 0.05;
        float ray = band(angularDistance(angle - a), 0.10) * (1.0 - smoothstep(0.04, 0.55, r));
        color += lerp(white, amber, 0.3) * ray * (0.6 + 0.4 * sin(t * 0.35 + rayIndex * 1.5)) * 0.06;
    }
    [unroll] for (int layer = 0; layer < 3; ++layer) {
        float f = float(layer), columns = 30.0 + f * 15.0;
        float column = floor(uv.x * columns), seed = hash21(float2(column, f * 7.0));
        float speed = (seed * 2.0 + 0.5) * (0.7 + f * 0.25);
        float direction = hash21(float2(column, f + 77.0)) > 0.5 ? 1.0 : -1.0;
        float y = frac(uv.y + direction * t * speed * 0.06 + hash21(float2(column, f * 13.0)));
        float h = hash21(float2(column, f + 50.0));
        float length = h < 0.2 ? 0.70 + h * 1.5 : h < 0.5 ? 0.20 + (h - 0.2) * 0.8 : 0.06 + (h - 0.5) * 0.2;
        float streak = (1.0 - smoothstep(length * 0.85, length, y)) * smoothstep(0.0, 0.01, y);
        float visible = smoothstep(-0.3, 0.3, sin(t * (0.15 + seed * 0.2) + seed * 6.28));
        float3 tint = lerp(lerp(grey, teal, 0.35), lerp(silver, amber, 0.25), f / 2.0);
        color += tint * streak * visible * 0.10 * (0.6 + f * 0.2);
        color += white * exp(-15.0 * y) * streak * visible * 0.08;
    }
    float aa = max(1.0 / max(1.0, resolution.y), 0.0006);
    [unroll] for (int ringIndex = 0; ringIndex < 4; ++ringIndex) {
        float f = float(ringIndex), radius = 0.10 + f * 0.12;
        float speed = 0.25 / (1.0 + f * 0.8), drift = 0.03 + f * 0.005;
        float2 q = p - drift * float2(sin(t * speed + f * 1.2), cos(t * speed * 0.85 + f * 2.1));
        float qr = length(q), qa = atan2(q.y, q.x);
        float pulse = 0.5 + 0.5 * sin(t * (0.45 + f * 0.3) + f * 1.57);
        float segment = smoothstep(0.0, 0.015, abs(sin(qa * (8.0 + f * 4.0) + t * (0.18 + f * 0.10))));
        float3 tint = lerp(silver, white, f / 3.0);
        color += tint * band(qr - radius, max(aa * 2.0, 0.003)) * segment * pulse * 0.35;
        color += tint * band(qr - radius, 0.025) * pulse * 0.04;
        float majorAngle = abs(frac((qa + 3.141593) / 0.523599 + 0.5) - 0.5) * 0.523599;
        float minorAngle = abs(frac((qa + 3.141593) / 0.174533 + 0.5) - 0.5) * 0.174533;
        color += lerp(tint, amber, 0.4) * band(majorAngle, 0.012) * band(qr - radius - 0.015, 0.008) * 0.30 * pulse;
        color += tint * band(minorAngle, 0.006) * band(qr - radius - 0.008, 0.004) * 0.12 * pulse;
    }
    float2 scanPoint = p - 0.035 * float2(sin(t * 0.18), cos(t * 0.15));
    [unroll] for (int beamIndex = 0; beamIndex < 3; ++beamIndex) {
        float f = float(beamIndex), a = t * (0.18 + f * 0.10) + f * 2.094;
        float d = frac((atan2(scanPoint.y, scanPoint.x) - a) / 6.2831853 + 1.0) * 6.2831853;
        float beam = band(d, 0.22) * exp(-d * 3.5) * (1.0 - smoothstep(0.03, 0.46, length(scanPoint)));
        color += lerp(white, teal, 0.35) * beam * (0.6 + 0.4 * sin(t * (0.6 + f * 0.35))) * 0.15;
    }
    float2 grid = uv * float2(aspect * 16.0, 10.0), cell = floor(grid), local = frac(grid);
    float seed = hash21(cell + float2(17.0, 31.0));
    float life = frac(t / (2.0 + seed * 5.0) + seed * 6.28);
    float visible = smoothstep(0.0, 0.15, life) * (1.0 - smoothstep(0.7, 0.85, life));
    float lines = 1.0 - step(0.03, local.x) * step(local.x, 0.97) * step(0.04, local.y) * step(local.y, 0.96);
    color += silver * lines * 0.04;
    float inner = step(0.06, local.x) * step(local.x, 0.94) * step(0.08, local.y) * step(local.y, 0.92);
    float data = step(0.45, frac(local.y * 3.0)) * 0.3 + step(0.1, local.x) * step(local.x, 0.1 + hash21(cell) * 0.7) * 0.5;
    color += lerp(silver, lerp(amber, teal, seed), 0.3) * inner * data * visible * 0.08;
    [unroll] for (int mote = 0; mote < 6; ++mote) {
        float f = float(mote);
        float2 pos = frac(float2(hash21(float2(f, 1)), hash21(float2(f, 2))) +
            float2(sin(f * 1.7 + t * 0.25), cos(f * 2.3 + t * 0.18)) * 0.12 + float2(0, -t * (0.02 + hash21(float2(f, 5)) * 0.03)));
        float2 d = (uv - pos) * float2(aspect, 1);
        color += lerp(white, mote % 2 == 0 ? amber : teal, 0.45) * (0.0008 / (dot(d,d) + 0.0008)) * 0.045;
    }
    color *= 0.975 + 0.025 * sin(uv.y * resolution.y * 1.5);
    return saturate(color * clamp(1.0 - r * 0.30, 0.72, 1.0));
}

float3 lensSample(float2 uv, float split) {
    return float3(fieldTexture.Sample(fieldSampler, uv + float2(split, 0)).r,
                  fieldTexture.Sample(fieldSampler, uv).g,
                  fieldTexture.Sample(fieldSampler, uv - float2(split, 0)).b);
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float t = reducedMotion > 0.5 ? 0.0 : time;
    if (renderPass < 0.5) return float4(procedural(uv, t), 1.0);
    float2 p = uv - 0.5;
    float r2 = dot(p, p), r = sqrt(r2);
    float edge = 1.0 - saturate(openness);
    float strength = 0.44 + edge * 0.18 + sin(t * 0.4) * 0.02;
    float2 drift = float2(sin(t * 0.62) * 0.006, cos(t * 0.51) * 0.004);
    float2 warped = uv + p * strength * r2 + drift;
    float2 direction = p / max(r, 0.001);
    float spread = (0.0008 + r2 * 0.003) * (1.0 + edge);
    float split = 0.0015 + r2 * 0.004;
    float3 color = lensSample(warped - direction * spread * 2.0, split) * 0.10;
    color += lensSample(warped - direction * spread, split) * 0.20;
    color += lensSample(warped, split) * 0.36;
    color += lensSample(warped + direction * spread, split) * 0.22;
    color += lensSample(warped + direction * spread * 2.0, split) * 0.12;
    color *= (1.0 - (0.34 + edge * 0.18)) * (1.0 - smoothstep(0.25, 0.75, r) * 0.12);
    color *= 0.985 + 0.015 * sin(uv.y * 980.0);
    float ring = band(r - lerp(0.18, 0.72, saturate(openness)), 0.0025);
    color += float3(0.30, 0.72, 0.72) * ring * 0.06;
    float alpha = saturate(openness) * 0.95;
    return float4(saturate(color) * alpha, alpha);
}
