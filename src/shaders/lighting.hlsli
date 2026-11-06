// The scene's light rig, shared by every shader that lights things (mesh.hlsl, impostor.hlsl) so a mesh and its
// impostor shade the same way. The CPU side is `Lighting` in renderer.h; it changes with the sun's time of day and
// is uploaded once per frame, together with the sun's shadow map.
#ifndef TBD_LIGHTING_HLSLI
#define TBD_LIGHTING_HLSLI

struct LightingData {
    float4 sunDir;       // xyz: unit vector toward the sun
    float4 sunColor;     // rgb: color x intensity, zero when the sun is down
    float4 skyColor;     // rgb: ambient light from straight up
    float4 groundColor;  // rgb: ambient light from straight down
    float4x4 shadowVP;   // world -> shadow map clip space (orthographic)
    float4 shadowParams; // y: one texel in uv, z: normal offset in world units, w: shadows on
    float4 eye;          // xyz: camera position
    float4x4 tightVP;    // world -> tight character shadow map clip space (orthographic)
    float4 tightParams;  // x: metres its depth spans, y: one texel in uv, z: normal offset in world units, w: tight map on
    float4 skySh[4];     // sky and ground radiance, first order: mean rgb, then the rgb gradient along x, y, z
    float4 skyGrad[3];   // reflections: zenith, horizon, ground radiance
    float4 probeOrigin;  // xyz: grid cell of the lowest corner, w: spacing across
    float4 probeGrid;    // cells x, y, z; w: spacing up
    float4 probeUpdate;  // first probe, probes per frame, rays per probe, on
    float4 probePrev;    // last frame's origin
    float4 lightDebug;   // x: lighting component to view
    float4 cloudMap;     // cloud shadow map: centre x, z, half extent, on
    float4x4 captureVP;  // the lit capture from the sun; read only by the post passes
    float4 portalCount;  // PortalBlock in renderer.h: x window panes, then centre, half-extent u, half-extent v for each
    float4 portals[72];
    float4 rooms[18];     // count.y rooms: centre and half height, half-extent x, half-extent z (InteriorRoom in renderer.h)
};

#ifdef VULKAN
[[vk::binding(0, 0)]] ConstantBuffer<LightingData> gLight;
[[vk::binding(1, 0)]] Texture2D<float> gShadowMap;
[[vk::binding(2, 0)]] SamplerComparisonState gShadowSampler;
[[vk::binding(3, 0)]] Texture2D<float> gTightMap;
[[vk::binding(4, 0)]] Texture2D<float4> gProbes;
[[vk::binding(5, 0)]] Texture2D<float4> gCloudShadow;
#else
ConstantBuffer<LightingData> gLight : register(b1);
Texture2D<float> gShadowMap : register(t0);
Texture2D<float> gTightMap : register(t4);
Texture2D<float4> gProbes : register(t5);
Texture2D<float4> gCloudShadow : register(t6);
SamplerComparisonState gShadowSampler : register(s0);
#endif

