Texture2D fieldTexture : register(t0);
SamplerState fieldSampler : register(s0);
cbuffer Backdrop : register(b0) {
    float2 resolution; float time; float openness;
    float reducedMotion; float renderPass; float darkness; float themeDirection;
    float4 menuRectUv;
    float4 hostUv;
    float menuVisibility; float childActivity; float fpsPressure; float highContrast;
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

float themeHash(uint column, uint salt) {
    uint value = column * 0x9e3779b9u ^ salt * 0x85ebca6bu;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    return float(value & 0xffffu) / 65535.0;
}
float2 hostPosition(float2 uv) { return hostUv.xy + uv * hostUv.zw; }
float themeFront(float2 sceneUv) {
    uint column = uint(clamp(floor(sceneUv.x * 48.0), 0.0, 47.0));
    float start = themeHash(column, 13u) * 0.24;
    float finish = 0.68 + themeHash(column, 37u) * 0.32;
    float local = saturate((darkness - start) / (finish - start));
    float travel = pow(local, 1.10 + themeHash(column, 71u) * 1.20);
    float drift = sin(time * 0.65 + themeHash(column, 63u) * 6.2831853);
    return -0.14 + 1.28 * travel + drift * 0.0225 * (4.0 * local * (1.0 - local));
}
float themeDarkness(float2 uv) {
    float2 sceneUv = hostPosition(uv);
    float front = themeFront(sceneUv);
    float aa = max(1.5 * hostUv.w / max(1.0, resolution.y), 0.0005);
    return 1.0 - smoothstep(front - aa, front + aa, sceneUv.y);
}
float menuQuiet(float2 uv) {
    if (menuVisibility <= 0.0 || menuRectUv.z <= 0.0 || menuRectUv.w <= 0.0)
        return 0.0;
    float aspect = resolution.x / max(1.0, resolution.y);
    float2 center = menuRectUv.xy + menuRectUv.zw * 0.5;
    float feather = lerp(0.08, 0.11, saturate(childActivity));
    float2 extent = max(menuRectUv.zw * 0.5 + float2(feather / aspect, feather), float2(0.001, 0.001));
    float2 distance = (uv - center) / extent;
    return exp(-1.35 * dot(distance, distance)) * saturate(menuVisibility);
}

float3 procedural(float2 uv, float t) {
    float aspect = resolution.x / max(1.0, resolution.y);
    float2 p = (uv - 0.5) * float2(aspect, 1.0);
    float r = length(p), angle = atan2(p.y, p.x);
    float localDarkness = themeDarkness(uv);
    const float3 white = lerp(float3(0.96, 0.96, 0.96), float3(0.76, 0.76, 0.76), localDarkness);
    const float3 silver = lerp(float3(0.76, 0.76, 0.76), float3(0.43, 0.43, 0.43), localDarkness);
    const float3 grey = lerp(float3(0.56, 0.56, 0.56), float3(0.25, 0.25, 0.25), localDarkness);
    const float3 amber = silver;
    const float3 teal = grey;
    const float3 centerColor = lerp(float3(0.96, 0.96, 0.96), float3(0.135, 0.135, 0.135), localDarkness);
    const float3 edgeColor = lerp(float3(0.81, 0.81, 0.81), float3(0.065, 0.065, 0.065), localDarkness);
    float3 color = lerp(centerColor, edgeColor, saturate(r));
    if (highContrast > 0.5) return lerp(float3(1, 1, 1), float3(0, 0, 0), localDarkness);
    const float quiet = menuQuiet(uv);
    const float detail = (1.0 - quiet * 0.78) * (1.0 - smoothstep(0.25, 0.85, r) * 0.55) *
        (1.0 - saturate(fpsPressure) * 0.65) * (1.0 - saturate(reducedMotion) * 0.5);
    const float3 surface = color + exp(-r * r * 8.0) * lerp(0.06, 0.03, localDarkness);
    color += 0.03 * (frost(uv * 6.0 + float2(t * 0.03, -t * 0.02)) - 0.5);
    color += lerp(float3(0.06, 0.06, 0.06), float3(0.03, 0.03, 0.03), localDarkness) * exp(-r * r * 8.0);
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
    float sweep = smoothstep(0.0, 0.08, darkness) * (1.0 - smoothstep(0.92, 1.0, darkness)) *
        saturate(abs(themeDirection)) * (1.0 - saturate(reducedMotion));
    if (sweep > 0.0) {
        float2 sceneUv = hostPosition(uv);
        float column = clamp(floor(sceneUv.x * 48.0), 0.0, 47.0);
        float columnSeed = noise2(float2(column * 3.1, t * 0.55 + 29.0));
        float detailSeed = noise2(float2(column * 2.3 + 11.0, t * 0.45 + 7.0));
        float speedSeed = hash21(float2(column, 91.0));
        float lengthSeed = hash21(float2(column, 121.0));
        float widthSeed = noise2(float2(column * 4.7 + 5.0, t * 0.28 + 17.0));
        float distance = sceneUv.y - themeFront(sceneUv);
        float trailDistance = -distance * themeDirection;
        float laneWidth = lerp(0.10, 0.32, detailSeed);
        float lane = 1.0 - smoothstep(laneWidth * 0.35, laneWidth, abs(frac(sceneUv.x * 48.0) - 0.5));
        float headWidth = max(1.0 / max(1.0, resolution.y), 0.0022) * lerp(1.0, 3.4, widthSeed);
        float head = band(distance, headWidth);
        float tailLength = lerp(0.035, 0.36, lengthSeed * lengthSeed) * lerp(0.85, 1.15, columnSeed);
        float tail = step(0.0, trailDistance) * (1.0 - smoothstep(0.0, tailLength, trailDistance));
        float baseSpeed = lerp(8.0, 26.0, speedSeed);
        float rate = lerp(0.7, 1.6, hash21(float2(column, 53.0)));
        float travel = t * baseSpeed + baseSpeed * 0.30 / rate *
            sin(t * rate + hash21(float2(column, 17.0)) * 6.2831853);
        float packetY = sceneUv.y * lerp(56.0, 116.0, hash21(float2(column, 43.0))) - themeDirection * travel;
        float packetSeed = hash21(float2(floor(packetY), column));
        float packetLength = hash21(float2(floor(packetY), column + 41.0));
        float packetEnd = lerp(0.20, 0.92, packetLength * 0.8 + detailSeed * 0.2);
        float packet = smoothstep(0.18, 0.38, packetSeed + (columnSeed - 0.5) * 0.3) *
            smoothstep(0.05, 0.16, frac(packetY)) *
            (1.0 - smoothstep(packetEnd - 0.16, packetEnd, frac(packetY)));
        float3 streamColor = lerp(float3(0.60, 0.60, 0.60), float3(0.94, 0.94, 0.94), head);
        float filament = step(0.74, lengthSeed) * tail *
            (1.0 - smoothstep(laneWidth * 0.16, laneWidth * 0.4, abs(frac(sceneUv.x * 48.0) - 0.5)));
        color = lerp(color, streamColor,
            saturate(sweep * (lane * (head * 0.75 + tail * packet * 0.48) + filament * 0.18)));
    }
    color *= 0.975 + 0.025 * sin(uv.y * resolution.y * 1.5);
    return saturate(lerp(surface, color, detail) * clamp(1.0 - r * 0.18, 0.82, 1.0));
}

float3 lensSample(float2 uv, float split) {
    return float3(fieldTexture.Sample(fieldSampler, uv + float2(split, 0)).r,
                  fieldTexture.Sample(fieldSampler, uv).g,
                  fieldTexture.Sample(fieldSampler, uv - float2(split, 0)).b);
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float t = reducedMotion > 0.5 ? 0.0 : time;
    if (renderPass < 0.5) return float4(procedural(uv, t), 1.0);
    if (highContrast > 0.5) {
        float alpha = saturate(openness);
        return float4(lerp(float3(1, 1, 1), float3(0, 0, 0), themeDarkness(uv)) * alpha, alpha);
    }
    float quiet = menuQuiet(uv);
    float2 p = uv - 0.5;
    float r2 = dot(p, p), r = sqrt(r2);
    float edge = 1.0 - saturate(openness);
    float strength = 0.44 + edge * 0.18 + sin(t * 0.4) * 0.02;
    float2 drift = float2(sin(t * 0.62) * 0.006, cos(t * 0.51) * 0.004);
    float2 direction = p / max(r, 0.001);
    float spread = (0.0008 + r2 * 0.003) * (1.0 + edge);
    float split = 0.0015 + r2 * 0.004;
    spread *= 1.0 - quiet * 0.85;
    split *= 1.0 - quiet;
    uint fieldWidth, fieldHeight;
    fieldTexture.GetDimensions(fieldWidth, fieldHeight);
    float2 sampleMargin = min(float2(0.49, 0.49),
        abs(direction) * spread * 2.0 + float2(split, 0.0) +
        0.5 / float2(fieldWidth, fieldHeight));
    float2 edgeUv = lerp(sampleMargin, 1.0 - sampleMargin, uv);
    float2 edgeWeight = smoothstep(float2(0.6, 0.6), float2(1.0, 1.0), abs(p) * 2.0);
    float2 warped = lerp(uv + p * strength * r2 + drift, edgeUv, edgeWeight);
    warped = lerp(warped, clamp(uv, sampleMargin, 1.0 - sampleMargin), quiet * 0.6);
    float3 color = lensSample(warped - direction * spread * 2.0, split) * 0.10;
    color += lensSample(warped - direction * spread, split) * 0.20;
    color += lensSample(warped, split) * 0.36;
    color += lensSample(warped + direction * spread, split) * 0.22;
    color += lensSample(warped + direction * spread * 2.0, split) * 0.12;
    color *= (1.0 - (0.34 + edge * 0.18)) * (1.0 - smoothstep(0.25, 0.75, r) * 0.12);
    color *= 0.985 + 0.015 * sin(uv.y * 980.0);
    float ring = band(r - lerp(0.18, 0.72, saturate(openness)), 0.0025);
    color += lerp(0.58, 0.42, themeDarkness(saturate(warped))) * ring * 0.035 *
        (1.0 - quiet) * (1.0 - saturate(fpsPressure));
    float alpha = saturate(openness);
    return float4(saturate(color) * alpha, alpha);
}
