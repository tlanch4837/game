// Sampling the irradiance probe grid; shared by the lit shaders and the post passes. The includer defines PROBE_ORIGIN,
// PROBE_GRID (see Lighting in renderer.h) and binds gProbes. After Majercik et al., "Dynamic Diffuse Global Illumination
// with Ray-Traced Irradiance Fields" (JCGT 2019), with first-order storage in place of octahedral maps: one texel of mean
// colour, one of the luminance gradient (a shared luminance direction), one of visibility. Two cascades: the second has
// cells four times as wide around the same focus, so the light carries on past the first without a seam. An includer that
// also defines PROBE_ROOMS and PROBE_ROOM_COUNT (the building interiors, InteriorRoom in renderer.h) keeps light from
// crossing their walls: a point reads the near probes of its own room, or of none.
#ifndef TBD_PROBE_HLSLI
#define TBD_PROBE_HLSLI

int3 ProbeIndex(int3 cell) {
    // Floor division in floats: integer % is slow on a GPU, and this runs for every lit pixel.
    float3 n = PROBE_GRID.xyz, c = float3(cell);
    return int3(c - n * floor(c / n));
}
float4 ProbeTexel(int3 cell, int panel, int cascade) {
    int3 i = ProbeIndex(cell);
    int3 n = int3(PROBE_GRID.xyz);
    return gProbes.Load(int3(i.x * 3 + panel, i.z + (i.y + cascade * n.y) * n.z, 0));
}
float3 ProbeSpacing() { return float3(PROBE_ORIGIN.w, PROBE_GRID.w, PROBE_ORIGIN.w); }
float3 ProbeHalf() { return float3(floor(PROBE_GRID.x * 0.5), 3.0, floor(PROBE_GRID.z * 0.5)); } // as ComputeProbeGrid places the grid
float3 ProbeCascadeOrigin(float3 origin, int cascade) {
    return cascade == 0 ? origin : floor((origin + ProbeHalf()) * 0.25) - ProbeHalf();
}
float ProbeScale(int cascade) { return cascade == 0 ? 1.0 : 4.0; }

#ifdef PROBE_ROOM_COUNT
// The room a point is in, -1 for none; `margin` is the slack across (x and z) and up, in metres.
int ProbeRoomOf(float3 p, float2 margin) {
    for (int k = 0; k < int(PROBE_ROOM_COUNT); ++k) {
        float3 c = PROBE_ROOMS[k * 3].xyz, ax = PROBE_ROOMS[k * 3 + 1].xyz, az = PROBE_ROOMS[k * 3 + 2].xyz, o = p - c;
        float ex = length(ax), ez = length(az);
        if (abs(dot(o, ax)) <= ex * (ex + margin.x) && abs(o.y) <= PROBE_ROOMS[k * 3].w + margin.y && abs(dot(o, az)) <= ez * (ez + margin.x)) return k;
    }
    return -1;
}
#endif

// How far inside a cascade a point is: 0 outside, 1 three cells in, eased so no line shows on the ground where the probes end.
float ProbeFade(float3 g, int cascade) {
    float3 loc = g - ProbeCascadeOrigin(PROBE_ORIGIN.xyz, cascade), m = min(loc, PROBE_GRID.xyz - 1.0 - loc);
    float horizontal = smoothstep(0.0, 3.0, min(m.x, m.z));
    return horizontal * saturate(m.y);
}

