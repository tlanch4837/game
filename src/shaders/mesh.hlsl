// One shader for everything: lit meshes (normal != 0) and flat-colored lines and 2D overlays (normal == 0).
// Lighting, and the sun's shadow, are evaluated per pixel. Direct3D 12 compiles this at runtime (fxc, SM 5.1);
// Vulkan gets SPIR-V from dxc at build time (-DVULKAN). Matrices arrive as 32 floats of push/root constants,
// column-major, used as mul(M, v).

struct PushConstants {
    float4x4 mvp;
    float4x4 model;
#ifdef VULKAN
    float4 material; // offset 128, fragment stage only: see MaterialConstants
#endif
};

// The bound material's constants (Renderer::materialConstants_): x the skin detail scale (SetSkinDetailScale), y the pore depth (SetSkinPoreDepth), zw spare.
#ifdef VULKAN
[[vk::push_constant]] PushConstants pc;
#define MATERIAL_CONSTANTS pc.material
#else
ConstantBuffer<PushConstants> pc : register(b0);
struct MaterialConstants { float4 material; };
ConstantBuffer<MaterialConstants> gMaterial : register(b3);
#define MATERIAL_CONSTANTS gMaterial.material
#endif
// How many times skin's pore normal repeats across the atlas; 32 unless the material says otherwise.
// How deep pores read (Renderer::SetSkinPoreDepth): 1 default, < 1 young and fine, > 1 old and coarse. 0 = never set = 1.
float SkinPoreDepth() { return MATERIAL_CONSTANTS.y > 0.0 ? MATERIAL_CONSTANTS.y : 1.0; }
float SkinDetailScale() { return MATERIAL_CONSTANTS.x > 0.0 ? MATERIAL_CONSTANTS.x : 32.0; } // pores ~0.4 mm (48: sub-pixel and averaged away; 20: too big)

#include "lighting.hlsli"
static float gPixelShadow = 1.0; // the sun's shadow at this pixel, set once by PSMain before any material runs

// Motion vectors: what a moving thing's vertex shader needs to say where it was last frame.
struct MotionData {
    float4x4 viewProj;     // this frame, unjittered
    float4x4 prevViewProj; // last frame, unjittered
    float4 misc;           // x: seconds since last frame
};
#ifdef VULKAN
[[vk::binding(6, 0)]] ConstantBuffer<MotionData> gMotion;
#else
ConstantBuffer<MotionData> gMotion : register(b2);
#endif

// Material textures: set 1 on Vulkan.
#ifdef VULKAN
[[vk::binding(0, 1)]] Texture2D<float4> gAlbedo;
[[vk::binding(1, 1)]] Texture2D<float4> gNormalMap;
[[vk::binding(2, 1)]] SamplerState gTexSampler;
[[vk::binding(3, 1)]] Texture2D<float4> gSurface;
[[vk::binding(4, 1)]] Texture2D<float4> gExtra; // R baked occlusion, B metalness scale, A emissive; see SetSurfaceExtra in renderer.h
#else
Texture2D<float4> gAlbedo : register(t1);
Texture2D<float4> gNormalMap : register(t2);
Texture2D<float4> gSurface : register(t3);
Texture2D<float4> gExtra : register(t0, space1);
SamplerState gTexSampler : register(s1);
#endif

// The project's pore texture as an atlas (gameenginetools/skin_tex/pore_atlas.py; alpha 0.5 marks it): four 1024 quadrants,
// each a 1008 px tile inside an 8 px wrapped border. The extra map's G picks the tile per region: 1 (the default) general
// face, 0.5 open pores (nose, inner cheeks), 0 thin eyelid skin; pore depth above 1 moves the general face to weathered.
// `plain` is the ordinary sample, kept when the normal map is not the atlas (a skin with its own normal map).
float3 SkinPoreQuadrant(float2 tiled, float2 q, float2 gx, float2 gy) {
    const float2 at = (q * 1024.0 + 8.0 + frac(tiled) * 1008.0) / 2048.0;
    return gNormalMap.SampleGrad(gTexSampler, at, gx, gy).xyz * 2.0 - 1.0;
}
float3 SkinPoreNormal(float2 uv, float2 tiled, float3 plain) {
    if (abs(gNormalMap.SampleLevel(gTexSampler, float2(0.5, 0.5), 20.0).a - 0.5) > 0.1) return plain;
    const float g = gExtra.Sample(gTexSampler, uv).g;
    const float worn = saturate(SkinPoreDepth() - 1.0);
    const float wThin = saturate(1.0 - g * 2.0), wOpen = saturate(1.0 - abs(g - 0.5) * 2.0), wFace = saturate(g * 2.0 - 1.0);
    const float2 gx = ddx(tiled) * (1008.0 / 2048.0), gy = ddy(tiled) * (1008.0 / 2048.0);
    float3 n = 0;
    [branch] if (wFace * (1.0 - worn) > 0.01) n += wFace * (1.0 - worn) * SkinPoreQuadrant(tiled, float2(0, 0), gx, gy);
    [branch] if (wFace * worn > 0.01) n += wFace * worn * SkinPoreQuadrant(tiled, float2(1, 1), gx, gy);
    [branch] if (wOpen > 0.01) n += wOpen * SkinPoreQuadrant(tiled, float2(1, 0), gx, gy);
    [branch] if (wThin > 0.01) n += wThin * SkinPoreQuadrant(tiled, float2(0, 1), gx, gy);
    return normalize(n + float3(0, 0, 1e-4));
}

struct VSIn {
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
    float2 uv : TEXCOORD1; // unused until materials sample textures
};

struct VSOut {
    float4 pos : SV_Position;
    float4 color : COLOR;
    float3 normal : NORMAL;      // world space, not normalized; zero means unlit
    float3 shadowPos : TEXCOORD0; // shadow map clip space (orthographic, so it interpolates linearly)
    float3 worldPos : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float3 tightPos : TEXCOORD4; // tight character shadow map clip space (orthographic too)
    float3 origin : TEXCOORD7;   // the object's origin in the world: where its ground is, and a seed so repeats differ
};

// One instance of an instanced draw: a float3x4 model matrix, row by row, from the second vertex stream.
struct InstanceIn {
    float4 row0 : INSTROW0;
    float4 row1 : INSTROW1;
    float4 row2 : INSTROW2;
};

// Everything but the clip position, from the world position and the model's three axes (its 3x3's columns).
VSOut ShadeVertex(VSIn v, float4 clip, float3 world, float3 c0, float3 c1, float3 c2) {
    VSOut o;
    o.pos = clip;
    o.color = v.color;
    o.uv = v.uv;
    o.worldPos = world;
    o.normal = float3(0, 0, 0);
    o.shadowPos = float3(0, 0, 0);
    o.tightPos = float3(0, 0, 0);
    o.origin = world;

    if (dot(v.normal, v.normal) > 1e-6) {
        // Normal matrix without an inverse: the cofactor of the model's 3x3 stays correct under non-uniform scale.
        float3 n = normalize(cross(c1, c2) * v.normal.x + cross(c2, c0) * v.normal.y + cross(c0, c1) * v.normal.z);
        o.normal = n;

        // Normal-offset the lookup (more at grazing angles) so surfaces don't shadow themselves.
        float grazing = 1.0 - saturate(dot(n, gLight.sunDir.xyz));
        o.shadowPos = mul(gLight.shadowVP, float4(world + n * (gLight.shadowParams.z * (1.0 + 2.0 * grazing)), 1.0)).xyz;
        o.tightPos = mul(gLight.tightVP, float4(world + n * (gLight.tightParams.z * (1.0 + 2.0 * grazing)), 1.0)).xyz;
    }
    return o;
}

VSOut VSMain(VSIn v) {
    VSOut o = ShadeVertex(v, mul(pc.mvp, float4(v.pos, 1.0)), mul(pc.model, float4(v.pos, 1.0)).xyz,
                          float3(pc.model[0][0], pc.model[1][0], pc.model[2][0]),
                          float3(pc.model[0][1], pc.model[1][1], pc.model[2][1]),
                          float3(pc.model[0][2], pc.model[1][2], pc.model[2][2]));
    o.origin = float3(pc.model[0][3], pc.model[1][3], pc.model[2][3]);
    return o;
}

float3 InstanceWorld(VSIn v, InstanceIn inst) {
    const float4 p = float4(v.pos, 1.0);
    return float3(dot(inst.row0, p), dot(inst.row1, p), dot(inst.row2, p));
}

// Instanced draws: pc.mvp holds the view-projection alone.
// Short-range instanced assets (grass) carry their draw distance in the vertex alpha: 0.3 + meters / 1000, set when the
// asset is uploaded. Such an instance shrinks to its base over the last half of that distance, so the field thins out
// instead of ending in a line. Everything else keeps its alpha's other meanings.
VSOut VSInstanced(VSIn v, InstanceIn inst) {
    float3 world = InstanceWorld(v, inst);
    if (v.color.a > 0.29 && v.color.a < 0.39) {
        const float reach = (v.color.a - 0.3) * 1000.0;
        const float3 root = float3(inst.row0.w, inst.row1.w, inst.row2.w);
        const float t = saturate((reach - length(root - gLight.eye.xyz)) / (reach * 0.5));
        const float shrink = t * t * (3.0 - 2.0 * t);
        world = root + (world - root) * shrink;
    }
    VSOut o = ShadeVertex(v, mul(pc.mvp, float4(world, 1.0)), world, float3(inst.row0.x, inst.row1.x, inst.row2.x),
                          float3(inst.row0.y, inst.row1.y, inst.row2.y), float3(inst.row0.z, inst.row1.z, inst.row2.z));
    o.origin = float3(inst.row0.w, inst.row1.w, inst.row2.w);
    return o;
}

// GPU-placed vegetation (see vegetation.hlsl). pc.model holds parameters, not a matrix: row 0 is
// (reach in meters, time in seconds, wind x, wind z), rows 1-3 are characters (xyz, radius).
// Flags ride in the unused y of the character rows: [1][1] camera tilt, [2][1] ecotope strength, [3][1] animated.
float VegetationNoise(float2 p) {
    const float2 c = floor(p);
    float2 f = p - c;
    f = f * f * (3.0 - 2.0 * f);
    const float a = frac(sin(dot(c, float2(127.1, 311.7))) * 43758.5453), b = frac(sin(dot(c + float2(1, 0), float2(127.1, 311.7))) * 43758.5453);
    const float d = frac(sin(dot(c + float2(0, 1), float2(127.1, 311.7))) * 43758.5453), e = frac(sin(dot(c + float2(1, 1), float2(127.1, 311.7))) * 43758.5453);
    return lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y);
}
// A ramp picked by world position, so a stretch of land has one character of plant: dry straw, ochre, sage, rust. A small
// procedural stand-in for Decima's ecotope ramp array; it tints and does not re-author the albedos.
float3 VegetationEcotope(float2 root) {
    const float n = VegetationNoise(root / 53.0) * 0.7 + VegetationNoise(root / 9.0) * 0.3;
    const float3 straw = float3(1.12, 1.02, 0.78), ochre = float3(1.15, 0.92, 0.66), sage = float3(0.86, 0.98, 0.80), rust = float3(1.12, 0.82, 0.66);
    const float3 a = lerp(sage, straw, smoothstep(0.25, 0.45, n));
    return lerp(a, lerp(ochre, rust, smoothstep(0.65, 0.8, n)), smoothstep(0.5, 0.65, n));
}
float3 VegetationWorld(VSIn v, InstanceIn inst, float time, bool camera) {
    float3 world = InstanceWorld(v, inst);
    const float reach = pc.model[0][0];
    const float3 root = float3(inst.row0.w, inst.row1.w, inst.row2.w);
    const float away = length(root - gLight.eye.xyz);
    const float t = saturate((reach - away) / (reach * 0.35));
    world = root + (world - root) * (t * t * (3.0 - 2.0 * t)); // thins toward the reach instead of ending in a line

    // Everything below moves more the higher it is above the plant's root, so roots stay planted. Grows gently with height: a
    // tuft's tip and a shrub's crown both move a believable amount.
    const float h = max(world.y - root.y, 0.0);
    const float weight = h * h / (h + 2.0);

    // Camera tilt, after Sanders, GDC 2018: looking down, plants lean along the view, so the gaps between tufts close up.
    if (camera && pc.model[1][1] > 0.0) {
        const float3 view = normalize(root - gLight.eye.xyz);
        world.xz += view.xz * min(h, 1.5) * saturate(view.y * -1.5) * pc.model[1][1];
    }
    if (pc.model[3][1] < 0.5) return world; // the far set does not animate

    // Wind: gusts travel across the land along the wind, a quicker flutter differs per plant and per height.
    const float2 wind = float2(pc.model[0][2], pc.model[0][3]);
    const float along = dot(root.xz, wind);
    const float gust = 0.55 + 0.45 * sin(along * 0.11 - time * 1.3) * sin(along * 0.037 + root.x * 0.02 - time * 0.47);
    const float flutter = sin(time * 3.1 + root.x * 1.7 + root.z * 2.3 + h * 2.0) * 0.35;
    world.xz += wind * (gust + flutter * gust) * 0.22 * weight * saturate(1.5 - away / 35.0);

    // Characters: a plant leans away from each, and dips, more the closer its root is.
    for (int c = 1; c < 4; ++c) {
        const float radius = pc.model[c][3];
        if (radius <= 0.0) continue;
        const float2 from = root.xz - float2(pc.model[c][0], pc.model[c][2]);
        const float d = length(from);
        const float push = saturate(1.0 - d / radius);
        const float bend = push * push * (3.0 - 2.0 * push);
        const float2 dir = d > 1e-3 ? from / d : float2(1.0, 0.0);
        world.xz += dir * bend * min(h, 1.6) * 0.85;
        world.y -= bend * h * 0.45;
    }
    return world;
}
struct VSOutMotion {
    VSOut base;
    float4 curClip : TEXCOORD5;
    float4 prevClip : TEXCOORD6;
};
VSOutMotion VSVegetation(VSIn v, InstanceIn inst) {
    const float time = pc.model[0][1];
    const float3 world = VegetationWorld(v, inst, time, true);
    VSOutMotion o;
    o.base = ShadeVertex(v, mul(pc.mvp, float4(world, 1.0)), world, float3(inst.row0.x, inst.row1.x, inst.row2.x),
                         float3(inst.row0.y, inst.row1.y, inst.row2.y), float3(inst.row0.z, inst.row1.z, inst.row2.z));
    o.base.origin = float3(inst.row0.w, inst.row1.w - 10.0, inst.row2.w); // seeds Weather by plant or stone; well below, so no dirt foot darkens all of something this small
    o.curClip = mul(gMotion.viewProj, float4(world, 1.0));
    o.prevClip = mul(gMotion.prevViewProj, float4(VegetationWorld(v, inst, time - gMotion.misc.x, true), 1.0));
    if (pc.model[2][1] > 0.0)
        o.base.color.rgb *= lerp(float3(1, 1, 1), VegetationEcotope(float2(inst.row0.w, inst.row2.w)), pc.model[2][1]);
    o.base.color.a += 2.0; // PSMain: foliage
    return o;
}
float4 ShadowVegetationVS(VSIn v, InstanceIn inst) : SV_Position {
    float3 world = VegetationWorld(v, inst, pc.model[0][1], false);
    // Shrinks to its root before the layer's shadow distance (Params, row 1), so shadows thin out instead of popping off.
    const float reach = pc.model[1][1];
    if (reach > 0.0) {
        const float3 root = float3(inst.row0.w, inst.row1.w, inst.row2.w);
        const float t = saturate((reach - length(root - gLight.eye.xyz)) / (reach * 0.35));
        world = root + (world - root) * (t * t * (3.0 - 2.0 * t));
    }
    return mul(pc.mvp, float4(world, 1.0));
}
// Two-pass foliage, after the talks the Decima reference cites: alpha test in a cheap depth-only pass, then shade only what survived.
struct VSOutDepth {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD3;
};
VSOutDepth VSVegetationDepth(VSIn v, InstanceIn inst) {
    VSOutDepth o;
    o.pos = mul(pc.mvp, float4(VegetationWorld(v, inst, pc.model[0][1], true), 1.0));
    o.uv = v.uv;
    return o;
}
// CPU-skinned meshes: last frame's vertices are the other per-frame copy, read as a second stream.
VSOutMotion VSSkinnedMotion(VSIn v, float3 prevPos : PREVPOS) {
    VSOutMotion o;
    o.base = VSMain(v);
    o.curClip = mul(gMotion.viewProj, float4(o.base.worldPos, 1.0));
    o.prevClip = mul(gMotion.prevViewProj, float4(mul(pc.model, float4(prevPos, 1.0)).xyz, 1.0));
    return o;
}