// PCSS (Fernando, "Percentage-Closer Soft Shadows", 2005): the penumbra widens with the distance to the caster, as the sun's disc
// (0.53 degrees) makes it. Vogel disc taps turned by interleaved gradient noise, which TAA averages; a receiver plane keeps wide filters
// off slopes (Isidoro 2006). Set per pixel by the shader's entry; the frame number rides in lightDebug.y.
#define FEATURE_OFF(bit) ((int(gLight.lightDebug.z) & (bit)) != 0) // TBD_FEATURECOST: see main.cpp
static float2 gShadowPixel = float2(0.0, 0.0);
static bool gFoliage = false; // thin leaves: light comes through, and self-shadow does not shut out the sky (set by PSMain)
static float gSunCover = 0.0; // 0 open .. 1 a close caster shuts out the sun's side of the sky (porches, under eaves); set by SunShadowFactor
static float3 gSkinSpecular = 0.0; // set by LitSkin: the part of its light reflected at the surface, never blurred by the scatter pass
static float gTightSoft = 1.0; // how far the tight map's taps spread: skin widens them, standing in for the light that travels under it (set by PSMain)
float ShadowNoise() {
    float2 p = gShadowPixel + fmod(gLight.lightDebug.y, 64.0) * 5.588238;
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}
float SunShadowFactor(float3 shadowPos) {
    gSunCover = 0.0;
    if (gLight.shadowParams.w < 0.5) return 1.0;
    float2 uv = float2(shadowPos.x * 0.5 + 0.5, 0.5 - shadowPos.y * 0.5);
    float edge = saturate(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 20.0);
    if (edge <= 0.0 || shadowPos.z >= 1.0) return 1.0;

    float2 size;
    gShadowMap.GetDimensions(size.x, size.y);
    const float texel = gLight.shadowParams.y;
    const float texelWorld = 2.0 / length(gLight.shadowVP[0].xyz) * texel, range = 1.0 / length(gLight.shadowVP[2].xyz);
    // dz per uv of the receiver's plane; clamped so a silhouette's huge derivative cannot move the test far.
    float3 dx = ddx(float3(uv, shadowPos.z)), dy = ddy(float3(uv, shadowPos.z));
    float det = dx.x * dy.y - dx.y * dy.x;
    float2 dz = abs(det) > 1e-12 ? float2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det : float2(0.0, 0.0);
    const float dzMax = 3.0 * texelWorld / (range * texel); // a 70 degree slope, in depth per uv
    dz = clamp(dz, -dzMax, dzMax);

    const float kSunTan = 0.0125, kMaxCaster = 14.0; // a little over the true 0.0093: reads better at game resolution
    float angle = ShadowNoise() * 6.2831853;
    float searchUv = max(kMaxCaster * kSunTan / texelWorld, 2.0) * texel;
    float blockers = 0.0, sumZ = 0.0;
    [unroll] for (int k = 0; k < 5; ++k) {
        float r = sqrt((k + 0.5) / 5.0), a = angle + k * 2.399963;
        float2 o = float2(cos(a), sin(a)) * r * searchUv;
        float z = gShadowMap.Load(int3(clamp((uv + o) * size, 0.0, size - 1.0), 0));
        if (z < shadowPos.z + dot(o, dz) - 0.0001) { blockers += 1.0; sumZ += z; }
    }
    if (FEATURE_OFF(2)) { const float one = gShadowMap.SampleCmpLevelZero(gShadowSampler, uv, shadowPos.z); return lerp(1.0, one, edge); }
    if (blockers < 0.5) return 1.0; // no caster near: the common case costs six loads, less than the old nine filtered taps
    float dist = (shadowPos.z - sumZ / blockers) * range;
    float penumbra = clamp(dist * kSunTan / texelWorld, 1.25, 24.0) * texel;
    float lit = 0.0;
    [unroll] for (int k = 0; k < 6; ++k) {
        float r = sqrt((k + 0.5) / 6.0), a = angle * 1.7 + k * 2.399963;
        float2 o = float2(cos(a), sin(a)) * r * penumbra;
        lit += gShadowMap.SampleCmpLevelZero(gShadowSampler, uv + o, shadowPos.z + dot(o, dz));
    }
    lit /= 6.0;
    // A caster only shuts out the sky if it is wide: a porch fills these taps, a head or a post misses them.
    float wide = 0.0;
    const float wideUv = 1.5 / texelWorld * texel;
    [unroll] for (int k = 0; k < 4; ++k) {
        float a = angle + k * 1.5707963;
        float2 o = float2(cos(a), sin(a)) * wideUv;
        float z = gShadowMap.Load(int3(clamp((uv + o) * size, 0.0, size - 1.0), 0));
        if (z < shadowPos.z + dot(o, dz) - 0.0001) wide += 0.25;
    }
    gSunCover = (1.0 - lit) * saturate(1.3 - dist / 6.0) * edge * wide;
    return lerp(1.0, lit, edge);
}

// The shadow a surface point receives: 1 = lit, 0 = in shadow. `shadowPos` is the point in the sun map's clip space,
// `tightPos` in the tight character map's. Where the point lies inside the tight map's box it uses that (5x5 taps, small
// sharp shadows); toward the box's edges and ends it blends to the sun map, which alone covers everything outside.
// Shadows off, or the sun down, leave the sun map's flag clear and everything lit.
float ShadowFactor(float3 shadowPos, float3 tightPos) {
    if (gLight.shadowParams.w < 0.5) return 1.0;
    if (gLight.tightParams.w < 0.5) return SunShadowFactor(shadowPos);

    float2 uv = float2(tightPos.x * 0.5 + 0.5, 0.5 - tightPos.y * 0.5);
    float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 8.0; // the blend band is an eighth of the box wide
    float ends = min(tightPos.z, 1.0 - tightPos.z) * 12.0;                // and near and far end (z runs 0..1 through the box)
    float weight = saturate(min(edge, ends));
    if (weight <= 0.0 || FEATURE_OFF(128)) return SunShadowFactor(shadowPos);

    float texel = gLight.tightParams.y * gTightSoft;
    const float turn = gTightSoft > 1.0 ? ShadowNoise() * 6.2831853 : 0.0; // turned per pixel so a wide spread shows no grid; TAA averages it
    const float2x2 spin = float2x2(cos(turn), -sin(turn), sin(turn), cos(turn));
    float lit = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x)
            lit += gTightMap.SampleCmpLevelZero(gShadowSampler, uv + mul(spin, float2(x, y)) * texel, tightPos.z);
    lit /= 25.0;
    if (lit < 0.99) SunShadowFactor(shadowPos); // for gSunCover: the sky a close caster shuts out
    if (weight >= 1.0) return lit;
    return lerp(SunShadowFactor(shadowPos), lit, weight);
}

