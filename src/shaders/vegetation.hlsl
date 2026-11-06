// GPU vegetation placement, after Guerrilla's run-time placement (van Muijden, "GPU-Based Procedural Placement in Horizon
// Zero Dawn", GDC 2017): data-driven rules evaluated around the camera from maps of the ground, world-locked so that the same
// place always gets the same plants. Not their exact algorithm: this one is a jittered grid thinned by a rule's density,
// regenerated every frame in tiles, with no generation cache. Compiled by fxc (cs_5_1) and by dxc (-DVULKAN).
#ifdef VULKAN
#define RO(n) [[vk::binding(n, 0)]]
#define RW(n) [[vk::binding(4 + n, 0)]]
#else
#define RO(n)
#define RW(n)
#endif

struct PlaceConstants {
    float4 planes[4];  // the frustum's side planes, xyz inward, w distance
    float4 eye;        // xyz; w: layer index
    float4 mapMin;     // xy: the map's world minimum, z: meters per texel
    int4   tile;       // xy: first candidate cell of the dispatch (CSArgs: x is the record count); zw: map width and height
    float4 misc;
};
#ifdef VULKAN
[[vk::push_constant]] PlaceConstants pc;
#else
ConstantBuffer<PlaceConstants> pc : register(b0);
#endif

struct VegLayer {
    float cell, reach, lod1, lod2;
    float shadowDistance, scaleMin, scaleMax, density;
    float4 splat;          // rgb: how much each ground weight (earth, grass, rock) counts; a: base value added
    float slopeMin, slopeMax, heightMin, heightMax;
    float noiseFrequency, noiseLo, noiseHi, align;
    uint bucketFirst, variantCount, casterFirst, radiusCm;
};
struct VegBucket { uint first, capacity; float scale, pad; };
struct VegRow { float4 r0, r1, r2; };

RO(0) ByteAddressBuffer gMap : register(t0);        // per texel: float height, then unorm8 earth, grass, rock, slope; slope's top bit excludes
RO(1) StructuredBuffer<VegLayer> gLayers : register(t1);
RO(2) StructuredBuffer<VegBucket> gBuckets : register(t2);
RO(3) ByteAddressBuffer gRecords : register(t3);    // per indirect record: its bucket
RW(0) RWStructuredBuffer<VegRow> gRows : register(u0);
RW(1) RWByteAddressBuffer gCounters : register(u1); // per bucket: instances appended
RW(2) RWByteAddressBuffer gArgs : register(u2);     // indexCount, instanceCount, firstIndex, baseVertex, firstInstance

uint3 Pcg3d(uint3 v) { // Jarzynski and Olano, "Hash Functions for GPU Rendering", JCGT 2020
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
float3 Rand3(int2 cell, uint salt) { return float3(Pcg3d(uint3(uint(cell.x), uint(cell.y), salt))) / 4294967295.0; }

float Noise(float2 p) {
    const float2 floored = floor(p);
    const int2 c = int2(floored);
    float2 f = p - floored;
    f = f * f * (3.0 - 2.0 * f);
    const float a = Rand3(c, 91u).x, b = Rand3(c + int2(1, 0), 91u).x, d = Rand3(c + int2(0, 1), 91u).x, e = Rand3(c + int2(1, 1), 91u).x;
    return lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y);
}

uint TexelOffset(int2 t) { return (uint)(t.y * pc.tile.z + t.x) * 8u; }
float HeightAt(int2 t) { return asfloat(gMap.Load(TexelOffset(clamp(t, int2(0, 0), pc.tile.zw - 1)))); }

[numthreads(64, 1, 1)]
void CSReset(uint3 id : SV_DispatchThreadID) {
    gCounters.Store(id.x * 4u, 0u);
}

