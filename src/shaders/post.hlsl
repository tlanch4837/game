// The frame after the lit geometry, on both backends: ambient occlusion, volumetric clouds (half resolution), the physical
// sky and aerial haze, temporal anti-aliasing, tone mapping. Independent implementations of techniques from public
// talks: single-scattering atmosphere (Nishita), Perlin-Worley cloud volumes with Beer-powder light (Schneider, GPU Pro 7).
// World units are metres.

#ifdef VULKAN
#define BIND(n) [[vk::binding(n, 0)]]
#else
#define BIND(n)
#endif

BIND(0) cbuffer PostParams : register(b0) {
    float4x4 invViewProj; // of the jittered matrix the depth buffer was drawn with
    float4x4 prevViewProj; // last frame, unjittered
    float4 eye;           // xyz, upscale (1 = the tone map reconstructs with Catmull-Rom; misc.w is taken by the history flag)
    float4 sunDir;
    float4 res;           // the render size (not the window's): w, h, 1/w, 1/h
    float4 misc;          // time (s), frame, sky on, history valid
    // PostLightBlock in renderer.h
    float4x4 shadowVP;
    float4 sunColorP;
    float4 skySh[4];
    float4 skyGrad[3];
    float4 probeOrigin;
    float4 probeGrid;
    float4 probeUpdate;   // first probe this frame, probes per frame, rays per probe, on
    float4 probePrev;
    float4 lightDebug;    // component to view, atlas just made, first pass over the probes, sun map valid
    float4 cloudMap;      // cloud shadow map: centre x, z, half extent, on
    float4x4 captureVP;   // world -> the lit capture from the sun; [3][3] is 0 until there is one
    float4 motion;        // x: TAA follows motion vectors
    float4 portalCount;   // PortalBlock in renderer.h: x window panes, then three float4 each (centre, half-extent u, half-extent v)
    float4 portals[72];
    float4 rooms[18];     // count.y rooms: centre and half height, half-extent x, half-extent z (InteriorRoom in renderer.h)
};

BIND(1) Texture2D<float4> gScene : register(t0);
BIND(2) Texture2D<float> gDepth : register(t1);
BIND(3) Texture2D<float> gAo : register(t2);
BIND(4) Texture2D<float4> gClouds : register(t3);
BIND(5) Texture3D<float4> gNoise : register(t4);
BIND(6) Texture2D<float4> gComposed : register(t5);
BIND(7) Texture2D<float4> gHistory : register(t6);
BIND(8) Texture2D<float4> gResolved : register(t7);
BIND(11) Texture2D<float> gShadow : register(t8);
BIND(12) Texture2D<float4> gProbes : register(t9);
BIND(13) Texture2D<float4> gRays : register(t10);
BIND(14) Texture2D<float4> gExposure : register(t11);
BIND(15) Texture2D<float4> gBloom : register(t12);
BIND(16) Texture2D<float4> gCloudShadow : register(t13);
BIND(18) Texture2D<float4> gCloudField : register(t15);
BIND(19) Texture2D<float4> gCloudHistory : register(t16);
BIND(20) Texture2D<float4> gCloudMarch : register(t17);
BIND(9) SamplerState gClamp : register(s0);
BIND(10) SamplerState gWrap : register(s1);
BIND(17) Texture2D<float4> gVelocity : register(t14); // xy motion; z skin's surface reflection (luminance), kept out of ScatterSkin's blur
BIND(21) Texture2D<float4> gCapture : register(t18);

#define PROBE_ORIGIN probeOrigin
#define PROBE_GRID probeGrid
#define PROBE_ROOMS rooms
#define PROBE_ROOM_COUNT portalCount.y
#include "probe.hlsli"
#include "city_air.hlsli"

static const float kPI = 3.14159265;
static const float kEarth = 6360e3, kTop = 6420e3;
static const float3 kBetaR = float3(5.8e-6, 13.5e-6, 33.1e-6);
static const float kBetaM = 21e-6;
static const float kSunI = 12.0;
static const float kHazeScale = 1.2; // close to Earth's air: dry desert, so a street stays clear and only the far hills separate
static const float kCloudLo = 1500.0, kCloudHi = 4400.0, kWispLo = 6500.0, kWispHi = 7800.0, kFieldTile = 32000.0, kCloudSigma = 0.009;

struct FsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

FsOut FullscreenVS(uint id : SV_VertexID) {
    FsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}

float3 WorldAt(float2 uv, float depth) {
    float4 w = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return w.xyz / w.w;
}

