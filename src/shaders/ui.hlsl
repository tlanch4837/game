// The smooth UI path (Renderer::DrawUi): every primitive is a screen-space quad whose pixel shader draws an anti-aliased
// shape by signed distance, so the in-game GUI gets rounded boxes, soft shadows, round knobs, stroked icons and SDF text
// with no geometry beyond two triangles each. Vertex layout: render/ui_vertex.h. Straight alpha, blended
// SrcAlpha / InvSrcAlpha into the display-space back buffer, no depth test.
// Direct3D 12 compiles this at runtime (fxc, SM 5.1); Vulkan gets SPIR-V from dxc at build time (-DVULKAN, spirv.bat).

struct UiConstants {
    float4x4 ortho; // OrthoPixels(width, height), column-major, used as mul(M, v)
};

#ifdef VULKAN
[[vk::push_constant]] UiConstants pc;
[[vk::binding(0, 0)]] Texture2D<float4> gAtlas;
[[vk::binding(1, 0)]] SamplerState gAtlasSampler;
#define LOC(n) [[vk::location(n)]]
#else
ConstantBuffer<UiConstants> pc : register(b0);
Texture2D<float4> gAtlas : register(t0);
SamplerState gAtlasSampler : register(s0);
#define LOC(n)
#endif

struct UiIn {
    LOC(0) float2 pos : POSITION;
    LOC(1) float2 local : TEXCOORD0;
    LOC(2) float2 ab : TEXCOORD1;
    LOC(3) float2 radiusBorder : TEXCOORD2;
    LOC(4) float2 modeSoft : TEXCOORD3;
    LOC(5) float4 fill : COLOR0;
    LOC(6) float4 stroke : COLOR1;
};

struct UiOut {
    float4 pos : SV_Position;
    LOC(0) float2 local : TEXCOORD0;
    LOC(1) nointerpolation float2 ab : TEXCOORD1;
    LOC(2) nointerpolation float2 radiusBorder : TEXCOORD2;
    LOC(3) nointerpolation float2 modeSoft : TEXCOORD3;
    LOC(4) float4 fill : COLOR0;
    LOC(5) nointerpolation float4 stroke : COLOR1;
};

UiOut UiVS(UiIn v) {
    UiOut o;
    o.pos = mul(pc.ortho, float4(v.pos, 0.0, 1.0));
    o.local = v.local;
    o.ab = v.ab;
    o.radiusBorder = v.radiusBorder;
    o.modeSoft = v.modeSoft;
    o.fill = v.fill;
    o.stroke = v.stroke;
    return o;
}

// Screen-space rate of change, with fine derivatives on both compilers (plain fwidth may be coarse on one and fine on the
// other, which shows as single-pixel differences along anti-aliased edges between Direct3D 12 and Vulkan).
float FineWidth(float v) { return abs(ddx_fine(v)) + abs(ddy_fine(v)); }

// Signed distance from p to a box of half size h with corners rounded by r (negative inside). Inigo Quilez's formula.
float SdRoundBox(float2 p, float2 h, float r) {
    r = min(r, min(h.x, h.y));
    const float2 q = abs(p) - h + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Signed distance to a capsule from the origin to ba with radius r.
float SdCapsule(float2 p, float2 ba, float r) {
    const float h = saturate(dot(p, ba) / max(dot(ba, ba), 1e-6));
    return length(p - ba * h) - r;
}

float4 UiPS(UiOut i) : SV_Target {
    const uint mode = (uint)(i.modeSoft.x + 0.5);
    if (mode == 2) { // SDF text: 0.5 is the outline of the glyph
        const float d = gAtlas.Sample(gAtlasSampler, i.local).r;
        const float w = max(FineWidth(d) * 0.5 * i.modeSoft.y, 1e-4); // half a screen pixel of distance: one pixel of anti-aliasing
        const float glyph = smoothstep(0.5 - w, 0.5 + w, d);
        if (i.radiusBorder.y > 0.0) { // outline: a second, wider threshold in the outline colour under the glyph
            const float outer = smoothstep(0.5 - i.radiusBorder.y - w, 0.5 - i.radiusBorder.y + w, d);
            const float4 c = lerp(float4(i.stroke.rgb, i.stroke.a * outer), i.fill, glyph);
            return c;
        }
        return float4(i.fill.rgb, i.fill.a * glyph);
    }
    float d = mode == 3 ? SdCapsule(i.local, i.ab, i.radiusBorder.x) : SdRoundBox(i.local, i.ab, i.radiusBorder.x);
    if (mode == 1) { // soft shadow: a smooth ramp from blur pixels inside the box to blur pixels outside
        const float blur = max(i.modeSoft.y, 1.0);
        const float t = saturate(0.5 - d / (2.0 * blur));
        return float4(i.fill.rgb, i.fill.a * t * t * (3.0 - 2.0 * t));
    }
    const float aa = max(FineWidth(d), 1e-4); // about one pixel: the distances are in pixels
    const float cover = saturate(0.5 - d / aa);
    float4 c = i.fill;
    if (i.radiusBorder.y > 0.0) { // border: the band between the edge and border pixels inside it
        const float inner = saturate(0.5 - (d + i.radiusBorder.y) / aa);
        c = lerp(i.stroke, i.fill, inner);
    }
    return float4(c.rgb, c.a * cover);
}
