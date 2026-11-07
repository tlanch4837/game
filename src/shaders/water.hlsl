// Water, on both backends: tiled waves and a sliding ripple simulation in compute, and the surface drawn after the sky and haze
// are composed. Built from public, long-standing methods: periodic Gerstner waves (Tessendorf's course notes describe the
// dispersion relation and choppy displacement), a 2D wave equation on a height field, Beer-Lambert absorption, two-phase flow
// mapping (Vlachos, "Water Flow in Portal 2", SIGGRAPH 2010). Not Guerrilla's method: their published water plays back offline
// simulations as deformations (Malan, SIGGRAPH 2022 Advances). Compiled by fxc (cs/vs/ps_5_1) and by dxc (-DVULKAN).
#ifdef VULKAN
#define RO(n) [[vk::binding(n, 0)]]
#define RW(n) [[vk::binding(4 + n, 0)]]
#define GX(n) [[vk::binding(n, 0)]]
#else
#define RO(n)
#define RW(n)
#define GX(n)
#endif

static const uint kTile = 128;       // texels per side of one wave tile
static const uint kCascades = 3;
static const uint kRippleSize = 512; // cells per side of the ripple simulation
static const uint kSkyW = 32, kSkyH = 16;

struct RippleCell { float h, v, foam, gx, gz, pad; };

float3 SkyBaseFn(float3 d, float3 eyePos, float3 sun);

// ------------------------------------------------------------------ compute
#ifdef WATER_COMPUTE
struct ComputeConstants {
    float4 a;           // CSRipple: dt, wave speed squared / cell squared, cell, impulse count. CSWaves: time, tile metres, cascade, amplitude. CSSky: eye
    float4 b;           // CSRipple: shift x, shift z, world origin x, z. CSWaves: wind x, z. CSSky: sun
    float4 impulses[6]; // world x, z, radius, velocity per step
};
#ifdef VULKAN
[[vk::push_constant]] ComputeConstants pc;
#else
ConstantBuffer<ComputeConstants> pc : register(b0);
#endif
RW(0) RWStructuredBuffer<RippleCell> gPrev : register(u0);
RW(1) RWStructuredBuffer<RippleCell> gNext : register(u1);
RW(2) RWStructuredBuffer<float4> gOut : register(u2); // CSWaves: tiles. CSSky: the sky table
RO(0) ByteAddressBuffer gUnused : register(t0);

RippleCell CellAt(int2 p) {
    if (any(p < int2(0, 0)) || any(p >= int2(kRippleSize, kRippleSize))) { RippleCell c = (RippleCell)0; return c; }
    return gPrev[p.y * kRippleSize + p.x];
}

[numthreads(8, 8, 1)]
void CSRipple(uint3 id : SV_DispatchThreadID) {
    const int2 here = int2(id.xy), shift = int2(pc.b.xy);
    const RippleCell c = CellAt(here + shift);
    const float l = CellAt(here + shift + int2(-1, 0)).h, r = CellAt(here + shift + int2(1, 0)).h;
    const float d = CellAt(here + shift + int2(0, -1)).h, u = CellAt(here + shift + int2(0, 1)).h;
    float v = c.v + (l + r + d + u - 4.0 * c.h) * pc.a.y * pc.a.x;
    float foam = c.foam * 0.9945; // half gone in about a second and a half

    const float2 world = pc.b.zw + (float2(here) + 0.5) * pc.a.z;
    for (int i = 0; i < (int)pc.a.w; ++i) {
        const float2 away = world - pc.impulses[i].xy;
        const float r2 = pc.impulses[i].z * pc.impulses[i].z;
        const float g = exp(-dot(away, away) / max(r2, 1e-4));
        v += pc.impulses[i].w * g;
        foam += abs(pc.impulses[i].w) * g * 0.04;
    }

    // A sponge at the border, so waves leave instead of bouncing off the edge of the sliding window.
    const float edge = min(min(here.x, here.y), min((int)kRippleSize - 1 - here.x, (int)kRippleSize - 1 - here.y));
    const float sponge = saturate(edge / 24.0);
    v *= 0.9985 * lerp(0.86, 1.0, sponge);
    const float h = clamp((c.h + v * pc.a.x) * lerp(0.9, 0.9985, sponge), -0.8, 0.8); // the slow pull back keeps long swells from piling up in a small window

    RippleCell o;
    o.h = h;
    o.v = v;
    o.foam = saturate(foam + saturate(abs(v) * 0.5 - 0.2) * 0.006); // what moves fast keeps churning
    o.gx = (r - l) / (2.0 * pc.a.z);
    o.gz = (u - d) / (2.0 * pc.a.z);
    o.pad = 0.0;
    gNext[here.y * kRippleSize + here.x] = o;
}

