cbuffer Constants : register(b0) {
    float2 resolution; float time; float sceneTime;
    float phaseProgress; float scenePhase; float connectedAlpha; float reducedMotion;
    float cameraZ; float alphaMul; float radiusMul; float energy;
    float flash; float startupBurst; float startupWave; float motionMix;
    float coolMix; float2 blurDirection; float bloomExtract;
    float3 backgroundColor; float padding1;
    float3 effectTint; float padding2;
};

float ring(float r, float target, float width) {
    return 1.0 - smoothstep(width * 0.35, width, abs(r - target));
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    const float aspect = resolution.x / max(1.0, resolution.y);
    const float2 p = (uv - 0.5) * float2(aspect, 1.0);
    const float r = length(p), angle = atan2(p.y, p.x);
    const float t = reducedMotion > 0.5 ? 0.0 : time;
    const float active = reducedMotion > 0.5 ? 0.0 : energy;
    const float opening = pow(smoothstep(0.02, 0.44, startupWave), 0.78);
    const float shutterFade = 1.0 - smoothstep(0.58, 0.98, startupWave);
    const float2 apertureSize = lerp(float2(0.10, 0.008), float2(aspect * 0.72, 0.65), opening);
    const float ellipse = dot(p / apertureSize, p / apertureSize);
    const float aperture = 1.0 - smoothstep(0.92, 1.08, ellipse);
    float3 color = backgroundColor * (0.88 + exp(-r * r * 3.0) * 0.20);
    const float core = exp(-r * r * 90.0);
    const float halo = exp(-r * r * 9.0);
    const float spoke = abs(frac((angle / 6.2831853 + 0.5) * 26.0 - t * 0.065) - 0.5);
    const float rays = pow(saturate(1.0 - spoke * 5.0), 3.0) * exp(-r * 5.5) * active;
    color += effectTint * (core * (0.10 + active * 0.28) + halo * (0.045 + active * 0.05) + rays * 0.22);
    const float flare = exp(-abs(p.y) * 150.0) * exp(-abs(p.x) * 7.0);
    color += lerp(effectTint, float3(0.88, 0.96, 1.0), 0.65) * flare * (0.05 + active * 0.10);
    if (reducedMotion < 0.5 && startupWave < 1.0) {
        const float explode = smoothstep(0.16, 0.56, startupWave);
        const float scan = smoothstep(0.36, 0.90, startupWave);
        color += float3(1.0, 0.92, 0.72) * core * startupBurst * 0.80;
        color += effectTint * ring(r, lerp(0.02, 0.62, explode), 0.014) * (1.0 - explode) * 0.55;
        color += float3(0.48, 0.86, 1.0) * ring(r, lerp(0.06, 0.70, scan), 0.0035) *
                 sin(scan * 3.141593) * 0.48;
        color += float3(1.0, 0.92, 0.74) * flare * (0.30 + startupBurst * 0.50);
        color = lerp(float3(0.004, 0.009, 0.019), color, max(aperture, 1.0 - shutterFade));
        color += float3(0.42, 0.75, 0.92) * ring(ellipse, 1.0, 0.08) * shutterFade * 0.08;
    }
    color += float3(0.55, 0.86, 1.0) * flash * exp(-r * r * 3.5) * 0.80;
    color = saturate(color);
    const float3 linearColor = lerp(color / 12.92, pow((color + 0.055) / 1.055, 2.4),
                                    step(0.04045, color));
    return float4(linearColor, 1.0);
}
