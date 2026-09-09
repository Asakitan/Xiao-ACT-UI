cbuffer Constants : register(b0) {
    float2 resolution; float time; float progress;
    float scenePhase; float connectedAlpha; float reducedMotion; float padding0;
    float2 blurDirection; float2 padding1;
};

struct VertexInput { float2 corner : POSITION; float4 data : INSTANCE; };
struct VertexOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float gold : GOLD; };

VertexOutput main(VertexInput input) {
    const float speed = scenePhase < 2.5 ? 0.22 : 0.38;
    const float travel = frac(time * (reducedMotion > 0.5 ? 0.0 : speed) + input.data.y);
    const float depth = travel * travel;
    const float phaseBoost = scenePhase > 2.5 ? 1.35 : 1.0;
    const float radius = min(resolution.x, resolution.y) * (0.06 + depth * 0.72) * phaseBoost;
    const float streakLength = min(resolution.x, resolution.y) *
        (0.02 + depth * 0.22) * phaseBoost;
    const float2 direction = float2(cos(input.data.x), sin(input.data.x));
    const float2 side = float2(-direction.y, direction.x);
    const float2 center = resolution * 0.5 + direction * radius;
    const float2 pixel = center + direction * (input.corner.x * streakLength) +
                         side * (input.corner.y * input.data.z * (1.0 + depth * 4.0));
    VertexOutput output;
    output.position = float4(pixel.x / resolution.x * 2.0 - 1.0,
                             1.0 - pixel.y / resolution.y * 2.0, 0.0, 1.0);
    output.uv = input.corner * 0.5 + 0.5;
    output.gold = max(input.data.w,
                      step(2.5, scenePhase) * step(frac(input.data.y * 17.0), 0.08));
    return output;
}
