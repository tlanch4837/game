// The smooth UI path (Renderer::DrawUi): every primitive is a screen-space quad whose pixel shader draws an anti-aliased
// shape by signed distance, so the in-game GUI gets rounded boxes, soft shadows, round knobs, stroked icons and SDF text
// with no geometry beyond two triangles each. Vertex layout: render/ui_vertex.h. Straight alpha, blended
// SrcAlpha / InvSrcAlpha into the display-space back buffer, no depth test.
// Direct3D 12 compiles this at runtime (fxc, SM 5.1); Vulkan gets SPIR-V from dxc at build time (-DVULKAN, spirv.bat).
//
// Two more modes for the Glass Dock GUI (src/gui/glass_dock.h): Image draws a rectangle of the RGBA sprite atlas
// (SetUiImages: icons and tile previews), Glass a rounded rectangle of frosted glass: the scene behind it, blurred and
// saturated, under a tint. The blur reads one shared backdrop texture the renderer makes once a frame
// (CaptureUiBackdrop: the picture on screen box-filtered down to 1/16 of its size), so a glass panel costs a 5x5 Gaussian
// of a tiny texture per pixel, not a full-resolution blur per panel.

struct UiConstants {
    float4x4 ortho; // OrthoPixels(width, height), column-major, used as mul(M, v)
};

#ifdef VULKAN
[[vk::push_constant]] UiConstants pc;
[[vk::binding(0, 0)]] Texture2D<float4> gAtlas;
[[vk::binding(1, 0)]] SamplerState gAtlasSampler;
[[vk::binding(2, 0)]] Texture2D<float4> gImages;   // the sprite atlas (Image mode)
[[vk::binding(3, 0)]] Texture2D<float4> gBackdrop; // the blurred picture behind the GUI (Glass mode)
#define LOC(n) [[vk::location(n)]]
#else
ConstantBuffer<UiConstants> pc : register(b0);
Texture2D<float4> gAtlas : register(t0);
Texture2D<float4> gImages : register(t1);
Texture2D<float4> gBackdrop : register(t2);
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
    LOC(6) float2 screenUv : TEXCOORD4; // 0..1 across the viewport (Glass mode)
};

UiOut UiVS(UiIn v) {
    UiOut o;
    o.pos = mul(pc.ortho, float4(v.pos, 0.0, 1.0));
    o.screenUv = float2(o.pos.x * 0.5 + 0.5, 0.5 - o.pos.y * 0.5);
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

// Glass: the backdrop under a 5x5 Gaussian of `sigma` screen pixels, saturated by `sat` (CSS saturate()).
float3 GlassBackdrop(float2 uv, float sigmaPx, float sat, float2 screenPerUv) {
    uint bw, bh;
    gBackdrop.GetDimensions(bw, bh);
    const float2 texel = 1.0 / float2(bw, bh);
    // sigma in backdrop texels; taps two thirds of a sigma apart reach 1.33 sigma, with bilinear filling between them
    const float sigma = max(sigmaPx * float(bw) / max(screenPerUv.x, 1.0), 0.5);
    const float step = sigma * 0.6667;
    float3 sum = 0.0;
    float wsum = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            const float w = exp(-0.5 * (float)(x * x + y * y) * 0.4444); // (step / sigma)^2
            sum += gBackdrop.SampleLevel(gAtlasSampler, uv + float2(x, y) * step * texel, 0.0).rgb * w;
            wsum += w;
        }
    }
    const float3 c = sum / wsum;
    const float luma = dot(c, float3(0.2126, 0.7152, 0.0722)); // the saturate() filter's matrix
    return saturate(lerp(luma.xxx, c, sat));
}

float4 UiPS(UiOut i) : SV_Target {
    const uint mode = (uint)(i.modeSoft.x + 0.5);
    if (mode == 4) { // sprite: local = atlas uv, fill = tint (multiplied)
        const float4 t = gImages.Sample(gAtlasSampler, i.local);
        return t * i.fill;
    }
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
    if (mode == 5) { // frosted glass: softness packs saturation * 100 * 1000 + blur sigma (screen pixels)
        const float sat = floor(i.modeSoft.y / 1000.0) / 100.0, sigmaPx = i.modeSoft.y - floor(i.modeSoft.y / 1000.0) * 1000.0;
        const float2 screenPerUv = float2(2.0 / max(abs(pc.ortho[0][0]), 1e-6), 2.0 / max(abs(pc.ortho[1][1]), 1e-6));
        uint bw, bh;
        gBackdrop.GetDimensions(bw, bh);
        if (bw > 1) c = float4(lerp(GlassBackdrop(i.screenUv, sigmaPx, sat, screenPerUv), i.fill.rgb, i.fill.a), 1.0);
        else c = float4(i.fill.rgb, 1.0); // no backdrop (Vulkan): the tint alone, at its own alpha below
        // inset 0 1px 0 #ffffff22: a one pixel highlight just inside the top edge
        const float fromTop = i.local.y + i.ab.y - i.radiusBorder.y;
        c.rgb = lerp(c.rgb, 1.0.xxx, 0.133 * saturate(1.0 - abs(fromTop - 0.5)) * saturate(-d - i.radiusBorder.y));
        if (i.radiusBorder.y > 0.0) {
            const float inner = saturate(0.5 - (d + i.radiusBorder.y) / aa);
            c.rgb = lerp(lerp(c.rgb, i.stroke.rgb, i.stroke.a), c.rgb, inner);
        }
        return float4(c.rgb, cover * (bw > 1 ? 1.0 : i.fill.a));
    }
    if (i.radiusBorder.y > 0.0) { // border: the band between the edge and border pixels inside it
        const float inner = saturate(0.5 - (d + i.radiusBorder.y) / aa);
        c = lerp(i.stroke, i.fill, inner);
    }
    return float4(c.rgb, c.a * cover);
}

// CaptureUiBackdrop on Direct3D 12: the copied back buffer box-filtered to 1/16 in one pass (16 bilinear taps, each a 2x2
// average, spread over the 16x16 block). Vulkan and Metal make the same texture with blits and mip generation instead.
struct BackdropOut { float4 pos : SV_Position; LOC(0) float2 uv : TEXCOORD0; };
BackdropOut UiBackdropVS(uint id : SV_VertexID) {
    BackdropOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
float4 UiBackdropPS(BackdropOut i) : SV_Target {
    uint w, h;
    gAtlas.GetDimensions(w, h); // the source rides in slot 0 for this pass
    const float2 texel = 1.0 / float2(w, h);
    float3 sum = 0.0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x) sum += gAtlas.SampleLevel(gAtlasSampler, i.uv + (float2(x, y) * 4.0 - 6.0) * texel, 0.0).rgb;
    return float4(sum / 16.0, 1.0);
}
