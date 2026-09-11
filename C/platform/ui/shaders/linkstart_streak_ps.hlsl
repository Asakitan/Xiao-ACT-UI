struct PixelInput {
    float4 position : SV_POSITION;
    float2 beam : TEXCOORD0;
    float3 viewPosition : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float3 color : COLOR;
    float alpha : ALPHA;
};

float4 main(PixelInput input) : SV_TARGET {
    const float3 normal = normalize(input.normal);
    const float3 view = normalize(float3(input.viewPosition.xy, max(1.0, input.viewPosition.z)));
    const float facing = saturate(abs(dot(normal, view)));
    const float coverage = smoothstep(0.0, max(fwidth(facing), 0.015), facing);
    const float roundedBody = 0.30 + 0.95 * sqrt(facing);
    const float highlight = pow(saturate(dot(normal, normalize(float3(-0.5, 0.65, -0.35)))), 8.0);
    const float tail = 1.0 - smoothstep(0.82, 1.0, input.beam.y);
    const float head = smoothstep(0.0, 0.008, input.beam.y);
    const float atmosphere = exp2(-max(0.0, input.viewPosition.z - 700.0) / 6000.0);
    const float3 coreColor = lerp(input.color, float3(0.65, 0.82, 1.0), highlight * 0.08);
    const float3 radiance = coreColor * (roundedBody * 1.70 + highlight * 0.75);
    return float4(radiance * input.alpha * coverage * head * tail * atmosphere * 0.65, 0.0);
}