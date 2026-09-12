Texture2D sceneTexture : register(t0);
Texture2D bloomTexture : register(t1);
Texture2D wideBloomTexture : register(t2);
Texture2D historyTexture : register(t3);
SamplerState linearSampler : register(s0);

cbuffer Constants : register(b0) {
    float2 resolution; float time; float sceneTime;
    float flightSpan; float historyScale; float connectedAlpha; float reducedMotion;
    float cameraZ; float alphaMul; float radiusMul; float energy;
    float flash; float birthLead; float startupWave; float motionMix;
    float coolMix; float2 blurDirection; float bloomExtract;
    float3 backgroundColor; float padding1;
    float3 effectTint; float exitProgress;
};

float3 bloom_sample(float2 uv) {
    const float3 color = max(0.0, sceneTexture.Sample(linearSampler, uv).rgb);
    const float peak = max(color.r, max(color.g, color.b));
    const float knee = clamp(peak - 0.90, 0.0, 0.40);
    const float contribution = max(peak - 1.10, knee * knee * 0.625);
    const float chroma = peak - min(color.r, min(color.g, color.b));
    return bloomExtract > 0.5 ? color * contribution * lerp(0.35, 1.0, smoothstep(0.04, 0.30, chroma)) /
                                max(peak, 0.0001) : color;
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

    const float aspect = resolution.x / max(1.0, resolution.y);
    const float2 p = (uv - 0.5) * float2(aspect, 1.0);
    const float radius = length(p);
    if (bloomExtract > 1.5) {
    const float2 direction = p / max(radius, 0.0001) / float2(aspect, 1.0);
    const float speed = reducedMotion > 0.5 ? 0.0 : saturate(motionMix);
    const float smear = min(7.0 / max(resolution.y, 1.0), 0.009 * speed) *
                         speed * smoothstep(0.03, 0.50, radius);
    float3 color = 0.0;
    const float weights[5] = {0.40, 0.25, 0.18, 0.11, 0.06};
    [unroll] for (int sampleIndex = 0; sampleIndex < 5; ++sampleIndex) {
        const float2 sampleUv = uv - direction * smear * float(sampleIndex) * 0.40;
        const float3 sampleColor = sceneTexture.Sample(linearSampler, sampleUv).rgb;
        color += sampleColor * weights[sampleIndex];
    }
    const float split = speed * smoothstep(0.18, 0.65, radius) * 0.65 / max(1.0, resolution.x);
    const float2 dispersed = float2(sceneTexture.Sample(linearSampler, uv + direction * split).r,
                                    sceneTexture.Sample(linearSampler, uv - direction * split).b);
    color.rb = lerp(color.rb, dispersed, speed * 0.16);
    float3 glow = 0.0;
    if (reducedMotion < 0.5) {
        glow = bloomTexture.Sample(linearSampler, uv).rgb * 0.22 +
               wideBloomTexture.Sample(linearSampler, uv).rgb * 0.08;
    }
    const float2 historyUv = 0.5 + (uv - 0.5) / max(1.0, historyScale);
    const float3 history = max(0.0, historyTexture.Sample(linearSampler, historyUv).rgb);
    color = lerp(max(0.0, color + glow), history, saturate(padding1));
    return float4(color, 1.0);
    }
    float3 color = max(0.0, sceneTexture.Sample(linearSampler, uv).rgb);
    const float2 texel = 1.0 / max(resolution, 1.0);
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    const float north = dot(sceneTexture.Sample(linearSampler, uv - float2(0, texel.y)).rgb, luma);
    const float south = dot(sceneTexture.Sample(linearSampler, uv + float2(0, texel.y)).rgb, luma);
    const float east = dot(sceneTexture.Sample(linearSampler, uv + float2(texel.x, 0)).rgb, luma);
    const float west = dot(sceneTexture.Sample(linearSampler, uv - float2(texel.x, 0)).rgb, luma);
    const float center = dot(color, luma);
    const float high = max(center, max(max(north, south), max(east, west)));
    const float low = min(center, min(min(north, south), min(east, west)));
    const float2 gradient = float2(east - west, south - north);
    const float2 edgeOffset = gradient / max(length(gradient), 0.0001) * texel * 0.65;
    const float edge = smoothstep(max(0.02, high * 0.12), max(0.04, high * 0.28), high - low);
    const float3 softened = (sceneTexture.Sample(linearSampler, uv + edgeOffset).rgb +
                             sceneTexture.Sample(linearSampler, uv - edgeOffset).rgb) * 0.5;
    color = lerp(color, softened, edge * 0.40);
    color *= 1.0 - smoothstep(0.35, 1.25, radius) * 0.018;
    const float peak = max(color.r, max(color.g, color.b));
    const float shoulder = 0.96 + 0.04 * (1.0 - exp(-max(0.0, peak - 0.96) * 4.0));
    color *= min(1.0, shoulder / max(peak, 0.0001));
    color = lerp(color * 12.92, 1.055 * pow(max(color, 0.0), 1.0 / 2.4) - 0.055,
                  step(0.0031308, color));
    const float dither = frac(52.9829189 * frac(dot(floor(position.xy),
                                                    float2(0.06711056, 0.00583715))));
    color = saturate(color + (dither - 0.5) / 255.0);
    float alpha = saturate(connectedAlpha);
    if (reducedMotion < 0.5 && exitProgress > 0.80) {
        const float edgeWidth = 1.5 / max(1.0, resolution.y);
        const float extent = length(float2(aspect, 1.0)) * 0.5 + edgeWidth * 3.0;
        const float aperture = extent * smoothstep(0.80, 1.0, exitProgress);
        alpha *= smoothstep(aperture - edgeWidth, aperture + edgeWidth, radius);
    }
    return float4(color * alpha, alpha);
}