float TileHash(float2 p) {
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.x + p.y) * p.x);
}
float TileNoise(float2 p) {
    float2 c = floor(p), f = p - c;
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(TileHash(c), TileHash(c + float2(1, 0)), f.x), lerp(TileHash(c + float2(0, 1)), TileHash(c + float2(1, 1)), f.x), f.y);
}
// One tile without visible repeats: hex tiling (after Mikkelsen, "Practical Real-Time Hex-Tiling", 2022). The plane is cut
// into a triangle grid; each corner of the triangle under the pixel owns a random offset into the texture, and the three
// taps blend by sharpened barycentric weights. Offsets only, no rotation, so tangent-space normals stay valid. Gradients
// come from the plain uv, so the offset jumps never disturb mip selection. Only for organic ground: on brick or cobble the
// blend would smear the joints.
static const float kHexRate = 0.6; // hex cells per texture repeat, roughly
float2 HexOffset(int2 v) {
    return float2(TileHash(float2(v) + 0.37), TileHash(float2(v).yx + 17.11));
}
float4 SampleNoRepeat(Texture2D<float4> tex, float2 uv) {
    const float2 dx = ddx(uv), dy = ddy(uv);
    const float2 st = uv * (kHexRate * 3.4641016); // 2 * sqrt(3)
    const float2 skew = float2(st.x, -0.57735027 * st.x + 1.15470054 * st.y);
    const int2 base = int2(floor(skew));
    const float3 t = float3(frac(skew), 1.0 - frac(skew).x - frac(skew).y);
    float3 w;
    int2 v0, v1, v2;
    if (t.z > 0.0) {
        w = float3(t.z, t.y, t.x);
        v0 = base; v1 = base + int2(0, 1); v2 = base + int2(1, 0);
    } else {
        w = float3(-t.z, 1.0 - t.y, 1.0 - t.x);
        v0 = base + int2(1, 1); v1 = base + int2(1, 0); v2 = base + int2(0, 1);
    }
    w = pow(w, 7.0); // sharper weights keep each tap's detail; the blend only shows in thin seams
    w /= (w.x + w.y + w.z);
    return tex.SampleGrad(gTexSampler, uv + HexOffset(v0), dx, dy) * w.x + tex.SampleGrad(gTexSampler, uv + HexOffset(v1), dx, dy) * w.y +
           tex.SampleGrad(gTexSampler, uv + HexOffset(v2), dx, dy) * w.z;
}

// Skin vertex alpha runs from 0.5 (at rest) to 0.59 (fully compressed); see FaceRig::Pose.
float SkinWrinkle(VSOut i) { return saturate((i.color.a - 0.5) / 0.09); }

// Parallax occlusion near the camera for world materials whose surface map holds a height (surface_maps.h). After Tatarchuk,
// "Dynamic Parallax Occlusion Mapping", 2006: a linear search whose step count follows the view angle, then one secant step.
// The tangent frame comes from screen derivatives as in Material, and the depth is in metres, so any UV tile gives the same relief.
static float2 sPomOffset = float2(0, 0);
static float sPomShadow = 1.0;
static float sCavity = 1.0;
static float sRoughScale = 1.0;
static bool sArt = false;
static const float kPomDepth = 0.010, kPomNear = 7.0, kPomFar = 16.0;
bool IsWorldSurface(float a) { return a > 0.855 && a < 0.915; }
bool IsReliefGround(float a) { return abs(a - 0.2) < 0.05; }
void SurfaceRelief(VSOut i, float depthMetres) {
    if (FEATURE_OFF(4)) return;
    const float marker = gSurface.SampleLevel(gTexSampler, float2(0.25, 0.25), 20.0).b;
    if (marker > 0.35 && marker < 0.65) { sArt = true; return; } // signs and lettering (surface_async.inl)
    const float4 top = gSurface.Sample(gTexSampler, i.uv);
    const float dist = length(gLight.eye.xyz - i.worldPos);
    const float3 ng = normalize(i.normal);
    const float2 uv = float2(i.uv.x, 1.0 - i.uv.y);
    const float3 dp1 = ddx(i.worldPos), dp2 = ddy(i.worldPos);
    const float2 du1 = ddx(uv), du2 = ddy(uv), gx = ddx(i.uv), gy = ddy(i.uv);
    if (top.b > 0.5) return; // the neutral default: no map
    sCavity = top.g;
    sRoughScale = top.r * 2.0;
    const float fade = saturate((kPomFar - dist) / (kPomFar - kPomNear));
    if (fade <= 0.0 || FEATURE_OFF(1)) return;
    const float metresPerUv = sqrt((dot(dp1, dp1) + dot(dp2, dp2)) / max(dot(du1, du1) + dot(du2, du2), 1e-12));
    const float3 dp2perp = cross(dp2, ng), dp1perp = cross(ng, dp1);
    const float3 T0 = dp2perp * du1.x + dp1perp * du2.x, B0 = dp2perp * du1.y + dp1perp * du2.y;
    if (dot(T0, T0) < 1e-20 || dot(B0, B0) < 1e-20) return;
    const float3 T = normalize(T0), B = normalize(B0);
    const float3 v = normalize(gLight.eye.xyz - i.worldPos);
    const float3 vt = float3(dot(v, T), dot(v, B), dot(v, ng));
    if (vt.z < 0.05) return;
    const float depthUv = depthMetres * fade / max(metresPerUv, 1e-4);
    float2 along = vt.xy / max(vt.z, 0.25) * depthUv;
    along.y = -along.y; // back to the texture's own v
    const int steps = int(lerp(14.0, 5.0, vt.z));
    const float stepSize = 1.0 / steps;
    float layer = 0.0, prevGap = 0.0, gap = 0.0;
    float2 at = i.uv;
    [loop] for (int k = 0; k < steps; ++k) {
        gap = (1.0 - gSurface.SampleGrad(gTexSampler, at, gx, gy).a) - layer;
        if (gap <= 0.0) break;
        prevGap = gap;
        layer += stepSize;
        at = i.uv - along * layer;
    }
    if (gap < 0.0) { // between the last two layers, by similar triangles
        const float t = prevGap / max(prevGap - gap, 1e-5);
        layer = layer - stepSize + stepSize * t;
        at = i.uv - along * layer;
    }
    sPomOffset = at - i.uv;
    const float4 hit = gSurface.SampleGrad(gTexSampler, at, gx, gy);
    sCavity = hit.g;
    sRoughScale = hit.r * 2.0;
    // Toward the sun from the hit, soft: what stands above the ray hides the sun from this pixel (the relief's own shadow).
    const float3 l = gLight.sunDir.xyz;
    const float3 lt = float3(dot(l, T), dot(l, B), dot(l, ng));
    if (lt.z > 0.02) {
        float2 toSun = lt.xy / max(lt.z, 0.2) * depthUv;
        toSun.y = -toSun.y;
        float block = 0.0;
        const float h0 = hit.a;
        [loop] for (int k = 1; k <= 4; ++k) {
            const float s = k / 4.0;
            const float h = gSurface.SampleGrad(gTexSampler, at + toSun * s * (1.0 - h0), gx, gy).a;
            block = max(block, (h - (h0 + (1.0 - h0) * s)) * (1.0 - s * 0.5));
        }
        sPomShadow = 1.0 - saturate(block * 6.0) * fade;
    }
}

// The layered ground (ground_bake.inl): dirt, gravel and wagon-track maps in a 2x2 atlas, with the bake of what stands on the ground in
// the fourth cell. Layers are chosen by noise, slope and nearness to buildings, height-blended (after Mikkelsen's atlas padding and the
// material rules in the Decima reference). Surface map layout here: R occlusion, G roughness, B marker, A height.
float2 GroundCell(float2 p, int cell) {
    const float2 f = frac(p) * 0.992 + 0.004;
    return (f + float2(cell & 1, cell >> 1)) * 0.5;
}
bool GroundAtlasBound() {
    const float marker = gSurface.SampleLevel(gTexSampler, float2(0.25, 0.25), 20.0).b;
    return marker > 0.15 && marker < 0.35;
}
// Wagon ruts down the street, which runs along x at z near 0. x: depth 0..1, y: slope across.
float2 GroundRuts(float2 xz) {
    const float mask = smoothstep(9.5, 6.0, abs(xz.y)) * (0.45 + 0.55 * smoothstep(0.25, 0.6, TileNoise(xz * 0.09)));
    float depth = 0.0, slope = 0.0;
    for (int k = 0; k < 3; ++k) {
        const float zc = (k - 1.0) * 2.7 + 0.9 * sin(xz.x * 0.021 + k * 2.1) + 0.3 * sin(xz.x * 0.073 + k);
        const float side = xz.y - zc;
        const float g = (abs(side) - 0.75) / 0.17;
        const float rut = exp(-g * g);
        if (rut > depth) { depth = rut; slope = -2.0 * g * rut * sign(side); }
    }
    return float2(depth, slope) * mask;
}
// Boot prints along two wandering lines. x: depth.
float GroundPrints(float2 xz) {
    float depth = 0.0;
    for (int k = 0; k < 2; ++k) {
        const float zc = (k == 0 ? -1.6 : 3.4) + 1.1 * sin(xz.x * 0.05 + k * 3.0);
        const float2 p = float2(xz.x / 0.74, 0.0);
        const float id = floor(p.x);
        if (TileHash(float2(id, k + 2.0)) > 0.75) continue;
        const float lateral = (fmod(abs(id), 2.0) - 0.5) * 0.24;
        const float2 d = float2((frac(p.x) - 0.5) * 0.74 / 0.13, (xz.y - zc - lateral) / 0.052);
        depth = max(depth, 1.0 - smoothstep(0.6, 1.0, dot(d, d)));
    }
    return depth;
}

void GroundLayers(VSOut i, float3 ng, out float3 albedo, out float3 n, out float occlusion, out float roughness, out float near) {
    const float2 xz = i.worldPos.xz;
    const float2 gx = ddx(xz), gy = ddy(xz);
    const float dist = length(i.worldPos - gLight.eye.xyz);
    if (FEATURE_OFF(64)) { albedo = gAlbedo.SampleGrad(gTexSampler, GroundCell(xz / 3.3, 0), gx / 6.6, gy / 6.6).rgb * 0.78; n = ng; occlusion = 1.0; roughness = 0.8; near = 0.0; return; }
    const float4 bake = gSurface.SampleLevel(gTexSampler, (saturate((xz + 256.0) / 512.0) * 0.992 + 0.004 + float2(1, 1)) * 0.5, 0.0);
    near = bake.g; // 1 at a wall's foot, gone about 3 m out
    // Which layers: gravel where noise and slope and building edges gather it, wagon dust in long slow bands.
    const float n0 = TileNoise(xz * 0.13) * 0.55 + TileNoise(xz * 0.61 + 3.0) * 0.3 + TileNoise(xz * 2.3) * 0.15;
    const float gravelWant = smoothstep(0.35, 0.7, (1.0 - ng.y) * 2.2 + (n0 - 0.5) * 0.4);
    const float tracks = smoothstep(0.42, 0.66, TileNoise(float2(xz.x * 0.03, xz.y * 0.28) + 11.0));
    const float2 pPom = float2(0.0, 0.0);
    const float3 v = normalize(gLight.eye.xyz - i.worldPos);
    const float fade = saturate((15.0 - dist) / 8.0);
    float2 pomXz = xz;
    const float kDirtM = 6.1, kGravelM = 0.75, kTrackM = 5.7;
    const int cellMain = gravelWant > 0.5 ? 1 : 0;
    const float mainM = cellMain == 1 ? kGravelM : kDirtM;
    if (fade > 0.0 && v.y > 0.08 && !FEATURE_OFF(1)) { // parallax on the leading layer, within 15 m
        const float depth = (cellMain == 1 ? 0.03 : 0.006) * fade;
        const float2 along = v.xz / max(v.y, 0.3) * depth;
        const int steps = int(lerp(10.0, 4.0, v.y));
        const float stepSize = 1.0 / steps;
        float layer = 0.0, prevGap = 0.0, gap = 0.0;
        float2 at = xz;
        [loop] for (int k = 0; k < steps; ++k) {
            gap = (1.0 - gSurface.SampleGrad(gTexSampler, GroundCell(at / mainM, cellMain), gx / mainM * 0.5, gy / mainM * 0.5).a) - layer;
            if (gap <= 0.0) break;
            prevGap = gap;
            layer += stepSize;
            at = xz - along * layer;
        }
        if (gap < 0.0) at = xz - along * (layer - stepSize + stepSize * prevGap / max(prevGap - gap, 1e-5));
        pomXz = at;
    }
    // Sample a layer and a rotated second scale of it, so no repeat lines up.
    float4 a0, s0;
    float3 t0;
    {
        const float2 p = pomXz / mainM;
        const float2 q = float2(p.x * 0.8 - p.y * 0.6, p.x * 0.6 + p.y * 0.8) * 1.37 + 0.37;
        const float2 d1 = gx / mainM * 0.5, d2 = gy / mainM * 0.5;
        const float m = smoothstep(0.35, 0.65, TileNoise(xz * 0.21 + 5.0));
        const float2 cp = GroundCell(p, cellMain), cq = GroundCell(q, cellMain);
        a0 = lerp(gAlbedo.SampleGrad(gTexSampler, cp, d1, d2), gAlbedo.SampleGrad(gTexSampler, cq, d1 * 1.37, d2 * 1.37), m);
        s0 = lerp(gSurface.SampleGrad(gTexSampler, cp, d1, d2), gSurface.SampleGrad(gTexSampler, cq, d1 * 1.37, d2 * 1.37), m);
        t0 = lerp(gNormalMap.SampleGrad(gTexSampler, cp, d1, d2).xyz, gNormalMap.SampleGrad(gTexSampler, cq, d1 * 1.37, d2 * 1.37).xyz, m) * 2.0 - 1.0;
    }
    // The other layer shows only in the band between them, by height.
    const float edge = cellMain == 1 ? gravelWant : max(gravelWant, 0.0);
    if (edge > 0.02 && edge < 0.98) {
        const int cellOther = cellMain == 1 ? 0 : 1;
        const float otherM = cellOther == 1 ? kGravelM : kDirtM;
        const float2 c = GroundCell(pomXz / otherM, cellOther), d1 = gx / otherM * 0.5, d2 = gy / otherM * 0.5;
        const float4 s1 = gSurface.SampleGrad(gTexSampler, c, d1, d2);
        const float gravelShare = cellMain == 1 ? edge : edge;
        const float hMain = s0.a + (cellMain == 1 ? gravelShare : 1.0 - gravelShare), hOther = s1.a + (cellOther == 1 ? gravelShare : 1.0 - gravelShare);
        const float w = smoothstep(-0.05, 0.05, hOther - hMain); // sharp: single stones sitting on the dirt, not a mush
        if (w > 0.0) {
            a0 = lerp(a0, gAlbedo.SampleGrad(gTexSampler, c, d1, d2), w);
            s0 = lerp(s0, s1, w);
            t0 = lerp(t0, gNormalMap.SampleGrad(gTexSampler, c, d1, d2).xyz * 2.0 - 1.0, w);
        }
    }
    if (tracks > 0.01 && cellMain == 0) { // wagon-track dust, faded in over the dirt in long soft bands
        const float2 c = GroundCell(pomXz / kTrackM, 2), d1 = gx / kTrackM * 0.5, d2 = gy / kTrackM * 0.5;
        const float w = tracks * 0.75;
        a0 = lerp(a0, gAlbedo.SampleGrad(gTexSampler, c, d1, d2) * 0.82, w);
        t0 = lerp(t0, gNormalMap.SampleGrad(gTexSampler, c, d1, d2).xyz * 2.0 - 1.0, w);
    }
    // Stones take the local dust: most of their photographed colour would read as confetti in this desert.
    const float stones = cellMain == 1 ? 1.0 : gravelWant;
    // Colour from a ramp of dry desert dirt, pale tan to rust (display values chosen by eye, not matched to photographs); the map keeps only a
    // gentle share of its detail, since at full contrast it reads as carpet.
    const float grey = dot(a0.rgb, float3(0.3, 0.6, 0.1));
    const float mean = cellMain == 1 ? 0.43 : 0.41;
    const float detail = lerp(1.0, grey / mean, lerp(0.55, 0.9, stones));
    const float ramp = smoothstep(0.3, 0.72, TileNoise(xz * 0.021 + 2.0) * 0.6 + TileNoise(xz * 0.13 + 9.0) * 0.4);
    const float3 dirt = lerp(float3(0.74, 0.58, 0.41), float3(0.66, 0.43, 0.28), ramp);
    albedo = dirt * detail * lerp(1.0, float3(0.92, 0.88, 0.84), stones);
    float3 extra = float3(0, 0, 0);
    // Ruts and prints are packed darker and tilt the light; small stones lie on top, each a stone.
    const float closeFade = saturate((60.0 - dist) / 25.0);
    if (closeFade > 0.0) {
        const float2 rut = GroundRuts(xz);
        albedo *= 1.0 - 0.2 * rut.x * closeFade;
        extra.z += rut.y * 0.28 * closeFade;
    }
    if (dist < 22.0) {
        const float printFade = saturate((22.0 - dist) / 8.0);
        albedo *= 1.0 - 0.2 * GroundPrints(xz) * printFade * smoothstep(9.0, 6.0, abs(xz.y));
    }
    occlusion = s0.r;
    roughness = s0.g;
    const float strength = lerp(0.12, 0.26, saturate((35.0 - dist) / 22.0));
    n = normalize(ng + float3(t0.x, 0.0, -t0.y) * strength + extra);
}

