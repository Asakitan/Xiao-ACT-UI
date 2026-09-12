cbuffer Constants : register(b0) {
    float2 resolution; float time; float sceneTime;
    float flightSpan; float historyScale; float connectedAlpha; float reducedMotion;
    float cameraZ; float alphaMul; float radiusMul; float energy;
    float flash; float birthLead; float startupWave; float motionMix;
    float coolMix; float2 blurDirection; float bloomExtract;
    float3 backgroundColor; float padding1;
    float3 effectTint; float exitProgress;
};

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
    const float3 view = normalize(-input.viewPosition);
    const float facing = dot(normal, view);
    clip(facing + 0.001);
    const float3 light = normalize(float3(-input.viewPosition.xy * 0.35 + float2(-260.0, -380.0), -750.0));
    const float3 halfVector = normalize(light + view);
    const float diffuse = saturate(dot(normal, light));
    const float specular = pow(saturate(dot(normal, halfVector)), 24.0);
    const float clearCoat = pow(saturate(dot(normal, halfVector)), 72.0);
    const float rim = pow(1.0 - saturate(facing), 2.5);
    const float coverage = smoothstep(0.0, max(fwidth(facing) * 1.1, 0.004), facing);
    const float longitudinal = 1.0 - smoothstep(0.08, 1.0, input.beam.y) * 0.14;
    const float3 emissive = input.color * (0.025 + energy * 0.04) * longitudinal;
    float3 color = input.color * (0.28 + diffuse * 0.62 + rim * 0.10) + emissive;
    color += lerp(float3(1.0, 0.98, 0.95), float3(0.94, 0.98, 1.0), coolMix) *
             (specular * 0.24 + clearCoat * 0.32);
    color += input.color * pow(saturate(-normal.z), 3.0) * 0.06;
    const float3 fogColor = lerp(backgroundColor / 12.92,
        pow(max(0.0, (backgroundColor + 0.055) / 1.055), 2.4), step(0.04045, backgroundColor));
    const float fog = smoothstep(600.0, 6500.0, input.viewPosition.z) * 0.55;
    color = lerp(color, fogColor, fog);
    const float alpha = saturate(input.alpha * coverage);
    clip(alpha - 0.0001);
    return float4(color * alpha, alpha);
}