uint Hash(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
float Rand(uint x) { return float(Hash(x)) / 4294967295.0; }

// Whole wave numbers on the tile, so it repeats without a seam. One thread group is a tile's 8 x 8 texels.
[numthreads(8, 8, 1)]
void CSWaves(uint3 id : SV_DispatchThreadID) {
    const float time = pc.a.x, L = pc.a.y, ampScale = pc.a.w;
    const uint cascade = (uint)pc.a.z;
    const float2 wind = pc.b.xy;
    const float2 p = (float2(id.xy) + 0.5) / float(kTile) * L;
    float3 disp = 0;
    float sx = 0, sz = 0, dxx = 0, dzz = 0, dxz = 0;
    const int kWaves = 28;
    for (int w = 0; w < kWaves; ++w) {
        const uint seed = uint(w) * 7u + cascade * 131u + 11u;
        const float harmonic = 2.0 + float(w) * 0.75 + Rand(seed) * 2.5;     // shorter waves as w grows
        const float angle = atan2(wind.y, wind.x) + (Rand(seed + 1u) - 0.5) * 3.4;
        const float2 n = round(float2(cos(angle), sin(angle)) * harmonic);    // wave number: an integer pair
        if (all(n == 0.0)) continue;
        const float2 k = n * 6.2831853 / L;
        const float kl = length(k);
        const float omega = round(sqrt(9.81 * kl) * 16.0) / 16.0;
        const float amplitude = ampScale * 0.1 / (kl * pow(float(w) + 3.0, 0.75)); // a share of steepness per wave
        const float phase = dot(k, p) - omega * time + Rand(seed + 2u) * 6.2831853;
        const float s = sin(phase), c = cos(phase);
        const float2 dir = k / kl;
        const float choppy = 0.8;
        disp += float3(-dir.x * choppy * amplitude * s, amplitude * c, -dir.y * choppy * amplitude * s);
        sx += -k.x * amplitude * s;
        sz += -k.y * amplitude * s;
        dxx += -dir.x * k.x * choppy * amplitude * c;
        dzz += -dir.y * k.y * choppy * amplitude * c;
        dxz += -dir.x * k.y * choppy * amplitude * c;
    }
    const float jacobian = (1.0 + dxx) * (1.0 + dzz) - dxz * dxz; // below 1: the surface folds together, where foam gathers
    const uint base = (cascade * kTile * kTile + id.y * kTile + id.x) * 2u;
    gOut[base] = float4(disp, jacobian);
    gOut[base + 1u] = float4(sx, sz, 0, 0);
}

// The sky table: the same single scattering as post.hlsl's SkyRadiance, a lat-long grid once per frame, so a reflection costs one lookup.
[numthreads(8, 8, 1)]
void CSSky(uint3 id : SV_DispatchThreadID) {
    const float2 uv = (float2(id.xy) + 0.5) / float2(kSkyW, kSkyH);
    const float y = uv.y * 2.0 - 1.0;
    const float az = uv.x * 6.2831853, sy = sqrt(saturate(1.0 - y * y));
    gOut[id.y * kSkyW + id.x] = float4(SkyBaseFn(float3(cos(az) * sy, y, sin(az) * sy), pc.a.xyz, pc.b.xyz), 1.0);
}
#endif

// A copy of the single-scattering sky in post.hlsl with its own constants.
static const float kEarth = 6360e3, kTop = 6420e3;
static const float3 kBetaR = float3(5.8e-6, 13.5e-6, 33.1e-6);
static const float kBetaM = 21e-6, kSunI = 12.0, kPI = 3.14159265;
float SphereFar(float3 o, float3 d, float r) {
    const float b = dot(o, d), c = dot(o, o) - r * r, h = b * b - c;
    return h < 0.0 ? -1.0 : -b + sqrt(h);
}
float2 OpticalToSun(float3 p, float3 l) {
    const float b0 = dot(p, l);
    if (b0 < 0.0 && dot(p, p) - b0 * b0 < kEarth * kEarth) return float2(1e9, 1e9);
    const float step = SphereFar(p, l, kTop) / 4.0;
    float2 od = 0;
    for (int i = 0; i < 4; ++i) od += exp(-max(length(p + l * (i + 0.5) * step) - kEarth, 0.0) / float2(8000.0, 1200.0)) * step;
    return od;
}
float3 Transmit(float2 od) { return exp(-(kBetaR * od.x + kBetaM * 1.1 * od.y)); }
float3 SkyBaseFn(float3 dIn, float3 eyePos, float3 sun) {
    const float3 o = float3(eyePos.x, max(eyePos.y, 1.0) + kEarth, eyePos.z);
    const float3 d = normalize(float3(dIn.x, max(dIn.y, 0.002), dIn.z));
    const float mu = dot(d, sun), g = 0.76;
    const float pr = 3.0 / (16.0 * kPI) * (1.0 + mu * mu);
    const float pm = 3.0 / (8.0 * kPI) * (1.0 - g * g) * (1.0 + mu * mu) / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    const float step = min(SphereFar(o, d, kTop), 150000.0) / 12.0;
    float2 od = 0;
    float3 sumR = 0, sumM = 0;
    for (int i = 0; i < 12; ++i) {
        const float3 p = o + d * (i + 0.5) * step;
        const float2 rho = exp(-max(length(p) - kEarth, 0.0) / float2(8000.0, 1200.0)) * step;
        od += rho;
        const float3 t = Transmit(od + OpticalToSun(p, sun));
        sumR += t * rho.x;
        sumM += t * rho.y;
    }
    return kSunI * (sumR * kBetaR * pr + sumM * kBetaM * pm) + float3(0.002, 0.003, 0.006);
}

// ------------------------------------------------------------------ the surface
#ifndef WATER_COMPUTE
GX(0) cbuffer PostParams : register(b0) { // the post chain's own constants, as ResolveScene filled them
    float4x4 invViewProj;
    float4x4 prevViewProj;
    float4 eye;
    float4 sunDir;
    float4 res;
    float4 misc;
};
struct LightingData {
    float4 sunDirL, sunColor, skyColor, groundColor;
    float4x4 shadowVP;
    float4 shadowParams, eyeL;
    float4x4 tightVP;
    float4 tightParams;
    float4 skySh[4];
    float4 skyGrad[3];
    float4 probeOrigin, probeGrid, probeUpdate, probePrev, lightDebug, cloudMap;
};
GX(1) ConstantBuffer<LightingData> gLight : register(b1);
struct WaterConstants {
    float4x4 viewProj; // jittered, as the scene was drawn
    float4 ripple;     // world origin x, z; cell; 1 when simulating
    float4 near;       // centre x, z; half size; time
    float4 tiles;      // tile metres per cascade; w: 1 for the near mesh
    float4 wind;       // x, z; displacement scale; debug
};
#ifdef VULKAN
[[vk::push_constant]] WaterConstants wc;
#else
ConstantBuffer<WaterConstants> wc : register(b2);
#endif
GX(2) Texture2D<float> gDepth : register(t0);
GX(3) Texture2D<float4> gRefract : register(t1);
GX(4) Texture2D<float> gShadow : register(t2);
GX(5) SamplerState gClamp : register(s0);
GX(6) SamplerComparisonState gCmp : register(s1);
GX(7) StructuredBuffer<float4> gWaves : register(t5);
GX(8) StructuredBuffer<RippleCell> gRipple : register(t6);
GX(9) StructuredBuffer<float4> gSky : register(t7);
GX(10) Texture2D<float4> gProbes : register(t3);
GX(11) Texture2D<float4> gCloudShadow : register(t4);

#define PROBE_ORIGIN gLight.probeOrigin
#define PROBE_GRID gLight.probeGrid
#include "probe.hlsli"
#include "city_air.hlsli"

// Bilinear reads of the tiles and of the ripples: these are buffers, not textures.
float3 Bilinear(float2 p, uint size, out int4 cells) {
    const float2 f = floor(p);
    const int2 c = int2(f);
    cells = int4(c, c + 1);
    return float3(p - f, 0);
}
float4 WaveTexel(uint cascade, int2 t, uint field) {
    const uint x = (uint)(t.x & int(kTile - 1)), y = (uint)(t.y & int(kTile - 1));
    return gWaves[(cascade * kTile * kTile + y * kTile + x) * 2u + field];
}
float4 WaveSample(uint cascade, float2 world, uint field) {
    int4 c;
    // Each cascade's size is picked with a branch: fxc read wc.tiles[cascade] as tiles[0] here, which made Direct3D's waves short and steep.
    const float metres = cascade == 0u ? wc.tiles.x : (cascade == 1u ? wc.tiles.y : wc.tiles.z);
    const float2 f = Bilinear(world / metres * float(kTile) - 0.5, kTile, c).xy;
    return lerp(lerp(WaveTexel(cascade, c.xy, field), WaveTexel(cascade, c.zy, field), f.x),
                lerp(WaveTexel(cascade, c.xw, field), WaveTexel(cascade, c.zw, field), f.x), f.y);
}
RippleCell RippleSample(float2 world) {
    RippleCell o = (RippleCell)0;
    if (wc.ripple.w < 0.5) return o;
    const float2 p = (world - wc.ripple.xy) / wc.ripple.z - 0.5;
    int4 c;
    const float2 f = Bilinear(p, kRippleSize, c).xy;
    if (c.x < 0 || c.y < 0 || c.z >= (int)kRippleSize || c.w >= (int)kRippleSize) return o;
    const RippleCell a = gRipple[c.y * kRippleSize + c.x], b = gRipple[c.y * kRippleSize + c.z];
    const RippleCell d = gRipple[c.w * kRippleSize + c.x], e = gRipple[c.w * kRippleSize + c.z];
    const float w0 = (1 - f.x) * (1 - f.y), w1 = f.x * (1 - f.y), w2 = (1 - f.x) * f.y, w3 = f.x * f.y;
    o.h = a.h * w0 + b.h * w1 + d.h * w2 + e.h * w3;
    o.foam = a.foam * w0 + b.foam * w1 + d.foam * w2 + e.foam * w3;
    o.gx = a.gx * w0 + b.gx * w1 + d.gx * w2 + e.gx * w3;
    o.gz = a.gz * w0 + b.gz * w1 + d.gz * w2 + e.gz * w3;
    return o;
}

struct VsIn {
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
    float2 flow : TEXCOORD1;
};
struct VsOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float2 rest : TEXCOORD1; // the undisplaced x, z: wave lookups stay on the surface they displaced
    float2 flow : TEXCOORD2;
    float3 hazeLight : TEXCOORD3; // the air between the camera and this point, as the composite computes it, per vertex
    float3 hazeKeep : TEXCOORD4;
};

