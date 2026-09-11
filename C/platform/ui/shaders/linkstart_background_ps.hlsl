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
    const float aa = max(fwidth(r), 0.0001);
    return 1.0 - smoothstep(width, width + aa, abs(r - target));
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    const float aspect = resolution.x / max(1.0, resolution.y);
    const float2 p = (uv - 0.5) * float2(aspect, 1.0);
    const float r = length(p), angle = atan2(p.y, p.x);
    const float t = reducedMotion > 0.5 ? 0.0 : time;
    const float active = reducedMotion > 0.5 ? 0.0 : energy;
    const float core = exp(-r * r * 24.0);
    const float halo = exp(-r * r * 3.0);
    float3 color = lerp(backgroundColor, float3(1.0, 1.0, 1.0), halo * 0.52 + core * 0.20);
    const float spoke = 0.5 + 0.5 * cos(angle * 18.0 + t * 0.025);
    const float rays = pow(spoke, 6.0) * exp(-r * 4.0) * active;
    color -= (1.0 - halo) * float3(0.014, 0.010, 0.004);
    color += effectTint * rays * 0.012;
    if (reducedMotion < 0.5 && startupWave < 1.0) {
        const float scan = smoothstep(0.08, 0.96, startupWave);
        const float rings = ring(r, lerp(0.06, 0.72, scan), 0.0015) +
                            ring(r, lerp(0.035, 0.56, scan), 0.0007) * 0.45;
        color -= float3(0.20, 0.13, 0.08) * rings * sin(scan * 3.141593) * 0.30;
        color = lerp(color, float3(1.0, 1.0, 1.0), core * startupBurst * 0.20);
    }
    color = lerp(color, float3(1.0, 1.0, 1.0), flash * halo);
    color = saturate(color);
    const float3 linearColor = lerp(color / 12.92, pow((color + 0.055) / 1.055, 2.4),
                                    step(0.04045, color));
    return float4(linearColor, 1.0);
}