// Albedo and normal after the material textures; alpha below 0.4 is cut out.
void Material(VSOut i, out float4 color, out float3 n) {
    n = normalize(i.normal);
    const bool ground = abs(i.color.a - 0.75) < 0.1; // tiling ground: hide the repeat and vary it at large scale
    const bool groundSplat = abs(i.color.a - 0.2) < 0.05; // Western terrain: COLOR_0 stores material weights
    float4 tex = ground ? SampleNoRepeat(gAlbedo, i.uv) : gAlbedo.SampleGrad(gTexSampler, i.uv + sPomOffset, ddx(i.uv), ddy(i.uv));
    clip(tex.a - 0.4);
    if (IsWorldSurface(i.color.a) && !sArt) { // a tile's own blotches beat against the eye from afar (old brick above all): pull them toward its average
        const float far = smoothstep(9.0, 28.0, length(i.worldPos - gLight.eye.xyz));
        if (far > 0.0) tex.rgb = lerp(tex.rgb, gAlbedo.SampleLevel(gTexSampler, i.uv, 7.0).rgb, far * 0.6);
    }
    color = i.color * tex;
    if (groundSplat) {
        // The export's COLOR_0 channels encode the splat: green carries the broad vegetation mask,
        // red the exposed earth mask, and blue the rocky mask. Retain the authored base texture as detail.
        float3 weights = max(i.color.rgb, 0.0);
        // Some export triangles have all three weights at zero (8000 vertices of perdition_ground.glb): that drew pure black
        // slashes on the hills. Fall back to a mostly-earth mix there, fading in so the edge of the patch does not show.
        const float total = weights.r + weights.g + weights.b;
        weights = lerp(float3(0.7, 0.2, 0.1), weights / max(total, 1e-4), saturate(total * 20.0));
        float3 earth = float3(0.46, 0.38, 0.29);
        float3 grass = float3(0.38, 0.37, 0.24);
        float3 rock = float3(0.39, 0.36, 0.31);
        float3 splat = earth * weights.r + grass * weights.g + rock * weights.b;
        color.rgb = tex.rgb * splat * 1.5;
        // A second copy at another scale and angle breaks the repeat, and slow patches of paler dust and darker packed earth sit on top.
        const float2 uv2 = float2(i.uv.x * 0.8 - i.uv.y * 0.6, i.uv.x * 0.6 + i.uv.y * 0.8) * 0.37 + 0.31;
        const float3 tex2 = gAlbedo.Sample(gTexSampler, uv2).rgb;
        const float patchA = TileNoise(i.worldPos.xz * 0.11) * 0.6 + TileNoise(i.worldPos.xz * 0.47) * 0.4;
        const float patchB = TileNoise(i.worldPos.xz * 0.033 + 7.0);
        color.rgb *= lerp(float3(1, 1, 1), tex2 / max(dot(gAlbedo.SampleLevel(gTexSampler, i.uv, 8.0).rgb, float3(0.33, 0.34, 0.33)), 0.05), 0.5);
        color.rgb *= lerp(float3(0.78, 0.77, 0.76), float3(1.05, 1.05, 1.05), smoothstep(0.25, 0.75, patchA));
        color.rgb *= lerp(0.88, 1.08, patchB);
        // Past the grass instances' reach (about 40 m) the ground takes the grass's dry olive, so the field's edge is a
        // change of colour, not of kind. One distance and a mix per pixel.
        const float far = smoothstep(28.0, 60.0, length(i.worldPos - gLight.eye.xyz));
        color.rgb = lerp(color.rgb, color.rgb * float3(0.80, 0.82, 0.62), far * (0.4 + 0.6 * saturate(weights.g * 2.0)));
    }
    if (ground) { // the tile's own contrast reads as blotches from afar: pull it toward its average, then vary at large scale and tint steep rock
        color.rgb = lerp(gAlbedo.SampleLevel(gTexSampler, i.uv, 8.0).rgb, color.rgb, 0.45);
        color.rgb *= lerp(0.72, 1.18, TileNoise(i.uv * 0.09) * 0.65 + TileNoise(i.uv * 0.41) * 0.35);
        color.rgb = lerp(color.rgb, color.rgb * float3(1.25, 0.82, 0.62), smoothstep(0.82, 0.55, n.y));
    }

    // Tangent frame from screen-space derivatives (no stored tangents). glTF uv has v pointing down the image, the
    // normal map convention has +Y up, hence the flip.
    float2 uv = float2(i.uv.x, 1.0 - i.uv.y);
    float3 dp1 = ddx(i.worldPos), dp2 = ddy(i.worldPos);
    float2 du1 = ddx(uv), du2 = ddy(uv);
    float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    float3 T = dp2perp * du1.x + dp1perp * du2.x;
    float3 B = dp2perp * du1.y + dp1perp * du2.y;
    float invMax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    const bool skin = abs(i.color.a - 0.5) < 0.1; // skin's normal slot holds a pore tile, repeated and kept subtle
    const float2 normalUv = skin ? i.uv * SkinDetailScale() : i.uv;
    float3 tn = (ground ? SampleNoRepeat(gNormalMap, i.uv) : gNormalMap.SampleGrad(gTexSampler, normalUv + sPomOffset, ddx(normalUv), ddy(normalUv))).xyz * 2.0 - 1.0;
    if (skin) tn = SkinPoreNormal(i.uv, normalUv + sPomOffset, tn);
    if (ground) tn = normalize(float3(tn.xy * 0.5, 1.0));
    if (abs(i.color.a - 0.2) < 0.05) { // the splat ground's tile bumps read as ripples from afar; near the feet they are the ground
        const float nearGround = saturate((30.0 - length(i.worldPos - gLight.eye.xyz)) / 18.0);
        tn = normalize(float3(tn.xy * lerp(0.25, 1.0, nearGround), 1.0));
    }
    if (skin) {
        // Pores: in skin's soft light (wrapped diffuse, broad highlights, mostly sky) a tilted normal alone barely shows. What
        // shows a pore is the pit holding back light: the tile's own slope (1 - z) darkens it, faded out before a pore is a pixel.
        const float pit = saturate((0.975 - tn.z) * 5.0) * saturate(2.0 - length(fwidth(normalUv)) * 512.0);
        const float depth = SkinPoreDepth();
        color.rgb *= 1.0 - saturate(0.24 * depth) * pit;
        // Thin skin (eyelids, around the eyes: the extra map's G toward 0) shows the blood under it: the tutorial's scatter map drives
        // a hue/saturation node, saturating and darkening the albedo there. Same mask the pore atlas uses to pick its thin-skin tile.
        const float thinSkin = saturate(1.0 - gExtra.Sample(gTexSampler, i.uv).g * 2.0);
        { // Blood flush: real skin is not one tone. Two scales of soft blotches push it between a warmer, redder and a cooler, paler
            // value (cheeks and nose run red, the brow and temples paler), the way capillary density shifts under the surface.
            const float2 fp = i.uv * 14.0, ft = i.uv * 52.0;
            const float2 fc = floor(fp), fu = frac(fp), gc = floor(ft), gu = frac(ft);
            const float2 fs = fu * fu * (3.0 - 2.0 * fu), gs = gu * gu * (3.0 - 2.0 * gu);
            #define SKIN_H(c) frac(sin(dot((c), float2(127.1, 311.7))) * 43758.5453)
            const float broad = lerp(lerp(SKIN_H(fc), SKIN_H(fc + float2(1, 0)), fs.x), lerp(SKIN_H(fc + float2(0, 1)), SKIN_H(fc + float2(1, 1)), fs.x), fs.y);
            const float fine = lerp(lerp(SKIN_H(gc), SKIN_H(gc + float2(1, 0)), gs.x), lerp(SKIN_H(gc + float2(0, 1)), SKIN_H(gc + float2(1, 1)), gs.x), gs.y);
            #undef SKIN_H
            const float flush = (broad - 0.5) * 0.7 + (fine - 0.5) * 0.3; // -0.5 .. 0.5
            color.rgb *= float3(1.0 + 0.10 * flush, 1.0 - 0.07 * flush, 1.0 - 0.09 * flush);
        }
        if (thinSkin > 0.01) {
            const float lum = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
            color.rgb = lerp(color.rgb, max(lerp(lum.xxx, color.rgb, 1.35) * 0.88, 0.0), thinSkin * 0.6);
        }
        tn = normalize(float3(-tn.xy * 1.1 * depth, 1.0)); // the tilt still catches the highlights; negated: the tile's bumps become pits (divots). 1.1: deep enough to break up the sheen
        const float w = SkinWrinkle(i);
        if (w > 0.01) { // the surface map's alpha is a groove height, faded in as the skin compresses
            const float e = 1.0 / 1024.0;
            tn.x -= (gSurface.Sample(gTexSampler, i.uv + float2(e, 0)).a - gSurface.Sample(gTexSampler, i.uv - float2(e, 0)).a) * w * 2.5;
            tn.y += (gSurface.Sample(gTexSampler, i.uv + float2(0, e)).a - gSurface.Sample(gTexSampler, i.uv - float2(0, e)).a) * w * 2.5;
            tn = normalize(tn);
        }
    }
    if (abs(i.color.a - 0.61) < 0.005) { // cloth: a twill of threads, faded before it is smaller than a pixel
        const float2 g = i.uv * 1600.0; // radians: 255 threads across the texture, about 3 mm on a shirt
        const float fade = saturate(2.0 - length(fwidth(g)) * 1.5); // gone before a thread is 4 pixels wide, so it never beats against the screen
        tn.xy += float2(cos(g.x + g.y), 0.5 * cos(g.x - g.y)) * 0.10 * fade;
        tn = normalize(tn);
        color.rgb *= 1.0 + 0.03 * sin(g.x + g.y) * fade;
    }
    n = normalize(mul(tn, float3x3(T * invMax, B * invMax, n)));
}

// ------------------------------------------------------------------ terrain material, per pixel from world position
float TerrainHash(float2 p) {
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.x + p.y) * p.x);
}
float TerrainNoise(float2 p) {
    float2 c = floor(p), f = p - c;
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(TerrainHash(c), TerrainHash(c + float2(1, 0)), f.x),
                lerp(TerrainHash(c + float2(0, 1)), TerrainHash(c + float2(1, 1)), f.x), f.y);
}
// Octaves smaller than a pixel fade out instead of shimmering. Bounded by the coarsest octave's 256 m.
float TerrainFbm(float2 p, float pixel) {
    float sum = 0.0, amp = 0.5, scale = 1.0 / 256.0, weight = 0.0;
    for (int k = 0; k < 11; ++k, amp *= 0.68, scale *= 2.0) { // down to about 12 cm
        float fade = saturate(0.5 / (pixel * scale) - 0.5);
        sum += amp * fade * TerrainNoise(p * scale + k * 17.0);
        sum += amp * (1.0 - fade) * 0.5;
        weight += amp;
    }
    return saturate((sum / weight - 0.5) * 2.6 + 0.5); // the sum of many octaves bunches around 0.5
}

float4 TerrainPS(VSOut i) {
    float3 n = normalize(i.normal);
    float2 p = i.worldPos.xz;
    float pixel = max(length(ddx(p)), length(ddy(p))) + 1e-4;
    float e = max(pixel, 0.25);
    float h0 = TerrainFbm(p, pixel);
    // The fine surface bumps the normal, from the same noise that colors it.
    float2 g = float2(TerrainFbm(p + float2(e, 0), pixel) - h0, TerrainFbm(p + float2(0, e), pixel) - h0) / e;
    float3 bumped = normalize(n - float3(g.x, 0, g.y) * clamp(e * 3.0, 0.4, 60.0));

    float y = i.worldPos.y + (h0 - 0.5) * 60.0;
    float slope = 1.0 - n.y;
    float3 grass = lerp(float3(0.03, 0.07, 0.016), float3(0.14, 0.16, 0.04), h0);
    float3 dirt = float3(0.14, 0.10, 0.065);
    float3 rock = lerp(float3(0.07, 0.065, 0.06), float3(0.30, 0.27, 0.23), h0);
    float3 sand = float3(0.48, 0.40, 0.25);
    float3 snow = float3(0.82, 0.86, 0.92);
    float3 a = lerp(grass, dirt, smoothstep(0.42, 0.62, TerrainNoise(p / 37.0)) * 0.45);
    a = lerp(a, sand, smoothstep(-9.0, -14.0, i.worldPos.y + (h0 - 0.5) * 8.0));
    a = lerp(a, rock, smoothstep(0.10, 0.22, slope + (h0 - 0.5) * 0.22 + saturate((i.worldPos.y - 180.0) / 300.0) * 0.12));
    a = lerp(a, snow, smoothstep(560.0, 680.0, y) * smoothstep(0.38, 0.14, slope + (0.5 - h0) * 0.2));
    float share;
    float3 lit = LitPbr(a, bumped, i.worldPos, gPixelShadow * CloudSun(i.worldPos), 0.88, 0.0, CloudSun(i.worldPos), share);
    return float4(lit, 0.76 + 0.23 * share);
}

// Material factors from the glTF, packed in the vertex alpha by mesh_import.h.
void PbrFromAlpha(float a, out float roughness, out float metal) {
    roughness = a < 0.25 ? 0.92 : 0.72; // the splat ground is dry dirt
    metal = 0.0;
    if (a > 0.855 && a < 0.915) {
        const int code = int(round((a - 0.86) / 0.05 * 63.0));
        metal = float(code & 1);
        roughness = max(float(code >> 1) / 31.0, 0.04);
    }
}

// Weathering by rule, no painting (the idea of Hsu and Wong, "Simulating dust accumulation", 1995, and of the material graphs in the Decima
// reference, reduced to what a pixel knows). Dirt climbs from the object's foot, dust lies on what faces up, streaks run down walls and
// rust eats metal, each broken by noise; the object's origin seeds a small tint so repeated props differ.
void Weather(VSOut i, float3 ng, inout float3 albedo, inout float roughness, inout float metal) {
    if (FEATURE_OFF(8)) return;
    const float3 w = i.worldPos;
    const float h = max(w.y - i.origin.y, 0.0);
    const float seed = TileHash(i.origin.xz * 0.37 + i.origin.y);
    const float seed2 = TileHash(i.origin.zx * 0.71 + 3.1);
    albedo *= lerp(0.84, 1.12, seed) * lerp(float3(1.05, 0.99, 0.93), float3(0.95, 1.0, 1.06), seed2);
    const float2 along = abs(ng.x) > abs(ng.z) ? w.zy : w.xy; // across the wall, then up it
    const float wall = 1.0 - abs(ng.y);
    const float blotch = TileNoise(w.xz * 1.3 + w.y * 1.1) * 0.6 + TileNoise(w.xz * 5.7 + w.y * 4.9) * 0.4;
    const float3 dust = float3(0.20, 0.14, 0.09);
    // Rain streaks: long in y, narrow across, a few of them strong.
    const float streak = pow(TileNoise(float2(along.x * 2.7 + seed * 40.0, along.y * 0.22)), 3.0) * (0.55 + 0.45 * TileNoise(float2(along.x * 11.0, along.y * 0.6)));
    if (metal > 0.5 || (h > 2.6 && ng.y > 0.4 && ng.y < 0.995)) { // tin, stove pipes, and a pitched roof high on a building: rust in patches and runs
        const float rust = smoothstep(0.42, 0.72, TileNoise(w.xz * 0.9 + seed * 30.0) * 0.55 + TileNoise(w.xz * 4.3 + w.y) * 0.3 + streak * 1.2 * (0.3 + wall));
        albedo = lerp(albedo, float3(0.13, 0.058, 0.03) * lerp(0.7, 1.3, blotch), rust * 0.9);
        metal = lerp(metal, 0.0, rust);
        roughness = lerp(max(roughness, 0.42), 0.9, rust);
    } else {
        albedo *= 1.0 - 0.32 * saturate(streak * 3.0) * wall;
    }
    const float upward = saturate(ng.y);
    albedo = lerp(albedo, dust * lerp(0.8, 1.2, blotch), upward * upward * smoothstep(0.25, 0.8, blotch) * 0.35);
    // Splash and blown dirt at the foot, about 55 cm with a noisy top; contact darkness in the last few centimetres (Grounding).
    const float rise = saturate(1.0 - (h + (blotch - 0.5) * 0.4) / 0.55);
    albedo = lerp(albedo, dust * lerp(0.75, 1.1, blotch), rise * rise * wall * 0.6);
    albedo *= lerp(0.35, 1.0, smoothstep(0.0, 0.22, h + (blotch - 0.5) * 0.08));
    roughness = lerp(roughness, 0.95, max(rise * rise * wall, upward * 0.4));
}