// The composite's aerial scattering, then its ground dust without probes in it (a mean colour and the sun).
void AirTo(float3 world, out float3 light, out float3 keep) {
    light = 0;
    keep = 1;
    if (CityAirOn(misc.z)) { // the city map: the land's own closed-form haze (city_air.hlsli), not the marched air below
        const float k = CityAirKeep(eye.xyz, world);
        light = CityHazeOf(normalize(world - eye.xyz), gLight.skyGrad[1].rgb, gLight.sunColor.rgb, sunDir.xyz) * (1.0 - k);
        keep = k;
        return;
    }
    if (misc.z < 0.5) return;
    const float3 away = world - eye.xyz;
    const float dist = length(away);
    const float3 d = away / max(dist, 1e-3);
    const float3 o = float3(eye.x, max(eye.y, 1.0) + kEarth, eye.z);
    const float mu = dot(d, sunDir.xyz), g = 0.76;
    const float pr = 3.0 / (16.0 * kPI) * (1.0 + mu * mu);
    const float pm = 3.0 / (8.0 * kPI) * (1.0 - g * g) * (1.0 + mu * mu) / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    const float step = min(dist * 2.5, 150000.0) / 4.0;
    float2 od = 0;
    float3 sumR = 0, sumM = 0;
    for (int i = 0; i < 4; ++i) {
        const float3 p = o + d * (i + 0.5) * step;
        const float2 rho = exp(-max(length(p) - kEarth, 0.0) / float2(8000.0, 1200.0)) * step;
        od += rho;
        const float3 t = Transmit(od + OpticalToSun(p, sunDir.xyz));
        sumR += t * rho.x;
        sumM += t * rho.y;
    }
    const float3 trans = Transmit(od);
    light = kSunI * (sumR * kBetaR * pr + sumM * kBetaM * pm);
    keep = trans;
    const float kDensity = 0.0005, kFalloff = 1.0 / 45.0;
    const float groundY = gLight.probeOrigin.y * gLight.probeGrid.w + 3.0 * gLight.probeGrid.w - 3.0;
    const float h0 = max(eye.y - groundY, 0.0), dy = d.y * kFalloff * dist;
    const float T = exp(-kDensity * exp(-h0 * kFalloff) * dist * (abs(dy) > 1e-3 ? (1.0 - exp(-dy)) / dy : 1.0));
    const float gg = 0.6;
    const float hg = (1.0 - gg * gg) / (4.0 * kPI * pow(1.0 + gg * gg - 2.0 * gg * mu, 1.5));
    light = light * T + (gLight.skySh[0].rgb + gLight.sunColor.rgb * 3.2 * kPI * hg) * (1.0 - T);
    keep *= T;
}

