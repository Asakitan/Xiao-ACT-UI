cbuffer Constants : register(b0) {
    float2 resolution; float time; float sceneTime;
    float phaseProgress; float scenePhase; float connectedAlpha; float reducedMotion;
    float cameraZ; float alphaMul; float radiusMul; float energy;
    float flash; float startupBurst; float startupWave; float motionMix;
    float coolMix; float2 blurDirection; float padding0;
    float3 backgroundColor; float padding1;
    float3 effectTint; float padding2;
};

struct PixelInput {
    float4 position : SV_POSITION;
    float3 world : WORLD;
    float3 normal : NORMAL;
    float3 color : COLOR;
    float alpha : ALPHA;
    float fog : FOG;
};

float4 main(PixelInput input) : SV_TARGET {
    const float3 normal = normalize(input.normal);
    const float3 light = normalize(float3(-input.world.xy, 0.0));
    const float3 view = normalize(float3(0.0, 0.0, cameraZ) - input.world);
    const float3 halfVector = normalize(light + view);
    const float diffuse = max(dot(normal, light), 0.0);
    const float specular = pow(max(dot(normal, halfVector), 0.0), 48.0);
    float rim = saturate(1.0 - saturate(dot(normal, view)));
    rim = pow(rim, 2.5) * 0.45;
    float3 lit = input.color * 0.20 + input.color * diffuse * 0.55 +
                 specular * 0.65 + input.color * 0.30 + input.color * rim;
    lit = saturate(lit);
    const float coverage = saturate(input.alpha * (1.0 - input.fog));
    return float4(lit * coverage, coverage);
}
