struct VertexOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };

VertexOutput main(uint id : SV_VertexID) {
    const float2 uv[6] = {
        float2(0, 0), float2(1, 0), float2(0, 1),
        float2(0, 1), float2(1, 0), float2(1, 1)
    };
    VertexOutput output;
    output.uv = uv[id];
    output.position = float4(uv[id].x * 2.0 - 1.0,
                             1.0 - uv[id].y * 2.0, 0.0, 1.0);
    return output;
}
