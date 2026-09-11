cbuffer Constants : register(b0) {
    float2 resolution; float time; float sceneTime;
    float phaseProgress; float scenePhase; float connectedAlpha; float reducedMotion;
    float cameraZ; float alphaMul; float radiusMul; float energy;
    float flash; float startupBurst; float startupWave; float motionMix;
    float coolMix; float2 blurDirection; float bloomExtract;
    float3 backgroundColor; float padding1;
    float3 effectTint; float padding2;
};

float hash11(float p) {
    p = frac(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return frac(p);
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    const float2 centered = uv - 0.5;
    const float aspect = resolution.x / max(1.0, resolution.y);
    const float2 lens = float2(centered.x * aspect, centered.y);
    const float radius = length(lens);
    const float angle = atan2(lens.y, lens.x);
    const float liveTime = reducedMotion > 0.5 ? 0.0 : time;
    const float liveEnergy = saturate(energy);
    const float liveFlash = saturate(flash);
    const float start = saturate(startupWave);
    const float burst = saturate(startupBurst);

    const float apertureOpen = pow(smoothstep(0.02, 0.42, start), 0.78);
    const float apertureFade = 1.0 - smoothstep(0.56, 0.96, start);
    const float slitX = lerp(0.10, 1.25, apertureOpen);
    const float slitY = lerp(0.008, 0.56, apertureOpen);
    const float2 apertureUv = float2(lens.x / max(slitX, 0.001),
                                     centered.y / max(slitY, 0.001));
    const float apertureMask = 1.0 - smoothstep(0.90, 1.10, dot(apertureUv, apertureUv));
    const float shutterMask = (1.0 - apertureMask) * apertureFade;
    float valveLine = exp(-abs(centered.y) * lerp(300.0, 56.0, apertureOpen));
    valveLine *= 1.0 - smoothstep(slitX * 0.10, slitX * 0.92, abs(lens.x));
    valveLine *= (0.08 + burst * 0.56 + (1.0 - apertureFade) * 0.10) * apertureFade;

    const float spokeCount = 24.0;
    const float angular = (angle / 6.2831853 + 0.5) * spokeCount;
    const float cell = floor(angular);
    const float ray = abs(frac(angular + liveTime * 0.045) - 0.5);
    const float jitter = hash11(cell) * 0.12;
    const float rayMask = 1.0 - smoothstep(0.03 + radius * 0.04, 0.22 + jitter, ray);
    const float rayFade = (1.0 - smoothstep(0.08, 1.08, radius)) *
                          pow(max(0.0, 1.0 - radius), 1.65);
    const float rays = rayMask * rayFade * (0.022 + liveEnergy * 0.055);

    const float core = 1.0 - smoothstep(0.0, 0.16, radius);
    const float halo = 1.0 - smoothstep(0.05, 0.48, radius);
    float flareLine = exp(-abs(centered.y) * (96.0 - liveEnergy * 24.0));
    flareLine *= exp(-lens.x * lens.x * 28.0);
    flareLine *= liveEnergy * 0.030;

    const float contract = smoothstep(0.0, 0.18, start) *
                           (1.0 - smoothstep(0.18, 0.36, start));
    const float explode = smoothstep(0.18, 0.44, start);
    const float scan = smoothstep(0.38, 0.78, start) *
                       (1.0 - smoothstep(0.78, 1.0, start));
    const float waveRadius = lerp(0.010, 0.64, explode);
    const float waveWidth = lerp(0.018, 0.070, burst);
    float shock = 1.0 - smoothstep(0.0, waveWidth, abs(radius - waveRadius));
    shock *= 1.0 - smoothstep(0.70, 1.0, start);
    const float startupCore = (1.0 - smoothstep(0.0, 0.24 - contract * 0.08, radius)) * burst;
    const float startupFlare = exp(-abs(centered.y) * 128.0) *
                               (1.0 - smoothstep(0.0, 0.82, abs(centered.x))) *
                               (0.10 + burst * 0.42) * apertureFade;
    const float scanRing = (1.0 - smoothstep(0.0, 0.015,
                                      abs(radius - lerp(0.06, 0.72, scan)))) * scan * 0.65;
    const float ripple = (1.0 - smoothstep(0.0, 0.022,
                                    abs(radius - lerp(0.05, 0.56, explode)))) *
                         (1.0 - smoothstep(0.52, 0.92, start)) *
                         (0.18 + burst * 0.34);

    const float pixelWidth = max(fwidth(radius), 0.0005);
    float guideRings = 0.0;
    [unroll]
    for (int ring = 0; ring < 4; ++ring) {
        const float travel = frac(liveTime * 0.24 + float(ring) * 0.25);
        const float ringRadius = 0.055 + travel * travel * 0.92;
        const float outline = 1.0 - smoothstep(pixelWidth, pixelWidth * 2.5,
                                               abs(radius - ringRadius));
        const float arc = smoothstep(-0.15, 0.25, sin(angle * 3.0 + float(ring) * 1.7));
        guideRings += outline * arc * smoothstep(0.0, 0.14, travel) *
                      (1.0 - smoothstep(0.72, 1.0, travel));
    }

    float3 color = backgroundColor;
    const float3 shutterColor = lerp(float3(0.005, 0.010, 0.018),
                                     backgroundColor * 0.16, apertureOpen * 0.34);
    color += effectTint * (core * (0.08 + liveFlash * 0.12));
    color += effectTint * (halo * 0.08 + rays + flareLine);
    color += effectTint * guideRings * liveEnergy * 0.018 * (1.0 - step(0.5, reducedMotion));
    color += float3(1.0, 0.94, 0.82) * startupCore * (0.22 + burst * 0.38);
    color += effectTint * shock * (0.16 + burst * 0.34);
    color += float3(0.92, 0.98, 1.0) * startupFlare;
    color += float3(0.70, 0.95, 1.0) * scanRing;
    color += effectTint * ripple;
    color += float3(1.0, 0.97, 0.88) * valveLine;
    color = lerp(shutterColor, color,
                 max(apertureMask * (0.22 + apertureOpen * 0.78), 1.0 - apertureFade));
    color += float3(0.86, 0.96, 1.0) * shutterMask * 0.032;
    color = max(0.0, color);
    const float3 linearColor = lerp(color / 12.92, pow((color + 0.055) / 1.055, 2.4),
                                    step(0.04045, color));
    return float4(linearColor, 1.0);
}