// How much sun the clouds let through to a point (the top-down map from post.hlsl's CloudShadowPS, bilinear by hand).
float CloudSun(float3 world) {
    if (gLight.cloudMap.w < 0.5 || FEATURE_OFF(32)) return 1.0;
    float2 uv = (world.xz - gLight.cloudMap.xy) / (2.0 * gLight.cloudMap.z) + 0.5;
    float2 dim;
    gCloudShadow.GetDimensions(dim.x, dim.y);
    float2 p = uv * dim - 0.5, b = floor(p), f = p - b;
    int3 c = int3(clamp(int2(b), int2(0, 0), int2(dim) - 2), 0);
    float t = lerp(lerp(gCloudShadow.Load(c).r, gCloudShadow.Load(c + int3(1, 0, 0)).r, f.x),
                   lerp(gCloudShadow.Load(c + int3(0, 1, 0)).r, gCloudShadow.Load(c + int3(1, 1, 0)).r, f.x), f.y);
    float edge = saturate(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 20.0);
    return lerp(1.0, t, edge);
}

// Diffuse light from the sky and ground, per unit albedo. Radiance stored as mean + gradient (order-1 spherical harmonics,
// Ramamoorthi and Hanrahan 2001), convolved with the cosine lobe: the gradient's band is scaled by 2/3.
float3 SkyAmbient(float3 n) {
    float3 mean = gLight.skySh[0].rgb;
    float3 e = mean + (gLight.skySh[1].rgb * n.x + gLight.skySh[2].rgb * n.y + gLight.skySh[3].rgb * n.z) * (2.0 / 3.0);
    return max(e, mean * 0.1);
}

#define PROBE_ORIGIN gLight.probeOrigin
#define PROBE_GRID gLight.probeGrid
#define PROBE_ROOMS gLight.rooms
#define PROBE_ROOM_COUNT gLight.portalCount.y
#include "probe.hlsli"

// Diffuse light arriving at a point: the probes where they are, the sky beyond them.
float3 IndirectDiffuse(float3 world, float3 n, out float skyVis) {
    skyVis = 1.0;
    if (gLight.probeUpdate.w < 0.5 || FEATURE_OFF(16)) return SkyAmbient(n);
    float fade;
    float3 e = ProbeIrradiance(world, n, fade, skyVis);
    skyVis = lerp(1.0, skyVis, fade);
    return fade > 0.0 ? lerp(SkyAmbient(n), e, fade) : SkyAmbient(n);
}

// Sky and ground in a direction, blurrier as the surface gets rougher: the gradient stands in for a prefiltered cube.
float3 SkyReflection(float3 d, float roughness) {
    float width = 0.03 + roughness * 0.6;
    float3 sky = lerp(gLight.skyGrad[1].rgb, gLight.skyGrad[0].rgb, sqrt(saturate(d.y)));
    float3 sharp = lerp(gLight.skyGrad[2].rgb, sky, smoothstep(-width, width, d.y));
    return lerp(sharp, SkyAmbient(d), roughness * roughness);
}
// The environment BRDF's analytic fit (Karis, "Physically Based Shading on Mobile", 2014, after Lazarov 2013).
float3 EnvBrdf(float3 f0, float roughness, float nv) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022), c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp(-9.28 * nv * log(2.0))) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// The Callisto BRDF (Jimenez and Petersen, "The Rendering of The Callisto Protocol", SIGGRAPH 2023, slides 85, 90, 95): Lambert + GGX
// with the three things measured materials do and Lambert cannot. Parameters left at the talk's defaults give back Lambert + GGX.
float CallistoRemap(float x) { return 2.0 * (1.0 - x); }
float CallistoPeak(float cosA, float n, float cosB, float m) { return pow(max(1.0 - cosA, 1e-4), 5.0 * n) * pow(max(cosB, 1e-4), 5.0 * m); }

// c1: diffuse Fresnel (rho f) and retroreflection (rho r). The falloffs and tangent falloffs stay at the talk's 0.75.
float CallistoDiffuse(float vh, float nh, float rhoF, float rhoR) {
    const float falloff = CallistoRemap(0.75);
    return lerp(1.0, rhoF, CallistoPeak(vh, falloff, nh, falloff)) * lerp(1.0, rhoR, CallistoPeak(nh, falloff, vh, falloff));
}