// Window panes: the sky in Fresnel over a room that is not there. Interior mapping (van Dongen, "Interior Mapping", 2008): the ray enters a
// box per pane cell, in world axes; floor, ceiling and walls are dim and plain, and a noisy shape stands in for what is inside.
float4 GlassPS(VSOut i) {
    const float3 v = normalize(gLight.eye.xyz - i.worldPos);
    // A pane is often a thin box, and its faces' normals can point away from the viewer: turned to face it, or Fresnel reads a
    // grazing angle head-on and the pane becomes a mirror.
    const float3 ng = faceforward(normalize(i.normal), -v, normalize(i.normal));
    const float3 d = -v;
    const float3 roomSize = float3(5.0, 3.6, 5.0);
    const float3 p = i.worldPos / roomSize;
    const float3 cell = floor(p);
    const float3 s = sign(d) * 0.5 + 0.5;
    const float3 t = (cell + s - p) / (d / roomSize + sign(d) * 1e-5 + 1e-6);
    const float tHit = min(t.x, min(t.y, t.z));
    const float3 q = (i.worldPos + d * tHit); // on a room's surface
    float3 room;
    const float2 grain = t.y <= t.x && t.y <= t.z ? q.xz : float2(q.x + q.z, q.y);
    const float planks = 0.8 + 0.2 * TileNoise(float2(grain.x * 0.7, grain.y * 6.0));
    if (t.y <= t.x && t.y <= t.z) room = (d.y < 0.0 ? float3(0.075, 0.048, 0.03) : float3(0.03, 0.024, 0.018)) * planks;
    else {
        room = float3(0.06, 0.045, 0.03) * planks;
        const float row = frac(q.y * 1.6);
        room *= lerp(1.0, 0.4, step(0.82, row)) * lerp(0.55, 1.0, TileNoise(float2((q.x + q.z) * 3.0, floor(q.y * 1.6) * 7.0)));
    }
    room *= saturate(1.0 - 0.12 * tHit) + 0.15;
    const float cloud = CloudSun(i.worldPos);
    float skyVis;
    const float3 ambientIn = IndirectDiffuse(i.worldPos, ng, skyVis);
    const float nv = saturate(dot(ng, v));
    const float F = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
    const float3 r = reflect(d, ng);
    const float wobble = (TileNoise(i.worldPos.xy * 5.0 + i.worldPos.z * 5.0) - 0.5) * 0.02; // old glass is not flat
    const float3 refl = SkyReflection(normalize(r + float3(wobble, wobble, -wobble)), 0.05);
    const float smudge = TileNoise(i.worldPos.xz * 3.0 + i.worldPos.y * 3.0) * 0.5 + TileNoise(i.worldPos.xz * 13.0 + i.worldPos.y * 13.0) * 0.5;
    const float3 film = float3(0.42, 0.33, 0.24) * ambientIn * 0.012 * smoothstep(0.3, 0.8, smudge);
    const float3 rh = normalize(v + gLight.sunDir.xyz);
    const float sunLit = gPixelShadow * cloud;
    const float glint = pow(saturate(dot(ng, rh)), 1500.0) * 40.0 * sunLit * saturate(dot(ng, gLight.sunDir.xyz) * 8.0);
    // Wear: a grayscale mask (scratches, fingerprints, drips, grime; Assets\glass) projected along the pane's dominant axis in metres, in
    // the albedo slot. Mirrored tiling hides the repeat. Dirt scatters light and dims the view through; scratches catch sun and sky.
    const float3 an = abs(ng);
    const float2 wuv = an.y > max(an.x, an.z) ? i.worldPos.xz : (an.x > an.z ? i.worldPos.zy : i.worldPos.xy);
    // Every pane differs: i.color is (across, up, seed) from TagPanes. The seed moves, flips and rescales the mask, and decides how worn
    // the pane is; the building's origin makes some buildings dirtier than others, and about half the panes gather dirt in their corners.
    const float2 pane = i.color.rg;
    const float seed = i.color.b, seed2 = TileHash(float2(seed * 91.7, 3.3)), seed3 = TileHash(float2(seed * 37.1, 8.9));
    const float building = TileHash(i.origin.xz * 0.37 + i.origin.y);
    const float2 flip = float2(seed2 > 0.5 ? -1.0 : 1.0, seed3 > 0.5 ? -1.0 : 1.0);
    const float2 tile = abs(frac((wuv * flip * lerp(0.28, 0.55, seed3) + seed * float2(7.3, 3.1)) * 0.5) * 2.0 - 1.0); // mirrored, 0.9 to 1.8 m per mask
    const float detail = gAlbedo.Sample(gTexSampler, tile).r;
    const float paneWear = lerp(0.35, 1.25, seed2 * seed2) * lerp(0.6, 1.2, building);
    const float sideDist = min(pane.x, 1.0 - pane.x); // in pane fractions
    const float vertDist = pane.y < 0.5 ? pane.y : (1.0 - pane.y) * 1.8; // dirt settles low: top corners need to be closer
    const float cornerDist = length(float2(sideDist, vertDist));
    const float cornerRaw = saturate(1.0 - cornerDist / lerp(0.22, 0.5, seed3));
    const float cornerGate = step(0.45, TileHash(float2(seed * 53.9, building * 17.0))) * lerp(0.5, 1.0, building); // some panes only
    const float breakup = gAlbedo.SampleLevel(gTexSampler, tile * 0.8 + 0.37, 2.0).r * 2.5 + TileNoise(pane * 9.0 + seed * 40.0) * 0.6;
    const float corner = saturate(cornerRaw * cornerRaw * (0.5 + breakup) * 1.6) * cornerGate;
    const float sill = saturate(1.0 - pane.y * 7.0) * (0.3 + 0.7 * TileNoise(float2(pane.x * 12.0 + seed * 20.0, 1.0))) * cornerGate * 0.5; // a grimy line along the bottom
    const float edgeDirt = saturate(corner + sill);
    // Clear glass stays clear: only blotches of the mask's blur count as film, never its overall average, so most of a pane is see-through.
    const float blotch = smoothstep(0.14, 0.4, gAlbedo.SampleLevel(gTexSampler, tile, 4.0).r * paneWear);
    const float grime = saturate(blotch * 0.4 + smoothstep(0.3, 0.7, detail) * 0.2 * paneWear + edgeDirt * 1.0);
    const float scratch = smoothstep(0.3, 0.65, detail) * saturate(paneWear * 1.2 - 0.1);
    const float fade = saturate(1.0 - length(gLight.eye.xyz - i.worldPos) / 60.0); // far panes read as plain glass
    const float3 dirtTint = float3(0.42, 0.36, 0.28);
    const float3 sunWear = gLight.sunColor.rgb * sunLit * saturate(abs(dot(ng, gLight.sunDir.xyz)) * 3.0);
    const float3 wearLight = dirtTint * grime * (ambientIn * 0.35 + sunWear * 0.04) + (ambientIn * 0.6 + sunWear * 0.5 + refl * 0.5) * scratch * 0.55;
    room *= 1.0 - 0.45 * grime * fade;
    const float Fw = saturate(F + grime * 0.02 * fade);
    // Glass is fully clear: only its reflection is drawn, where it is strong enough to stand alone (towards grazing angles) or the
    // sun glints in it. Nothing here is blended, so a weaker reflection over the view through cannot be shown and is left out;
    // dirt and scratches stay in the reflection's colour only. TAA's clamp turns a dithered pane back into a mirror, so no dither.
    const float3 added = refl * Fw * lerp(1.0, skyVis, 0.3) + gLight.sunColor.rgb * glint + (film + wearLight * fade) * Fw;
    const float cover = saturate(Fw + glint * 0.05);
    if (cover < 0.25) discard;
    return float4(added, -0.99); // negative: the composite's screen-space reflection
}

// A tooth from FaceRig (alpha 0.995): vertex colour is enamel times how deep in the mouth it stands; u runs from the biting edge to
// the neck, v is the tooth's own variation. Thin enamel at the edge shows the mouth's dark through it, dentine warms the neck.
float4 ToothPS(VSOut i) {
    const float t = i.uv.x, seed = i.uv.y;
    float3 n = normalize(i.normal);
    // perikymata: fine horizontal growth ridges in the enamel, gone before they are smaller than a pixel
    const float phase = t * 62.0 + seed * 40.0, fade = saturate(1.5 - fwidth(phase) * 0.8);
    const float3 dT = ddy(i.worldPos) * ddx(t) - ddx(i.worldPos) * ddy(t);
    n = normalize(n + normalize(cross(n, cross(dT, n)) + 1e-6) * sin(phase) * 0.012 * fade);
    const float ao = saturate(max(i.color.r, max(i.color.g, i.color.b)) / 0.95) * lerp(1.0, 0.8, smoothstep(0.5, 1.0, t));
    float3 albedo = pow(saturate(i.color.rgb / max(max(i.color.r, max(i.color.g, i.color.b)), 0.02) * 0.92), 2.2);
    albedo *= lerp(0.93, 1.03, seed) * lerp(0.97, 1.02, TileNoise(float2(t * 7.0, seed * 91.0)));
    const float edge = 1.0 - smoothstep(0.0, 0.22, t);
    albedo = lerp(albedo, albedo * float3(0.62, 0.70, 0.80), edge); // translucent edge, cool
    albedo = lerp(albedo, albedo * float3(1.0, 0.92, 0.78), smoothstep(0.45, 0.95, t)); // dentine through the thin neck
    const float3 lit = LitMouth(albedo, n, i.worldPos, gPixelShadow, CloudSun(i.worldPos), ao, lerp(0.16, 0.24, t), 0.3, float3(1.0, 0.88, 0.72));
    return float4(lit, 1.0);
}

// Gums, palate, floor of the mouth, tongue (alpha 0.985): wet, and full of blood, so light spreads wide and comes out red.
float4 MouthWetPS(VSOut i) {
    const float deep = max(i.color.r, max(i.color.g, i.color.b));
    const float3 lit = LitMouth(pow(saturate(i.color.rgb / max(deep, 0.02) * 0.75), 2.2), normalize(i.normal), i.worldPos, gPixelShadow, CloudSun(i.worldPos),
                                pow(saturate(deep / 0.76), 1.5), 0.28, 0.6, float3(1.0, 0.45, 0.4));
    return float4(lit, 1.0);
}

// An eyeball with the generated map (alpha 0.975): UVs are a front view, r = 1 at the silhouette, iris edge at 0.49 (see
// gameenginetools\eye_atlas). The iris lies behind the clear cornea, so it is sampled where a ray bent by the cornea would land on it.
float4 EyePS(VSOut i) {
    const float3 n = normalize(i.normal), v = normalize(gLight.eye.xyz - i.worldPos);
    const float2 c = i.uv - 0.5;
    const float r = length(c) * 2.0, iris = smoothstep(0.53, 0.46, r);
    const float3 dp1 = ddx(i.worldPos), dp2 = ddy(i.worldPos);
    const float2 du1 = ddx(i.uv), du2 = ddy(i.uv);
    const float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    const float3 T = normalize(dp2perp * du1.x + dp1perp * du2.x + 1e-6), B = normalize(dp2perp * du1.y + dp1perp * du2.y + 1e-6);
    const float3 radial = normalize(T * c.x + B * c.y + 1e-6);
    const float chamber = iris * sqrt(saturate(1.0 - r * r / (0.49 * 0.49))); // the anterior chamber is deepest over the pupil
    const float2 refracted = i.uv - float2(dot(v, T), dot(v, B)) / max(dot(n, v), 0.35) * 0.075 * chamber;
    float3 albedo = pow(saturate(gAlbedo.Sample(gTexSampler, refracted).rgb), 2.2);
    // The white of the eye is not white: a dim grey-pink, so the iris and the wet glint carry the eye. And the limbal ring,
    // the dark band where the iris meets the white, which reads strongly on a real eye.
    albedo = lerp(albedo * float3(0.74, 0.68, 0.66), albedo, iris);
    albedo *= 1.0 - 0.5 * smoothstep(0.40, 0.48, r) * smoothstep(0.57, 0.49, r);
    // The cornea is a tighter sphere than the ball, so its normal turns faster; the limbus is where it meets the white.
    const float3 corneaN = normalize(n + radial * r * 0.55 * smoothstep(0.62, 0.42, r));
    const float3 irisN = normalize(n - radial * r * 0.4);
    const float up = -c.y * 2.0, side = abs(c.x) * 2.0;
    // The upper lid and lashes shade the top of the eyeball deeply (the iris sits in that shadow), less from below and in the corners.
    const float lid = (1.0 - 0.85 * smoothstep(-0.05, 0.55, up)) * (1.0 - 0.35 * smoothstep(0.15, 0.62, -up)) * (1.0 - 0.45 * smoothstep(0.45, 0.95, side));
    const float3 lit = LitEyeball(albedo, n, corneaN, irisN, i.worldPos, gPixelShadow, CloudSun(i.worldPos), lid, iris, radial, r);
    return float4(lit, 1.0);
}

// ---- Hair cards (vertex alpha 0.68) ---------------------------------------------------------------------------------------------
// Built by AddHairCards / AddScalpCards in mesh_import.h; the vertex colour packs per card data (src/character/hair_vertex.h):
//   r = floor(cardRand * 1000) + t * 0.999      g = class + depth * 0.999      b = 0 (colour from the sheet) or packed melanin
// Classes: 0 scalp, 1 fine (brows, lashes), 2 beard. depth: 0 on the outside of the hair volume .. 1 against the skin.
// Data mode (b > 0): the albedo slot is the generated strand atlas (R brightness, G root-tip, B strand random, A coverage) and
// the normal slot its material (R occlusion, G roughness, B cuticle shift); colour comes from melanin, as real hair's does.
struct HairData {
    float t, cardRand, depth, melanin, red, grey;
    int cls;
    bool melaninMode;
};
HairData HairDecode(float4 c) {
    HairData h;
    h.cardRand = floor(c.r) / 1000.0;
    h.t = saturate((c.r - floor(c.r)) / 0.999);
    h.cls = (int)floor(c.g + 1e-3);
    h.depth = saturate((c.g - floor(c.g)) / 0.999);
    const float pb = floor(c.b);
    h.melaninMode = c.b > 1e-4;
    h.melanin = saturate((c.b - pb) / 0.999);
    h.red = floor(pb / 100.0) / 100.0;
    h.grey = fmod(pb, 100.0) / 99.0;
    return h;
}

// Hashed alpha (Wyman and McGuire 2017), screen space and per frame: a pixel keeps a fragment with probability = coverage, so a
// strand thinner than a pixel, or a far card whose mips averaged its strands into a haze, keeps its true share and TAA resolves
// it to a soft edge. No sorting, no blending, no mip that eats the hair as it recedes.
float HairHash(float2 pixel, float salt) {
    const float2 p = pixel + (gLight.eye.w + salt * 17.0) * 5.588238;
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Melanin to the strand's base colour (Karis's, linear): a ramp through typical linear reflectances of real hair, platinum to
// black, interpolated in log space (melanin absorbs exponentially). `red` (pheomelanin) keeps red and takes green and blue.
float3 HairMelaninColor(float m, float red) {
    static const float3 ramp[6] = {float3(0.78, 0.66, 0.48), float3(0.46, 0.33, 0.18), float3(0.19, 0.115, 0.062),
                                   float3(0.075, 0.042, 0.024), float3(0.028, 0.017, 0.011), float3(0.0095, 0.0075, 0.0068)};
    const float x = saturate(m) * 5.0;
    const int k = min((int)x, 4);
    float3 c = exp(lerp(log(ramp[k]), log(ramp[k + 1]), x - (float)k));
    return c * lerp(float3(1.0, 1.0, 1.0), float3(1.12, 0.78, 0.5), saturate(red));
}
float3 HairBaseColor(HairData h, float strandRand, float brightness) {
    float m = h.melanin;
    m += (strandRand - 0.5) * 0.16 + (h.cardRand - 0.5) * 0.06 + (0.5 - brightness) * 0.08; // no two strands alike
    m -= 0.07 * smoothstep(0.35, 1.0, h.t);                                                    // sun-worn tips
    if (frac(strandRand * 7.31 + h.cardRand * 3.17) < h.grey) return float3(0.62, 0.6, 0.56);  // unpigmented strands
    return HairMelaninColor(m, h.red);
}
// The inverse for coloured strand sheets (brows, lashes): pick the absorption colour that shows this colour once scattered.
float3 HairBaseFromColor(float3 srgb) {
    return saturate(pow(saturate(srgb), 2.2) * 1.1);
}

