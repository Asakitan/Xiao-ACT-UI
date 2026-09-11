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
    const float period = 4800.0 + input.length + radius * 2.0;
    const float baseDepth = frac((input.center.z - cameraZ) / period) * period - input.length - radius;
    const float rotation = sceneTime * 0.008;
    const float cr = cos(rotation), sr = sin(rotation);
    const float2 center = float2(input.center.x * cr - input.center.y * sr,
                                 input.center.x * sr + input.center.y * cr);
    const float2 crossSection = float2(input.position.x * cr - input.position.y * sr,
                                       input.position.x * sr + input.position.y * cr);
    const float2 xy = center + crossSection * radius;
    const float axial = input.position.z * input.length + input.normal.z * radius;
    const float depth = baseDepth + axial;
    const float focalLength = resolution.y * lerp(0.88, 0.80, saturate(motionMix));
    const float2 projection = max(1.0, focalLength) / max(1.0, resolution * 0.5);
    const float visible = 1.0 - smoothstep(3600.0, 4800.0, baseDepth);
    VertexOutput output;
    output.position = float4(xy.x * projection.x, -xy.y * projection.y, depth - 1.0, depth);
    output.beam = float2(0.0, (axial + radius) / (input.length + radius * 2.0));
    output.viewPosition = float3(xy, depth);
    output.normal = float3(input.normal.x * cr - input.normal.y * sr,
                            input.normal.x * sr + input.normal.y * cr, input.normal.z);
    output.color = lerp(input.warmColor, input.coolColor, saturate(coolMix));
    output.alpha = alphaMul * input.brightness * visible * (0.9 + 0.1 * input.variation);
    return output;
}