float Ign(float2 p) { // interleaved gradient noise, shifted per frame so TAA averages it away
    p += fmod(misc.y, 64.0) * 5.588238;
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Distance along the ray to a sphere around the planet's center; negative when there is none ahead.
float SphereFar(float3 o, float3 d, float r) {
    float b = dot(o, d), c = dot(o, o) - r * r, h = b * b - c;
    if (h < 0.0) return -1.0;
    return -b + sqrt(h);
}

float3 PlanetPos(float3 world) { return float3(world.x, max(world.y, 1.0) + kEarth, world.z); }

float3 OpticalToSun(float3 p, float3 l) { // Rayleigh, Mie (in x, y) optical depth from p toward the sun
    if (dot(p, l) < 0.0) {
        float b = dot(p, l), h = dot(p, p) - b * b;
        if (h < kEarth * kEarth) return float3(1e9, 1e9, 0);
    }
    float len = SphereFar(p, l, kTop), step = len / 4.0;
    float2 od = 0;
    for (int i = 0; i < 4; ++i) {
        float h = length(p + l * (i + 0.5) * step) - kEarth;
        od += exp(-max(h, 0.0) / float2(8000.0, 1200.0)) * step;
    }
    return float3(od, 0);
}

// Ozone rides on the Rayleigh depth: its Chappuis band takes green from the long dusk path, which leaves the sky olive without it
// (coefficients after Hillaire 2020, scaled to the column it has per Rayleigh metre, and less at the horizon for its higher layer).
static const float3 kBetaO = float3(0.73e-6, 2.1e-6, 0.1e-6);
float3 Transmit(float2 od) { return exp(-((kBetaR + kBetaO) * od.x + kBetaM * 1.1 * od.y)); }

float3 SunLight(float3 p) { return kSunI * Transmit(OpticalToSun(p, sunDir.xyz).xy); }

// Light scattered more than once in the air: an even, sky-blue fill that outlasts the direct beam at dusk (the idea of Hillaire 2020's
// isotropic multiple scattering term, reduced to a constant). Single scattering alone turns the dusk zenith olive.
float3 AirMultiScatter(float odR, float3 trans) {
    return kBetaR * odR * kSunI * 0.016 * smoothstep(-0.18, 0.08, sunDir.y) * lerp(1.0, trans, 0.5);
}

// In-scattered light over `dist` metres, and the transmittance over it.
float3 Scatter(float3 o, float3 d, float dist, int steps, out float3 trans) {
    float mu = dot(d, sunDir.xyz), g = 0.76;
    float pr = 3.0 / (16.0 * kPI) * (1.0 + mu * mu);
    float pm = 3.0 / (8.0 * kPI) * (1.0 - g * g) * (1.0 + mu * mu) / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    float step = dist / steps;
    float2 od = 0;
    float3 sumR = 0, sumM = 0;
    for (int i = 0; i < steps; ++i) {
        float3 p = o + d * (i + 0.5) * step;
        float h = length(p) - kEarth;
        float2 rho = exp(-max(h, 0.0) / float2(8000.0, 1200.0)) * step;
        od += rho;
        float3 t = Transmit(od + OpticalToSun(p, sunDir.xyz).xy);
        sumR += t * rho.x;
        sumM += t * rho.y;
    }
    trans = Transmit(od);
    return kSunI * (sumR * kBetaR * pr + sumM * kBetaM * pm) + AirMultiScatter(od.x, trans);
}

// Scatter for the short paths to the ground: the sun's optical depth changes smoothly along them, so it is traced at the ends only.
float3 ScatterNear(float3 o, float3 d, float dist, out float3 trans) {
    float mu = dot(d, sunDir.xyz), g = 0.76;
    float pr = 3.0 / (16.0 * kPI) * (1.0 + mu * mu);
    float pm = 3.0 / (8.0 * kPI) * (1.0 - g * g) * (1.0 + mu * mu) / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    float step = dist / 6.0;
    float2 sun0 = OpticalToSun(o, sunDir.xyz).xy, sun1 = OpticalToSun(o + d * dist, sunDir.xyz).xy, od = 0;
    float3 sumR = 0, sumM = 0;
    for (int i = 0; i < 6; ++i) {
        float u = (i + 0.5) / 6.0;
        float h = length(o + d * u * dist) - kEarth;
        float2 rho = exp(-max(h, 0.0) / float2(8000.0, 1200.0)) * step;
        od += rho;
        float3 t = Transmit(od + lerp(sun0, sun1, u));
        sumR += t * rho.x;
        sumM += t * rho.y;
    }
    trans = Transmit(od);
    return kSunI * (sumR * kBetaR * pr + sumM * kBetaM * pm) + AirMultiScatter(od.x, trans);
}

float3 SkyRadiance(float3 d, out float3 trans) {
    float3 o = PlanetPos(eye.xyz);
    if (d.y < 0.0) { // unseen distant land under a fixed haze depth, so it meets the hazed terrain's far edge
        float3 inS = Scatter(o, normalize(float3(d.x, 0.0, d.z)), 30000.0, 8, trans);
        float3 land = float3(0.05, 0.07, 0.035) * (saturate(sunDir.y) * SunLight(o) * 0.35 + 0.08);
        trans = 0.0; // no sun disc through the ground
        return land * exp(-(kBetaR + kBetaM) * 30000.0) + inS;
    }
    return Scatter(o, d, SphereFar(o, d, kTop), 16, trans) + float3(0.002, 0.003, 0.006);
}

// ------------------------------------------------------------------ ambient occlusion
// Ground truth ambient occlusion (Jimenez, Wu, Pesce, Jarabo, "Practical Realtime Strategies for Accurate Indirect Occlusion", 2016):
// horizons found both ways along screen slices, and the cosine-weighted arc between them integrated exactly.
float FastAcos(float x) { // Abramowitz and Stegun's fit; the slices' angles need no better
    float a = abs(x), r = sqrt(max(1.0 - a, 0.0)) * (1.5707963 + a * (-0.2121144 + a * (0.0742610 - a * 0.0187293)));
    return x < 0.0 ? 3.14159265 - r : r;
}
float3 AoPos(int2 px, int2 last, out float d) {
    px = clamp(px, int2(0, 0), last);
    d = gDepth.Load(int3(px, 0));
    return WorldAt((float2(px) + 0.5) * res.zw, max(d, 1e-6));
}

// Half resolution: each pixel reads the depth of the full-res texel AoUpsample compares against. The normal comes from the nearer
// neighbour on each axis, so a depth edge does not bend it.
#ifdef RT_AO
// The ray traced variant (renderer.h RayTracingSupported; compiled to DXIL / SPIR-V with RT_AO): real rays into the static scene
// from the surface, so what stands on or beside it (a barrel on the floor, a shelf by the wall) shades it as far as kReach, and
// not only creases the screen holds. Cosine-weighted, nearest hit weighted by distance; jittered per pixel, TAA averages.
BIND(22) RaytracingAccelerationStructure gRayScene : register(t20);
float RayVisibility(float3 p, float3 n, float2 pixel, float dist) {
    // main.cpp lists only parts within 60 m of the eye (kRayReach), so farther pixels would trace into a half empty scene: they keep
    // the screen-space result. Past 20 m a 2 m reach covers a few pixels, where 4 rays averaged by TAA look the same as 8.
    if (dist > 55.0) return 1.0;
    const int kRays = dist > 20.0 ? 4 : 8;
    const float kReach = 2.0;
    float3 t = abs(n.y) < 0.9 ? normalize(cross(float3(0, 1, 0), n)) : normalize(cross(float3(1, 0, 0), n)), b = cross(n, t);
    float noise = Ign(pixel), noise2 = frac(noise * 13.7 + 0.31), occluded = 0.0;
    for (int k = 0; k < kRays; ++k) {
        float2 u = frac(float2(noise, noise2) + k * float2(0.7548777, 0.5698403)); // R2 sequence
        float r = sqrt(u.x), phi = 6.2831853 * u.y;
        RayDesc ray;
        ray.Origin = p + n * 0.03;
        ray.Direction = t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(saturate(1.0 - u.x));
        ray.TMin = 0.0;
        ray.TMax = kReach;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(gRayScene, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            float f = q.CommittedRayT() / kReach;
            occluded += 1.0 - f * f;
        }
    }
    return lerp(1.0 - occluded / kRays, 1.0, saturate((dist - 45.0) / 10.0)); // no ring where the rays stop
}

// How much of the windows a point in a room sees: shadow rays to random points on the nearest panes, each pane weighted by the
// light it sends (solid angle times cosine, as WindowSkyLight). Indoors the windows bring nearly all the light, so this soft
// shadow of the barrels, shelves and posts is what scales it. 1 outside rooms and where no pane faces the point.
float WindowVisibility(float3 p, float3 n, float2 pixel) {
    if (portalCount.y < 0.5 || ProbeRoomOf(p, 0.3) < 0) return 1.0;
    float noise = Ign(pixel + 17.0), noise2 = frac(noise * 7.3 + 0.61);
    float seen = 0.0, total = 0.0;
    int traced = 0;
    for (int k = 0; k < int(portalCount.x) && traced < 6; ++k) {
        float3 c = portals[k * 3].xyz, u = portals[k * 3 + 1].xyz, v = portals[k * 3 + 2].xyz, toC = c - p;
        float d2 = dot(toC, toC);
        if (d2 > 144.0) continue;
        float3 pn = normalize(cross(u, v));
        float w = saturate(dot(n, toC) * rsqrt(d2)) * abs(dot(pn, toC)) * rsqrt(d2) * length(cross(u, v)) * 4.0 / max(d2, 0.25);
        if (w < 1e-4) continue;
        for (int s = 0; s < 2; ++s) {
            float2 r = frac(float2(noise, noise2) + (traced * 2 + s) * float2(0.7548777, 0.5698403)) * 2.0 - 1.0;
            float3 target = c + u * r.x * 0.9 + v * r.y * 0.9, to = target - p;
            float len = length(to);
            RayDesc ray;
            ray.Origin = p + n * 0.03;
            ray.Direction = to / len;
            ray.TMin = 0.0;
            ray.TMax = max(len - 0.1, 0.0); // stop short of the frame
            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
            q.TraceRayInline(gRayScene, RAY_FLAG_NONE, 0xFF, ray);
            q.Proceed();
            seen += w * (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0);
            total += w;
        }
        ++traced;
    }
    return total > 0.0 ? seen / total : 1.0;
}
#endif

float AoPS(FsOut i) : SV_Target {
    int2 full = int2(i.pos.xy) * 2, last = int2(res.xy) - 1;
    float depth = gDepth.Load(int3(full, 0));
    if (depth <= 0.0) return 1.0;
    float2 uv0 = (float2(full) + 0.5) * res.zw;
    float3 p = WorldAt(uv0, depth);
    float dl, dr, du, dd;
    float3 pl = AoPos(full - int2(2, 0), last, dl), pr = AoPos(full + int2(2, 0), last, dr), pu = AoPos(full - int2(0, 2), last, du), pd = AoPos(full + int2(0, 2), last, dd);
    float3 tx = abs(dl - depth) < abs(dr - depth) ? p - pl : pr - p;
    float3 ty = abs(du - depth) < abs(dd - depth) ? p - pu : pd - p;
    float3 n = normalize(cross(ty, tx));
    float3 toEye = eye.xyz - p;
    float dist = length(toEye);
    float3 V = toEye / dist;
    if (dot(n, V) < 0.0) n = -n;
    float radius = clamp(dist * 0.05, 0.25, CityAirOn(misc.z) ? 14.0 : 5.0); // the city camera stands far off: a building must still darken the street at its foot
    float screen = radius / max(dist, 0.1) * 0.8; // the screen radius is approximate: enough for a horizon search
    float noise = Ign(i.pos.xy), jitter = frac(noise * 7.0 + 0.37);
    float vis = 0.0, weights = 0.0;
    const int kSlices = 2, kSteps = 3;
    for (int s = 0; s < kSlices; ++s) {
        float phi = (s + noise) * 3.14159265 / kSlices;
        float2 dirUv = float2(cos(phi) * res.y * res.z, sin(phi));
        float3 side = WorldAt(uv0 + dirUv * res.w, depth) - p;
        float3 axis = normalize(cross(side, V));
        float3 sliceDir = cross(V, axis);
        float3 projN = n - axis * dot(n, axis);
        float w = length(projN);
        if (w < 1e-3) continue;
        float cosN = saturate(dot(projN / w, V));
        float angN = (dot(projN, sliceDir) >= 0.0 ? 1.0 : -1.0) * FastAcos(cosN);
        float slice = 0.0;
        for (int sd = 0; sd < 2; ++sd) {
            float sgn = sd == 0 ? 1.0 : -1.0;
            float best = cos(angN + sgn * 1.5707963); // the hemisphere's edge on this side
            const float floorCos = best;
            for (int k = 0; k < kSteps; ++k) {
                float t = (k + jitter) / kSteps;
                float2 uv = uv0 + sgn * dirUv * t * t * screen;
                float d = gDepth.SampleLevel(gClamp, uv, 0);
                if (d <= 0.0) continue;
                float3 v = WorldAt(uv, d) - p;
                float l2 = dot(v, v);
                float c = dot(v, V) / sqrt(l2 + 1e-5);
                float fall = saturate(1.0 - l2 / (radius * radius * 2.0));
                best = max(best, lerp(floorCos, c, fall * fall));
            }
            float h = clamp(sgn * FastAcos(best), angN - 1.5707963, angN + 1.5707963);
            slice += 0.25 * (cosN + 2.0 * h * sin(angN) - cos(2.0 * h - angN));
        }
        vis += slice * w;
        weights += w;
    }
    float ao = weights > 0.0 ? saturate(vis / weights) : 1.0;
#ifdef RT_AO
    ao = min(ao, RayVisibility(p, n, i.pos.xy, dist)) * WindowVisibility(p, n, i.pos.xy);
#endif
    return ao;
}

// Back to full resolution: bilinear, weighted down where a half-res texel sits at another depth (reversed Z: compared as a ratio).
float AoUpsample(float2 pixel, float depth) {
    float2 pos = pixel * 0.5 - 0.5, b = floor(pos), f = pos - b;
    int2 last = int2(res.xy) / 2 - 1;
    float sum = 0.0, total = 0.0;
    for (int k = 0; k < 4; ++k) {
        int2 o = int2(k & 1, k >> 1), q = clamp(int2(b) + o, int2(0, 0), last);
        float2 t = lerp(1.0 - f, f, float2(o));
        float w = (t.x * t.y + 1e-3) / (0.01 + abs(gDepth.Load(int3(q * 2, 0)) - depth) / max(depth, 1e-6));
        sum += w * gAo.Load(int3(q, 0));
        total += w;
    }
    return sum / total;
}

// ------------------------------------------------------------------ clouds
// After Andrew Schneider's SIGGRAPH Advances talks: "The Real-time Volumetric Cloudscapes of Horizon: Zero Dawn" (2015), "Nubis:
// Authoring Real-time Volumetric Cloudscapes with the Decima Engine" (2017), "Nubis, Evolved" (2022): 2D data fields, a vertical
// profile by type, a noise composite from wispy to billowy, erosion by the inverse profile, light from the profile. Wrenninge et al.,
// "Oz: The Great and Volumetric" (2013), for the octaves of multiple scattering. Own implementation; constants set by eye.
float Remap(float v, float a, float b, float c, float d) { return c + (v - a) / (b - a) * (d - c); }

struct CloudSample { float density, profile, height; };
static float sCloudDetail = 1.0; // 1 near, 0 far: the noise has no mips, and fine erosion far away is only aliasing

CloudSample CloudAt(float3 p, bool detail) {
    CloudSample s;
    s.density = 0.0; s.profile = 0.0; s.height = 0.0;
    float alt = length(p) - kEarth;
    float3 w = float3(p.x, p.y - kEarth, p.z);
    float3 wind = float3(misc.x * 12.0, 0, misc.x * 4.0);
    float4 f = gCloudField.SampleLevel(gWrap, (w.xz + wind.xz * 0.5) / kFieldTile, 0);
    if (alt >= kWispLo) { // high wisps: a thin sheet, stretched along the wind
        float hh = saturate((alt - kWispLo) / (kWispHi - kWispLo));
        s.height = hh;
        s.profile = f.a * saturate(hh / 0.3) * saturate((1.0 - hh) / 0.4);
        if (s.profile <= 0.0) return s;
        float n = gNoise.SampleLevel(gWrap, (w + wind * 2.0) * float3(1.0 / 21000.0, 1.0 / 3000.0, 1.0 / 7000.0), 0).r;
        s.density = 0.22 * saturate(Remap(n, 1.0 - s.profile * 0.75, 1.0, 0.0, 1.0));
        if (detail && s.density > 0.0) {
            float e = gNoise.SampleLevel(gWrap, (w + wind * 2.4) * float3(1.0 / 5000.0, 1.0 / 900.0, 1.0 / 1800.0), 0).b;
            s.density = saturate(s.density - e * 0.12);
        }
        return s;
    }
    float top = lerp(kCloudLo + 450.0, kCloudHi, f.b * lerp(0.35, 1.0, f.g)); // stratus stays low; cumulus reaches the field's height
    float hh = (alt - kCloudLo) / (top - kCloudLo);
    if (hh <= 0.0 || hh >= 1.0) return s;
    s.height = hh;
    // The vertical profile by type: flat and thin to round, with a firm base under cumulus.
    float type = f.g;
    float profile = saturate(hh / lerp(0.18, 0.07, type)) * saturate((1.0 - hh) / lerp(0.75, 0.55, type));
    s.profile = f.r * profile;
    if (s.profile <= 0.0) return s;
    float4 n = gNoise.SampleLevel(gWrap, (w + wind) / 8000.0, 0);
    float wispy = n.r, billowy = n.g * 0.75 + n.b * 0.25;
    float composite = lerp(wispy, billowy, type);
    // Erosion: the noise cut by the inverse of the profile.
    s.density = saturate(Remap(composite, 1.0 - s.profile * 0.82, 1.0, 0.0, 1.0));
    if (detail && s.density > 0.0) {
        float e1 = gNoise.SampleLevel(gWrap, (w + wind * 1.2) / 2200.0, 0).g;
        float e2 = gNoise.SampleLevel(gWrap, (w + wind * 1.6) / 560.0, 0).b;
        e2 = lerp(0.5, e2, sCloudDetail);
        float e = lerp(e2, e1 * 0.72 + e2 * 0.28, type); // wisps at the fine scale, billows at the coarse one
        s.density = saturate(Remap(s.density, (1.0 - e) * lerp(0.45, 0.55, saturate(hh * 2.0)), 1.0, 0.0, 1.0));
    }
    return s;
}

float CloudDensity(float3 p, bool detail) { CloudSample s = CloudAt(p, detail); return s.density; }

float Hg(float mu, float g) { return (1.0 - g * g) / (4.0 * kPI * pow(abs(1.0 + g * g - 2.0 * g * mu), 1.5)); }

// What one density sample scatters toward the eye: direct (octaves of Beer and phase, the extra ones scaled by how deep in the
// profile we are), plus ambient from the sky, stronger where the inverse profile says the point is near the outside.
float3 CloudLight(float3 p, CloudSample s, float mu, float3 sun, float3 ambient) {
    const float sigma = kCloudSigma;
    float od = 0.0;
    for (int k = 1; k <= 5; ++k) od += CloudDensity(p + sunDir.xyz * k * k * 45.0, false) * (2 * k - 1) * 45.0;
    od *= sigma;
    float deep = saturate(s.profile), direct = 0.0, a = 1.0, b = 1.0, c = 1.0;
    float octaveShare = lerp(0.35, 0.65, deep);
    for (int k = 0; k < 3; ++k) {
        float phase = 0.55 * Hg(mu, 0.8 * c) + 0.3 * Hg(mu, 0.25 * c) + 0.15 * Hg(mu, -0.3 * c);
        direct += a * exp(-od * b) * phase;
        a *= octaveShare; b *= 0.35; c *= 0.5;
    }
    float powder = 1.0 - exp(-od * 2.0 - s.density * 2.0 - 0.05);
    float amb = (0.45 + 0.55 * s.height) * lerp(1.0, 0.55, deep);
    return sun * direct * lerp(0.55, 1.0, powder) * 4.0 * kPI * 0.2 + ambient * amb;
}

int2 CloudPhase() { // which of each 2x2 of the half-res target is marched this frame
    int f = int(misc.y) & 3;
    return f == 0 ? int2(0, 0) : f == 1 ? int2(1, 1) : f == 2 ? int2(1, 0) : int2(0, 1);
}

// A quarter of the half-res pixels each frame, one in each 2x2; CloudResolvePS rebuilds the rest from history.
float4 CloudsPS(FsOut i) : SV_Target {
    float2 half0 = floor(res.xy * 0.5);
    float2 uv = (floor(i.pos.xy) * 2.0 + float2(CloudPhase()) + 0.5) / half0;
    int2 full = int2(uv * res.xy), last = int2(res.xy) - 1;
    float sky = 0.0; // behind solid ground on every side of the block: nothing will read it
    for (int k = 0; k < 5; ++k) {
        int2 off = k == 0 ? int2(0, 0) : int2((k & 1) * 8 - 4, (k & 2) * 4 - 4);
        if (gDepth.Load(int3(clamp(full + off, int2(0, 0), last), 0)) <= 0.0) sky = 1.0;
    }
    if (sky < 0.5) return float4(0, 0, 0, 1);
    float3 d = normalize(WorldAt(uv, 0.5) - eye.xyz);
    float3 o = PlanetPos(eye.xyz);
    if (d.y < 0.01) return float4(0, 0, 0, 1);
    float mu = dot(d, sunDir.xyz), jitter = Ign(i.pos.xy);
    float3 ambient = skySh[0].rgb * 0.6;
    float3 light = 0;
    float T = 1.0;
    const float sigma = kCloudSigma;
    for (int shell = 0; shell < 2 && T > 0.01; ++shell) {
        float lo = shell == 0 ? kCloudLo : kWispLo, hi = shell == 0 ? kCloudHi : kWispHi;
        float t0 = SphereFar(o, d, kEarth + lo), t1 = min(SphereFar(o, d, kEarth + hi), t0 + 30000.0);
        float3 sun = SunLight(o + d * t0);
        // Adaptive steps: longer with distance, and the cheap density alone through empty air (Schneider 2015). Doubling the step there left holes in thin clouds.
        sCloudDetail = saturate(1.3 - t0 / 18000.0);
        float dt = (t1 - t0) / (shell == 0 ? 36.0 : 5.0);
        dt *= lerp(1.0, 1.6, saturate(t0 / 30000.0));
        float t = t0 + dt * jitter;
        int empty = 8;
        int maxSteps = shell == 0 ? 56 : 7;
        for (int k = 0; k < maxSteps && t < t1 && T > 0.01; ++k) {
            float3 p = o + d * t;
            if (empty >= 4) {
                CloudSample c = CloudAt(p, false);
                if (c.density <= 0.0) { t += dt; continue; }
                empty = 0;
                t = max(t - dt, t0);
                p = o + d * t;
            }
            CloudSample s = CloudAt(p, true);
            if (s.density <= 0.001) { ++empty; t += dt; continue; }
            empty = 0;
            float tr = exp(-sigma * s.density * dt);
            light += T * CloudLight(p, s, mu, sun, ambient) * (1.0 - tr); // the energy-conserving form: independent of the step length
            T *= tr;
            t += dt;
        }
    }
    float fade = saturate((d.y - 0.01) * 12.0); // melt into the horizon's haze
    return float4(light * fade, lerp(1.0, T, fade));
}

// The half-res cloud image: the marched quarter where it is new, elsewhere last frame's, reprojected for the camera only (a point at
// sky depth through last frame's matrix) and held to the new neighbourhood. Schneider 2015 for the idea of the rolling update.
float4 CloudResolvePS(FsOut i) : SV_Target {
    int2 ph = int2(i.pos.xy), m = ph / 2, o = ph - m * 2, phase = CloudPhase();
    int2 size = max(int2(res.xy) / 4, int2(1, 1)), lastM = size - 1;
    float2 muv = ((float2(ph) - float2(phase)) * 0.5 + 0.5) / float2(size);
    float4 c = gCloudMarch.Load(int3(min(m, lastM), 0));
    float4 cur = all(o == phase) ? c : gCloudMarch.SampleLevel(gClamp, muv, 0);
    if (misc.w < 0.5) return cur;
    float4 prev = mul(prevViewProj, float4(WorldAt(i.uv, 1e-5), 1.0));
    float2 puv = float2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
    if (any(puv < 0.0) || any(puv > 1.0)) return cur;
    float4 lo = c, hi = c;
    for (int k = 0; k < 4; ++k) {
        int2 off = k == 0 ? int2(1, 0) : k == 1 ? int2(-1, 0) : k == 2 ? int2(0, 1) : int2(0, -1);
        float4 n = gCloudMarch.Load(int3(clamp(m + off, int2(0, 0), lastM), 0));
        lo = min(lo, n); hi = max(hi, n);
    }
    float4 margin = (hi - lo) * 0.25 + 0.01; // a tight box pins history to single noisy rays, which shows as holes
    float4 h = clamp(gCloudHistory.SampleLevel(gClamp, puv, 0), lo - margin, hi + margin);
    return lerp(h, cur, all(o == phase) ? 0.2 : 0.02);
}

// How much sun gets through the layer to each ground point, from above: the same CloudAt, wind and sigma as the visible clouds,
// along the same sun ray, through both shells. Beer's law, with a forward-scattered share that keeps the thin parts soft.
float4 CloudShadowPS(FsOut i) : SV_Target {
    float2 xz = cloudMap.xy + (i.uv - 0.5) * 2.0 * cloudMap.z;
    float3 o = PlanetPos(float3(xz.x, eye.y, xz.y));
    float3 l = sunDir.xyz;
    float od = 0.0;
    for (int shell = 0; shell < 2; ++shell) {
        float lo = shell == 0 ? kCloudLo : kWispLo, hi = shell == 0 ? kCloudHi : kWispHi;
        float t0 = SphereFar(o, l, kEarth + lo), t1 = min(SphereFar(o, l, kEarth + hi), t0 + 30000.0); // a low sun: long shadows, capped as the march caps
        int steps = shell == 0 ? 6 : 2;
        float step = (t1 - t0) / steps;
        for (int k = 0; k < steps; ++k) od += CloudDensity(o + l * (t0 + (k + 0.5) * step), false) * step;
    }
    od *= kCloudSigma;
    return float4(exp(-od) * 0.8 + exp(-od * 0.2) * 0.2, 0, 0, 1);
}
float CloudSunAt(float3 world) {
    if (cloudMap.w < 0.5) return 1.0;
    float2 uv = (world.xz - cloudMap.xy) / (2.0 * cloudMap.z) + 0.5;
    return gCloudShadow.SampleLevel(gClamp, uv, 0).r;
}

// ------------------------------------------------------------------ small-scale light
// Skin: a stochastic disc of Burley-style falloffs, wider in red (after Golubev, "Efficient Screen-Space Subsurface
// Scattering Using Burley's Normalized Diffusion"). TAA averages the noise. Only skin pixels pay for it.
// Only the light that went under the skin spreads: each pixel's surface reflection (the motion target's z, from LitSkin) is
// taken out before the blur and the centre's is added back sharp, so pores and the oily sheen keep breaking up the highlight.
// The reflection is stored as luminance and comes back white; a dielectric's highlight is the light's colour, near white here.
float3 SkinDiffuse(float3 lit, float specular) {
    const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
    const float l = max(dot(lit, kLuma), 1e-4);
    return lit * saturate(1.0 - specular / l); // keeps the diffuse's own colour; never below zero where the highlight is most of it
}
float3 ScatterSkin(float2 uv, float2 pixel, float3 center, float3 p, float eyeDist) {
    static const float3 kScatter = float3(0.0024, 0.0008, 0.0005); // metres
    const float kMax = 0.008;
    const float centerSpec = max(gVelocity.Load(int3(pixel, 0)).z, 0.0);
    const float3 centerDiffuse = SkinDiffuse(center, centerSpec);
    const float3 highlight = center - centerDiffuse;
    if (lightDebug.x > 8.5 && lightDebug.x < 9.5) return highlight; // skin's surface reflection alone, for tuning the shine
    float metresPerPixel = length(WorldAt(uv + float2(res.z, 0), gDepth.SampleLevel(gClamp, uv, 0)) - p);
    float pixels = kMax / max(metresPerPixel, 1e-5);
    if (pixels < 1.0) return center;
    float angle = Ign(pixel) * 6.2831853;
    float3 sum = centerDiffuse * 3.0, total = 3.0;
    for (int k = 0; k < 16; ++k) {
        float t = (k + 0.5) / 16.0, a = angle + k * 2.399963, r = t * t * kMax;
        float2 suv = uv + float2(cos(a), sin(a)) * t * t * pixels * res.zw;
        float4 s = gScene.SampleLevel(gClamp, suv, 0);
        if (abs(s.a) > 0.75) continue; // not skin
        float gap = abs(length(WorldAt(suv, gDepth.SampleLevel(gClamp, suv, 0)) - eye.xyz) - eyeDist);
        float3 w = (exp(-r / kScatter) + exp(-r / (3.0 * kScatter))) * saturate(1.0 - gap / 0.012) * t; // t: area of the ring
        sum += SkinDiffuse(s.rgb, max(gVelocity.SampleLevel(gClamp, suv, 0).z, 0.0)) * w;
        total += w;
    }
    // The scene holds lit color, albedo included: keep most of the original brightness detail (stubble, brows, the colour of
    // pore openings) and let the blur carry the color bleed.
    float3 blur = sum / total;
    const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
    return blur * pow(max(dot(centerDiffuse, kLuma), 1e-4) / max(dot(blur, kLuma), 1e-4), 0.75) + highlight;
}

// Shadows smaller than a shadow map texel: march toward the sun through the depth buffer. Positions project with last
// frame's matrix and only the difference is used, so camera motion and jitter cancel. Returns 1 = occluded.
float ContactShadow(float2 uv, float2 pixel, float3 p, float eyeDist) {
    if (sunDir.y <= 0.0 || eyeDist > 40.0) return 0.0;
    float reach = clamp(eyeDist * 0.08, 0.03, 0.5);
    float4 c0 = mul(prevViewProj, float4(p, 1.0));
    float2 ndc0 = c0.xy / c0.w;
    float jitter = Ign(pixel);
    float hit = 0.0;
    for (int k = 0; k < 12; ++k) {
        float t = (k + jitter) / 12.0;
        float3 q = p + sunDir.xyz * t * t * reach;
        float4 c = mul(prevViewProj, float4(q, 1.0));
        float2 suv = uv + (c.xy / c.w - ndc0) * float2(0.5, -0.5);
        if (any(suv < 0.0) || any(suv > 1.0)) break;
        float d = gDepth.SampleLevel(gClamp, suv, 0);
        if (d <= 0.0) continue;
        float front = length(eye.xyz - q) - length(eye.xyz - WorldAt(suv, d));
        float bias = 0.0015 + eyeDist * 0.0015, thick = 0.03 + eyeDist * 0.02;
        if (front > bias && front < thick) hit = max(hit, 1.0 - t * t);
    }
    return hit * saturate((40.0 - eyeDist) / 10.0);
}

// Screen-space reflection on smooth surfaces (they say so with a negative scene alpha): a world-space march through the depth buffer
// (McGuire and Mara, "Efficient GPU Screen-Space Ray Tracing", 2014, without the depth pyramid). Rough things keep the sky and probes.
float3 ScreenReflection(float2 uv, float2 pixel, float3 p, float3 d, float3 n, float eyeDist, float3 c) {
    float3 r = reflect(d, n);
    float nv = saturate(dot(-d, n));
    float F = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
    float4 c0 = mul(prevViewProj, float4(p, 1.0));
    float2 ndc0 = c0.xy / c0.w;
    float t = 0.05 + 0.1 * Ign(pixel), stepLen = 0.12 + eyeDist * 0.006;
    for (int k = 0; k < 16; ++k, t += stepLen, stepLen *= 1.22) {
        float3 q = p + r * t;
        float4 h = mul(prevViewProj, float4(q, 1.0));
        if (h.w <= 0.0) break;
        float2 suv = uv + (h.xy / h.w - ndc0) * float2(0.5, -0.5);
        if (any(suv < 0.0) || any(suv > 1.0)) break;
        float z = gDepth.SampleLevel(gClamp, suv, 0);
        if (z <= 0.0) continue;
        float front = length(eye.xyz - q) - length(eye.xyz - WorldAt(suv, z));
        if (front > 0.0 && front < stepLen * 2.0 + 0.1) {
            float2 e = saturate(min(suv, 1.0 - suv) * 12.0);
            float3 sky = lerp(skyGrad[1].rgb, skyGrad[0].rgb, sqrt(saturate(r.y)));
            sky = lerp(skyGrad[2].rgb, sky, smoothstep(-0.03, 0.03, r.y));
            float3 seen = min(gScene.SampleLevel(gClamp, suv, 0).rgb, 16.0);
            return max(c + (seen - sky) * F * e.x * e.y, 0.0); // the lit shader already added the sky here
        }
    }
    return c;
}

// Dust near the ground, lit where the light is: density falls off with height (Quilez, "Better Fog"; integral of an exponential),
// and the colour comes from the same irradiance probes as the surfaces, sun blocked at probes in shadow (after Wronski 2014 and
// Hillaire 2015 in idea, not froxels: a few jittered samples per pixel that TAA averages).
float3 GroundHaze(float3 c, float3 d, float dist, float2 pixel) {
    if (lightDebug.x > 3.5 && lightDebug.x < 4.5) c = 0.0;
    const float kDensity = 0.00016, kFalloff = 1.0 / 45.0;
    float groundY = probeOrigin.y * probeGrid.w + 3.0 * probeGrid.w - 3.0;
    float h0 = max(eye.y - groundY, 0.0), dy = d.y * kFalloff * dist;
    float od = kDensity * exp(-h0 * kFalloff) * dist * (abs(dy) > 1e-3 ? (1.0 - exp(-dy)) / dy : 1.0);
    float T = exp(-od);
    float mu = dot(d, sunDir.xyz), g = 0.6;
    float hg = (1.0 - g * g) / (4.0 * kPI * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    float3 sunL = sunColorP.rgb * 3.2 * kPI * hg;
    float3 sum = 0.0;
    float jitter = Ign(pixel), cloudAtEye = CloudSunAt(eye.xyz + d * min(dist, 3000.0) * 0.5);
    const int kSamples = 4;
    for (int k = 0; k < kSamples; ++k) {
        float u = (k + jitter) / kSamples, t = dist * u * u;
        float3 mean, fadeMean = skySh[0].rgb;
        float vis, fade;
        if (probeUpdate.w > 0.5) ProbeHaze(eye.xyz + d * t, mean, vis, fade);
        else { mean = fadeMean; vis = 1.0; fade = 0.0; }
        sum += lerp(fadeMean, mean, fade) + sunL * lerp(1.0, vis, fade) * cloudAtEye;
    }
    return c * T + sum / kSamples * (1.0 - T);
}

// Dust hanging in a building's rooms (InteriorRoom in renderer.h), lit only where the sun reaches it: the view ray's stretch inside
// each room is sampled against the sun's shadow map, and the roof and walls leave lit only what came through a window, so the
// shafts take the panes' shape. Forward scattering makes them strongest looking toward the sun. Jittered per pixel, TAA averages.
float3 RoomShafts(float3 c, float3 d, float dist, float2 pixel) {
    const float kDust = 0.06, kGlow = 2.5; // per metre: a dry, dusty store; and how bright a lit mote reads, set by eye
    float2 size;
    gShadow.GetDimensions(size.x, size.y);
    float range = 1.0 / length(shadowVP[2].xyz);
    float mu = dot(d, sunDir.xyz), g = 0.7;
    float hg = (1.0 - g * g) / (4.0 * kPI * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    float3 sunL = sunColorP.rgb * 3.2 * kPI * hg * kGlow;
    float jitter = Ign(pixel);
    for (int k = 0; k < int(portalCount.y); ++k) {
        float3 rc = rooms[k * 3].xyz, ax = rooms[k * 3 + 1].xyz, az = rooms[k * 3 + 2].xyz;
        float ex = length(ax), ez = length(az), hy = rooms[k * 3].w;
        float3 o = eye.xyz - rc, l = float3(dot(o, ax) / (ex * ex), o.y / hy, dot(o, az) / (ez * ez));
        // Port Lazare's exported facade meshes can produce oversized inferred rooms.  Outside
        // the room, their dust volumes leak into streets and alleys as bright slabs of haze.
        // Keep the indoor shafts, but do not render those inferred volumes from outdoors.
        float2 portOffset = rc.xz - float2(4420.0, 2880.0);
        if (dot(portOffset, portOffset) < 700.0 * 700.0 && any(abs(l) >= 1.0)) continue;
        float3 dl = float3(dot(d, ax) / (ex * ex), d.y / hy, dot(d, az) / (ez * ez));
        float3 inv = 1.0 / (((float3)(dl >= 0.0) * 2.0 - 1.0) * max(abs(dl), 1e-6)), ta = (-1.0 - l) * inv, tb = (1.0 - l) * inv;
        float t0 = max(max(min(ta.x, tb.x), min(ta.y, tb.y)), max(min(ta.z, tb.z), 0.0));
        float t1 = min(min(max(ta.x, tb.x), max(ta.y, tb.y)), min(max(ta.z, tb.z), dist));
        if (t1 <= t0) continue;
        const int kSteps = 12;
        float step = (t1 - t0) / kSteps, lit = 0.0;
        for (int s = 0; s < kSteps; ++s) {
            float3 q = eye.xyz + d * (t0 + (s + jitter) * step);
            float3 sp = mul(shadowVP, float4(q, 1.0)).xyz;
            float2 uv = float2(sp.x * 0.5 + 0.5, 0.5 - sp.y * 0.5); // ShadowUv, defined further down
            if (all(uv > 0.0) && all(uv < 1.0) && (sp.z - gShadow.Load(int3(uv * size, 0))) * range < 0.05) lit += CloudSunAt(q);
        }
        float T = exp(-kDust * (t1 - t0));
        c = c * T + sunL * (lit / kSteps) * (1.0 - T);
    }
    return c;
}

// ------------------------------------------------------------------ the city's air (misc.z = 0.5; city_air.hlsli, docs/cs2_look_pass.md)
// In place of the marched sky, clouds and probe haze on the city map, at every preset.
int PostTier() { return motion.w > 0.5 ? int(motion.w) - 1 : 4; } // FrameView::postTier: 0 Low .. 4 Ultra / PC (unset reads as PC)
bool CityLook() { return CityAirOn(misc.z); }
// Sun shadows past the shadow map's reach (60–120 m round the camera's target) and from the baked far buildings, which cast none:
// march the depth buffer from the pixel toward the sun, as ContactShadow does up close but metres to a hundred and fifty long, so a
// tower throws its shadow across the street at any zoom (after Bend Studio's screen-space shadows, "Days Gone", 2020, in idea).
// Steps by preset: Low 8, Low/Med and Medium 12, High and Ultra 16; jittered per pixel and frame, TAA averages the steps.
float CitySunShadow(float2 uv, float2 pixel, float3 p, float dist) {
    if (sunDir.y <= 0.03) return 0.0;
    const int steps = PostTier() == 0 ? 8 : PostTier() < 3 ? 12 : 16;
    const float reach = clamp(dist * 0.14, 3.0, 150.0) * saturate(0.35 / max(sunDir.y, 0.05) + 0.4); // longer as the sun sinks
    const float4 c0 = mul(prevViewProj, float4(p, 1.0));
    const float2 ndc0 = c0.xy / c0.w;
    const float jitter = Ign(pixel);
    const float bias = 0.05 + dist * 0.002, thick = 2.0 + reach * 0.35;
    float hit = 0.0;
    for (int k = 0; k < 16; ++k) {
        if (k >= steps) break;
        const float t = (k + jitter) / steps;
        const float3 q = p + sunDir.xyz * (t * t * reach + bias);
        const float4 c = mul(prevViewProj, float4(q, 1.0));
        const float2 suv = uv + (c.xy / c.w - ndc0) * float2(0.5, -0.5);
        if (any(suv < 0.0) || any(suv > 1.0)) break;
        const float d = gDepth.SampleLevel(gClamp, suv, 0);
        if (d <= 0.0) continue;
        const float front = length(eye.xyz - q) - length(eye.xyz - WorldAt(suv, d));
        if (front > bias && front < thick) { hit = 1.0 - t * t * 0.5; break; } // nearer casters give darker, harder shadow
    }
    const float2 e = saturate(min(uv, 1.0 - uv) * 16.0); // rays toward the screen's edge find nothing: fade instead of a line
    return hit * e.x * e.y;
}
float3 CityComposite(float3 c, float sa, float2 uv, float2 pixel, float depth) {
    float3 world = WorldAt(uv, max(depth, 1e-5));
    float3 d = normalize(world - eye.xyz);
    if (depth <= 0.0) {
        float disc = smoothstep(0.99990, 0.99996, dot(d, sunDir.xyz)) * smoothstep(-0.02, 0.01, d.y);
        return CitySkyOf(d, skyGrad[0].rgb, skyGrad[1].rgb, sunColorP.rgb, sunDir.xyz) + disc * sunColorP.rgb * 60.0;
    }
    // Occlusion dims the ambient share only (see CompositePS; Low has no AO pass), the screen-space sun shadow the sunlit share only:
    // a pixel the shadow map already shades has almost no sunlit share, so the two never double up.
    sa = abs(sa);
    const float share = sa > 0.995 ? 1.0 : saturate((sa - 0.76) / 0.23);
    const float dist = length(world - eye.xyz);
    const float ao = PostTier() > 0 ? AoUpsample(pixel, depth) : 1.0;
    c *= share * ao + (1.0 - share) * (1.0 - 0.92 * CitySunShadow(uv, pixel, world, dist));
    return lerp(CityHazeOf(d, skyGrad[1].rgb, sunColorP.rgb, sunDir.xyz), c, CityAirKeep(eye.xyz, world));
}

// ------------------------------------------------------------------ sky, clouds and haze over the lit scene
float4 CompositePS(FsOut i) : SV_Target {
    float3 c = gScene.SampleLevel(gClamp, i.uv, 0).rgb;
    float depth = gDepth.SampleLevel(gClamp, i.uv, 0);
    if (lightDebug.x > 5.5 && lightDebug.x < 7.5) return float4(gCloudShadow.SampleLevel(gClamp, i.uv, 0).rrr, 1); // the whole cloud shadow map (north up is -z), 30 km across
    if (CityLook()) return float4(min(CityComposite(c, gScene.SampleLevel(gClamp, i.uv, 0).a, i.uv, i.pos.xy, depth), 2000.0), 1);
    if (misc.z < 0.5) return float4(c * (depth > 0.0 ? AoUpsample(i.pos.xy, depth) : 1.0), 1);
    float3 world = WorldAt(i.uv, max(depth, 1e-5));
    float3 d = normalize(world - eye.xyz);
    float3 trans;
    if (depth <= 0.0) {
        if (lightDebug.x > 7.5) return float4(0, 0, 0, 1); // surface light only: tbd_reference leaves the sky black
        c = SkyRadiance(d, trans);
        float disc = smoothstep(0.99990, 0.99996, dot(d, sunDir.xyz)) * smoothstep(-0.02, 0.01, d.y);
        c += disc * trans * 600.0;
        // Stars, hashed per direction cell; they fade out as the sky brightens.
        float3 cell = floor(d * 280.0);
        float star = frac(sin(dot(cell, float3(12.9898, 78.233, 37.719))) * 43758.5453);
        c += step(0.9975, star) * frac(star * 91.7) * 0.5 * saturate(-sunDir.y * 6.0) * saturate(d.y * 8.0);
        float4 cloud = gClouds.SampleLevel(gClamp, i.uv, 0);
        if (lightDebug.x > 6.5 && lightDebug.x < 7.5) return float4(cloud.aaa, 1); // how much of the sky the visible clouds let through, for checking CloudShadowPS against them
        c = c * cloud.a + cloud.rgb;
        if (portalCount.y > 0.5) c = RoomShafts(c, d, 1e4, i.pos.xy); // seen through a window from inside
    } else {
        float dist = length(world - eye.xyz);
        if (lightDebug.x > 4.5 && lightDebug.x < 7.5) return float4(CloudSunAt(world).xxx, 1); // the cloud shadow map on the ground
        float sa = gScene.SampleLevel(gClamp, i.uv, 0).a;
        if (sa < 0.0) {
            sa = -sa;
            float3 sn = normalize(cross(ddy(world), ddx(world)));
            if (dot(sn, d) > 0.0) sn = -sn;
            c = ScreenReflection(i.uv, i.pos.xy, world, d, sn, dist, c);
        }
        if (sa < 0.75) c = ScatterSkin(i.uv, i.pos.xy, c, world, dist);
        float ao = AoUpsample(i.pos.xy, depth);
        // Not on skin: marching a face's faceted depth tears hard patches into it, and the tight shadow map is sharp enough there.
        float contact = sa < 0.75 ? 1.0 : 1.0 - 0.8 * ContactShadow(i.uv, i.pos.xy, world, dist);
        if (sa > 0.995) c *= ao * contact; // eyes
        else { // occlusion dims only ambient, with bounces inside the crease on a desert albedo (Jimenez 2016); contact only the sun
            float share = sa < 0.75 ? (sa - 0.25) / 0.45 : (sa - 0.76) / 0.23; // skin keeps its share below 0.75
            const float3 rho = saturate(c / max(max(c.r, max(c.g, c.b)), 1e-4)) * 0.6; // the crease's own colour bounces back, about as bright as this desert's surfaces
            float3 ma = 2.0404 * rho - 0.3324, mb = -4.7951 * rho + 0.6417, mc = 2.7552 * rho + 0.6903;
            float3 multi = max(ao, ((ma * ao + mb) * ao + mc) * ao);
            c = c * share * multi + c * (1.0 - share) * contact;
        }
        if (lightDebug.x > 7.5) return float4(c, 1); // surface light only: no air between it and the camera
        float3 inS = ScatterNear(PlanetPos(eye.xyz), d, min(dist * kHazeScale, 150000.0), trans);
        c = c * trans + inS;
        c = GroundHaze(c, d, dist, i.pos.xy);
        if (portalCount.y > 0.5) c = RoomShafts(c, d, dist, i.pos.xy);
    }
    return float4(min(c, 2000.0), 1);
}

// ------------------------------------------------------------------ irradiance probes
// Rays from each probe, marched through the sun's shadow map instead of a BVH: no ray tracing hardware, same result on both
// backends. What the sun sees is a surface it lights (Dachsbacher and Stamminger, "Reflective Shadow Maps", 2005), with a
// lit picture of the scene taken from the sun (BeginCapture): red brick and wood send their own colour. A shaded hit, which the
// sun's view cannot see, takes a desert albedo. A hit also picks up last update's probes, so bounces build over frames as in DDGI. A ray leaving the map takes the sky and ground.
float3 SkyLightDir(float3 d) { return max(skySh[0].rgb + skySh[1].rgb * d.x + skySh[2].rgb * d.y + skySh[3].rgb * d.z, 0.0); }

static const float3 kBounceAlbedo = float3(0.42, 0.33, 0.24);
#define CAPTURE_SIZE 512 // kCaptureSize in renderer.h

int3 ProbeCellOfIndex(int3 idx, int cascade) { // the world cell a texel holds now
    int3 origin = int3(ProbeCascadeOrigin(probeOrigin.xyz, cascade));
    return origin + ProbeIndex(idx - origin);
}
int3 ProbeIndexOfLinear(int l) { // within one cascade
    int nx = int(probeGrid.x), nz = int(probeGrid.z);
    return int3(l % nx, (l / nx) / nz, (l / nx) % nz);
}
float2 ShadowUv(float3 s) { return float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5); }

float3 ProbeRayDir(int ray, int row) { // spherical Fibonacci, turned a new way each frame so the updates cover the sphere
    float rays = probeUpdate.z, y = 1.0 - (2.0 * ray + 1.0) / rays, r = sqrt(max(1.0 - y * y, 0.0));
    float seed = frac(misc.y * 0.61803399 + row * 0.7548777), az = ray * 2.399963 + seed * 6.2831853;
    float tilt = (frac(misc.y * 0.7548777) - 0.5) * 1.2, ct = cos(tilt), st = sin(tilt);
    float3 d = float3(r * cos(az), y, r * sin(az));
    return float3(d.x * ct - d.y * st, d.x * st + d.y * ct, d.z);
}

// Window panes, the openings of a room whose roof the sun map cannot tell from open sky: where the ray crosses the nearest
// pane (1e9 for none) and whether any pane lies within reach of the probe, which is what makes it an interior probe.
static const float kWindowReach = 12.0;
static const float3 kWindowTint = float3(0.78, 0.82, 0.80); // clean glass takes a little, grime and the frame a little more
static const float3 kCeilingAlbedo = float3(0.34, 0.25, 0.17); // bare boards seen from below
static const float3 kRoomAlbedo = float3(0.20, 0.14, 0.09); // a room's aged boards, as tbd_reference finds them

float PortalCross(float3 p, float3 d, int k, out float near) {
    float3 c = portals[k * 3].xyz, u = portals[k * 3 + 1].xyz, v = portals[k * 3 + 2].xyz;
    near = length(p - c);
    float3 n = normalize(cross(u, v));
    float denom = dot(d, n);
    if (abs(denom) < 1e-3) return 1e9;
    float t = dot(c - p, n) / denom;
    if (t < 0.05) return 1e9;
    float3 h = p + d * t - c;
    return abs(dot(h, u) / dot(u, u)) <= 1.0 && abs(dot(h, v) / dot(v, v)) <= 1.0 ? t : 1e9;
}

// The sky seen through the panes from a point in a room, from each pane's exact solid angle: WindowSkyLight in lighting.hlsli.
float3 PaneSky(float3 world, float3 n) {
    float3 sum = 0.0;
    for (int k = 0; k < int(portalCount.x); ++k) {
        float3 c = portals[k * 3].xyz, u = portals[k * 3 + 1].xyz, v = portals[k * 3 + 2].xyz, toC = c - world;
        if (dot(toC, toC) > 144.0) continue;
        float side = dot(cross(u, v), world - c) > 0.0 ? 1.0 : -1.0;
        float3 p[4] = {c - u - v, c + u - v, c + u + v, c - u + v};
        float f = 0.0;
        for (int e = 0; e < 4; ++e) {
            float3 a = normalize(p[e] - world), b = normalize(p[(e + 1) & 3] - world), ax = cross(a, b);
            float len = length(ax);
            if (len > 1e-5) f += acos(clamp(dot(a, b), -1.0, 1.0)) * dot(ax / len, n);
        }
        sum += SkyLightDir(normalize(toC)) * (saturate(-f * side) / 6.2831853) * kWindowTint;
    }
    return sum;
}

// A probe in a building's room (InteriorRoom in renderer.h) traces the room itself: the box's walls, floor and ceiling, with the
// panes as the openings. One just outside the box (under the floor, over the ceiling, inside a wall) works from the nearest point
// inside it, so the probes the room's surfaces read are not the ones under the floorboards (DDGI's probe relocation, done once).
// Its probes hold the room's bounce only: the sky through the panes reaches each surface exactly (WindowSkyLight), so a ray
// leaving by a pane brings nothing, and a hit sends on its own window sky, its sun (the patch under a window) and its bounce.
bool RoomTrace(float3 p, float3 d, out float4 result) {
    result = 0.0;
    for (int k = 0; k < int(portalCount.y); ++k) {
        float3 c = rooms[k * 3].xyz, ax = rooms[k * 3 + 1].xyz, az = rooms[k * 3 + 2].xyz;
        float hy = rooms[k * 3].w, ex = length(ax), ez = length(az);
        float3 o = p - c, l = float3(dot(o, ax) / (ex * ex), o.y / hy, dot(o, az) / (ez * ez));
        float3 ext = float3(ex, hy, ez);
        if (any(abs(l) > 1.0 + float3(0.5, 1.6, 0.5) / ext)) continue;
        l = clamp(l, -1.0 + 0.15 / ext, 1.0 - 0.15 / ext);
        float3 q = c + ax * l.x + float3(0, hy * l.y, 0) + az * l.z;
        float3 dl = float3(dot(d, ax) / (ex * ex), d.y / hy, dot(d, az) / (ez * ez));
        float3 tt = (1.0 - l * sign(dl)) / max(abs(dl), 1e-5); // to the face the ray leaves through, on each axis
        float t = min(tt.x, min(tt.y, tt.z));
        int axis = tt.x <= t ? 0 : tt.y <= t ? 1 : 2;
        float tPane = 1e9, reach;
        for (int w = 0; w < int(portalCount.x); ++w) tPane = min(tPane, PortalCross(q, d, w, reach));
        if (tPane < t + 0.6) { result = float4(0, 0, 0, 1e3); return true; }
        float3 n = axis == 0 ? -sign(dl.x) * ax / ex : axis == 1 ? float3(0, -sign(dl.y), 0) : -sign(dl.z) * az / ez;
        float3 h = q + d * t;
        float fade, vis;
        float3 e = ProbeIrradiance(h + n * 0.3, n, fade, vis);
        if (fade <= 0.0) e = 0.0;
        e += PaneSky(h + n * 0.05, n);
        float ndl = dot(n, sunDir.xyz);
        if (ndl > 0.0) {
            float2 size;
            gShadow.GetDimensions(size.x, size.y);
            float3 s = mul(shadowVP, float4(h + n * 0.08, 1.0)).xyz;
            float2 uv = ShadowUv(s);
            if (all(uv > 0.0) && all(uv < 1.0) && (s.z - gShadow.Load(int3(uv * size, 0))) / length(shadowVP[2].xyz) < 0.25)
                e += sunColorP.rgb * 3.2 * CloudSunAt(h) * ndl;
        }
        result = float4(kRoomAlbedo * e, t);
        return true;
    }
    return false;
}

float4 ProbeTracePS(FsOut i) : SV_Target {
    int ray = int(i.pos.x), row = int(i.pos.y);
    int count = int(probeGrid.x * probeGrid.y * probeGrid.z);
    int lin = (int(probeUpdate.x) + row) % (2 * count), cascade = lin >= count ? 1 : 0;
    int3 cell = ProbeCellOfIndex(ProbeIndexOfLinear(lin - cascade * count), cascade);
    float scale = ProbeScale(cascade);
    float3 p = float3(cell) * ProbeSpacing() * scale;
    float3 d = ProbeRayDir(ray, row);
    if (lightDebug.w < 0.5) return float4(SkyLightDir(d), 1e3);
    float4 inRoom;
    if (scale < 1.5 && RoomTrace(p, d, inRoom)) return inRoom;

    float2 size;
    gShadow.GetDimensions(size.x, size.y);
    float range = 1.0 / length(shadowVP[2].xyz), texelWorld = 2.0 / length(shadowVP[0].xyz) / size.x;
    float3 s0 = mul(shadowVP, float4(p, 1.0)).xyz;
    float2 uv0 = ShadowUv(s0);
    if (any(uv0 <= 0.0) || any(uv0 >= 1.0)) return float4(SkyLightDir(d), 1e3);
    float b0 = (s0.z - gShadow.Load(int3(uv0 * size, 0))) * range;
    bool free = b0 < 0.3;
    // A shaded probe with a window nearby is inside a building: its rays reach the sky only through a pane, and the roof stops the rest.
    float tPane = 1e9, nearest = 1e9;
    if (!free && scale < 1.5)
        for (int w = 0; w < int(portalCount.x); ++w) {
            float reach;
            tPane = min(tPane, PortalCross(p, d, w, reach));
            nearest = min(nearest, reach);
        }
    const bool indoors = !free && scale < 1.5 && nearest < kWindowReach;
    float t = 0.0, stepLen = 0.4 * scale;
    for (int k = 0; k < 12; ++k) {
        t += stepLen;
        if (indoors && tPane < t) return float4(SkyLightDir(d) * kWindowTint, 1e3);
        float3 q = p + d * t;
        float3 s = mul(shadowVP, float4(q, 1.0)).xyz;
        float2 uv = ShadowUv(s);
        if (any(uv <= 0.0) || any(uv >= 1.0)) break;
        int2 px = int2(uv * size);
        float b = (s.z - gShadow.Load(int3(px, 0))) * range;
        bool hit = false, lit = false;
        if (free) {
            if (b > 0.05) { hit = true; lit = b < stepLen * 1.5 + 0.3; }
        } else if (b > b0 + 2.5) hit = true; // deep behind what shades the probe: a floor or wall the sun cannot see
        else if (b < -0.2) {
            if (!indoors) free = true;
            else { // came out above the roof that shades the probe: indoors that roof is a ceiling, lit by what the room already holds
                float fade, vis;
                float3 back = ProbeIrradiance(q, -d, fade, vis);
                return float4(kCeilingAlbedo * (fade > 0.0 ? back : SkyLightDir(float3(0, 1, 0)) * 0.3), t);
            }
        }
        stepLen *= 1.35;
        if (!hit) continue;
        float fade, vis;
        float3 back = ProbeIrradiance(q, -d, fade, vis);
        float3 e = fade > 0.0 ? back : SkyLightDir(float3(0, 1, 0)) * 0.6;
        if (lit) { // the sun's cosine from the map's slope: n.l = 1 / sqrt(1 + |grad d|^2)
            float dx = gShadow.Load(int3(px + int2(2, 0), 0)) - gShadow.Load(int3(px - int2(2, 0), 0));
            float dy = gShadow.Load(int3(px + int2(0, 2), 0)) - gShadow.Load(int3(px - int2(0, 2), 0));
            float2 grad = float2(dx, dy) * range / (4.0 * texelWorld);
            e += sunColorP.rgb * 3.2 * CloudSunAt(q) / sqrt(1.0 + dot(grad, grad));
            if (captureVP[3][3] > 0.5) { // the surface's own colour and light, where the capture saw it
                float3 cp = mul(captureVP, float4(q, 1.0)).xyz;
                float2 cuv = ShadowUv(cp);
                float4 seen = all(cuv > 0.0) && all(cuv < 1.0) ? gCapture.Load(int3(cuv * float(CAPTURE_SIZE), 0)) : float4(0, 0, 0, 0);
                if (abs(seen.a) > 0.5) return float4(min(seen.rgb, 8.0), t);
            }
        }
        return float4(kBounceAlbedo * e, t);
    }
    return float4(SkyLightDir(d), 1e3);
}

// Folds the frame's rays into the atlas with hardware blending, which is the temporal filter (DDGI's hysteresis): alpha is the
// new data's weight. Only texels of probes traced this frame are touched. RGB only; the target's alpha is not used.
float4 ProbeBlendPS(FsOut i) : SV_Target {
    const float3 kLum = float3(0.2126, 0.7152, 0.0722);
    int3 n = int3(probeGrid.xyz);
    int2 px = int2(i.pos.xy);
    int panel = px.x % 3;
    int cascade = px.y >= n.y * n.z ? 1 : 0, py = px.y - cascade * n.y * n.z;
    int3 idx = int3(px.x / 3, py / n.z, py % n.z);
    int3 cell = ProbeCellOfIndex(idx, cascade);
    float scale = ProbeScale(cascade);
    float3 sky = skySh[0].rgb;
    float3 defGrad = float3(dot(skySh[1].rgb, kLum), dot(skySh[2].rgb, kLum), dot(skySh[3].rgb, kLum));
    float3 prevLoc = float3(cell) - ProbeCascadeOrigin(probePrev.xyz, cascade);
    bool fresh = lightDebug.y > 0.5 || any(prevLoc < 0.0) || any(prevLoc > probeGrid.xyz - 1.0);
    float weight = lightDebug.z > 0.5 ? 1.0 : 0.15;
    int count = n.x * n.y * n.z, total = 2 * count;
    int lin = idx.x + n.x * (idx.z + n.z * idx.y) + cascade * count;
    int row = (lin - int(probeUpdate.x) + total) % total;
    if (row >= int(probeUpdate.y)) {
        if (!fresh) discard;
        return float4(panel == 0 ? sky : panel == 1 ? defGrad : float3(1, 1, 0), 1.0);
    }
    float3 mean = 0.0, grad = 0.0, result;
    float misses = 0.0, close = 0.0;
    float rays = probeUpdate.z;
    for (int r = 0; r < int(rays); ++r) {
        float4 L = gRays.Load(int3(r, row, 0));
        mean += L.rgb;
        grad += dot(L.rgb, kLum) * ProbeRayDir(r, row);
        if (L.a > 999.0) misses += 1.0;
        if (L.a < scale) close += 1.0;
    }
    mean /= rays;
    grad *= 3.0 / rays;
    // A probe buried in the ground sees mostly nearby hits: it holds the sky's value and does not drag its neighbours dark.
    bool buried = close > rays * 0.5;
    if (panel == 0) result = buried ? sky : mean;
    else if (panel == 1) result = buried ? defGrad : grad;
    else {
        float vis = 1.0;
        if (lightDebug.w > 0.5) {
            float2 size;
            gShadow.GetDimensions(size.x, size.y);
            float3 s = mul(shadowVP, float4(float3(cell) * ProbeSpacing() * scale, 1.0)).xyz;
            float2 uv = ShadowUv(s);
            if (all(uv > 0.0) && all(uv < 1.0)) vis = s.z > gShadow.Load(int3(uv * size, 0)) + 0.0005 ? 0.0 : 1.0;
        }
        result = float3(vis, buried ? 1.0 : misses / rays, 0);
    }
    return float4(result, fresh ? 1.0 : weight);
}

// ------------------------------------------------------------------ camera measurements
// The scene's average brightness for auto exposure: log average of a sparse grid of taps, kept over time by blending.
float4 ExposurePS(FsOut i) : SV_Target {
    float sum = 0.0, weightSum = 0.0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 12; ++x) {
            float2 uv = (float2(x, y) + 0.5) / float2(12.0, 8.0);
            float3 c = gResolved.SampleLevel(gClamp, uv, 0).rgb;
            float w = 1.0 + 2.0 * exp(-dot(uv - 0.5, uv - 0.5) * 6.0); // the middle of the frame counts more
            sum += w * log(max(dot(c, float3(0.2126, 0.7152, 0.0722)), 1e-3));
            weightSum += w;
        }
    return float4(sum / weightSum, 0, 0, lightDebug.y > 0.5 ? 1.0 : 0.04);
}

// What the tone map multiplies by. A frame darker than the open air around it (a room by day, the shade under a porch) is
// met only partway: the eye stays set for the brighter world outside, so a room keeps its gloom and its windows burn white.
// The open air's level is the sky's own light: at night it is low too, and the dark is met in full.
float ExposureValue() {
    if (motion.z > 0.0) return motion.z * (motion.y > 0.0 ? motion.y * motion.y : 1.0); // the scene's locked exposure (Scene::exposure)
    const float key = 0.19; // a bright desert frame is metered a little under mid grey, as a photographer would
    const float kFree = 1.0, kMet = 0.6; // the first stop down is met in full, then this much of the rest (in log terms)
    float metered = gExposure.Load(int3(0, 0, 0)).r;
    float open = log(1.1 * max(dot(skySh[0].rgb, float3(0.2126, 0.7152, 0.0722)), 1e-4)); // what the street meters at this hour
    float below = open - kFree * 0.6931 - metered;
    float adapted = below > 0.0 ? metered + below * (1.0 - kMet) : metered;
    return clamp(key / exp(adapted), 0.3, 6.0) * (motion.y > 0.0 ? motion.y * motion.y : 1.0); // brightness squared: the slider needs to be felt
}

// A quarter-size wide glow of what is brighter than paper white, with a soft knee.
float4 BloomPS(FsOut i) : SV_Target {
    float3 sum = 0.0;
    float total = 0.0;
    float ex = ExposureValue();
    for (int k = 0; k < 20; ++k) {
        float t = (k + 0.5) / 20.0, a = k * 2.399963, r = sqrt(t) * 0.035;
        float2 uv = i.uv + float2(cos(a) * r * res.y * res.z, sin(a) * r);
        float3 c = min(gResolved.SampleLevel(gClamp, uv, 0).rgb * ex, 16.0);
        float l = dot(c, float3(0.2126, 0.7152, 0.0722));
        float w = 1.0 - t * 0.7;
        sum += c * smoothstep(1.2, 3.0, l) * w;
        total += w;
    }
    return float4(sum / total, 1);
}

// ------------------------------------------------------------------ temporal anti-aliasing
float4 TaaPS(FsOut i) : SV_Target {
    float3 cur = gComposed.SampleLevel(gClamp, i.uv, 0).rgb;
    if (misc.w < 0.5) return float4(cur, 1);
    float depth = gDepth.SampleLevel(gClamp, i.uv, 0);
    float4 prev = mul(prevViewProj, float4(WorldAt(i.uv, max(depth, 1e-5)), 1.0));
    float2 puv = float2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
    // A moving thing wrote its own motion; the closest of five pixels decides, so its edges follow it too (reversed Z: larger is closer).
    float keep = gComposed.SampleLevel(gClamp, i.uv, 0).a < 0.75 ? 0.55 : 0.9; // water (alpha 0.5, see water.hlsl) changes shape on its own; history only softens it
    const bool isWater = keep < 0.75;
    if (motion.x > 0.5) {
        float2 nearest = i.uv;
        float closest = depth;
        const float2 cross[4] = {float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1)};
        for (int k = 0; k < 4; ++k) {
            const float2 q = i.uv + cross[k] * res.zw;
            const float d = gDepth.SampleLevel(gClamp, q, 0);
            if (d > closest) { closest = d; nearest = q; }
        }
        const float2 velocity = gVelocity.SampleLevel(gClamp, nearest, 0).xy;
        if (dot(velocity, velocity) > 0.0) {
            puv = i.uv - velocity;
            if (!isWater) keep = 0.85; // what moves has less to gain from its history and more to smear
        }
    }
    if (any(puv < 0.0) || any(puv > 1.0)) return float4(cur, 1);
    // Clamp in compressed space so one bright pixel (the sun) can't widen the box for its neighbours.
    float3 lo = 1e9, hi = -1e9;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            float3 s = gComposed.SampleLevel(gClamp, i.uv + float2(x, y) * res.zw, 0).rgb;
            s = s / (1.0 + s);
            lo = min(lo, s);
            hi = max(hi, s);
        }
    float3 h = gHistory.SampleLevel(gClamp, puv, 0).rgb;
    h = clamp(h / (1.0 + h), lo, hi);
    h = h / max(1.0 - h, 1e-3);
    return float4(lerp(cur, h, keep), 1);
}