VsOut WaterVS(VsIn i) {
    VsOut o;
    float3 p = i.pos;
    if (wc.tiles.w > 0.5) { // the dense mesh around the camera; it flattens toward its edge, where the coarse mesh takes over
        const float2 away = abs(i.pos.xz - wc.near.xy);
        const float fade = saturate((wc.near.z - max(away.x, away.y)) / 32.0);
        const float distanceFade = saturate(1.0 - length(i.pos - eye.xyz) / 160.0);
        float3 d = WaveSample(0, i.pos.xz, 0).xyz + WaveSample(1, i.pos.xz, 0).xyz + WaveSample(2, i.pos.xz, 0).xyz;
        d *= wc.wind.z * fade * saturate(distanceFade * 2.5);
        d.xz *= 1.0 - saturate(length(i.flow) * 2.0); // moving water keeps its displacement vertical: flow slides the normals only
        p += d;
        p.y += RippleSample(i.pos.xz).h * fade;
    }
    o.pos = mul(wc.viewProj, float4(p, 1.0));
    o.world = p;
    o.rest = i.pos.xz;
    o.flow = i.flow;
    AirTo(p, o.hazeLight, o.hazeKeep);
    return o;
}

float3 SkyLookup(float3 d) {
    const float az = atan2(d.z, d.x);
    const float2 p = float2(az / 6.2831853 + 1.0, saturate(d.y * 0.5 + 0.5)) * float2(kSkyW, kSkyH) - 0.5;
    const float2 fl = floor(p);
    const float2 f = p - fl;
    const int x0 = ((int)fl.x + (int)kSkyW) % (int)kSkyW, x1 = (x0 + 1) % (int)kSkyW;
    const int y0 = clamp((int)fl.y, 0, (int)kSkyH - 1), y1 = min(y0 + 1, (int)kSkyH - 1);
    return lerp(lerp(gSky[y0 * kSkyW + x0].rgb, gSky[y0 * kSkyW + x1].rgb, f.x), lerp(gSky[y1 * kSkyW + x0].rgb, gSky[y1 * kSkyW + x1].rgb, f.x), f.y);
}

