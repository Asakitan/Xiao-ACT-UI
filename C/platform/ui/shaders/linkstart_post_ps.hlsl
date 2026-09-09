Texture2D sceneTexture : register(t0);
Texture2D bloomTexture : register(t1);
SamplerState linearSampler : register(s0);

cbuffer Constants : register(b0) {
    float2 resolution; float time; float progress;
    float scenePhase; float connectedAlpha; float reducedMotion; float padding0;
    float2 blurDirection; float2 padding1;
};

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    if (abs(blurDirection.x) + abs(blurDirection.y) > 0.1) {
        const float2 delta = blurDirection / resolution;
        float4 sample = sceneTexture.Sample(linearSampler, uv) * 0.227027;
        sample += sceneTexture.Sample(linearSampler, uv + delta * 1.384615) * 0.316216;
        sample += sceneTexture.Sample(linearSampler, uv - delta * 1.384615) * 0.316216;
        sample += sceneTexture.Sample(linearSampler, uv + delta * 3.230769) * 0.070270;
        sample += sceneTexture.Sample(linearSampler, uv - delta * 3.230769) * 0.070270;
        const float luminance = max(sample.r, max(sample.g, sample.b));
        return sample * saturate((luminance - 0.12) * 4.0);
    }
    const float4 base = sceneTexture.Sample(linearSampler, uv);
    const float4 glow = bloomTexture.Sample(linearSampler, uv);
    const float2 centered = uv - 0.5;
    const float vignette = saturate(1.0 - dot(centered, centered) * 1.1);
    const float flash = saturate(scenePhase - 3.5) * connectedAlpha * 0.08;
    float3 rgb = (base.rgb + glow.rgb * 1.22) * vignette +
                       flash * float3(0.65, 0.9, 1.0);
    // Accelerate through the vanishing point into a clean white field. The
    // welcome phase then reveals the real UI by fading premultiplied alpha.
    const float whiteField = scenePhase >= 3.5 ? 1.0 :
        (scenePhase >= 2.5 && reducedMotion < 0.5 ? smoothstep(0.64, 0.91, progress) : 0.0);
    rgb = lerp(rgb, float3(0.973, 0.973, 0.973), whiteField);
    const float alpha = scenePhase >= 3.5 ? saturate(connectedAlpha) : 1.0;
    return float4(rgb * alpha, alpha);
}