float HairGauss(float b, float x) { return exp(-0.5 * x * x / (b * b)) / (2.5066283 * b); }
float HairFresnel(float c) { const float f0 = 0.0465; return f0 + (1.0 - f0) * pow(1.0 - c, 5.0); } // eta 1.55

// Marschner et al. 2003 in Karis's real-time form ("Physically Based Hair Shading in Unreal", 2016): the R, TT and TRT lobes
// around the strand's tangent, and his multiple-scattering diffuse with the colour that deep light takes on. Per unit irradiance,
// times pi to match this engine's sun (LitPbr's diffuse carries no 1/pi). `shadow` is how much direct sun reaches the strand.
void HairBsdf(float3 T, float3 V, float3 L, float3 N, float3 base, float rough, float shift, float shadow, float backlit,
              out float3 spec, out float3 diffuse) {
    const float sinL = clamp(dot(T, L), -1.0, 1.0), sinV = clamp(dot(T, V), -1.0, 1.0);
    const float cosThetaD = cos(0.5 * abs(asin(sinV) - asin(sinL)));
    const float3 Lp = L - sinL * T, Vp = V - sinV * T;
    const float cosPhi = dot(Lp, Vp) * rsqrt(dot(Lp, Lp) * dot(Vp, Vp) + 1e-4);
    const float cosHalfPhi = sqrt(saturate(0.5 + 0.5 * cosPhi));
    const float n = 1.55, np = 1.19 / cosThetaD + 0.36 * cosThetaD, a = 1.0 / np;
    const float B0 = rough * rough, B1 = 0.5 * B0, B2 = 2.0 * B0;
    const float x = sinL + sinV;
    spec = 0.0;
    // R: the cuticle's own reflection, white, shifted toward the root by the scales' tilt
    {
        const float Mp = HairGauss(B0, x + 2.0 * shift), Np = 0.25 * cosHalfPhi;
        const float Fp = HairFresnel(sqrt(saturate(0.5 + 0.5 * dot(L, V))));
        spec += Mp * Np * Fp * lerp(1.0, backlit, saturate(-cosPhi));
    }
    // TT: through the strand, the glow of back-lit hair
    {
        const float Mp = HairGauss(B1, x - shift);
        const float h = cosHalfPhi * (1.0 + a * (0.6 - 0.8 * cosPhi));
        const float f = HairFresnel(cosThetaD * sqrt(saturate(1.0 - h * h)));
        const float Fp = (1.0 - f) * (1.0 - f);
        const float3 Tp = pow(base, 0.5 * sqrt(saturate(1.0 - h * h * a * a)) / cosThetaD);
        const float Np = exp(-3.65 * cosPhi - 3.98);
        spec += Mp * Np * Fp * Tp * backlit;
    }
    // TRT: in, off the far wall, out: the coloured second highlight
    {
        const float Mp = HairGauss(B2, x - 4.0 * shift);
        const float f = HairFresnel(cosThetaD * 0.5);
        const float Fp = (1.0 - f) * (1.0 - f) * f;
        const float3 Tp = pow(base, 0.8 / cosThetaD);
        const float Np = exp(17.0 * cosPhi - 16.78);
        spec += Mp * Np * Fp * Tp;
    }
    // Multiple scattering: light that wandered through many strands comes out soft, from all round, and in the hair's colour
    const float kajiya = 1.0 - abs(sinL);
    const float3 fakeN = normalize(V - T * sinV);
    const float wrapNl = saturate((dot(fakeN, L) + 1.0) * 0.25);
    const float shellNl = saturate((dot(N, L) + 0.35) / 1.35);
    const float luma = max(dot(base, float3(0.2126, 0.7152, 0.0722)), 1e-4);
    const float3 scatterTint = pow(base / luma, 1.0 - shadow);
    // Karis takes sqrt(base) here; against this engine's sun that lit brown hair tan, brighter than the skin beside it. Real
    // multiple scattering brightens brown hair by well under 2x (more for blond, where little is absorbed between bounces).
    diffuse = base * lerp(1.45, 1.15, luma * 2.0) * (lerp(wrapNl, kajiya, 0.33) * shellNl / 3.14159265) * scatterTint;
    spec *= 3.14159265;
    diffuse *= 3.14159265;
}

// The hair cap's strands, procedural: uv is (across, along) in metres on the cap. Clumps ~0.6 mm across and strands ~0.12 mm,
// long along the flow; as they shrink under a pixel they settle to their mean, so the cap never shimmers. Returns the same
// channels as the strand atlas (R brightness, G unused, B strand random, A coverage 1).
float4 HairCapStrands(float2 uv) {
    const float2 clumpUv = float2(uv.x * 1650.0, uv.y * 90.0), fineUv = float2(uv.x * 8300.0, uv.y * 260.0);
    const float clumpFade = saturate(1.6 - max(fwidth(clumpUv.x), fwidth(clumpUv.y)) * 1.2);
    const float fineFade = saturate(1.6 - max(fwidth(fineUv.x), fwidth(fineUv.y)) * 1.2);
    const float clump = lerp(0.5, TileNoise(clumpUv) * 0.7 + TileNoise(clumpUv * float2(2.1, 1.7) + 3.1) * 0.3, clumpFade);
    const float fine = lerp(0.5, TileNoise(fineUv + clump * 4.0), fineFade);
    return float4(lerp(0.5, fine, 0.6) * 0.6 + clump * 0.4, 0.5, frac(fine * 3.7 + clump * 1.3), 1.0);
}

// Hairline fur through the shell stack (AddHairShell): uv is (across, along) in metres over the hairline band, t the shell's height.
// Roots sit in cells 0.16 mm across by 0.6 mm along the flow, one jittered root each, kept with the band's density; every hair has
// its own length and leans over along the flow (it lies ~25 degrees off the skin), so through the shells it is a slanted, tapering
// line. Returns the atlas channels (R brightness, G unused, B strand random, A coverage).
float4 HairShellFur(float2 fc, float t, float density) {
    const float thick = 0.0035, lean = 2.4; // metres of shell stack; flow travel per metre of height
    const float2 cell = float2(0.00013, 0.0006);
    const float2 p = float2(fc.x, fc.y - t * thick * lean) / cell;
    const float2 base = floor(p);
    // this cell's root and the one below it (a leaning hair reaches into the next cell); no branches, so it compiles small and fast
    const float2 c0 = base, c1 = base - float2(0.0, 1.0);
    const float4 ha = float4(TileHash(c0), TileHash(c0 + 17.31), TileHash(c0 + 41.7), TileHash(c0 + 7.9));
    const float4 hb = float4(TileHash(c1), TileHash(c1 + 17.31), TileHash(c1 + 41.7), TileHash(c1 + 7.9));
    const float2 len = lerp(0.35, 1.0, float2(ha.z * ha.z, hb.z * hb.z)); // each hair's length, as a share of the stack
    const float2 keep = step(float2(ha.x, hb.x), density) * step(t, len);
    const float2 r = 0.42 * (1.0 - 0.7 * t / len);                      // tapering toward the tip
    const float2 da = p - (c0 + float2(0.2 + 0.6 * ha.y, 0.5 * ha.w)), db = p - (c1 + float2(0.2 + 0.6 * hb.y, 0.5 * hb.w));
    const float a0 = saturate(1.0 - length(float2(da.x / r.x, da.y / 1.1))) * keep.x;
    const float a1 = saturate(1.0 - length(float2(db.x / r.y, db.y / 1.1))) * keep.y;
    return float4(0.5, 0.5, a0 >= a1 ? ha.z : hb.z, smoothstep(0.0, 0.35, max(a0, a1)));
}

float4 HairCardPS(VSOut i) {
    const HairData h = HairDecode(i.color);
    const bool cap = h.cls == 3, shell = h.cls == 4;
    float4 tex = cap ? HairCapStrands(i.uv) : shell ? HairShellFur(i.uv, h.t, h.depth) : gAlbedo.Sample(gTexSampler, i.uv);
    if (shell) tex.a *= saturate((1.5 - length(gLight.eye.xyz - i.worldPos)) / 0.45); // close range only: gone by 1.5 m
    const float3 dpx = ddx(i.worldPos), dpy = ddy(i.worldPos); // before any clip: gradients stay defined
    const float dtx = ddx(h.t), dty = ddy(h.t), dvx = ddx(i.uv.y), dvy = ddy(i.uv.y);
    // Coverage. Magnified (a strand several pixels wide): sharpened by its screen-space slope to a one-pixel ramp, so strands are
    // crisp and only their edge pixel is hashed (after Golus). Minified: the slope is wide, the sharpening fades out, and the mip's
    // averaged alpha is hashed as it is, which keeps the far hair's true coverage. The root of a scalp card is dense; a little
    // extra there keeps the parting from reading bald.
    float coverage = tex.a;
    if (h.cls == 0) coverage = saturate(coverage * lerp(1.25, 1.0, smoothstep(0.0, 0.3, h.t)));
    const float slope = max(fwidth(coverage), 1e-4);
    coverage = lerp(saturate((coverage - 0.42) / slope + 0.5), coverage, saturate(slope * 1.5 - 0.25));
    if (cap) coverage = h.t; // the cap: whole inside, fading out at the hairline
    if (shell) coverage = tex.a;
    if (h.cls == 5) coverage = 1.0 - smoothstep(0.5, 1.0, h.t); // wet tissue: solid, thinning out toward the card's far edge
    clip(coverage - lerp(0.02, 0.98, HairHash(i.pos.xy, h.cardRand)));

    if (h.cls == 5) { // wet tissue (caruncle, tear line): a moist pink, glossy as the inside of the mouth, blood-red where light spreads
        const float3 wetN = normalize(i.normal) * (dot(normalize(i.normal), gLight.eye.xyz - i.worldPos) < 0.0 ? -1.0 : 1.0);
        const float3 wet = LitMouth(float3(0.52, 0.17, 0.16), wetN, i.worldPos, gPixelShadow, CloudSun(i.worldPos),
                                    0.8, 0.12, 0.6, float3(1.0, 0.45, 0.4));
        return float4(wet, 1.0);
    }
    float3 N = normalize(i.normal);
    // The strand runs root to tip: the direction t grows in, across the pixel. Fall back to the atlas's v where t is flat.
    float3 tg = dpx * dty - dpy * dtx;
    if (dot(tg, tg) < 1e-24) tg = dpx * dvy - dpy * dvx;
    if (cap || shell) tg = cross(dpy, N) * dvx + cross(N, dpx) * dvy; // the surface gradient of `along`: the combing direction
    float3 T = dot(tg, tg) > 1e-24 ? normalize(tg - N * dot(tg, N) * 0.5) : normalize(cross(N, float3(0.0, 1.0, 0.0)) + 1e-5);

    float4 mat = float4(0.9, 0.55, 0.5, 1.0);
    float3 base;
    if (cap || shell) {
        mat = float4(0.8, 0.62, 0.5, 1.0);
        base = HairBaseColor(h, tex.b, tex.r);
    } else if (h.melaninMode) {
        mat = gNormalMap.Sample(gTexSampler, i.uv);
        base = HairBaseColor(h, tex.b, tex.r);
    } else {
        base = HairBaseFromColor(tex.rgb) * lerp(0.9, 1.1, h.cardRand);
    }
    // Per strand: each one wanders a few degrees off the card's direction, so highlights break up along the strands
    const float strand = h.melaninMode ? tex.b : frac(h.cardRand * 13.7 + i.uv.x * 41.0);
    const float wander = strand - 0.5;
    const float3 B = normalize(cross(N, T) + 1e-5);
    T = normalize(T + B * wander * 0.15 + N * (frac(strand * 9.1) - 0.5) * 0.15); // small: big tilts lit whole cards in blotches

    const float3 V = normalize(gLight.eye.xyz - i.worldPos), L = gLight.sunDir.xyz;
    // wide enough that neighbouring cards a few degrees apart share one sheen band; narrow lobes lit card by card in blotches
    const float rough = lerp(0.32, 0.5, mat.g) * (h.cls == 1 ? 1.0 : 1.0);
    const float shift = lerp(0.025, 0.07, mat.b) * (h.cls == 1 ? 0.5 : 1.0);
    const float cloud = CloudSun(i.worldPos);
    const float shadow = gPixelShadow * cloud;
    // Inside the volume: the strands around shut out sky and bounce, more the deeper (Karis's "hair occlusion" by depth).
    const float rootDark = lerp(h.cls == 1 ? 0.75 : 0.45, 1.0, smoothstep(0.0, 0.45, h.t));
    const float vdepth = shell ? 0.25 * (1.0 - h.t) : h.depth; // shells carry density where the others carry depth
    // the cap is the hair's inside: what shows of it between cards is in the shade of the strands around it
    const float ao = saturate(mat.r * 1.05) * rootDark * lerp(1.0, 0.35, vdepth) * (cap ? 0.6 : 1.0);
    // The sun's way out through the hair around it: shadowed strands deep inside still see a little, coloured light
    const float volumeShadow = shadow * lerp(1.0, 0.55, vdepth * saturate(1.0 - dot(N, L)));
    // Back light: how much reaches the strand from behind; the head itself blocks it unless the strand is at the rim
    const float backlit = saturate(volumeShadow * 1.4);

    float3 spec, diffuse;
    HairBsdf(T, V, L, N, base, rough, shift, volumeShadow, backlit, spec, diffuse);
    spec *= 0.38 * lerp(0.45, 1.25, frac(strand * 3.7 + h.cardRand)); // strands differ in how clean their cuticle is
    const float3 sun = gLight.sunColor.rgb * 3.2;
    float3 direct = sun * (spec * volumeShadow + diffuse * lerp(volumeShadow, 1.0, 0.12) * lerp(1.0, 0.6, vdepth)) * (cap ? 0.55 : 1.0);

    float skyVis;
    gSunCover = 0.0;
    const float3 irradiance = SurfaceIrradiance(i.worldPos, N, cloud, skyVis);
    // Ambient: the multiple-scattered hair colour under the sky and probes, and a dim sheen of the sky along the strands
    const float3 ambientDiffuse = irradiance * base * 0.92 * ao;
    const float3 R = normalize(reflect(-V, N) * 0.5 + N * 0.5);
    const float3 sheenDir = normalize(R - T * dot(R, T) * 0.5);
    const float3 ambientSpec = lerp(irradiance, SkyReflection(sheenDir, 0.5), skyVis) * 0.06 * ao * lerp(1.0, base * 2.0, 0.4);
    const float3 ambientPart = ambientDiffuse + ambientSpec;
    gMatAO = ao;
    if (gLight.lightDebug.x > 0.5) {
        if (gLight.lightDebug.x < 1.5) return float4(irradiance, 0.9);
        if (gLight.lightDebug.x < 2.5) return float4(direct, 0.9);
    }
    const float la = dot(ambientPart, float3(0.2126, 0.7152, 0.0722)), ld = dot(direct, float3(0.2126, 0.7152, 0.0722));
    const float share = la / max(la + ld, 1e-4);
    return float4(ambientPart + direct, 0.76 + 0.23 * share);
}

// Hair cards into the shadow maps (DrawShadowCasterCutout): depth only where the strands are. Hashed against a fixed pattern in
// the map, so the shadow holds still from frame to frame; the map's filter turns the hash into partial shadow.
struct VSOutShadowCut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float cls : TEXCOORD1;
};
VSOutShadowCut ShadowCutoutVS(VSIn v) {
    VSOutShadowCut o;
    o.pos = mul(pc.mvp, float4(v.pos, 1.0));
    o.uv = v.uv;
    o.cls = floor(v.color.g + 1e-3);
    return o;
}
void PSShadowCutout(VSOutShadowCut i) {
    if (i.cls > 2.5) discard; // the hair cap lies on the scalp under the cards: they cast, it need not
    const float a = gAlbedo.Sample(gTexSampler, i.uv).a;
    clip(a - lerp(0.08, 0.92, frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))))));
}

