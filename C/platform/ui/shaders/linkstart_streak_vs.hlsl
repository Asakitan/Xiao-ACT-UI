cbuffer Constants : register(b0) {
    float2 resolution; float time; float sceneTime;
    float phaseProgress; float scenePhase; float connectedAlpha; float reducedMotion;
    float cameraZ; float alphaMul; float radiusMul; float energy;
    float flash; float startupBurst; float startupWave; float motionMix;
    float coolMix; float2 blurDirection; float padding0;
    float3 backgroundColor; float padding1;
    float3 effectTint; float padding2;
};

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 center : INSTANCE_CENTER;
    float length : INSTANCE_LENGTH;
    float radius : INSTANCE_RADIUS;
    float3 warmColor : INSTANCE_WARM;
    float3 coolColor : INSTANCE_COOL;
    float brightness : INSTANCE_EFFECT;
    float flicker : INSTANCE_EFFECT1;
};

struct VertexOutput {
    float4 position : SV_POSITION;
    float3 world : WORLD;
    float3 normal : NORMAL;
    float3 color : COLOR;
    float alpha : ALPHA;
    float fog : FOG;
};

VertexOutput main(VertexInput input) {
    const float rotation = sceneTime * 0.06;
    const float cr = cos(rotation);
    const float sr = sin(rotation);
    float3 world = input.position;
    world.xy *= input.radius * radiusMul;
    world.z = world.z * input.length + input.center.z;
    world.xy += input.center.xy;
    world.xy = float2(world.x * cr - world.y * sr,
                      world.x * sr + world.y * cr);
    float3 normal = input.normal;
    normal.xy = float2(normal.x * cr - normal.y * sr,
                       normal.x * sr + normal.y * cr);

    const float zNear = input.center.z - cameraZ;
    const float shimmer = 0.85 + 0.15 * sin(sceneTime * input.flicker +
                                            input.center.z * 0.005);
    const float fade = alphaMul * input.brightness * shimmer;
    const float fog = min(0.95, max(0.0, (zNear - 150.0) / 2200.0));
    const float focal = 720.0;
    const float2 halfSize = max(float2(1.0, 1.0), resolution * 0.5);
    const float2 projectionScale = focal / halfSize;
    const float eyeZ = world.z - cameraZ;

    VertexOutput output;
    output.position = float4(world.x * projectionScale.x,
                             -world.y * projectionScale.y,
                             eyeZ - 1.0, eyeZ);
    output.world = world;
    output.normal = normalize(normal);
    output.color = lerp(input.warmColor, input.coolColor, coolMix);
    output.alpha = fade;
    output.fog = fog;
    return output;
}