// ------------------------------------------------------------------ to the display
// Catmull-Rom through five bilinear taps (after Jimenez, "Filmic SMAA"): a sharper upscale than plain bilinear.
float3 SampleCatmullRom(float2 uv) {
    float2 pos = uv * res.xy;
    float2 center = floor(pos - 0.5) + 0.5;
    float2 f = pos - center, f2 = f * f, f3 = f2 * f;
    float2 w0 = -0.5 * f3 + f2 - 0.5 * f;
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    float2 w3 = 0.5 * f3 - 0.5 * f2;
    float2 w12 = w1 + w2;
    float2 t0 = (center - 1.0) * res.zw, t3 = (center + 2.0) * res.zw, t12 = (center + w2 / w12) * res.zw;
    float3 sum = gResolved.SampleLevel(gClamp, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y) +
                 gResolved.SampleLevel(gClamp, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y) +
                 gResolved.SampleLevel(gClamp, t12, 0).rgb * (w12.x * w12.y) +
                 gResolved.SampleLevel(gClamp, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y) +
                 gResolved.SampleLevel(gClamp, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);
    float total = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y; // the corners are left out
    return max(sum / total, 0.0);
}

// The city's grade (docs/cs2_look_pass.md), on exposed linear light before the filmic curve: a handful of ALU folded into the tone
// map, no LUT and no pass of its own. Cities: Skylines II's screenshots read low key, with olive greens, moderate saturation, a
// firmer midtone contrast, warm light and cool shade.
float3 CityGrade(float3 c) {
    const float3 kLum = float3(0.2126, 0.7152, 0.0722);
    // Greens toward olive: where green leads, some of its lead goes to red and the whole is dimmed. Lime grass reads as a lawn.
    float lead = c.g - max(c.r, c.b), green = saturate(lead / max(c.g, 1e-4) * 3.0);
    c = float3(c.r + lead * 0.30 * green, c.g - lead * 0.12 * green, c.b) * (1.0 - 0.10 * green);
    float l = max(dot(c, kLum), 1e-5);
    c = max(lerp(l.xxx, c, 1.10), 0.0); // a little more saturation: the haze already greys the distance
    c *= 0.52; // low key: CS2's median sits near 80 of 255, a stop under a metered mid grey
    l *= 0.52;
    float t = saturate(log2(l / 0.18) * 0.22 + 0.5); // 0 deep shade .. 0.5 mid grey .. 1 sunlit highlight
    c *= pow(l / 0.18, 0.28); // midtone contrast in log space (luminance only, so hue holds): deep shade, highlights kept
    c *= lerp(float3(0.92, 0.98, 1.10), float3(1.04, 1.0, 0.94), smoothstep(0.15, 0.85, t)); // cool blue-grey shade, warm light
    return c;
}

