cbuffer Constants : register(b0) {
    float2 resolution; float time; float sceneTime;
    float flightSpan; float historyScale; float connectedAlpha; float reducedMotion;
    float cameraZ; float alphaMul; float radiusMul; float energy;
    float flash; float birthLead; float startupWave; float motionMix;
    float coolMix; float2 blurDirection; float bloomExtract;
    float3 backgroundColor; float padding1;
    float3 effectTint; float exitProgress;
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
    float birthFraction : INSTANCE_EFFECT1;
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
    const float focalLength = resolution.y * (720.0 / 820.0);
    const float2 projection = max(1.0, focalLength) / max(1.0, resolution * 0.5);
    const float guardRadius = input.radius * 1.05;
    const float2 edgeDepth = (abs(input.center.xy) - guardRadius) * projection;
    const float exitDepth = max(edgeDepth.x, edgeDepth.y) - guardRadius * 0.18 - 32.0;
    const float progress = cameraZ / max(1.0, flightSpan);
    const float age = saturate((progress - input.birthFraction * 0.45) / 0.55);
    const float acceleration = age * age;
    const float maxStretch = 1.16;
    const float flightDistance = max(1.0,
        input.center.z + input.length * maxStretch - exitDepth);
    const float travel = flightDistance * acceleration;
    const float baseDepth = input.center.z - travel;
    const float radius = input.radius * radiusMul * acceleration;
    const float columnLength = input.length * lerp(1.0, maxStretch, acceleration);
    const float2 xy = input.center.xy * acceleration + input.position.xy * radius;
    const float capDepth = 0.18;
    const float axial = input.position.z * columnLength + input.normal.z * radius * capDepth;
    const float depth = baseDepth + axial;
    VertexOutput output;
    output.position = float4(xy.x * projection.x, -xy.y * projection.y, depth - 1.0, depth);
    output.beam = float2(saturate((baseDepth - 150.0) / 2200.0) * 0.88,
                         (axial + radius * capDepth) / (columnLength + radius * capDepth * 2.0));
    output.viewPosition = float3(xy, depth);
    output.normal = normalize(float3(input.normal.xy * capDepth, input.normal.z));
    output.color = lerp(input.warmColor, input.coolColor, saturate(coolMix)) * input.brightness;
    output.alpha = alphaMul;
    return output;
}