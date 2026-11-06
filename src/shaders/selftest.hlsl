// Phase 0c self-test for the texture plumbing (set TBD_TEXTURE_SELFTEST=1 and read engine.log).
// FillPS writes known values into three render targets (a layer of the albedo / normal / depth arrays);
// SamplePS reads them back through the shader-visible descriptors so render-to-texture, layer addressing,
// the sampler and the descriptor binding are all exercised on both APIs.
// Direct3D 12 compiles this at runtime (SM 5.1); Vulkan gets SPIR-V from dxc at build time (-DVULKAN).

#ifdef VULKAN
#define VK_BIND(b) [[vk::binding(b, 0)]]
#else
#define VK_BIND(b)
#endif

VK_BIND(0) Texture2DArray<float4> gAlbedo : register(t0);
VK_BIND(1) Texture2DArray<float4> gNormal : register(t1);
VK_BIND(2) Texture2DArray<float> gDepth : register(t2);
VK_BIND(3) SamplerState gSampler : register(s0);

// One triangle that covers the whole target.
float4 FullscreenVS(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

struct FillOut {
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float depth : SV_Target2;
};

FillOut FillPS(float4 pos : SV_Position) {
    FillOut o;
    o.albedo = float4(1.0, 0.5, 0.25, 1.0);
    o.normal = float4(0.5, 0.5, 1.0, 0.0);
    o.depth = 0.75;
    return o;
}

// Pixel 0 samples layer 3 (filled), pixel 1 samples layer 2 (cleared): (albedo.r, albedo.g, normal.b, depth).
float4 SamplePS(float4 pos : SV_Position) : SV_Target {
    float3 uvw = float3(0.5, 0.5, pos.x < 1.0 ? 3.0 : 2.0);
    float4 a = gAlbedo.SampleLevel(gSampler, uvw, 0);
    float4 n = gNormal.SampleLevel(gSampler, uvw, 0);
    float d = gDepth.SampleLevel(gSampler, uvw, 0);
    return float4(a.r, a.g, n.b, d);
}
