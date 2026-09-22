Texture2D layerTexture : register(t0);
SamplerState layerSampler : register(s0);

cbuffer LayerConstants : register(b0) {
    float2 viewportSize;
    float2 originPx;
    float2 layerSize;
    float opacity;
    float straightAlpha;
};

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = layerTexture.Sample(layerSampler, uv);
    if (straightAlpha > 0.5)
        color.rgb *= color.a;
    return color * opacity;
}