// c2: the shading terminator, masked so it leaves the Fresnel and retroreflection regions alone. The slide writes the mask in theta; cosines are what make it do that.
float CallistoTerminator(float nl, float vh, float nh, float smoothing, float reach) {
    const float d = 1.0 - vh, h = 1.0 - nh;
    const float mask = (1.0 - d * d * d) * (1.0 - h * h * h);
    return lerp(1.0, smoothstep(0.0, max(mask * reach, 1e-4), nl), mask * smoothing);
}

// The sky seen through the panes near a point inside a room, from the exact solid angle of each pane (Lambert's polygon formula):
// the probes are four metres apart, which is too coarse for the fall-off from a window. Points that see the sky themselves skip it.
float3 WindowSkyLight(float3 world, float3 n, float skyVis) {
    float3 sum = 0.0;
    float inside = saturate(1.0 - skyVis * 1.5);
    if (inside <= 0.0) return sum;
    for (int k = 0; k < int(gLight.portalCount.x); ++k) {
        float3 c = gLight.portals[k * 3].xyz, u = gLight.portals[k * 3 + 1].xyz, v = gLight.portals[k * 3 + 2].xyz;
        float3 toC = c - world;
        if (dot(toC, toC) > 144.0) continue;
        float3 pn = cross(u, v);
        float side = dot(pn, world - c) > 0.0 ? 1.0 : -1.0;
        float3 p[4] = {c - u - v, c + u - v, c + u + v, c - u + v};
        float f = 0.0;
        for (int e = 0; e < 4; ++e) {
            float3 a = normalize(p[e] - world), b = normalize(p[(e + 1) & 3] - world);
            float3 ax = cross(a, b);
            float len = length(ax);
            if (len > 1e-5) f += acos(clamp(dot(a, b), -1.0, 1.0)) * dot(ax / len, n);
        }
        float ff = saturate(-f * side) / 6.2831853;
        float3 outward = normalize(toC);
        float3 L = max(gLight.skySh[0].rgb + (gLight.skySh[1].rgb * outward.x + gLight.skySh[2].rgb * outward.y + gLight.skySh[3].rgb * outward.z), 0.0);
        sum += L * ff * float3(0.78, 0.82, 0.80);
    }
    return sum * inside;
}

// Diffuse light arriving at a lit surface: probes or sky, what the clouds take, and the bounce off the sunlit floor. Call after ShadowFactor (gSunCover).
// The scene's ambient fill (Scene::ambientFill, main.cpp sends fill + 1 so an unset slot reads as 1): scales sky, probe and
// ground-bounce light, for a scene lit by one hard source.
float AmbientFill() { return gLight.lightDebug.w > 0.5 ? gLight.lightDebug.w - 1.0 : 1.0; }
float3 SurfaceIrradiance(float3 world, float3 n, float cloudSun, out float skyVis) {
    const float kSunScale = 3.2;
    const float3 l = gLight.sunDir.xyz;
    float3 irradiance = IndirectDiffuse(world, n, skyVis) * lerp(0.8, 1.0, cloudSun); // heavy cover takes a little of the ambient too
    // Inside a building's room the probes hold only the room's bounce (RoomTrace in post.hlsl) and no street lies outside the
    // wall to light it; WindowSkyLight below gives the sky through each pane, with its fall-off.
    const bool inRoom = gLight.portalCount.y > 0.5 && ProbeRoomOf(world, 0.3) >= 0;
    // Sunlit desert floor lights what faces away from the sun: half the hemisphere of a wall is ground (form factor 0.5). The coarse probes
    // miss most of this close to a wall, which left shaded fronts near black beside a bright street.
    if (!inRoom) irradiance += gLight.sunColor.rgb * kSunScale * saturate(l.y) * float3(0.40, 0.29, 0.19) * (0.5 - 0.5 * n.y) * cloudSun * lerp(0.35, 1.0, skyVis) * 0.5;
    irradiance *= 1.0 - 0.6 * gSunCover;
    skyVis *= 1.0 - gSunCover;
    irradiance += WindowSkyLight(world, n, inRoom ? 0.0 : skyVis);
    const float fill = AmbientFill();
    skyVis *= fill; // sky reflections fade with it
    return irradiance * fill;
}