// Silhouette parallax occlusion volumes ("SPOM"): a balcony or canopy drawn as a few quads, its real shape carried by a relief
// texture. Vertex alpha 0.8 marks it (a glTF material whose name contains "spom"; mesh_import.h). The mesh is a plain box standing out
// from a wall; each pixel of the box marches a ray through it against the relief and discards where the ray leaves empty-handed, so the
// outline is the relief's outline, not the box's. After Policarpo/Oliveira "Relief Mapping of Non-Height-Field Surface Details" (2006)
// and Dummer's cone-free linear+binary search; the volume form is the "parallax box" of Tatarchuk-style silhouette work.
//   volume frame  T = cross(up, N), B = cross(N, T) (= up for an upright N), N = the vertex normal (the FACADE's outward normal on all six faces, not the face's)
//   uv            this vertex's (x, y) in metres from the volume's lower-left corner on the wall, along T and B
//   color.rgb     width, height (metres) and the vertex's depth out from the wall along N (0 .. kSpomDepth)
//   relief        a layer of solid per texel, [start, end] metres out from the wall: albedo.a = end / kSpomDepth, normal.a = start / kSpomDepth.
//                 Empty (end <= start) lets the ray pass. Two layers (not a height field) so a baluster can stand free of the wall.
//                 albedo.rgb and the normal map's rgb are the relief's own colour and fine detail, sampled at the same uv.
// Not written: depth (a pixel keeps its box face's depth: writing SV_Depth would switch off early-z for every material in this shader).
static const float kSpomDepth = 1.2;
float2 SpomRange(float2 m, float2 size) { // x: where the solid starts, y: where it ends, metres from the wall
    // Snapped to the texel's centre, so bilinear filtering returns that texel's own start and end: blended between a rail and
    // the empty texel beside it, the pair would read as a phantom sliver of solid half way out.
    float2 dim; gAlbedo.GetDimensions(dim.x, dim.y); // any size: a 9 m wall strip needs more texels across than a balcony
    const float2 uv = (floor(float2(m.x / size.x, 1.0 - m.y / size.y) * dim) + 0.5) / dim;
    return float2(gNormalMap.SampleLevel(gTexSampler, uv, 0.0).a, gAlbedo.SampleLevel(gTexSampler, uv, 0.0).a) * kSpomDepth;
}
bool SpomSolid(float3 q, float2 size) { const float2 r = SpomRange(q.xy, size); return r.y > r.x + 0.004 && q.z >= r.x && q.z <= r.y; }
float4 SpomPS(VSOut i) {
    // A volume may lean (a tapering lattice face): T stays horizontal, B runs up the face. For an upright N, B = world up.
    // color.b > 5 marks a single-quad volume ("SPOM quad"): one 2-triangle face lying on the relief's centre plane instead of a
    // box. Its depth is color.b - 10, and the ray is first stepped back to where it entered the volume.
    const float3 N = normalize(i.normal);
    const float3 T = normalize(cross(float3(0.0, 1.0, 0.0), N));
    const float3 B = normalize(cross(N, T));
    const float2 size = max(i.color.rg, 0.05);
    const bool quad = i.color.b > 5.0;
    float3 p0 = float3(i.uv, quad ? i.color.b - 10.0 : i.color.b);
    const float3 vw = normalize(i.worldPos - gLight.eye.xyz);
    const float3 d = float3(dot(vw, T), dot(vw, B), dot(vw, N));
    float tBack = 0.0;
    if (quad) {
        float3 tb = float3(1e9, 1e9, 1e9);
        if (abs(d.x) > 1e-5) tb.x = d.x > 0.0 ? p0.x / d.x : (p0.x - size.x) / d.x;
        if (abs(d.y) > 1e-5) tb.y = d.y > 0.0 ? p0.y / d.y : (p0.y - size.y) / d.y;
        if (abs(d.z) > 1e-5) tb.z = d.z > 0.0 ? p0.z / d.z : (p0.z - kSpomDepth) / d.z;
        tBack = max(min(tb.x, min(tb.y, tb.z)), 0.0);
        p0 -= d * tBack;
    }
    // Where the ray leaves the box [0,W] x [0,H] x [0,kSpomDepth] (slab method), starting from this face's point.
    float3 tFar = float3(1e9, 1e9, 1e9);
    if (abs(d.x) > 1e-5) tFar.x = d.x > 0.0 ? (size.x - p0.x) / d.x : -p0.x / d.x;
    if (abs(d.y) > 1e-5) tFar.y = d.y > 0.0 ? (size.y - p0.y) / d.y : -p0.y / d.y;
    if (abs(d.z) > 1e-5) tFar.z = d.z > 0.0 ? (kSpomDepth - p0.z) / d.z : -p0.z / d.z;
    const float tExit = max(min(tFar.x, min(tFar.y, tFar.z)), 0.0);
    if (tExit < 1e-4) discard;
    // Linear search for the first solid sample, then bisection: the thin slab and the balusters need the 2nd part to be sharp.
    const int steps = clamp((int)ceil(tExit / 0.02), 64, 256); // thin parts must not fall between two samples; long volumes (a whole
                                                                   // pylon face) need more than a 4 m bay
    const float dt = tExit / steps;
    float tLo = 0.0, tHi = 0.0;
    bool hit = false;
    [loop] for (int k = 0; k <= steps; ++k) {
        tHi = dt * k;
        if (SpomSolid(p0 + d * tHi, size)) { hit = true; break; }
        tLo = tHi;
    }
    if (!hit) discard;
    [unroll] for (int r = 0; r < 6; ++r) {
        const float tm = 0.5 * (tLo + tHi);
        if (SpomSolid(p0 + d * tm, size)) tHi = tm; else tLo = tm;
    }
    const float3 q = p0 + d * tHi;
    const float2 uv = float2(q.x / size.x, 1.0 - q.y / size.y);
    const float4 albedo = gAlbedo.SampleLevel(gTexSampler, uv, 0.0);
    // Normal: the relief's own edge, from the gradient of whichever face of the layer was hit (its front, or its back when the
    // ray came at it from behind), so the slab's rim and a baluster's flank point sideways; plus the normal map's fine detail.
    float2 dim; gAlbedo.GetDimensions(dim.x, dim.y);
    const float2 e = 1.5 / dim * size;
    const float2 rC = SpomRange(q.xy, size);
    const bool back = abs(q.z - rC.x) < abs(q.z - rC.y) && rC.y > rC.x + 0.004 && rC.x > 0.02;
    const float2 rx = SpomRange(q.xy + float2(e.x, 0), size) - SpomRange(q.xy - float2(e.x, 0), size);
    const float2 ry = SpomRange(q.xy + float2(0, e.y), size) - SpomRange(q.xy - float2(0, e.y), size);
    const float gx = (back ? rx.x : rx.y) / (2.0 * e.x), gy = (back ? ry.x : ry.y) / (2.0 * e.y);
    float3 nl = normalize(float3(-gx, -gy, 1.0) * (back ? -1.0 : 1.0));
    const float3 nm = gNormalMap.SampleLevel(gTexSampler, uv, 0.0).xyz * 2.0 - 1.0;
    nl = normalize(nl + float3(nm.xy * 0.6, 0.0));
    const float3 n = normalize(T * nl.x + B * nl.y + N * nl.z);
    const float3 wp = i.worldPos + vw * (tHi - tBack);
    // Its own shadow: toward the sun through the volume, four taps. The slab shades the rail below it, the rail the floor behind it.
    float shade = 1.0;
    const float3 l = gLight.sunDir.xyz;
    const float3 lt = float3(dot(l, T), dot(l, B), dot(l, N));
    if (lt.z > 0.02) {
        const float reach = min((kSpomDepth - q.z) / lt.z, 1.0);
        [unroll] for (int s = 1; s <= 4; ++s)
            if (SpomSolid(q + lt * (reach * s * 0.25 + 0.02), size)) { shade = 0.45; break; }
    }
    const float cloudSun = CloudSun(wp);
    float share;
    const float3 lit = LitPbr(pow(saturate(albedo.rgb), 2.2), n, wp, gPixelShadow * cloudSun * shade, 0.7, 0.0, cloudSun, share);
    return float4(lit, 0.76 + 0.23 * share);
}

// Railway track ("SPOM track", alpha 0.83): every track is a thin box (top + two sides, no culling) swept along the line; each pixel
// marches the eye ray through the box against the baked track height field (assets/rail/track/tex, height in albedo.a) and discards
// on a miss, so rails, sleepers and the ballast shoulder make the silhouette. Same scheme as assets/rail/preview.html.
//   normal     the track's forward direction (horizontal unit); across = cross(up, forward)
//   uv         metres across the track from its centre line, metres along it
//   color.r    this vertex's height relative to the rail top (0 .. -kTrackDepth), color.g the box width
// Back faces (no culling) step back to where the ray entered, so either face finds the same hit; the nearer face wins the depth test.
static const float kTrackDepth = 0.55, kTrackSleeper = 0.6;
float TrackH(float3 p, float W) { return -kTrackDepth + kTrackDepth * gAlbedo.SampleLevel(gTexSampler, float2((p.x + 0.5 * W) / W, p.z / kTrackSleeper), 0.0).a; }
float4 TrackPS(VSOut i) {
    const float3 F = normalize(float3(i.normal.x, 0.0, i.normal.z));
    const float3 X = float3(F.z, 0.0, -F.x);
    const float W = max(i.color.g, 1.0), hw = 0.5 * W;
    const float3 vw = normalize(i.worldPos - gLight.eye.xyz);
    const float3 d = float3(dot(vw, X), vw.y, dot(vw, F));
    float3 p0 = float3(i.uv.x, i.color.r, i.uv.y);
    const float camDist = length(i.worldPos - gLight.eye.xyz);
    const float2 uvIn = float2((i.uv.x + hw) / W, i.uv.y / kTrackSleeper);
    float t = 0.0, tBack = 0.0;
    bool hit = true;
    if (camDist < 160.0) { // far away the relief is under a pixel: shade the box top flat
        float2 tb = float2(1e9, 1e9);
        if (abs(d.x) > 1e-5) tb.x = d.x > 0.0 ? (p0.x + hw) / d.x : (p0.x - hw) / d.x;
        if (abs(d.y) > 1e-5) tb.y = d.y > 0.0 ? (p0.y + kTrackDepth) / d.y : p0.y / d.y;
        tBack = max(min(tb.x, tb.y), 0.0);
        p0 -= d * tBack;
        float2 tf = float2(1e9, 1e9);
        if (abs(d.x) > 1e-5) tf.x = d.x > 0.0 ? (hw - p0.x) / d.x : (-hw - p0.x) / d.x;
        if (abs(d.y) > 1e-5) tf.y = d.y > 0.0 ? -p0.y / d.y : (-kTrackDepth - p0.y) / d.y;
        const float tExit = max(min(tf.x, tf.y), 0.0);
        const int steps = 40;
        const float dt = tExit / steps;
        float tLo = 0.0, tHi = 0.0;
        hit = false;
        [loop] for (int k = 0; k <= steps; ++k) {
            tHi = dt * k;
            const float3 q = p0 + d * tHi;
            if (q.y <= TrackH(q, W)) { hit = true; break; }
            tLo = tHi;
        }
        if (!hit) discard;
        [unroll] for (int r = 0; r < 5; ++r) {
            const float tm = 0.5 * (tLo + tHi);
            const float3 q = p0 + d * tm;
            if (q.y <= TrackH(q, W)) tHi = tm; else tLo = tm;
        }
        t = tHi;
    }
    const float3 q = p0 + d * t;
    const float2 uv = float2((q.x + hw) / W, q.z / kTrackSleeper);
    const float3 albedo = camDist < 160.0 ? gAlbedo.SampleLevel(gTexSampler, uv, 0.0).rgb : gAlbedo.Sample(gTexSampler, uvIn).rgb;
    float3 nl = float3(0, 1, 0);
    if (camDist < 160.0) {
        const float e = 0.01, h0 = TrackH(q, W);
        nl = normalize(float3(-(TrackH(q + float3(e, 0, 0), W) - TrackH(q - float3(e, 0, 0), W)) / (2.0 * e), 1.0,
                              -(TrackH(q + float3(0, 0, e), W) - TrackH(q - float3(0, 0, e), W)) / (2.0 * e)));
        if (q.y < h0 - 0.003) nl = abs(d.x) > abs(d.z) ? float3(-sign(d.x), 0, 0) : float3(0, 0, -sign(d.z)); // entered through a side
    }
    const float3 n = normalize(X * nl.x + float3(0, nl.y, 0) + F * nl.z);
    const float3 wp = i.worldPos + vw * (t - tBack);
    float shade = 1.0;
    if (camDist < 160.0) {
        const float3 l = gLight.sunDir.xyz, lt = float3(dot(l, X), l.y, dot(l, F));
        if (lt.y > 0.02) [unroll] for (int s = 1; s <= 6; ++s) { const float3 sp = q + lt * (s * 0.06 + 0.004); if (sp.y <= TrackH(sp, W)) { shade = 0.4; break; } }
    }
    const float steel = smoothstep(0.55, 0.85, albedo.g);
    const float cloudSun = CloudSun(wp);
    float share;
    const float3 lit = LitPbr(pow(saturate(albedo), 2.2), n, wp, gPixelShadow * cloudSun * shade, lerp(0.85, 0.35, steel), steel * 0.8, cloudSun, share);
    return float4(lit, 0.76 + 0.23 * share);
}

// SPOM slabs (alpha 0.806 .. 0.824): a whole building face as one thin box kSlabDepth deep standing out of the face; each pixel
// marches the eye ray against the relief in gAlbedo.a (height out of the face, 0..1 of the depth) so pilasters, recessed windows,
// brick, door portals, skylights and parapets are real silhouettes from a handful of triangles (tools/building_gen/depot_tex.py).
// Alpha below 0.815: a wall (out = (-T.z, 0, T.x), up the face = world up); above: a roof (out = up, across = (T.z, 0, -T.x)).
//   normal    T, the horizontal direction along the face (the texture repeats along it)
//   uv        metres along T and across the face (up a wall, across a roof); color.r this vertex's height out of the face
//   color.g   the texture's repeat along T (metres), color.b the face's extent across (metres, the texture spans it once)
// Back faces (no culling) step back to where the ray entered, so either face of the slab finds the same hit.
static const float kSlabDepth = 0.8;
float SlabH(float2 m, float2 size, bool roof) {
    const float2 uv = float2(m.x / size.x, roof ? m.y / size.y : 1.0 - m.y / size.y);
    return kSlabDepth * gAlbedo.SampleLevel(gTexSampler, uv, 0.0).a;
}
float4 SlabPS(VSOut i) {
    const bool roof = i.color.a > 0.815;
    const float3 T = normalize(float3(i.normal.x, 0.0, i.normal.z));
    const float3 N = roof ? float3(0, 1, 0) : float3(-T.z, 0.0, T.x);
    const float3 B = roof ? float3(T.z, 0.0, -T.x) : float3(0, 1, 0);
    const float2 size = max(i.color.gb, 0.1);
    const float3 vw = normalize(i.worldPos - gLight.eye.xyz);
    const float3 d = float3(dot(vw, T), dot(vw, B), dot(vw, N));
    float3 p0 = float3(i.uv, i.color.r);
    const float camDist = length(i.worldPos - gLight.eye.xyz);
    const bool near = camDist < 260.0;
    float t = 0.0, tBack = 0.0;
    if (near) {
        float2 tb = float2(1e9, 1e9); // back to where the ray entered the slab (across: [0, extent], out: [0, depth])
        if (abs(d.y) > 1e-5) tb.x = d.y > 0.0 ? p0.y / d.y : (p0.y - size.y) / d.y;
        if (abs(d.z) > 1e-5) tb.y = d.z > 0.0 ? p0.z / d.z : (p0.z - kSlabDepth) / d.z;
        tBack = max(min(tb.x, tb.y), 0.0);
        p0 -= d * tBack;
        float2 tf = float2(1e9, 1e9);
        if (abs(d.y) > 1e-5) tf.x = d.y > 0.0 ? (size.y - p0.y) / d.y : -p0.y / d.y;
        if (abs(d.z) > 1e-5) tf.y = d.z > 0.0 ? (kSlabDepth - p0.z) / d.z : -p0.z / d.z;
        const float tExit = max(min(tf.x, tf.y), 0.0);
        const int steps = clamp((int)ceil(tExit / 0.03), 16, 64);
        const float dt = tExit / steps;
        float tLo = 0.0, tHi = 0.0;
        bool hit = false;
        [loop] for (int k = 0; k <= steps; ++k) {
            tHi = dt * k;
            const float3 q = p0 + d * tHi;
            if (q.z <= SlabH(q.xy, size, roof)) { hit = true; break; }
            tLo = tHi;
        }
        // the face's own plane (height 0) is always solid: a ray leaving through the back has hit it, however the samples fell
        const bool outBack = d.z < 0.0 && tf.y <= tf.x + 1e-4;
        if (!hit && !outBack) discard;
        if (!hit) tLo = tHi = tExit;
        [unroll] for (int r = 0; r < 5; ++r) {
            const float tm = 0.5 * (tLo + tHi);
            const float3 q = p0 + d * tm;
            if (q.z <= SlabH(q.xy, size, roof)) tHi = tm; else tLo = tm;
        }
        t = tHi;
    }
    const float3 q = p0 + d * t;
    const float2 uv = float2(q.x / size.x, roof ? q.y / size.y : 1.0 - q.y / size.y);
    const float2 uvIn = float2(i.uv.x / size.x, roof ? i.uv.y / size.y : 1.0 - i.uv.y / size.y);
    const float3 albedo = near ? gAlbedo.SampleLevel(gTexSampler, uv, 0.0).rgb : gAlbedo.Sample(gTexSampler, uvIn).rgb;
    float3 nl = float3(0, 0, 1);
    if (near) {
        const float e = 0.02, h0 = SlabH(q.xy, size, roof);
        nl = normalize(float3(-(SlabH(q.xy + float2(e, 0), size, roof) - SlabH(q.xy - float2(e, 0), size, roof)) / (2.0 * e),
                              -(SlabH(q.xy + float2(0, e), size, roof) - SlabH(q.xy - float2(0, e), size, roof)) / (2.0 * e), 1.0));
        if (q.z < h0 - 0.004) nl = abs(d.x) > abs(d.y) ? float3(-sign(d.x), 0, 0) : float3(0, -sign(d.y), 0); // a step's side
    }
    const float3 n = normalize(T * nl.x + B * nl.y + N * nl.z);
    const float3 wp = i.worldPos + vw * (t - tBack);
    float shade = 1.0;
    if (near) { // its own shadow: toward the sun through the relief
        const float3 l = gLight.sunDir.xyz, lt = float3(dot(l, T), dot(l, B), dot(l, N));
        if (lt.z > 0.02) {
            const float reach = min((kSlabDepth - q.z) / lt.z, 3.0);
            [unroll] for (int s = 1; s <= 6; ++s) { const float3 sp = q + lt * (reach * s / 6.0 + 0.005); if (sp.z <= SlabH(sp.xy, size, roof)) { shade = 0.4; break; } }
        }
    }
    const float glass = saturate((0.25 - dot(albedo, float3(0.3, 0.5, 0.2))) * 6.0) * step(q.z, 0.06); // the dark panes sit at the back
    const float cloudSun = CloudSun(wp);
    float share;
    const float3 lit = LitPbr(pow(saturate(albedo), 2.2), n, wp, gPixelShadow * cloudSun * shade, lerp(0.8, 0.12, glass), glass * 0.6, cloudSun, share);
    return float4(lit, 0.76 + 0.23 * share);
}