float3 WorldAt(float2 uv, float depth) {
    const float4 w = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return w.xyz / w.w;
}
float Hash2(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float ValueNoise(float2 p) {
    const float2 c = floor(p), f0 = p - c, f = f0 * f0 * (3.0 - 2.0 * f0);
    return lerp(lerp(Hash2(c), Hash2(c + float2(1, 0)), f.x), lerp(Hash2(c + float2(0, 1)), Hash2(c + float2(1, 1)), f.x), f.y);
}
float SunShadow(float3 world) {
    if (gLight.shadowParams.w < 0.5) return 1.0;
    const float3 s = mul(gLight.shadowVP, float4(world, 1.0)).xyz;
    const float2 uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    const float edge = saturate(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 20.0);
    if (edge <= 0.0 || s.z >= 1.0) return 1.0;
    return lerp(1.0, gShadow.SampleCmpLevelZero(gCmp, uv, s.z), edge);
}

// Slopes and fold of the tiles at a point. Distance fades short waves out before they alias; their share of slope goes into roughness.
void Waves(float2 p, float dist, out float2 slope, out float fold, out float variance) {
    const float near0 = saturate(1.0 - dist / 70.0), near1 = saturate(1.0 - dist / 320.0);
    slope = 0;
    fold = 1.0;
    variance = 0.0;
    if (near0 > 0.0) { slope += WaveSample(0, p, 1).xy * near0; fold = min(fold, lerp(1.0, WaveSample(0, p, 0).w, near0)); }
    if (near1 > 0.0) { slope += WaveSample(1, p, 1).xy * near1; fold = min(fold, lerp(1.0, WaveSample(1, p, 0).w, near1)); }
    slope += WaveSample(2, p, 1).xy;
    variance = (1.0 - near0) * 0.012 + (1.0 - near1) * 0.02;
}

struct PsOut {
    float4 color : SV_Target0; // alpha 0.5: TaaPS keeps less history here
    float2 velocity : SV_Target1;
};

float CloudSun(float3 world) { // lighting.hlsli's, on our own constants
    if (gLight.cloudMap.w < 0.5) return 1.0;
    const float2 uv = (world.xz - gLight.cloudMap.xy) / (2.0 * gLight.cloudMap.z) + 0.5;
    float2 dim;
    gCloudShadow.GetDimensions(dim.x, dim.y);
    const float2 p = uv * dim - 0.5, b = floor(p), f = p - b;
    const int3 c = int3(clamp(int2(b), int2(0, 0), int2(dim) - 2), 0);
    const float t = lerp(lerp(gCloudShadow.Load(c).r, gCloudShadow.Load(c + int3(1, 0, 0)).r, f.x), lerp(gCloudShadow.Load(c + int3(0, 1, 0)).r, gCloudShadow.Load(c + int3(1, 1, 0)).r, f.x), f.y);
    return lerp(1.0, t, saturate(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 20.0));
}
float3 SkyAmbient(float3 n) {
    const float3 mean = gLight.skySh[0].rgb;
    return max(mean + (gLight.skySh[1].rgb * n.x + gLight.skySh[2].rgb * n.y + gLight.skySh[3].rgb * n.z) * (2.0 / 3.0), mean * 0.1);
}

float4 WaterColor(VsOut i);

// The water's own motion on screen: only the camera's, taken at the surface and not at the bed the depth buffer holds. Never exactly zero,
// so TaaPS trusts it over the depth.
PsOut WaterPS(VsOut i) {
    PsOut o;
    o.color = WaterColor(i);
    const float4 c = mul(prevViewProj, float4(i.world, 1.0));
    const float2 uv = i.pos.xy * res.zw;
    const float2 prevUv = float2(c.x / c.w + wc.wind.x, -(c.y / c.w + wc.wind.y)) * 0.5 + 0.5;
    o.velocity = uv - prevUv + float2(1e-5, 0.0);
    return o;
}

float4 WaterColor(VsOut i) {
    if (wc.near.z > 0.0) { // the two meshes meet on the near square's edge
        const float2 away = abs(i.rest - wc.near.xy);
        const bool inside = max(away.x, away.y) < wc.near.z;
        if (inside != (wc.tiles.w > 0.5)) discard;
    }
    const float2 uv = i.pos.xy * res.zw;
    const float3 toEye = eye.xyz - i.world;
    const float dist = length(toEye);
    const float3 v = toEye / dist;

    // ---- the surface's slope: tiles (slid along the flow in two phases when the water moves), then the ripples
    float2 slope;
    float fold, variance;
    const float speed = length(i.flow);
    if (speed > 0.03) {
        const float time = wc.near.w * 0.6, ph0 = frac(time), ph1 = frac(time + 0.5);
        const float blend = abs(ph0 * 2.0 - 1.0);
        float2 s0, s1;
        float f0, f1;
        Waves(i.rest - i.flow * (ph0 - 0.5) / 0.6, dist, s0, f0, variance);
        Waves(i.rest - i.flow * (ph1 - 0.5) / 0.6 + float2(3.7, 1.3), dist, s1, f1, variance);
        slope = lerp(s0, s1, blend);
        fold = lerp(f0, f1, blend);
    } else {
        Waves(i.rest, dist, slope, fold, variance);
    }
    const RippleCell ripple = RippleSample(i.rest);
    float2 rippleSlope = float2(ripple.gx, ripple.gz) * 2.2;
    rippleSlope *= min(1.0, 0.7 / max(length(rippleSlope), 1e-3));
    slope += rippleSlope;
    if (wc.wind.w > 0.5) return float4(saturate(slope.x * 4.0 + 0.5), saturate(slope.y * 4.0 + 0.5), frac(i.world.y * 4.0), 1.0); // TBD_WATER=3: the slopes as colour
    const float3 n = normalize(float3(-slope.x, 1.0, -slope.y));

    // ---- what lies below
    const float rawDepth = gDepth.SampleLevel(gClamp, uv, 0);
    const float3 below = rawDepth > 0.0 ? WorldAt(uv, rawDepth) : i.world - v * 200.0;
    const float column = max(i.world.y - below.y, 0.0); // straight down, for the shore
    float2 refractUv = uv + n.xz * float2(0.045, 0.045) * saturate(column * 0.5) / max(dist * 0.08, 1.0);
    const float refractRaw = gDepth.SampleLevel(gClamp, refractUv, 0);
    float3 refractPos = refractRaw > 0.0 ? WorldAt(refractUv, refractRaw) : i.world - v * 200.0;
    if (refractPos.y > i.world.y) { refractUv = uv; refractPos = below; } // what the bend landed on stands above the water
    const float3 scene = gRefract.SampleLevel(gClamp, refractUv, 0).rgb;
    const float path = min(length(refractPos - i.world), 80.0);

    // ---- river water: suspended silt, red gone first, then green; the silt scatters what light reaches it
    // The city map's sea is clear and deep (CS2: deep blue-teal, 15 54 69 to 43 76 110 of 255): red goes fast, blue slowest,
    // and what it scatters back is blue-green. The river keeps its silt.
    const bool sea = CityAirOn(misc.z);
    const float3 kAbsorb = sea ? float3(0.45, 0.075, 0.045) : float3(0.55, 0.22, 0.14);
    const float3 kSilt = sea ? float3(0.006, 0.030, 0.042) : float3(0.055, 0.075, 0.045);
    const float3 sunRgb = gLight.sunColor.rgb * 3.2;
    const float shadow = SunShadow(i.world);
    const float cloudSun = CloudSun(i.world);
    float skyVis = 1.0, probeFade = 0.0;
    float3 ambient = SkyAmbient(float3(0, 1, 0));
    if (gLight.probeUpdate.w > 0.5) { // the same indirect light as the ground beside it
        const float3 e = ProbeIrradiance(i.world + float3(0, 0.5, 0), float3(0, 1, 0), probeFade, skyVis);
        skyVis = lerp(1.0, skyVis, probeFade);
        ambient = lerp(ambient, e, probeFade);
    }
    const float3 extinction = exp(-kAbsorb * path);
    // The silt's glow follows the surface's tilt a little: light enters better where a wave faces the sun, and that is most of what shows looking down.
    const float tilt = 1.0 + 2.2 * dot(n.xz, normalize(gLight.sunDirL.xz + float2(1e-4, 0.0)));
    const float3 inScatter = kSilt * (ambient * 1.1 + sunRgb * saturate(sunDir.y) * shadow * cloudSun * max(tilt, 0.15)) * (1.0 - extinction);
    float3 water = scene * extinction + inScatter;

    // ---- reflection: the sky, then the depth buffer where a short march finds something
    const float nv = saturate(dot(n, v));
    const float F = 0.02 + 0.98 * pow(1.0 - nv, 5.0);
    float3 r = reflect(-v, n);
    r.y = abs(r.y);
    float3 reflection = lerp(ambient, SkyLookup(r), skyVis); // where the probes see no sky (under a bank, a bridge), what they do see
    if (F > 0.04 && dist < 400.0) { // a short march in screen space; what the screen does not hold stays sky
        float4 c0 = mul(prevViewProj, float4(i.world, 1.0));
        const float reach = clamp(dist * 0.6, 6.0, 120.0);
        for (int k = 1; k <= 8; ++k) {
            const float t = (float(k) + Hash2(i.pos.xy + misc.y) * 0.8) / 8.0;
            const float3 q = i.world + r * t * t * reach;
            const float4 c = mul(prevViewProj, float4(q, 1.0));
            if (c.w <= 0.0) break;
            const float2 suv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
            if (any(suv < 0.0) || any(suv > 1.0)) break;
            const float d = gDepth.SampleLevel(gClamp, suv, 0);
            if (d <= 0.0) continue;
            const float gap = length(q - eye.xyz) - length(WorldAt(suv, d) - eye.xyz);
            if (gap > 0.0 && gap < reach * 0.25 * t && WorldAt(suv, d).y > i.world.y - 0.2) {
                const float2 fade = saturate(min(suv, 1.0 - suv) * 12.0);
                reflection = lerp(reflection, gRefract.SampleLevel(gClamp, suv, 0).rgb, fade.x * fade.y * (1.0 - t * t));
                break;
            }
        }
    }

    // ---- sun glitter: GGX, its width kept up with the slope lost to distance so far water still glints
    const float3 l = gLight.sunDirL.xyz, h = normalize(v + l);
    const float roughness = clamp(0.055 + sqrt(variance) * 0.9, 0.04, 0.6);
    const float a2 = roughness * roughness * roughness * roughness, nh = saturate(dot(n, h));
    const float dd = nh * nh * (a2 - 1.0) + 1.0;
    const float nl = saturate(dot(n, l));
    const float specular = min(a2 / (kPI * dd * dd) * 0.25 / max(nv * nl + 0.05, 0.05) * nl, 60.0);
    float3 color = lerp(water, reflection, F) + sunRgb * shadow * cloudSun * specular * F * 12.0;

    // ---- foam: the shore's edge, folds of the waves, fast water, and whatever the ripple simulation has stirred up
    // Warped, and octaves turned against each other: plain value noise shows its square lattice as blocks.
    const float2 foamBase = i.rest * 2.3 - i.flow * wc.near.w * 1.3;
    const float2 foamUv = foamBase + (float2(ValueNoise(foamBase * 0.37 + 5.0), ValueNoise(foamBase * 0.37 + 29.0)) - 0.5) * 2.6;
    const float2 foamTurn = float2(foamUv.x * 0.8 - foamUv.y * 0.6, foamUv.x * 0.6 + foamUv.y * 0.8);
    const float grain = ValueNoise(foamUv) * 0.5 + ValueNoise(foamTurn * 2.7 + 13.0) * 0.32 + ValueNoise(foamTurn.yx * 6.1 + 3.0) * 0.18;
    const float shore = saturate(1.0 - column / 0.55) * (0.55 + 0.45 * sin(wc.near.w * 0.8 + column * 16.0 + grain * 6.0));
    const float folds = saturate((0.72 - fold) * 3.0);
    const float rapids = saturate((speed - 1.2) * 0.8);
    const float stirred = saturate(ripple.foam * 1.6);
    const float lace = abs(ValueNoise(foamUv * 1.9 + 41.0) * 2.0 - 1.0); // veins, the way thin foam breaks up
    const float level = saturate(max(max(shore, folds), max(rapids, stirred)));
    const float foamAmount = smoothstep(0.98 - level, 1.42 - level, grain * 0.7 + lace * 0.45) * saturate(level * 2.2) * 0.82; // soft edge, never opaque paint
    const float3 foamLit = float3(0.82, 0.84, 0.82) * (ambient * 1.1 + sunRgb * saturate(dot(n, l)) * shadow * cloudSun);
    color = lerp(color, foamLit, foamAmount * (1.0 - saturate(dist / 400.0)));

    // ---- a soft edge where the water thins to nothing
    const float edge = saturate(column / 0.12);
    color = lerp(gRefract.SampleLevel(gClamp, uv, 0).rgb, color, edge * edge * (3.0 - 2.0 * edge));

    color = color * i.hazeKeep + i.hazeLight;
    return float4(min(color, 2000.0), 0.5);
}
#endif