// `n` is a unit world-space normal; `shadow` scales the sun's share (1 = fully lit).
// Physically based sun (GGX specular, Schlick Fresnel, dielectric) on top of sky ambient, in linear HDR.
// `ambientShare` tells the composite pass how much of the result the screen-space occlusion should dim.
static float gMatAO = 1.0; // the material's baked occlusion (its extra map's R): dims what the sky and the probes give, not the sun
static const float kEmissiveScale = 6.0; // emissive strength 1 gives this many times the surface's albedo, in HDR scene units
float3 LitPbr(float3 albedo, float3 n, float3 world, float shadow, float roughness, float metal, float cloudSun, out float ambientShare) {
    const float kSunScale = 3.2; // sunColor is authored for the old LDR look
    float3 v = normalize(gLight.eye.xyz - world), l = gLight.sunDir.xyz, h = normalize(v + l);
    if (gFoliage) { n = normalize(n + float3(0.0, 0.8, 0.0)); gSunCover = 0.0; }
    float nl = saturate(dot(n, l)), nv = max(dot(n, v), 1e-3), nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    float a = roughness * roughness, a2 = a * a;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * d * d);
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float G = 1.0 / ((nl * (1.0 - k) + k) * (nv * (1.0 - k) + k) * 4.0);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float3 F = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    float3 diffuseColor = albedo * (1.0 - metal);
    float3 sun = gLight.sunColor.rgb * kSunScale * nl * shadow;
    float skyVis;
    float3 irradiance = SurfaceIrradiance(world, n, cloudSun, skyVis);
    // Reflections: the sky where the probes see it, otherwise the light around the point. No screen-space or traced pass.
    float3 rd = reflect(-v, n);
    rd = normalize(lerp(rd, n, roughness * roughness)); // the lobe's dominant direction (Lagarde and de Rousiers 2014)
    float3 envL = lerp(irradiance, SkyReflection(rd, roughness), skyVis);
    float3 reflection = envL * EnvBrdf(f0, roughness, nv);
    float3 ambientPart = (diffuseColor * irradiance + reflection * lerp(gMatAO, 1.0, 0.25)) * gMatAO;
    // Rough surfaces flatten when front lit and lose the hard Lambert edge; polished and wet ones stay close to the baseline. TBD_LIGHTDEBUG=-1 views the baseline.
    const float callisto = gLight.lightDebug.x < -0.5 ? 0.0 : smoothstep(0.35, 0.75, roughness);
    const float c1 = CallistoDiffuse(vh, nh, 1.0 + 0.6 * callisto, 1.0 + 0.3 * callisto);
    const float c2 = CallistoTerminator(nl, vh, nh, 0.4 * callisto, 0.5);
    float3 direct = (c1 * diffuseColor * sun * (1.0 - F) + sun * 3.14159265 * D * G * F) * c2;
    if (gFoliage) direct += diffuseColor * gLight.sunColor.rgb * kSunScale * 0.25 * max(shadow, 0.35); // through the blade
    float la = dot(ambientPart, float3(0.2126, 0.7152, 0.0722)), ld = dot(direct, float3(0.2126, 0.7152, 0.0722));
    ambientShare = la / max(la + ld, 1e-4);
    if (gLight.lightDebug.x > 0.5) {
        if (gLight.lightDebug.x < 1.5) return irradiance;
        if (gLight.lightDebug.x < 2.5) return direct;
        if (gLight.lightDebug.x < 3.5) return reflection;
        if (gLight.lightDebug.x > 7.5) return irradiance + sun; // the light reaching the surface, as tbd_reference traces it
    }
    return ambientPart + direct;
}

// Cloth: no sharp lobe. The "Charlie" sheen distribution (Estevez and Kulla, "Production Friendly Microfacet Sheen BRDF", 2017)
// with Ashikhmin's visibility as simplified by Neubelt and Pettineo (2013), and wrapped diffuse for the fuzz.
float3 LitCloth(float3 albedo, float3 n, float3 world, float shadow, float roughness, float cloudSun, out float ambientShare) {
    float3 v = normalize(gLight.eye.xyz - world), l = gLight.sunDir.xyz, h = normalize(v + l);
    float nlRaw = dot(n, l), nl = saturate(nlRaw), nv = max(dot(n, v), 1e-3), nh = saturate(dot(n, h));
    float inv = 1.0 / max(roughness * roughness, 0.1), sinH = sqrt(max(1.0 - nh * nh, 1e-4));
    float D = (2.0 + inv) * pow(sinH, inv) / 6.2831853;
    float V = 1.0 / (4.0 * max(nl + nv - nl * nv, 1e-3));
    float3 sheen = lerp(float3(0.04, 0.04, 0.04), sqrt(albedo) * 0.3, 0.7);
    float3 sun = gLight.sunColor.rgb * 3.2 * shadow;
    const float wrap = 0.3;
    float keep = 1.0 - 0.5 * dot(sheen, float3(0.2126, 0.7152, 0.0722)); // what the sheen layer takes from below
    float skyVis;
    float3 irradiance = IndirectDiffuse(world, n, skyVis) * lerp(0.8, 1.0, cloudSun) * (1.0 - 0.6 * gSunCover);
    float3 ambientPart = albedo * irradiance * keep + irradiance * sheen * 0.25;
    float3 direct = albedo * sun * saturate((nlRaw + wrap) / ((1.0 + wrap) * (1.0 + wrap))) * keep + sun * nl * 3.14159265 * D * V * sheen;
    float la = dot(ambientPart, float3(0.2126, 0.7152, 0.0722)), ld = dot(direct, float3(0.2126, 0.7152, 0.0722));
    ambientShare = la / max(la + ld, 1e-4);
    return ambientPart + direct;
}