// One thread group is one tile of 16 x 16 candidates.
[numthreads(16, 16, 1)]
void CSPlace(uint3 groupId : SV_GroupID, uint3 local : SV_GroupThreadID) {
    const uint layer = (uint)pc.eye.w;
    const VegLayer L = gLayers[layer];
    const int2 cell = pc.tile.xy + int2(groupId.xy) * 16 + int2(local.xy);
    const float3 r0 = Rand3(cell, 11u + layer * 7u);
    const float2 xz = (float2(cell) + r0.xy) * L.cell;
    const float2 away = xz - pc.eye.xz;
    const float distance2 = dot(away, away);
    const float farthest = max(L.reach, L.shadowDistance);
    if (distance2 > farthest * farthest) return;

    const float2 mapPos = (xz - pc.mapMin.xy) / pc.mapMin.z;
    const int2 t = int2(floor(mapPos));
    if (any(t < int2(0, 0)) || any(t >= pc.tile.zw - 1)) return;
    const uint packed = gMap.Load(TexelOffset(t) + 4u);
    if (packed & 0x80000000u) return; // something stands here
    const bool stones = L.density < 0.0; // a negative density marks a stone layer: it ignores the plants' bare-street rule (bit 16)
    if (!stones && (packed & 0x10000u)) return;
    const float3 ground = float3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 254u) / 254.0;
    const float slope = float((packed >> 24) & 127u) / 127.0;
    if (slope < L.slopeMin || slope > L.slopeMax) return;

    float value = dot(ground, L.splat.rgb) + L.splat.a;
    if (L.noiseFrequency > 0.0) value *= smoothstep(L.noiseLo, L.noiseHi, Noise(xz * L.noiseFrequency) * 0.65 + Noise(xz * L.noiseFrequency * 3.1) * 0.35);
    if (stones) { // thrown clear of the wheels and gathered at the street's edges; the same ruts as GroundRuts in mesh.hlsl
        float rut = 0.0;
        for (int k = 0; k < 3; ++k) {
            const float zc = (k - 1.0) * 2.7 + 0.9 * sin(xz.x * 0.021 + k * 2.1) + 0.3 * sin(xz.x * 0.073 + k);
            const float g = (abs(xz.y - zc) - 0.75) / 0.3;
            rut = max(rut, exp(-g * g));
        }
        value *= (1.0 - 0.9 * rut * smoothstep(9.5, 6.0, abs(xz.y))) * lerp(0.3, 1.0, smoothstep(2.5, 9.0, abs(xz.y)));
    }
    if (r0.z >= saturate(value) * abs(L.density)) return;

    const float2 f = mapPos - float2(t);
    const float h00 = HeightAt(t), h10 = HeightAt(t + int2(1, 0)), h01 = HeightAt(t + int2(0, 1)), h11 = HeightAt(t + int2(1, 1));
    const float y = lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
    if (y < L.heightMin || y > L.heightMax) return;

    const float3 r1 = Rand3(cell, 29u + layer * 7u);
    const uint variant = min((uint)(r1.x * float(L.variantCount)), L.variantCount - 1u);
    const uint lodBucket = L.bucketFirst + variant * 3u;
    const float scale = lerp(L.scaleMin, L.scaleMax, r1.y) * gBuckets[lodBucket].scale;
    const float angle = r1.z * 6.2831853;

    // Partly upright, partly with the ground: the y axis leans toward the ground's normal.
    const float3 n = normalize(float3(h00 - h10 + h01 - h11, 2.0 * pc.mapMin.z, h00 - h01 + h10 - h11));
    const float3 up = normalize(lerp(float3(0, 1, 0), n, L.align));
    const float3 right = normalize(cross(up, float3(sin(angle), 0, cos(angle))));
    const float3 forward = cross(right, up);
    VegRow row;
    row.r0 = float4(right.x * scale, up.x * scale, forward.x * scale, xz.x);
    row.r1 = float4(right.y * scale, up.y * scale, forward.y * scale, y - 0.02 * scale + (stones ? 0.05 : 0.0));
    row.r2 = float4(right.z * scale, up.z * scale, forward.z * scale, xz.y);

    const float distance = sqrt(distance2);
    const float radius = float(L.radiusCm) * 0.01 * scale;
    const float3 center = float3(xz.x, y, xz.y);
    bool seen = distance <= L.reach;
    for (int p = 0; p < 4; ++p) seen = seen && dot(pc.planes[p].xyz, center) + pc.planes[p].w > -radius;
    if (seen) {
        // A little of the hash in the distance, so a LOD's edge is a band and not a line.
        const float d = distance * (0.92 + 0.16 * r0.x);
        const uint b = lodBucket + (d < L.lod1 ? 0u : (d < L.lod2 ? 1u : 2u));
        uint slot;
        gCounters.InterlockedAdd(b * 4u, 1u, slot);
        const VegBucket bucket = gBuckets[b];
        if (slot < bucket.capacity) gRows[bucket.first + slot] = row;
    }
    if (distance < L.shadowDistance) {
        const uint b = L.casterFirst + variant;
        uint slot;
        gCounters.InterlockedAdd(b * 4u, 1u, slot);
        const VegBucket bucket = gBuckets[b];
        if (slot < bucket.capacity) gRows[bucket.first + slot] = row;
    }
}

[numthreads(64, 1, 1)]
void CSArgs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)pc.tile.x) return;
    const uint b = gRecords.Load(id.x * 4u);
    gArgs.Store(id.x * 20u + 4u, min(gCounters.Load(b * 4u), gBuckets[b].capacity));
}
