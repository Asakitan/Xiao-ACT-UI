cbuffer Constants : register(b0) {
    float2 resolution; float time; float progress;
    float scenePhase; float connectedAlpha; float reducedMotion; float padding0;
    float2 blurDirection; float2 padding1;
};

float hash21(float2 p) {
    p = frac(p * float2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    const float t = reducedMotion > 0.5 ? 0.0 : time;
    const float2 q = (uv - 0.5) * float2(resolution.x / resolution.y, 1.0);
    const float radius = length(q);
    const float angle = atan2(q.y, q.x);
    const float tunnel = saturate(1.0 - abs(scenePhase - 1.0));
    const float reveal = saturate(1.0 - abs(scenePhase - 2.0));
    const float burst = saturate(1.0 - abs(scenePhase - 3.0));
    const float pulse = 0.5 + 0.5 * sin(t * 2.1);
    const float rings = pow(saturate(1.0 - abs(frac(radius * (7.0 + burst * 5.0) -
        t * (0.42 + burst * 0.25)) - 0.5) * 13.0), 3.0);
    const float spokes = pow(saturate(0.5 + 0.5 * cos(angle * (20.0 + burst * 12.0) +
        t * 0.35)), 24.0) * saturate(radius * 2.0);
    const float star = step(0.994, hash21(floor(uv * resolution / 7.0) + floor(t * 0.12))) *
        (1.0 - saturate(radius));
    const float burstRing = pow(saturate(1.0 - abs(radius - progress * 0.72) * 22.0), 3.0) * burst;
    const float3 deep = lerp(float3(0.012, 0.02, 0.038), float3(0.035, 0.075, 0.095),
                             saturate(1.0 - radius));
    const float3 cyan = float3(0.18, 0.78, 1.0);
    const float3 gold = float3(1.0, 0.63, 0.12);
    float3 color = deep + cyan * (rings * (0.06 + 0.11 * tunnel) +
                                  spokes * (0.035 + 0.08 * burst)) +
                   lerp(cyan, gold, pulse) * star * 0.65;
    color += cyan * pow(saturate(1.0 - radius * 1.8), 4.0) * (0.06 + 0.05 * pulse) +
             gold * burstRing * 0.72;
    color += gold * reveal * pow(saturate(1.0 - radius * 2.4), 6.0) * 0.08;
    color += float3(0.72, 0.92, 1.0) * connectedAlpha * saturate(scenePhase - 3.5) *
             pow(saturate(1.0 - radius * 1.5), 5.0) * 0.14;
    return float4(color, 1.0);
}