// Sunlight that came through the flesh, per channel: the tight map holds where the sun's ray first met the body, so the
// distance from there to this point is how much it crossed. Red gets furthest. Ears and nostrils glow; a head stays dark.
float3 SkinTransmission(float3 world) {
    if (gLight.tightParams.w < 0.5) return 0.0;
    float3 p = mul(gLight.tightVP, float4(world, 1.0)).xyz;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv <= 0.0) || any(uv >= 1.0)) return 0.0;
    const float size = 1.0 / gLight.tightParams.y;
    // Bilinear over the four texels around the point: unweighted, the thickness stepped texel by texel and drew a staircase.
    const float2 at = uv * size - 0.5, f = frac(at);
    const int2 texel = int2(floor(at));
    const int last = (int)size - 1;
    float t[4];
    for (int k = 0; k < 4; ++k) t[k] = gTightMap.Load(int3(clamp(texel + int2(k & 1, k >> 1), 0, last), 0));
    const float first = lerp(lerp(t[0], t[1], f.x), lerp(t[2], t[3], f.x), f.y);
    // Light comes out having travelled at least a few millimetres under the skin: just round the shadow line of a nose or a
    // cheek the map reads almost no thickness, and a full glow there drew a hard red stripe brighter than the lit side.
    const float metres = max((p.z - first) * gLight.tightParams.x, 0.0) + 0.004;
    return exp(-metres / float3(0.0060, 0.0016, 0.0010));
}

// Skin: two specular lobes (the oily film and the skin below it) and a softer shadow line in red, where light travels
// furthest under the surface. The screen-space scattering in post.hlsl does the rest of the blur.
// `surface`: R roughness (0.5 = neutral), G cavity (dark in pores and creases, which hold back light and highlights).
// `shadow` is the shadow maps alone. `ambientShare` as in LitPbr.
float3 LitSkin(float3 albedo, float3 n, float3 world, float shadow, float cloudSun, float3 surface, out float ambientShare) {
    const float kSunScale = 3.2;
    float3 v = normalize(gLight.eye.xyz - world), l = gLight.sunDir.xyz, h = normalize(v + l);
    float nlRaw = dot(n, l), nl = saturate(nlRaw), nv = max(dot(n, v), 1e-3), nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    float3 wrap = float3(0.28, 0.08, 0.04);
    float3 diffuseNl = saturate((nlRaw + wrap) / ((1.0 + wrap) * (1.0 + wrap)));
    float F = 0.028 + 0.972 * pow(1.0 - vh, 5.0);
    float spec = 0.0;
    // surface.r is oil shine: the shipped maps sit between 0.35 and 0.72 (0.7x .. 1.3x of the base roughness, unchanged). Painted shine
    // (Character Creator, SKIN PAINT) lowers red under 0.3, which keeps going down to a wet 0.22x instead of stopping at 0.7x.
    const float shineMul = surface.r >= 0.3 ? lerp(0.7, 1.3, surface.r) : lerp(0.22, lerp(0.7, 1.3, 0.3), surface.r / 0.3);
    const float2 rough = float2(0.54, 0.34) * shineMul, weight = float2(0.75, 0.25);
    for (int k = 0; k < 2; ++k) {
        float a = rough[k] * rough[k], a2 = a * a;
        float d = nh * nh * (a2 - 1.0) + 1.0;
        float g = (rough[k] + 1.0) * (rough[k] + 1.0) / 8.0;
        spec += weight[k] * a2 / (3.14159265 * d * d) / ((nl * (1.0 - g) + g) * (nv * (1.0 - g) + g) * 4.0);
    }
    float skyVis;
    // The shadow map's sky cover (gSunCover, dims the ambient under a close caster) sees the face's own facets too: on the side
    // turned from the sun it tore a jagged dark line down the face. Trusted only where the face turns toward the sun.
    gSunCover *= smoothstep(0.1, 0.4, nlRaw);
    float3 irradiance = SurfaceIrradiance(world, n, cloudSun, skyVis);
    const float3 rd = reflect(-v, n), f0 = float3(0.028, 0.028, 0.028);
    float3 reflection = 0.0;
    for (int k = 0; k < 2; ++k) reflection += weight[k] * lerp(irradiance, SkyReflection(rd, rough[k]), skyVis) * EnvBrdf(f0, rough[k], nv);
    float3 ambientPart = albedo * irradiance * lerp(0.6, 1.0, surface.g) + reflection * surface.g;
    // Near the shadow line the normals already fade the sun, and the map only sees the mesh's facets, which tear hard lit patches into a face.
    // Further round, the map is trusted: that is where a nose or a hat brim casts a real shadow.
    float3 sun = gLight.sunColor.rgb * kSunScale * lerp(1.0, shadow, smoothstep(-0.05, 0.25, nlRaw)) * cloudSun;
    // Only well round the side away from the sun: just past the shadow line the map's first surface is the point itself, so the
    // thickness reads zero there and a full red glow drew a hard line down a face lit from the side.
    float3 through = gLight.sunColor.rgb * kSunScale * SkinTransmission(world) * smoothstep(-0.15, -0.6, nlRaw) * 0.45;
    // Peach fuzz: the fine vellus hair catches light along the silhouette and softens it (cheeks, jaw, brow). A grazing-angle sheen in
    // the skin's own colour, lit by the sun where it faces it and by the sky all round; none face-on, so it never greys a lit cheek.
    const float fuzz = pow(1.0 - nv, 3.0) * 0.16 * lerp(0.5, 1.0, surface.g);
    ambientPart += albedo * irradiance * fuzz;
    float3 direct = albedo * (sun * diffuseNl * (1.0 - F) + through + sun * fuzz * saturate(nlRaw * 0.5 + 0.5)) + sun * nl * 3.14159265 * spec * F * surface.g;
    float la = dot(ambientPart, float3(0.2126, 0.7152, 0.0722)), ld = dot(direct, float3(0.2126, 0.7152, 0.0722));
    ambientShare = la / max(la + ld, 1e-4);
    // The surface's own reflection (oil film and the skin's top layer), already inside the return: the scatter pass takes it
    // back out, blurs only the light that went under the skin, and adds this back sharp (PSMainMotion, post.hlsl ScatterSkin).
    gSkinSpecular = reflection * surface.g + sun * nl * 3.14159265 * spec * F * surface.g;
    return ambientPart + direct;
}