float4 TonemapPS(FsOut i) : SV_Target {
    float3 c = eye.w > 0.5 ? SampleCatmullRom(i.uv) : gResolved.SampleLevel(gClamp, i.uv, 0).rgb;
    // Win back what temporal accumulation softens. Limited to a local fraction so edges don't ring.
    float3 blur = (gResolved.SampleLevel(gClamp, i.uv + float2(res.z, 0), 0).rgb + gResolved.SampleLevel(gClamp, i.uv - float2(res.z, 0), 0).rgb +
                   gResolved.SampleLevel(gClamp, i.uv + float2(0, res.w), 0).rgb + gResolved.SampleLevel(gClamp, i.uv - float2(0, res.w), 0).rgb) * 0.25;
    c = max(c + clamp((c - blur) * 0.9, -c * 0.3, c * 0.3), 0.0);
    if (lightDebug.x > 0.5) return float4(pow(saturate(c * 0.5 / (1.0 + c * 0.5)), 1.0 / 2.2), 1); // component views: fixed exposure, no grade
    c *= ExposureValue();
    if (PostTier() > 1) c += gBloom.SampleLevel(gClamp, i.uv, 0).rgb * 0.06; // Low and Low/Med skip the bloom pass
    float2 q = i.uv - 0.5;
    if (CityLook()) c = CityGrade(c);
    else c *= float3(1.0, 1.0, 1.02); // grade: the sun's warmth stays (a daylight-balanced camera would not cool it), and no desaturation
    c *= 1.0 - 0.35 * dot(q, q); // vignette
    // Filmic curve: Stephen Hill's fit of the ACES RRT and ODT (BakingLab, MIT licence), which holds hue better than a per-channel curve.
    const float3x3 acesIn = float3x3(0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383, 0.83777);
    const float3x3 acesOut = float3x3(1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327, -0.07276, 1.07602);
    c = mul(acesIn, c);
    float3 curved = (c * (c + 0.0245786) - 0.000090537) / (c * (0.983729 * c + 0.4329510) + 0.238081);
    // Half of it hue preserving: the curve on the brightest channel alone, so sunlit red brick stays red instead of going pink.
    float peak = max(max(c.r, max(c.g, c.b)), 1e-4), peakOut = (peak * (peak + 0.0245786) - 0.000090537) / (peak * (0.983729 * peak + 0.4329510) + 0.238081);
    c = saturate(mul(acesOut, lerp(curved, c * (peakOut / peak), 0.5)));
    return float4(pow(c, 1.0 / (2.2 * sqrt(motion.y > 0.0 ? motion.y : 1.0))), 1); // brightness also lifts the midtones
}