// Building windows (alpha 0.665, material name contains "roomglass"; tools/building_gen/roomglass.py): a room behind every pane by
// interior mapping (van Dongen, "Interior Mapping", 2008), under smooth scratched glass. Unlike GlassPS the room is drawn, so the pane
// is opaque and needs nothing behind it.
//   uv          metres across the window plane (along T = cross(up, N)) and up it, from the plane's origin
//   color.rgb   one room's width and height in metres, and a seed; rooms repeat along the plane (a facade's bays and storeys, or a
//               single window's own size)
//   gAlbedo     a 4x4 atlas of back walls seen straight on (sRGB): furnishings, curtains, blinds; one per room, chosen by hash
//   gNormalMap  R: a wear mask (scratches, drips, grime), tiled in metres; brighter = dirtier
float4 RoomGlassPS(VSOut i) {
    const float3 v = normalize(gLight.eye.xyz - i.worldPos);
    const float3 N = normalize(i.normal);
    const float3 B = float3(0.0, 1.0, 0.0);
    const float3 T = normalize(cross(B, N));
    const float2 cell = max(i.color.rg, 0.3);
    const float seed = i.color.b;
    const float2 ci = floor(i.uv / cell);
    const float2 f = i.uv / cell - ci;                            // where in its room the pixel's pane is, 0..1
    const float depth = clamp(cell.x * 1.2, 2.5, 5.0);
    // The ray into the room, in room units: x across, y up, z inward (0 at the glass, 1 at the back wall).
    const float3 d = -v;
    float3 rd = float3(dot(d, T) / cell.x, dot(d, B) / cell.y, max(-dot(d, N), 1e-3) / depth);
    const float3 ro = float3(f, 0.0);
    const float tx = rd.x > 1e-5 ? (1.0 - ro.x) / rd.x : (rd.x < -1e-5 ? -ro.x / rd.x : 1e9);
    const float ty = rd.y > 1e-5 ? (1.0 - ro.y) / rd.y : (rd.y < -1e-5 ? -ro.y / rd.y : 1e9);
    const float tz = 1.0 / rd.z;
    const float t = min(tz, min(tx, ty));
    const float3 h = ro + rd * t;
    const float pick = TileHash(ci * float2(1.37, 2.71) + seed * 17.3);
    const float k = floor(pick * 16.0);
    const float2 atlas0 = float2(fmod(k, 4.0), floor(k / 4.0)) * 0.25;
    const float3 wallAvg = pow(saturate(gAlbedo.SampleLevel(gTexSampler, atlas0 + 0.125, 5.0).rgb), 2.2);
    float3 room;
    if (t == tz) {                                                // the back wall: this room's picture
        const float2 auv = atlas0 + float2(clamp(h.x, 0.01, 0.99), clamp(1.0 - h.y, 0.01, 0.99)) * 0.25;
        room = pow(saturate(gAlbedo.SampleLevel(gTexSampler, auv, 0.0).rgb), 2.2);
    } else if (t == ty) {                                         // floor or ceiling
        room = rd.y < 0.0 ? wallAvg * float3(0.62, 0.5, 0.4) * (0.85 + 0.15 * TileNoise(float2(h.x * 9.0, h.z * 3.0)))
                          : float3(0.55, 0.54, 0.52) * (0.7 + 0.3 * h.z);
    } else {                                                      // a side wall
        room = wallAvg * 0.78 * (0.75 + 0.25 * h.z);
    }
    room *= lerp(1.0, 0.55, saturate(h.z));                       // light falls off toward the back
    float skyVis;
    const float3 ambientIn = IndirectDiffuse(i.worldPos, N, skyVis);
    const float lit = step(0.9, TileHash(ci * float2(3.1, 5.7) + seed * 9.1));
    const float3 lamp = float3(1.0, 0.78, 0.5) * lit * 0.9;
    room *= ambientIn * 0.55 + lamp;
    // The glass: Fresnel sky, the sun's glint, and wear from the mask (mirrored tiling, 1.2 m per repeat, shifted per room).
    const float nv = saturate(dot(N, v));
    const float F = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
    const float2 wuv = abs(frac((i.uv / 1.2 + pick * float2(3.7, 1.3)) * 0.5) * 2.0 - 1.0);
    const float wear = gNormalMap.Sample(gTexSampler, wuv).r;
    const float grime = smoothstep(0.35, 0.8, wear) * lerp(0.4, 1.0, TileHash(ci + seed));
    const float scratch = smoothstep(0.8, 0.95, wear);
    const float3 refl = SkyReflection(reflect(d, N), 0.04 + grime * 0.25);
    const float3 rh = normalize(v + gLight.sunDir.xyz);
    const float sunLit = gPixelShadow * CloudSun(i.worldPos);
    const float glint = pow(saturate(dot(N, rh)), 1500.0) * 40.0 * sunLit * saturate(dot(N, gLight.sunDir.xyz) * 8.0);
    const float3 dust = float3(0.42, 0.37, 0.3) * ambientIn * 0.25;
    float3 col = room * (1.0 - F) * (1.0 - 0.45 * grime) + dust * grime * 0.6;
    col += refl * lerp(F, saturate(F + 0.08), grime) * lerp(1.0, skyVis, 0.3);
    col += (refl * 0.5 + gLight.sunColor.rgb * sunLit * 0.3) * scratch * 0.25;
    col += gLight.sunColor.rgb * glint;
    return float4(col, -0.99);                                    // negative: the composite adds screen-space reflection
}

// Vehicle interior behind glass (alpha 0.70, material name contains "interior"; tools/vehicle_gen/gen_transit.py). Interior
// mapping after van Dongen 2008, "Interior Mapping": no geometry behind the pane, the ray is cast into an analytic saloon instead,
// so seats, poles and the far side shift with the view like real depth.
//   uv.x  pane index + frac across the pane (kept small: fp32 derivatives of big uvs are noise)
//   uv.y  style * 16 + overlay variant (0..15) + frac down the pane. Width and height in metres come from the screen derivatives;
//         the position along the vehicle comes from world space.
//   rgb   seat fabric colour (the material's baseColorFactor)
//   gAlbedo  4x4 atlas of glass overlays (tint, frit, blinds, curtains, vents, ad wrap, sash), alpha = coverage
// Styles: 0 city bus, 1 coach, 2 school bus, 3 tram, 4 windshield / end window looking down the saloon.
struct InteriorStyle { float depth, sill, top, pitch, back, poles; };
InteriorStyle InteriorOf(int s) {
    InteriorStyle o;
    o.depth = 2.25; o.sill = 0.85; o.top = 0.35; o.pitch = 0.80; o.back = 1.00; o.poles = 1.0;
    if (s == 1) { o.sill = 0.70; o.top = 0.45; o.pitch = 0.86; o.back = 1.18; o.poles = 0.0; }
    if (s == 2) { o.depth = 2.15; o.sill = 0.70; o.top = 0.40; o.pitch = 0.90; o.back = 0.92; o.poles = 0.0; }
    if (s == 3) { o.depth = 2.15; o.sill = 0.55; o.top = 0.50; o.pitch = 1.00; o.back = 0.95; }
    if (s == 4) { o.depth = 9.0; o.sill = 0.80; o.top = 0.35; o.pitch = 0.85; o.poles = 0.0; } // floor 0.35 m up, windshield from 1.15 m
    return o;
}
// Ray (o + d t) against a circle in 2D: first t > 0, or 1e9.
float RayCircle(float2 o, float2 d, float2 c, float r) {
    const float2 m = o - c;
    const float a = dot(d, d), b = dot(m, d), cc = dot(m, m) - r * r;
    const float disc = b * b - a * cc;
    if (disc < 0.0 || a < 1e-8) return 1e9;
    const float t = (-b - sqrt(disc)) / a;
    return t > 0.0 ? t : 1e9;
}
float4 InteriorPS(VSOut i) {
    const float3 N = normalize(i.normal);
    const float3 eyeToP = normalize(i.worldPos - gLight.eye.xyz);
    const int style = (int)floor(i.uv.y / 16.0 + 1e-4);
    const float u = i.uv.x;
    const int variant = clamp((int)floor(i.uv.y) - style * 16, 0, 15);
    const float lv = frac(i.uv.y), lu = frac(u);
    // metres per uv, by the chain rule on this pixel's derivatives (frac(v) keeps neighbouring variants from tearing the solve)
    const float3 dp1 = ddx(i.worldPos), dp2 = ddy(i.worldPos);
    const float2 d1 = float2(ddx(u), ddx(lv)), d2 = float2(ddy(u), ddy(lv));
    const float det = d1.x * d2.y - d1.y * d2.x;
    float3 dPdu = (dp1 * d2.y - dp2 * d1.y) / (abs(det) > 1e-12 ? det : 1e-12);
    float3 dPdv = (dp2 * d1.x - dp1 * d2.x) / (abs(det) > 1e-12 ? det : 1e-12);
    const float W = clamp(length(dPdu), 0.3, 4.0), H = clamp(length(dPdv), 0.3, 3.0);
    const float3 T = normalize(dPdu - N * dot(dPdu, N));
    const float3 U = normalize(cross(N, T)) * (dot(cross(N, T), -dPdv) < 0.0 ? -1.0 : 1.0); // up the pane
    const InteriorStyle st = InteriorOf(style);
    const bool endView = style == 4;
    // local frame: x along the vehicle (T), y up from the window bottom, z into the vehicle
    // along the vehicle from world space (exact); derivative-based W only scales terms within one pane, where its jitter is harmless
    const float3 o = float3(endView ? lu * W : dot(i.worldPos - i.origin, T), (1.0 - lv) * H, 0.0);
    const float3 d = float3(dot(eyeToP, T), dot(eyeToP, U), max(dot(eyeToP, -N), 0.02));
    // shell
    float tBest = (st.depth - o.z) / d.z; int hit = 0; // 0 far wall, 1 floor, 2 ceiling, 3 seat back, 4 cushion, 5 pole, 6 rack, 7 side wall
    const float yFloor = -st.sill, yCeil = H + st.top;
    if (d.y < 0.0) { const float t = (yFloor - o.y) / d.y; if (t < tBest) { tBest = t; hit = 1; } }
    if (d.y > 0.0) { const float t = (yCeil - o.y) / d.y; if (t < tBest) { tBest = t; hit = 2; } }
    if (endView) { // the pane spans the vehicle's width: its edges are the side walls
        const float t = d.x > 0.0 ? (W - o.x) / d.x : -o.x / d.x;
        if (abs(d.x) > 1e-4 && t > 0.0 && t < tBest) { tBest = t; hit = 7; }
    }
    // seats: transverse rows. Side view: back planes cross x; end view: they cross z. "a" = crossing axis, "c" = across it.
    const float3 oa = endView ? float3(o.z, o.y, o.x) : o;
    const float3 da = endView ? float3(d.z, d.y, d.x) : d;
    const float span = endView ? W : st.depth;
    const float aisle0 = span * 0.42, aisle1 = span * 0.58;
    float3 seatN = 0; float seatFace = 0;
    if (abs(da.x) > 1e-4) {
        const float k0 = da.x > 0.0 ? ceil(oa.x / st.pitch) : floor(oa.x / st.pitch);
        [loop] for (int k = 0; k < 10; ++k) {
            const float plane = (k0 + (da.x > 0.0 ? k : -k)) * st.pitch;
            const float t = (plane - oa.x) / da.x;
            if (t >= tBest) break;
            if (t < 0.0) continue;
            const float3 p = oa + da * t;
            const float bay = TileHash(float2(plane * 1.7 + style, 4.1));
            if ((style == 0 || style == 3) && bay < 0.18) continue; // standing / wheelchair bays
            const bool inSeat = (p.z > 0.06 && p.z < aisle0) || (p.z > aisle1 && p.z < span - 0.06);
            const float zEdge = min(abs(p.z - (p.z < span * 0.5 ? 0.06 : aisle1)), abs(p.z - (p.z < span * 0.5 ? aisle0 : span - 0.06)));
            const float topY = yFloor + st.back - 0.10 * (1.0 - saturate(zEdge / 0.10)); // rounded shoulders
            if (inSeat && p.y > yFloor + 0.05 && p.y < topY && !(endView && oa.x + da.x * t < 2.2)) { tBest = t; hit = 3; seatFace = da.x > 0.0 ? -1.0 : 1.0; break; }
        }
    }
    if (abs(da.y) > 1e-4) { // cushions, 0.45 m up, in front of each back
        const float t = (yFloor + 0.45 - oa.y) / da.y;
        if (t > 0.0 && t < tBest) {
            const float3 p = oa + da * t;
            const float f = frac(p.x / st.pitch);
            if (((p.z > 0.06 && p.z < aisle0) || (p.z > aisle1 && p.z < span - 0.06)) && f > 0.04 && f < 0.52 && TileHash(float2(floor(p.x / st.pitch) * st.pitch * 1.7 + style, 4.1)) >= ((style == 0 || style == 3) ? 0.18 : 0.0)) { tBest = t; hit = 4; }
        }
    }
    if (endView) { // driver's cab: dashboard just behind the glass, driver's seat on the left
        if (d.y < 0.0) {
            const float t = (0.06 - o.y) / d.y; const float3 p = o + d * t;
            if (t > 0.0 && t < tBest && p.z < 0.75) { tBest = t; hit = 8; }
        }
        const float t = (1.20 - o.z) / d.z; const float3 p = o + d * t;
        if (t > 0.0 && t < tBest && p.x > W * 0.10 && p.x < W * 0.40 && p.y > yFloor + 0.45 && p.y < yFloor + 1.30) { tBest = t; hit = 3; seatFace = 1.0; }
    }
    float3 poleN = 0;
    if (st.poles > 0.5) {
        // vertical poles in the aisle every 2.4 m (2D circle test in the x/z plane), and one ceiling rail over each seat row
        const float2 o2 = float2(oa.x, oa.z), d2r = float2(da.x, da.z);
        const float xa = oa.x, xb = oa.x + da.x * min(tBest, 20.0);
        const float kLo = floor(min(xa, xb) / 2.4) - 1.0;
        [loop] for (int k = 0; k < 6; ++k) {
            const float2 c = float2((kLo + k) * 2.4 + 0.3, aisle0 + 0.02);
            const float t = RayCircle(o2, d2r, c, 0.022);
            if (t < tBest) { const float3 p = oa + da * t; if (p.y > yFloor && p.y < yCeil) { tBest = t; hit = 5; poleN = float3(normalize(float2(p.x, p.z) - c), 0); } }
        }
        const float2 oy = float2(oa.y, oa.z), dy2 = float2(da.y, da.z);
        [unroll] for (int r = 0; r < 2; ++r) {
            const float2 c = float2(yCeil - 0.18, r == 0 ? aisle0 - 0.10 : aisle1 + 0.10);
            const float t = RayCircle(oy, dy2, c, 0.018);
            if (t < tBest) { tBest = t; hit = 5; }
        }
    }
    if (style == 1 && !endView && d.y > 0.0) { // coach parcel racks over both seat rows
        const float t = (H + 0.08 - o.y) / d.y;
        const float3 p = o + d * t;
        if (t > 0.0 && t < tBest && (p.z < 0.48 || p.z > st.depth - 0.48)) { tBest = t; hit = 6; }
    }
    // shade the hit
    const float3 q = endView ? float3(oa.z, oa.y, oa.x) + d * tBest : o + d * tBest; // back to x-along / z-in for patterns
    const float3 qa = oa + da * tBest;
    const float3 fabric = saturate(i.color.rgb);
    float3 alb = float3(0.55, 0.55, 0.56); float glow = 0.0; float ao = 1.0;
    if (hit == 0 || hit == 7) { // far wall / side wall: windows with pillars, ad cards above, panelling below
        const float along = hit == 7 ? qa.x : q.x;
        const float pil = endView ? 0.5 : frac(along / max(W, 0.8) + 0.37); // end view: plain walls, one rear window
        if (q.y > 0.05 && q.y < H - 0.05 && pil > 0.06) { alb = lerp(float3(0.55, 0.62, 0.70), float3(0.80, 0.85, 0.90), saturate(q.y / H)); glow = 0.9; }
        else if (q.y > H + 0.06 && q.y < yCeil - 0.08) { alb = lerp(float3(0.85, 0.3, 0.2), float3(0.2, 0.45, 0.8), TileHash(float2(floor(along / 1.2), 9.0))) * (frac(along / 1.2) > 0.08 ? 1.0 : 0.3); }
        else alb = float3(0.52, 0.53, 0.55) * (0.9 + 0.1 * TileNoise(float2(along * 2.0, q.y * 2.0)));
    } else if (hit == 1) { alb = float3(0.30, 0.31, 0.34) * (0.85 + 0.3 * TileNoise(q.xz * 4.0)); ao = lerp(0.55, 1.0, saturate(q.z / 0.6)); }
    else if (hit == 2) {
        alb = float3(0.78, 0.78, 0.76);
        const float zz = endView ? qa.z : q.z;
        if (abs(zz - span * 0.22) < 0.09 || abs(zz - span * 0.78) < 0.09) { alb = float3(1.0, 0.97, 0.9); glow = 2.2; }
    }
    else if (hit == 3) {
        const float top = smoothstep(st.back - 0.16, st.back - 0.10, qa.y - yFloor);
        alb = seatFace < 0.0 ? lerp(fabric, float3(0.75, 0.75, 0.75), top) : float3(0.28, 0.29, 0.31); // fabric front, plastic shell behind; grab top
        alb *= 0.85 + 0.3 * TileNoise(float2(qa.z * 9.0, qa.y * 9.0)); // moquette
    }
    else if (hit == 4) alb = fabric * (0.9 + 0.2 * TileNoise(float2(qa.x * 8.0, qa.z * 8.0)));
    else if (hit == 5) { alb = float3(0.85, 0.65, 0.08); glow = 0.15; }
    else if (hit == 6) alb = float3(0.35, 0.36, 0.38);
    else if (hit == 8) { // dashboard: dark moulding with lit instrument screens
        const float3 p = o + d * tBest;
        alb = float3(0.10, 0.10, 0.11);
        if (p.z > 0.25 && p.z < 0.55 && abs(frac(p.x / max(W, 0.8) * 3.0) - 0.5) < 0.30) { alb = float3(0.20, 0.45, 0.65); glow = 1.2; }
    }
    const float cloud = CloudSun(i.worldPos);
    float skyVis;
    const float3 ambientIn = IndirectDiffuse(i.worldPos, N, skyVis);
    const float depthFade = saturate(1.0 - tBest * 0.08);
    const float3 lamps = float3(1.0, 0.96, 0.88) * 0.55;
    float3 room = pow(alb, 2.2) * (ambientIn * 0.55 + lamps) * ao * (0.55 + 0.45 * depthFade) + pow(alb, 2.2) * glow * (ambientIn * 0.5 + 0.25);
    // glass overlay atlas: tint, frit, blinds, curtains, vents, ad wrap, sash bars and the black border all live here
    const float2 auv = (float2(variant % 4, variant / 4) + float2(lu, lv)) / 4.0;
    const float4 ov = gAlbedo.Sample(gTexSampler, auv);
    float share;
    const float3 ovLit = LitPbr(pow(saturate(ov.rgb), 2.2), N, i.worldPos, gPixelShadow * cloud, 0.45, 0.0, cloud, share);
    const float3 v = -eyeToP;
    const float nv = saturate(dot(N, v));
    const float F = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
    const float3 refl = SkyReflection(reflect(eyeToP, N), 0.05);
    const float3 glass = room * 0.80 * (1.0 - F) + refl * F;
    return float4(lerp(glass, ovLit, ov.a), 0.76 + 0.23 * share);
}