// Eye: a wet cornea over the iris and sclera. It mirrors the sky and pins a small sharp sun highlight, and the socket
// keeps some ambient light off what lies beneath.
float3 LitEye(float3 albedo, float3 n, float3 world, float shadow) {
    const float kSunScale = 3.2, a2 = 0.0016;
    float3 v = normalize(gLight.eye.xyz - world), l = gLight.sunDir.xyz, h = normalize(v + l), r = reflect(-v, n);
    float nl = saturate(dot(n, l)), nv = max(dot(n, v), 1e-3), nh = saturate(dot(n, h));
    float F = 0.025 + 0.975 * pow(1.0 - nv, 5.0);
    float d = nh * nh * (a2 - 1.0) + 1.0;
    float3 sun = gLight.sunColor.rgb * kSunScale * shadow;
    float skyVis;
    float3 ambient = SurfaceIrradiance(world, n, 1.0, skyVis);
    float3 mirror = lerp(ambient, SkyReflection(r, 0.03), skyVis);
    return albedo * (ambient * 0.8 + sun * saturate(dot(n, l) * 0.8 + 0.2) * 0.8) + mirror * F + sun * nl * min(a2 / (12.566 * d * d), 12.0);
}

float GgxSun(float nh, float nl, float nv, float roughness) {
    const float a2 = max(roughness * roughness * roughness * roughness, 1e-4), d = nh * nh * (a2 - 1.0) + 1.0, k = roughness * roughness * 0.5;
    return min(a2 / (3.14159 * d * d) * nl / max(4.0 * (nl * (1.0 - k) + k) * (nv * (1.0 - k) + k), 1e-3), 40.0);
}

