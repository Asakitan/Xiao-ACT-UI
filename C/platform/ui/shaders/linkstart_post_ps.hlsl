Texture2D sceneTexture : register(t0);
Texture2D bloomTexture : register(t1);
SamplerState linearSampler : register(s0);

cbuffer Constants : register(b0) {
    float2 resolution; float time; float sceneTime;
    float phaseProgress; float scenePhase; float connectedAlpha; float reducedMotion;
    float cameraZ; float alphaMul; float radiusMul; float energy;
    float flash; float startupBurst; float startupWave; float motionMix;
    float coolMix; float2 blurDirection; float padding0;
    float3 backgroundColor; float padding1;
    float3 effectTint; float padding2;
};

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    if (abs(blurDirection.x) + abs(blurDirection.y) > 0.1) {
        const float2 delta = blurDirection / resolution;
        float4 sampleColor = sceneTexture.Sample(linearSampler, uv) * 0.227027;
        sampleColor += sceneTexture.Sample(linearSampler, uv + delta * 1.384615) * 0.316216;
        sampleColor += sceneTexture.Sample(linearSampler, uv - delta * 1.384615) * 0.316216;
        sampleColor += sceneTexture.Sample(linearSampler, uv + delta * 3.230769) * 0.070270;
        sampleColor += sceneTexture.Sample(linearSampler, uv - delta * 3.230769) * 0.070270;
        const float luminance = max(sampleColor.r, max(sampleColor.g, sampleColor.b));
        return sampleColor * saturate((luminance - 0.10) * 4.4);
    }

    const float2 centered = uv - 0.5;
    const float aspect = resolution.x / max(1.0, resolution.y);
    const float2 lens = float2(centered.x * aspect, centered.y);
    const float radius = length(lens);
    const float2 direction = radius > 0.0001 ? lens / radius : float2(0.0, 0.0);
    const float liveEnergy = saturate(energy);
    const float liveFlash = saturate(flash);
    const float2 smear = direction * (0.016 * liveEnergy + 0.022 * liveFlash);
    const float2 squeeze = float2(1.0 + liveFlash * 0.015,
                                  1.0 - liveEnergy * 0.010);
    const float2 zoomUv = centered * squeeze + 0.5;
    const float3 sample0 = sceneTexture.Sample(linearSampler, saturate(zoomUv)).rgb;
    const float3 sample1 = sceneTexture.Sample(linearSampler, saturate(zoomUv - smear * 0.8)).rgb;
    const float3 sample2 = sceneTexture.Sample(linearSampler, saturate(zoomUv - smear * 1.8)).rgb;
    const float3 smeared = sample0 * 0.46 + sample1 * 0.34 + sample2 * 0.20;
    const float chroma = (2.0 / max(1.0, resolution.x)) *
                         (1.0 + liveEnergy * 0.9 + liveFlash * 0.8);
    const float red = sceneTexture.Sample(linearSampler, saturate(uv + float2(chroma, 0.0))).r;
    const float blue = sceneTexture.Sample(linearSampler, saturate(uv - float2(chroma, 0.0))).b;
    float3 result = float3(red, smeared.g, blue);

    const float3 bloom = bloomTexture.Sample(linearSampler, uv).rgb;
    const float centerGlow = pow(max(0.0, 1.0 - radius * 1.85), 2.6);
    float horizontal = exp(-abs(centered.y) * (74.0 - 22.0 * liveEnergy));
    horizontal *= smoothstep(0.52, 0.0, abs(centered.x));
    const float outerGlow = pow(max(0.0, 1.0 - radius * 0.92), 1.8) *
                            (0.010 + liveEnergy * 0.020 + liveFlash * 0.018);
    const float localBloom = centerGlow *
                             (0.055 + liveEnergy * 0.085 + liveFlash * 0.080);
    const float flare = horizontal * (liveEnergy * 0.125 + liveFlash * 0.180);
    result += bloom * 1.18 + effectTint * (localBloom + flare + outerGlow);
    result = lerp(result, result + effectTint * 0.12, liveFlash * centerGlow);
    result *= lerp(0.92, 1.04, smoothstep(1.22, 0.18, radius));

    const float connected = step(3.5, scenePhase);
    const float whiteField = connected *
        (reducedMotion > 0.5 ? 1.0 : smoothstep(0.0, 0.56, phaseProgress));
    const float blueFlash = connected * (reducedMotion > 0.5 ? 0.0 :
        (1.0 - smoothstep(0.0, 0.18, phaseProgress)));
    result += float3(0.45, 0.80, 1.0) * blueFlash * (0.18 + centerGlow * 0.34);
    result = lerp(saturate(result), float3(0.973, 0.973, 0.973), whiteField);
    const float alpha = connected > 0.5 ? saturate(connectedAlpha) : 1.0;
    return float4(result * alpha, alpha);
}