// One cascade's trilinear lookup. The lookup is pushed off the surface by a fraction of a cell, which keeps a wall from
// reading the probes behind it, and a probe behind the surface counts for little (DDGI's smooth backface weight).
float3 ProbeCascade(float3 world, float3 n, int cascade, out float skyVis) {
    float3 spacing = ProbeSpacing() * ProbeScale(cascade);
    float3 g = (world + n * 0.45 * spacing.x) / spacing;
    int3 c = int3(floor(g));
    float3 f = g - floor(g), col = 0.0, dir = 0.0;
    float vis = 0.0, total = 0.0;
    // The wrap once per axis rather than once per texel: this runs for every lit pixel.
    int3 size = int3(PROBE_GRID.xyz), i0 = ProbeIndex(c), i1 = i0 + 1;
    i1 = int3(i1.x == size.x ? 0 : i1.x, i1.y == size.y ? 0 : i1.y, i1.z == size.z ? 0 : i1.z);
#ifdef PROBE_ROOM_COUNT
    // Near a room, a probe on the other side of its walls counts for almost nothing. A probe belongs to a room with the slack
    // RoomTrace in post.hlsl gives it (it traces from inside); -2 skips the test where no room is within a cell.
    int room = cascade == 0 && PROBE_ROOM_COUNT > 0.5 && ProbeRoomOf(world, spacing.xy + 0.6) >= 0 ? ProbeRoomOf(world, 0.3) : -2;
#endif
    for (int k = 0; k < 8; ++k) {
        int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        float3 t = lerp(1.0 - f, f, float3(o));
        float back = saturate(dot(float3(o) - f, n) + 0.5); // no square root: only which side of the surface matters
        float w = t.x * t.y * t.z * (back + 0.1);
#ifdef PROBE_ROOM_COUNT
        if (room > -2 && ProbeRoomOf(float3(c + o) * spacing, float2(0.5, 1.6)) != room) w *= 0.02;
#endif
        total += w;
        if (w < 0.004) continue;
        int3 idx = int3(o.x ? i1.x : i0.x, o.y ? i1.y : i0.y, o.z ? i1.z : i0.z);
        int2 texel = int2(idx.x * 3, idx.z + (idx.y + cascade * size.y) * size.z);
        col += w * gProbes.Load(int3(texel, 0)).rgb;
        dir += w * gProbes.Load(int3(texel + int2(1, 0), 0)).rgb;
        vis += w * gProbes.Load(int3(texel + int2(2, 0), 0)).g;
    }
    col /= total; dir /= total;
    skyVis = vis / total;
    float lum = max(dot(col, float3(0.2126, 0.7152, 0.0722)), 1e-4);
    return col * max(1.0 + dot(dir, n) * (2.0 / 3.0) / lum, 0.1);
}

// The far cascade, for most of a wide view, kept cheap: colour bilinear at the nearest height, gradient and sky visibility from the nearest probe (6 loads, not 24).
float3 ProbeFar(float3 world, float3 n, out float skyVis) {
    float3 spacing = ProbeSpacing() * 4.0;
    float3 g = (world + n * 0.45 * spacing.x) / spacing;
    int3 c = int3(floor(g));
    float3 f = g - floor(g), col = 0.0;
    const int row = int(PROBE_GRID.y), rowZ = int(PROBE_GRID.z);
    float yn = round(g.y); // far cells are 12 m tall and light barely changes across one: four corners at the nearest height
    for (int k = 0; k < 4; ++k) {
        int2 o = int2(k & 1, k >> 1);
        float2 t = lerp(1.0 - f.xz, f.xz, float2(o));
        int3 i = ProbeIndex(int3(c.x + o.x, int(yn), c.z + o.y));
        col += t.x * t.y * gProbes.Load(int3(i.x * 3, i.z + (i.y + row) * rowZ, 0)).rgb;
    }
    int3 i = ProbeIndex(int3(round(g)));
    int3 at = int3(i.x * 3, i.z + (i.y + row) * rowZ, 0);
    float3 dir = gProbes.Load(at + int3(1, 0, 0)).rgb;
    skyVis = gProbes.Load(at + int3(2, 0, 0)).g;
    float lum = max(dot(col, float3(0.2126, 0.7152, 0.0722)), 1e-4);
    return col * max(1.0 + dot(dir, n) * (2.0 / 3.0) / lum, 0.1);
}

// Irradiance / pi at a point: the near cascade, melting into the far one, and `fade` for what lies beyond both.
float3 ProbeIrradiance(float3 world, float3 n, out float fade, out float skyVis) {
    float3 s = ProbeSpacing();
    float near = ProbeFade((world + n * 0.45 * s.x) / s, 0);
    float far = ProbeFade((world + n * 1.8 * s.x) / (s * 4.0), 1);
    fade = max(near, far);
    skyVis = 1.0;
    if (fade <= 0.0) return 0.0;
    float visA = 1.0, visB = 1.0;
    float3 a = 0.0, b = 0.0;
    if (near > 0.0) a = ProbeCascade(world, n, 0, visA);
    if (near < 1.0) b = ProbeFar(world, n, visB); // both only in the band one near cell wide
    skyVis = lerp(visB, visA, near);
    return lerp(b, a, near);
}

// Mean light and how much of the sun gets through at the nearest probe, for the haze; its jitter and TAA hide the cells.
void ProbeHaze(float3 world, out float3 mean, out float sunVis, out float fade) {
    float3 s = ProbeSpacing();
    mean = 0.0; sunVis = 1.0;
    int cascade = ProbeFade(world / s, 0) > 0.5 ? 0 : 1;
    float3 g = world / (s * ProbeScale(cascade));
    fade = ProbeFade(g, cascade);
    if (fade <= 0.0) return;
    int3 c = int3(round(g));
    mean = ProbeTexel(c, 0, cascade).rgb;
    sunVis = ProbeTexel(c, 2, cascade).r;
}
#endif