// Enamel and the wet tissue of the mouth. `ao` is how far this point sits in the mouth's dark: it takes ambient light, and
// reflections more so, since a mirror direction leaving a mouth mostly finds the mouth. `wrap` and `scatter` are light carried
// under the surface: wide and red in a gum, narrow and warm in a tooth's dentine.
float3 LitMouth(float3 albedo, float3 n, float3 world, float shadow, float cloudSun, float ao, float roughness, float wrap, float3 scatter) {
    const float kSunScale = 3.2;
    const float3 v = normalize(gLight.eye.xyz - world), l = gLight.sunDir.xyz, h = normalize(v + l), r = reflect(-v, n);
    const float nlRaw = dot(n, l), nl = saturate(nlRaw), nv = max(dot(n, v), 1e-3), nh = saturate(dot(n, h));
    const float3 sun = gLight.sunColor.rgb * kSunScale * shadow * cloudSun;
    float skyVis;
    // The mouth's walls bounce light around, warm, so even a shaded tooth is never as dark as its direct share of the sky.
    const float3 ambient = SurfaceIrradiance(world, float3(n.x, max(n.y, 0.0), n.z), cloudSun, skyVis) * ao * float3(1.2, 1.03, 0.88) + gLight.sunColor.rgb * kSunScale * cloudSun * 0.26 * lerp(0.3, 1.0, ao) * float3(1.0, 0.86, 0.78);
    const float wrapped = saturate((nlRaw + wrap) / (1.0 + wrap));
    const float3 diffuse = lerp(float3(nl, nl, nl), wrapped.xxx * scatter, saturate(wrapped - nl + 0.5 * wrap));
    const float F = 0.045 + 0.955 * pow(1.0 - nv, 5.0);
    const float specOcc = saturate(ao * ao * (0.4 + 0.6 * nv));
    const float3 mirror = lerp(ambient, SkyReflection(r, roughness), skyVis * specOcc * specOcc); // inside a mouth a reflection finds the mouth, not the sky
    return albedo * (ambient + sun * diffuse) + mirror * F * specOcc + sun * GgxSun(nh, nl, nv, roughness) * 0.045 * ao;
}

// The eye as two layers. Sclera: wet, with light soaking a little past the terminator. Iris: under the cornea, so it is lit from a
// concave normal, and the cornea gathers sun into a crescent on the side away from the light. `lid` is the lids' and lashes' occlusion.
float3 LitEyeball(float3 albedo, float3 n, float3 corneaN, float3 irisN, float3 world, float shadow, float cloudSun, float lid, float iris, float3 radial, float r) {
    const float kSunScale = 3.2;
    const float3 v = normalize(gLight.eye.xyz - world), l = gLight.sunDir.xyz;
    const float3 sun = gLight.sunColor.rgb * kSunScale * shadow * cloudSun * lerp(0.08, 1.0, lid); // the lid stops most of the sun, not a third
    float skyVis;
    const float3 ambient = SurfaceIrradiance(world, n, cloudSun, skyVis) * lid;
    const float nlSclera = dot(n, l), wrapped = saturate((nlSclera + 0.35) / 1.35);
    const float3 sclera = lerp(wrapped.xxx * float3(1.0, 0.86, 0.80), saturate(nlSclera).xxx, saturate(nlSclera * 2.0));
    const float irisNl = saturate(dot(irisN, l) * 0.75 + 0.25);
    const float3 tangentL = l - n * dot(n, l);
    const float away = saturate(dot(normalize(tangentL + 1e-4), -radial) * 0.5 + 0.35);
    const float caustic = pow(away, 3.0) * smoothstep(0.16, 0.44, r) * saturate(dot(corneaN, l) * 1.5 + 0.3) * 1.2;
    const float3 diffuse = lerp(sclera, irisNl.xxx + caustic * float3(1.0, 0.95, 0.85), iris);
    float3 color = albedo * (ambient * lerp(0.95, 0.75, iris) + sun * diffuse);
    // light scattering through the sclera picks up the blood of the lids and the caruncle: a warm red lift toward the rim, never on the iris
    color += albedo * float3(0.20, 0.035, 0.025) * (ambient + sun * 0.25) * (1.0 - iris) * smoothstep(0.55, 1.0, r);
    // the tear film over everything: a very smooth coat; the cornea's curve gives the catchlight its place
    const float3 h = normalize(v + l), rv = reflect(-v, corneaN);
    const float nv = max(dot(corneaN, v), 1e-3), F = 0.025 + 0.975 * pow(1.0 - nv, 5.0);
    // The lid hides the sky only from the reflections that look up into it: most of what the cornea mirrors stays in view.
    const float mirrorLid = lerp(0.15, 1.0, lid);
    const float3 mirror = lerp(ambient, SkyReflection(rv, 0.03), skyVis * mirrorLid);
    color += mirror * F * mirrorLid;
    color += sun * GgxSun(saturate(dot(corneaN, h)), saturate(dot(corneaN, l)), nv, lerp(0.07, 0.04, iris)) * 0.11; // wet: one tight, hot glint
    color += sun * GgxSun(saturate(dot(corneaN, h)), saturate(dot(corneaN, l)), nv, 0.22) * 0.012; // and the tear film's wider sheen
    return color;
}

float3 Lit(float3 albedo, float3 n, float shadow) {
    float sun = saturate(dot(n, gLight.sunDir.xyz)) * shadow;
    float3 ambient = lerp(gLight.groundColor.rgb, gLight.skyColor.rgb, n.y * 0.5 + 0.5);
    return albedo * (ambient + gLight.sunColor.rgb * sun);
}

#endif
