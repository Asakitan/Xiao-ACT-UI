Texture2D sceneTexture : register(t0);
Texture2D bloomTexture : register(t1);
Texture2D wideBloomTexture : register(t2);
SamplerState linearSampler : register(s0);

cbuffer Constants : register(b0) {
    float2 resolution; float time; float sceneTime;
    float phaseProgress; float scenePhase; float connectedAlpha; float reducedMotion;
    float cameraZ; float alphaMul; float radiusMul; float energy;
    float flash; float startupBurst; float startupWave; float motionMix;
    float coolMix; float2 blurDirection; float bloomExtract;
    float3 backgroundColor; float padding1;
    float3 effectTint; float padding2;
};

float3 bloom_sample(float2 uv) {
    const float3 color = max(0.0, sceneTexture.Sample(linearSampler, uv).rgb);
    const float peak = max(color.r, max(color.g, color.b));
    const float knee = clamp(peak - 0.25, 0.0, 0.50);
    const float contribution = max(peak - 0.50, knee * knee);
    return bloomExtract > 0.5 ? color * contribution / max(peak, 0.0001) : color;
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    if (abs(blurDirection.x) + abs(blurDirection.y) > 0.1) {
        const float2 delta = blurDirection / max(resolution, 1.0);
        float3 color = bloom_sample(uv) * 0.227027;
        color += bloom_sample(uv + delta * 1.384615) * 0.316216;
        color += bloom_sample(uv - delta * 1.384615) * 0.316216;
        color += bloom_sample(uv + delta * 3.230769) * 0.070270;
        color += bloom_sample(uv - delta * 3.230769) * 0.070270;
        return float4(color, 1.0);
    }

    float3 color = max(0.0, sceneTexture.Sample(linearSampler, uv).rgb);
    if (reducedMotion < 0.5) {
        color += bloomTexture.Sample(linearSampler, uv).rgb * 0.24;
        color += wideBloomTexture.Sample(linearSampler, uv).rgb * 0.10;
    }
    const float2 p = (uv - 0.5) * float2(resolution.x / max(1.0, resolution.y), 1.0);
    color *= 1.0 - smoothstep(0.38, 1.3, length(p)) * 0.18;
    const float peak = max(color.r, max(color.g, color.b));
    color /= 1.0 + peak;
    color = lerp(color * 12.92, 1.055 * pow(max(color, 0.0), 1.0 / 2.4) - 0.055,
                  step(0.0031308, color));
    const float dither = frac(52.9829189 * frac(dot(floor(position.xy),
                                                    float2(0.06711056, 0.00583715))));
    color = saturate(color + (dither - 0.5) / 255.0);
    const float entrance = reducedMotion > 0.5 ? 1.0 : smoothstep(0.0, 0.38, time);
    const float alpha = saturate(connectedAlpha) * entrance;
    return float4(color * alpha, alpha);
}