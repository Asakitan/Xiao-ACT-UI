cbuffer LayerConstants : register(b0) {
    float2 viewportSize;
    float2 originPx;
    float2 layerSize;
    float opacity;
    float padding0;
};

struct VertexOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VertexOutput main(uint id : SV_VertexID) {
    const float2 uv[6] = {
        float2(0, 0), float2(1, 0), float2(0, 1),
        float2(0, 1), float2(1, 0), float2(1, 1)
    };
    VertexOutput output;
    output.uv = uv[id];
    const float2 pixel = originPx + output.uv * layerSize;
    output.position = float4(pixel.x / viewportSize.x * 2.0 - 1.0,
                             1.0 - pixel.y / viewportSize.y * 2.0,
                             0.0, 1.0);
    return output;
}
