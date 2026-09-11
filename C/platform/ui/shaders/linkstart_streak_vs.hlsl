cbuffer Constants : register(b0) {
    float2 resolution; float time; float sceneTime;
    float phaseProgress; float scenePhase; float connectedAlpha; float reducedMotion;
    float cameraZ; float alphaMul; float radiusMul; float energy;
    float flash; float startupBurst; float startupWave; float motionMix;
    float coolMix; float2 blurDirection; float bloomExtract;
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
    float variation : INSTANCE_EFFECT1;
};

struct VertexOutput {
    float4 position : SV_POSITION;
    float2 beam : TEXCOORD0;
    float3 viewPosition : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float3 color : COLOR;
    float alpha : ALPHA;
};

VertexOutput main(VertexInput input) {
    const float radius = input.radius * radiusMul;
    const float baseDepth = input.center.z - cameraZ;
    const float rotation = sceneTime * 0.06;
    const float cr = cos(rotation), sr = sin(rotation);
    const float2 center = float2(input.center.x * cr - input.center.y * sr,
                                 input.center.x * sr + input.center.y * cr);
    const float2 crossSection = float2(input.position.x * cr - input.position.y * sr,
                                       input.position.x * sr + input.position.y * cr);
    const float2 xy = center + crossSection * radius;
    const float capDepth = 0.18;
    const float axial = input.position.z * input.length + input.normal.z * radius * capDepth;
    const float depth = baseDepth + axial;
    const float focalLength = resolution.y * (720.0 / 820.0);
    const float2 projection = max(1.0, focalLength) / max(1.0, resolution * 0.5);
    const float visible = 1.0 - smoothstep(2600.0, 3400.0, baseDepth);
    VertexOutput output;
    output.position = float4(xy.x * projection.x, -xy.y * projection.y, depth - 1.0, depth);
    output.beam = float2(saturate((baseDepth - 150.0) / 2200.0) * 0.88,
                         (axial + radius * capDepth) / (input.length + radius * capDepth * 2.0));
    output.viewPosition = float3(xy, depth);
    output.normal = normalize(float3((input.normal.x * cr - input.normal.y * sr) * capDepth,
                            (input.normal.x * sr + input.normal.y * cr) * capDepth, input.normal.z));
    const float shimmer = 0.95 + 0.05 * sin(sceneTime * (2.5 + input.variation * 2.0) + input.center.z * 0.005);
    output.color = lerp(input.warmColor, input.coolColor, saturate(coolMix)) * input.brightness * shimmer;
    output.alpha = alphaMul * visible;
    return output;
}