// The HDR scene: linear color, physically based sun. Vertex alpha below 0.25 marks terrain.
float4 PSMain(VSOut i) : SV_Target {
    gShadowPixel = i.pos.xy;
    if (FEATURE_OFF(256)) { // Graphics > Quality: Unlit (and TBD_FEATURECOST): the texture under a fixed half-Lambert key, no shadows or probes
        float4 tex = gAlbedo.Sample(gTexSampler, i.uv);
        clip(tex.a - 0.4);
        if (abs(i.color.a - 0.68) < 0.003) { // hair: its colour channels carry data (hair_vertex.h), and its texture may be a data atlas
            const HairData h = HairDecode(i.color);
            tex.rgb = h.melaninMode ? sqrt(HairBaseColor(h, tex.b, tex.r)) * 0.5 : tex.rgb;
            i.color.rgb = 1.0;
        }
        float3 n = dot(i.normal, i.normal) > 1e-6 ? normalize(i.normal) : float3(0, 1, 0);
        float3 base = (i.color.a < 0.15 || IsReliefGround(i.color.a)) ? float3(0.40, 0.30, 0.21) : pow(saturate(tex.rgb), 2.2) * (abs(i.color.a - 0.645) < 0.003 ? 1.0 : saturate(i.color.rgb));
        float key = 0.35 + 0.65 * saturate(dot(n, normalize(float3(0.4, 0.8, 0.3))) * 0.5 + 0.5);
        return float4(base * key * 6.0, 0.9);
    }
    if (i.color.a > 1.5) { gFoliage = true; i.color.a -= 2.0; } // VSVegetation's mark
    if (dot(i.normal, i.normal) < 1e-6) return float4(pow(saturate(i.color.rgb), 2.2), i.color.a);
    // The sun's shadow, once per pixel, for whichever material below lights it: each one used to call ShadowFactor itself, and
    // fxc inlined a copy of the whole PCSS filter into every material path (most of a 4 minute D3D12 compile). The tight map's
    // spread is the material's: hair 2x (many see-through strands), skin 3x (light travelling under it), else 1x.
    gTightSoft = abs(i.color.a - 0.68) < 0.003 ? 2.0 : (abs(i.color.a - 0.5) < 0.1 ? 3.0 : 1.0);
    gPixelShadow = ShadowFactor(i.shadowPos, i.tightPos);
    if (i.color.a < 0.15) return TerrainPS(i);
    if (abs(i.color.a - 0.8) < 0.004) return SpomPS(i); // silhouette POM volume (balcony, canopy)
    if (abs(i.color.a - 0.83) < 0.004) return TrackPS(i); // SPOM railway track
    if (i.color.a > 0.806 && i.color.a < 0.824) return SlabPS(i); // SPOM building faces (walls, roofs)
    if (abs(i.color.a - 0.645) < 0.003) return GlassPS(i);
    if (abs(i.color.a - 0.70) < 0.003) return InteriorPS(i); // vehicle windows: interior mapping
    if (abs(i.color.a - 0.665) < 0.003) return RoomGlassPS(i); // building windows: a room behind scratched glass
    if (abs(i.color.a - 0.68) < 0.003) return HairCardPS(i);
    if (abs(i.color.a - 0.995) < 0.003) return ToothPS(i);
    if (abs(i.color.a - 0.985) < 0.003) return MouthWetPS(i);
    if (abs(i.color.a - 0.975) < 0.003) return EyePS(i);
    float4 color;
    float3 n;
    if (IsReliefGround(i.color.a) && GroundAtlasBound()) {
        // Slow patches and the splat's mix tint the layers; the wet rule below does not reach this path.
        float3 tex, groundN;
        float occlusion, rough, near;
        const float3 ng = normalize(i.normal);
        GroundLayers(i, ng, tex, groundN, occlusion, rough, near);
        float3 w = max(i.color.rgb, 0.0);
        const float total = w.r + w.g + w.b;
        w = lerp(float3(0.7, 0.2, 0.1), w / max(total, 1e-4), saturate(total * 20.0));
        const float3 tint = float3(1.0, 1.0, 1.0) * w.r + float3(0.84, 0.97, 0.82) * w.g + float3(0.85, 0.94, 1.07) * w.b;
        const float patchA = TileNoise(i.worldPos.xz * 0.11) * 0.6 + TileNoise(i.worldPos.xz * 0.47) * 0.4;
        float3 a = pow(saturate(tex * tint), 2.2) * lerp(0.82, 1.06, smoothstep(0.2, 0.8, patchA)) * lerp(0.85, 1.0, occlusion);
        a *= lerp(1.0, 0.42, near * near); // dirt held in the shade of walls: contact darkening beside buildings
        const float groundWet = saturate(gLight.probePrev.w * 1.6 - TileNoise(i.worldPos.xz * 0.15) * 0.6) * saturate(ng.y * 2.0 - 1.0); // as in PSMain (Lagarde 2013)
        a = pow(a, lerp(1.0, 1.35, groundWet));
        rough = lerp(rough, 0.14, groundWet);
        float groundShare;
        const float groundCloud = CloudSun(i.worldPos);
        const float3 lit = LitPbr(a, groundN, i.worldPos, gPixelShadow * groundCloud, lerp(max(rough, 0.6), rough, saturate(gLight.probePrev.w * 2.0)), 0.0, groundCloud, groundShare);
        return float4(lit, 0.76 + 0.23 * groundShare);
    }
    const bool world = IsWorldSurface(i.color.a);
    if (world) SurfaceRelief(i, kPomDepth);
    else if (IsReliefGround(i.color.a)) SurfaceRelief(i, 0.035);
    Material(i, color, n);
    if (abs(i.color.a - 0.5) < 0.1) { // scene alpha below 0.75 tells the composite pass to scatter this pixel
        float4 surface = gSurface.Sample(gTexSampler, i.uv);
        surface.g *= 1.0 - SkinWrinkle(i) * saturate((0.5 - surface.a) * 3.0) * 0.6; // an open groove is a cavity too
        float skinShare;
        const float3 skin = LitSkin(pow(saturate(color.rgb), 2.2), n, i.worldPos, gPixelShadow, CloudSun(i.worldPos), surface.rgb, skinShare);
        return float4(skin, 0.25 + 0.45 * skinShare);
    }
    if (abs(i.color.a - 0.95) < 0.03)
        return float4(LitEye(pow(saturate(color.rgb), 2.2), n, i.worldPos, gPixelShadow * CloudSun(i.worldPos)), 1.0);
    float share;
    if (abs(i.color.a - 0.61) < 0.005) {
        float clothShare;
        const float clothCloud = CloudSun(i.worldPos);
        float3 cloth = LitCloth(pow(saturate(color.rgb), 2.2), n, i.worldPos, gPixelShadow * clothCloud, 0.75, clothCloud, clothShare);
        return float4(cloth, 0.76 + 0.23 * clothShare);
    }
    float roughness, metal;
    PbrFromAlpha(i.color.a, roughness, metal);
    if (abs(i.color.a - 0.63) < 0.005) roughness = 0.5; // leather
    if (i.color.a < 0.25 || abs(i.color.a - 0.75) < 0.1) { // ground moisture: dark and glossy, gathering in the low-frequency hollows (Lagarde, "Water drop", 2013)
        const float wet = saturate(gLight.probePrev.w * 1.6 - TileNoise(i.worldPos.xz * 0.15) * 0.6) * saturate(n.y * 2.0 - 1.0);
        color.rgb = pow(saturate(color.rgb), lerp(1.0, 1.35, wet)); // albedo drops, in display space before the pow below
        roughness = lerp(roughness, 0.14, wet);
    }
    const float cloudSun = CloudSun(i.worldPos);
    float3 albedoLinear = pow(saturate(color.rgb), 2.2);
    const float3 emissiveColor = albedoLinear;
    const float4 extra = gExtra.SampleGrad(gTexSampler, i.uv + sPomOffset, ddx(i.uv), ddy(i.uv));
    metal = saturate(metal * extra.b); // the default texel is 1: the material's own factor stands
    gMatAO = extra.r;
    if (world) {
        // First-order ambient barely answers a small tilt, so a wall out of the sun lost its relief: boards facing up see more sky, and crevices less of anything.
        roughness = saturate(roughness * sRoughScale);
        if (!sArt) albedoLinear *= lerp(0.45, 1.0, sCavity) * clamp(1.0 + 1.1 * (n.y - normalize(i.normal).y), 0.55, 1.4);
        if (!sArt && !gFoliage) { // the extra map's G says how much weathering the material takes (1 = all, 0 = it is authored as it should look)
            const float3 albedo0 = albedoLinear;
            const float rough0 = roughness, metal0 = metal;
            Weather(i, normalize(i.normal), albedoLinear, roughness, metal);
            albedoLinear = lerp(albedo0, albedoLinear, extra.g);
            roughness = lerp(rough0, roughness, extra.g);
            metal = lerp(metal0, metal, extra.g);
        }
    } else if (IsReliefGround(i.color.a)) {
        roughness = saturate(roughness * sRoughScale);
        albedoLinear *= lerp(0.55, 1.0, sCavity);
    }
    float3 lit = LitPbr(albedoLinear, n, i.worldPos, gPixelShadow * cloudSun * sPomShadow, roughness, metal, cloudSun, share);
    lit += emissiveColor * (extra.a * extra.a * kEmissiveScale); // glass of a lantern, embers, a lit window: light the surface gives off
    if (gFoliage) share *= 0.5; // screen-space occlusion overdoes a tuft of thin blades
    const float shareAlpha = 0.76 + 0.23 * share; // above 0.75: not skin; 1.0 (eyes, clear) keeps the older occlusion
    return float4(lit, roughness < 0.35 ? -shareAlpha : shareAlpha); // negative: smooth enough for the composite's screen-space reflection
}

// The scene, for things that say how they moved: uv this frame minus uv last frame. Zero means "ask the depth buffer" (see TaaPS).
// The motion target's z is skin's surface reflection (luminance, see LitSkin): the scatter pass keeps it out of the blur. 0 elsewhere.
struct PSOutMotion {
    float4 color : SV_Target0;
    float4 velocity : SV_Target1;
};
PSOutMotion PSMainMotion(VSOutMotion i) {
    PSOutMotion o;
    o.color = PSMain(i.base);
    const float2 cur = i.curClip.xy / i.curClip.w, prev = i.prevClip.xy / i.prevClip.w;
    o.velocity = float4(float2(cur.x - prev.x, prev.y - cur.y) * 0.5, dot(gSkinSpecular, float3(0.2126, 0.7152, 0.0722)), 0.0);
    return o;
}

void PSVegetationDepth(VSOutDepth i) {
    clip(gAlbedo.Sample(gTexSampler, i.uv).a - 0.4);
}
// Every pixel that reaches this already passed the alpha test, so the depth test may run first even though Material can clip.
[earlydepthstencil]
PSOutMotion PSVegetationShade(VSOutMotion i) { return PSMainMotion(i); }

// Editor overlays, drawn after tone mapping: display-space colors go straight through.
float4 PSOverlay(VSOut i) : SV_Target {
    if (dot(i.normal, i.normal) < 1e-6) return i.color;
    float4 color;
    float3 n;
    Material(i, color, n);
    return float4(Lit(color.rgb, n, ShadowFactor(i.shadowPos, i.tightPos)), color.a);
}

// The shadow passes (the sun's map and the tight character map): depth only, from the sun.
float4 ShadowVS(VSIn v) : SV_Position {
    return mul(pc.mvp, float4(v.pos, 1.0));
}

float4 ShadowInstancedVS(VSIn v, InstanceIn inst) : SV_Position {
    float3 world = InstanceWorld(v, inst);
    if (v.color.a > 0.29 && v.color.a < 0.39) { // short-range grass: as in ShadowVegetationVS, gone by 20 m
        const float3 root = float3(inst.row0.w, inst.row1.w, inst.row2.w);
        const float t = saturate((20.0 - length(root - gLight.eye.xyz)) / 7.0);
        world = root + (world - root) * (t * t * (3.0 - 2.0 * t));
    }
    return mul(pc.mvp, float4(world, 1.0));
}
