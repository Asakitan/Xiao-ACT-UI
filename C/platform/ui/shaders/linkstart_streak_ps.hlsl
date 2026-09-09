struct PixelInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float gold : GOLD; };

float4 main(PixelInput input) : SV_TARGET {
    const float edge = saturate(1.0 - abs(input.uv.y - 0.5) * 2.0);
    const float tail = saturate(input.uv.x);
    const float alpha = edge * tail * 0.48;
    const float3 color = lerp(float3(0.25, 0.86, 1.0), float3(1.0, 0.64, 0.12), input.gold);
    return float4(color * alpha, alpha);
}
