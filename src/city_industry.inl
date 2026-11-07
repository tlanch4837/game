// ------------------------------------------------------------------- specialized industry (src/city/industry.h)
// Included by main.cpp after city_zones.inl. An "Industry" launcher beside Zoning opens the tool:
//   pick a type      grain, livestock, timber, oil, ore, landfill; the natural-resource heat map for it covers the terrain;
//   place the hub    it snaps beside the nearest road (3 x 4 cells), green when free;
//   draw the area    the polygon starts at the hub's back edge; click to add corners, click the first corner,
//                    the last one again, Enter or right click to finish; Backspace removes a corner;
//   edit             click a site to select it, drag its corners, Delete removes it.
// Efficiency = the mean resource value under the area, so the heat map decides where a site produces well.
// city/industry.h is included at the top of main.cpp (outside its anonymous namespace)
#include "city/industry_spom.h" // SPOM volume table (tools/building_gen/industry_spom.py)

tbd::city::IndustryWorld g_ind;
struct IndustryTool {
    enum Mode { Pick, Place, Draw };
    bool active = false;
    bool waste = false; // opened from Waste Management: landfill (hub + drawn area) and the point buildings
    int point = 0;      // waste mode: 0 = landfill tool, else a WasteType index + 1 being placed
    int wsel = -1;      // waste mode: selected point building
    Mode mode = Pick;
    int kind = 0;
    bool have = false;
    tbd::city::V2 cur;
    tbd::city::IndSite ghost;
    bool ghostOk = false, ghostShown = false;
    int drawing = -1, sel = -1, dragIdx = -1; // dragIdx: index into sites[sel].area
} g_indTool;

// The terrain mesh is coarser than the height field: take the highest ground nearby so draped geometry never sinks into it.
float IndGround(float x, float z) {
    float y = RoadsGround(x, z);
    for (float o : {-2.5f, 2.5f}) y = std::max({y, RoadsGround(x + o, z), RoadsGround(x, z + o)});
    return y;
}
// A hub sits on the highest ground under its footprint; its foundation reaches down into the slope instead of digging.
float HubHeight(const tbd::city::IndSite& s) {
    float y = -1e30f;
    for (float a = -0.5f; a <= 0.51f; a += 0.25f)
        for (float b = -0.5f; b <= 0.51f; b += 0.25f) {
            const tbd::city::V2 q = s.c + s.Side() * (a * tbd::city::kHubW) + s.front * (b * tbd::city::kHubD);
            y = std::max(y, IndGround(q.x, q.z));
        }
    return y;
}

namespace ind {
using tbd::Vec3;
using tbd::Vertex;
using tbd::city::V2;

// SPOM slab slots per kind (tools/building_gen/industry_tex.py -> assets/industry/tex/<kind>_<slot>.png): 0..2 walls, 3 roof.
// The vertex alpha picks the slot; the host draws each slot apart with that kind's texture bound (SlabPS in mesh.hlsl).
constexpr float kSlabA[4] = {0.809f, 0.811f, 0.813f, 0.821f};
constexpr float kSlabD = 0.8f; // = kSlabDepth in mesh.hlsl
inline int SlabSlot(float a) {
    if (a <= 0.806f || a >= 0.824f) return -1;
    for (int k = 0; k < 4; ++k)
        if (std::fabs(a - kSlabA[k]) < 0.0009f) return k;
    return -1;
}

// A triangle lit with its normal turned away from `in` (a point inside the solid).
inline void TriC(std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c, Vec3 in, Vec3 col) {
    Vec3 n = tbd::Cross(b - a, c - a);
    const float l = std::sqrt(tbd::Dot(n, n));
    if (l < 1e-7f) return;
    n = n * (1.0f / l);
    if (tbd::Dot(n, (a + b + c) * (1.0f / 3.0f) - in) < 0) n = n * -1.0f;
    zfx::Tri(o, a, b, c, n, col);
}
inline void TriN(std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c, Vec3 col) {
    Vec3 n = tbd::Cross(b - a, c - a);
    const float l = std::sqrt(tbd::Dot(n, n));
    n = l > 1e-6f ? n * (1.0f / l) : Vec3{0, 1, 0};
    if (n.y < -0.2f) n = n * -1.0f;
    zfx::Tri(o, a, b, c, n, col);
}
// A thin bar as two crossed quads (4 triangles, seen from any side): rails, rods, wires, braces, reel bats.
inline void Card(std::vector<Vertex>& o, Vec3 a, Vec3 b, float w, Vec3 col) {
    const Vec3 d = tbd::Normalize(b - a);
    Vec3 u = tbd::Cross(d, Vec3{0, 1, 0});
    if (tbd::Dot(u, u) < 1e-4f) u = Vec3{1, 0, 0};
    u = tbd::Normalize(u) * (w * 0.5f);
    const Vec3 v = tbd::Normalize(tbd::Cross(d, u)) * (w * 0.5f);
    for (Vec3 e : {u, v}) {
        const Vec3 n = tbd::Normalize(tbd::Cross(d, e));
        zfx::Quad(o, a - e, b - e, b + e, a + e, n.y < -0.2f ? n * -1.0f : n, col);
    }
}
// A bar: thin ones (most rails, braces, pipes) are cards, thick ones a box.
inline void Beam(std::vector<Vertex>& o, Vec3 a, Vec3 b, float t, Vec3 col) {
    if (t <= 0.3f) Card(o, a, b, t, col);
    else zfx::Beam(o, a, b, t, col);
}
inline void QuadN(std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 col) { TriN(o, a, b, c, col), TriN(o, a, c, d, col); }
inline void Cyl(std::vector<Vertex>& o, Vec3 base, float r, float h, Vec3 col, int seg = 8, bool cap = true) {
    seg = std::min(seg, 8); // never above 8 sides: the round reads from the shading, not the facets
    for (int i = 0; i < seg; ++i) {
        const float a0 = i * 6.2831853f / seg, a1 = (i + 1) * 6.2831853f / seg;
        const Vec3 p0 = base + Vec3{std::cos(a0) * r, 0, std::sin(a0) * r}, p1 = base + Vec3{std::cos(a1) * r, 0, std::sin(a1) * r};
        const Vec3 n = tbd::Normalize(Vec3{std::cos((a0 + a1) * 0.5f), 0, std::sin((a0 + a1) * 0.5f)});
        zfx::Quad(o, p0, p1, p1 + Vec3{0, h, 0}, p0 + Vec3{0, h, 0}, n, col);
        if (cap) zfx::Tri(o, base + Vec3{0, h, 0}, p0 + Vec3{0, h, 0}, p1 + Vec3{0, h, 0}, {0, 1, 0}, col);
    }
}
inline void Cone(std::vector<Vertex>& o, Vec3 base, float r, float h, Vec3 col, int seg = 8) {
    const Vec3 top = base + Vec3{0, h, 0};
    for (int i = 0; i < seg; ++i) {
        const float a0 = i * 6.2831853f / seg, a1 = (i + 1) * 6.2831853f / seg;
        TriN(o, base + Vec3{std::cos(a0) * r, 0, std::sin(a0) * r}, base + Vec3{std::cos(a1) * r, 0, std::sin(a1) * r}, top, col);
    }
}
// A side profile (along fd, up) extruded across sd from s0 to s1: the low-poly vehicle/animal body. The profile must be star
// shaped about its centroid; edges lying on the ground (both ends at h <= 0) are skipped.
struct PF { float a, h; };
inline void Extrude(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd, const PF* p, int n, float s0, float s1, Vec3 col, bool caps = true) {
    const Vec3 up{0, 1, 0};
    float ca = 0, ch = 0;
    for (int i = 0; i < n; ++i) ca += p[i].a, ch += p[i].h;
    ca /= n, ch /= n;
    auto P = [&](float a, float h, float s) { return g + fd * a + up * h + sd * s; };
    const Vec3 in = P(ca, ch, 0.5f * (s0 + s1));
    for (int i = 0; i < n; ++i) {
        const PF a = p[i], b = p[(i + 1) % n];
        if (a.h <= 0.001f && b.h <= 0.001f) continue;
        TriC(o, P(a.a, a.h, s0), P(b.a, b.h, s0), P(b.a, b.h, s1), in, col);
        TriC(o, P(a.a, a.h, s0), P(b.a, b.h, s1), P(a.a, a.h, s1), in, col);
        if (caps)
            for (float s : {s0, s1}) TriC(o, P(ca, ch, s), P(a.a, a.h, s), P(b.a, b.h, s), P(ca, ch, 0.5f * (s0 + s1)), col);
    }
}
// A wheel / drum / reel: an n-gon in the (fd, up) plane round (a, h), across sd from s0 to s1.
inline void Wheel(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd, float a, float h, float r, float s0, float s1, Vec3 col, int seg = 6) {
    PF p[12];
    seg = std::min(seg, 12);
    for (int i = 0; i < seg; ++i) {
        const float t = (i + 0.5f) * 6.2831853f / seg;
        p[i] = {a + std::cos(t) * r, h + std::sin(t) * r};
    }
    Extrude(o, g, fd, sd, p, seg, s0, s1, col);
}
// ---- SPOM volumes (SpomPS in mesh.hlsl): a 12-triangle box whose whole shape is a two-layer relief texture
// (tools/building_gen/industry_spom.py, table in city/industry_spom.h). Vertex: normal = the volume's N, its LENGTH - 1 carrying
// which texture (volume id * 2 + mirrored) so the host can draw each with its own; uv = metres along T and up; colour = (W, H, z).
// g: the object's origin on the ground; fd: its "along" axis (the volume's a); N: out of the volume (horizontal, unit).
inline void SpomVolume(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 N, int id, float length = 0.0f) {
    const indspom::Vol& V = indspom::kVol[id];
    const Vec3 up{0, 1, 0}, T = tbd::Normalize(tbd::Cross(up, N));
    const bool mirror = tbd::Dot(T, fd) < 0; // T runs the other way: start at the far end with the mirrored texture
    const float L = length > 0 ? length : V.L; // a fence run may be shorter or longer than its texture: the relief stretches
    const Vec3 O = g + fd * (mirror ? V.a0 + L : V.a0) + N * V.sIn;
    const float W = L, H = V.H, D = 1.2f;
    const Vec3 nEnc = N * (1.0f + (float)(id * 2 + (mirror ? 1 : 0)));
    auto P = [&](int i, int j, int k) { return O + T * (i * W) + up * (j * H) + N * (k * D); };
    const Vec3 c = O + T * (W * 0.5f) + up * (H * 0.5f) + N * (D * 0.5f);
    static const int faces[6][4][3] = {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}, {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}},
                                       {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}, {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},
                                       {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}, {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}};
    for (const auto& f : faces) {
        int idx[4] = {0, 1, 2, 3};
        const Vec3 a = P(f[0][0], f[0][1], f[0][2]), b = P(f[1][0], f[1][1], f[1][2]), d = P(f[3][0], f[3][1], f[3][2]);
        if (tbd::Dot(tbd::Cross(b - a, d - a), (a + b + d) * (1.0f / 3.0f) - c) < 0) idx[1] = 3, idx[3] = 1; // outward winding
        auto vtx = [&](int q) {
            const auto& k = f[idx[q]];
            const Vec3 p = P(k[0], k[1], k[2]);
            o.push_back({p.x, p.y, p.z, nEnc.x, nEnc.y, nEnc.z, W, H, k[2] * D, 0.8f, k[0] * W, k[1] * H});
        };
        vtx(0), vtx(1), vtx(2), vtx(0), vtx(2), vtx(3);
    }
}
// Which SPOM texture a vertex wants (-1: not a SPOM volume).
inline int SpomTexOf(const Vertex& v) {
    if (std::fabs(v.a - 0.8f) > 0.002f) return -1;
    const int k = (int)std::lround(std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz) - 1.0f);
    return k >= 0 && k < indspom::COUNT * 2 ? k : -1;
}
// Both side volumes of a vehicle (left: +sd, right: -sd).
inline void SpomSides(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd, int idLeft, int idRight) {
    SpomVolume(o, g, fd, sd, idLeft);
    SpomVolume(o, g, fd, sd * -1.0f, idRight);
}
// One fence volume from a to b (the fence stands on the line; its relief sits 0.6 m into the volume).
inline void FencePiece(std::vector<Vertex>& o, Vec3 a, Vec3 b, int id) {
    Vec3 d = b - a;
    d.y = 0;
    const float l = std::sqrt(tbd::Dot(d, d));
    if (l < 0.3f) return;
    d = d * (1.0f / l);
    const Vec3 N{d.z, 0, -d.x};
    SpomVolume(o, a - N * 0.6f, d, N, id, l);
}
// A straight run split into pieces of about the texture's length (8 m), heights interpolated.
inline void FenceRun(std::vector<Vertex>& o, Vec3 a, Vec3 b, int id) {
    const float l = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    const int n = std::max(1, (int)std::lround(l / indspom::kVol[id].L));
    for (int i = 0; i < n; ++i) FencePiece(o, a + (b - a) * ((float)i / n), a + (b - a) * ((float)(i + 1) / n), id);
}
// A tyre with its steel rim showing on the outer face, and a tread band: reads as a real wheel, not a black disc.
inline void Tyre(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd, float a, float h, float r, float s0, float s1, int seg) {
    Wheel(o, g, fd, sd, a, h, r, s0, s1, {0.09f, 0.09f, 0.1f}, seg);
    const float out = std::fabs(s1) > std::fabs(s0) ? s1 : s0, dir = out > 0 ? 1.0f : -1.0f;
    Wheel(o, g, fd, sd, a, h, r * 0.58f, out - dir * 0.02f, out + dir * 0.03f, {0.5f, 0.51f, 0.52f}, 6); // rim
}
// An oriented box (no bottom): centre-bottom g, half sizes along fd / sd, from y0 to y1 above g.
inline void OBox(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd, float hl, float hw, float y0, float y1, Vec3 col) {
    const PF p[4] = {{-hl, y0}, {hl, y0}, {hl, y1}, {-hl, y1}};
    if (y0 > 0.001f) { // lifted: keep its underside
        Extrude(o, g + Vec3{0, 0, 0}, fd, sd, p, 4, -hw, hw, col);
        const Vec3 a = g + fd * -hl + sd * -hw + Vec3{0, y0, 0}, b = g + fd * hl + sd * -hw + Vec3{0, y0, 0};
        const Vec3 c = g + fd * hl + sd * hw + Vec3{0, y0, 0}, d = g + fd * -hl + sd * hw + Vec3{0, y0, 0};
        const Vec3 in = g + Vec3{0, 0.5f * (y0 + y1), 0};
        TriC(o, a, b, c, in, col), TriC(o, a, c, d, in, col);
    } else
        Extrude(o, g, fd, sd, p, 4, -hw, hw, col);
}

// One SPOM slab face: from corner a along horizontal unit T for len metres, across it ext metres (up for a wall, along
// (T.z, 0, -T.x) for a roof), the relief kSlabD thick out of it. 10 triangles; the detail is the texture's.
inline void SlabFace(std::vector<Vertex>& o, Vec3 a, Vec3 T, float len, float ext, float period, float alpha) {
    const bool roof = alpha > 0.815f;
    const Vec3 N = roof ? Vec3{0, 1, 0} : Vec3{-T.z, 0, T.x}, B = roof ? Vec3{T.z, 0, -T.x} : Vec3{0, 1, 0};
    auto vtx = [&](float t, float b, float z) {
        const Vec3 p = a + T * t + B * b + N * z;
        o.push_back({p.x, p.y, p.z, T.x, 0, T.z, z, period, ext, alpha, t, b});
    };
    auto quad = [&](float t0, float b0, float z0, float t1, float b1, float z1, float t2, float b2, float z2, float t3, float b3, float z3) {
        vtx(t0, b0, z0), vtx(t1, b1, z1), vtx(t2, b2, z2);
        vtx(t0, b0, z0), vtx(t2, b2, z2), vtx(t3, b3, z3);
    };
    const float D = kSlabD;
    quad(0, 0, D, len, 0, D, len, ext, D, 0, ext, D);
    quad(0, 0, 0, 0, ext, 0, 0, ext, D, 0, 0, D);
    quad(len, 0, 0, len, ext, 0, len, ext, D, len, 0, D);
    quad(0, 0, 0, len, 0, 0, len, 0, D, 0, 0, D);
    quad(0, ext, 0, len, ext, 0, len, ext, D, 0, ext, D);
}

// A hub-local frame in the prototype's units (industry_zones.html): x -6..6 across, z -16 (back) .. 0 (road), 1 unit = 2 m.
// Box / Roof sizes w, d are full widths in units (= half widths in metres).
struct Frame {
    Vec3 c, fz, fx;
    float y;
    std::vector<Vertex>* o;
    Vec3 At(float x, float z, float h) const { return Vec3{c.x, y, c.z} + fx * (x * 2) + fz * ((z + 8) * 2) + Vec3{0, h * 2, 0}; }
    void Box(float x, float z, float w, float d, float y0, float h, Vec3 col) const { zfx::Block(*o, At(x, z, 0), fz, fx, d, w, y + y0 * 2, y + (y0 + h) * 2, col); }
    void Cyl(float x, float z, float r, float y0, float h, Vec3 col, int seg = 10, bool cap = true) const { ind::Cyl(*o, At(x, z, y0), r * 2, h * 2, col, seg, cap); }
    void Cone(float x, float z, float r, float y0, float h, Vec3 col, int seg = 8) const { ind::Cone(*o, At(x, z, y0), r * 2, h * 2, col, seg); }
    void Beam(Vec3 a, Vec3 b, float t, Vec3 col) const { ind::Beam(*o, At(a.x, a.z, a.y), At(b.x, b.z, b.y), t * 2, col); }
    // A building of SPOM slabs: walls facing the road / back use slot sx (repeat px metres, 0 = once across), the side walls
    // slot sz (pz), and a flat slab roof of slot roof (repeat pr) unless roof < 0 (then a pitched roof goes on at y0 + h).
    void SlabBox(float x, float z, float w, float d, float y0, float h, int sx, float px, int sz, float pz, int roof = -1, float pr = 6, bool roofAlongX = true, float ov = 0.2f) const {
        const float D = kSlabD, hx = w - D, hz = d - D, H = h * 2;
        const Vec3 C = At(x, z, y0);
        auto wall = [&](Vec3 n, float dist, float half, int slot, float per) {
            const Vec3 T{n.z, 0, -n.x};
            SlabFace(*o, C + n * dist - T * half, T, 2 * half, H, per > 0 ? per : 2 * half, kSlabA[slot]);
        };
        wall(fz, hz, hx + D, sx, px), wall(fz * -1.0f, hz, hx + D, sx, px);
        wall(fx, hx, hz, sz, pz), wall(fx * -1.0f, hx, hz, sz, pz);
        if (roof < 0) return;
        const Vec3 T = roofAlongX ? fx : fz, B{T.z, 0, -T.x};
        const float hl = (roofAlongX ? w : d) + ov, he = (roofAlongX ? d : w) + ov;
        SlabFace(*o, C + Vec3{0, H, 0} - T * hl - B * he, T, 2 * hl, 2 * he, pr, kSlabA[roof]);
    }
    // A roof of a cross-section profile (pairs of (fraction of the half width, rise in units)) over a w x d top at y0, ridge
    // along z or x, with the gable ends filled in `gable`. {1,0},{0,rise},{-1,0} is a gable roof; five points a gambrel.
    void ProfRoof(float x, float z, float w, float d, float y0, const PF* p, int n, bool ridgeZ, Vec3 roof, Vec3 gable, float ov = 0.2f) const {
        // across the ridge is the box's z depth when the ridge runs along x
        const float hw = (ridgeZ ? w : d) * 0.5f + ov, hd = (ridgeZ ? d : w) * 0.5f + ov;
        auto Q =[&](float across, float along, float h) { return ridgeZ ? At(x + across, z + along, y0 + h) : At(x + along, z + across, y0 + h); };
        const Vec3 in = Q(0, 0, p[n / 2].h * 0.4f);
        for (int i = 0; i + 1 < n; ++i) {
            const Vec3 a = Q(p[i].a * hw, -hd, p[i].h), b = Q(p[i + 1].a * hw, -hd, p[i + 1].h), c2 = Q(p[i + 1].a * hw, hd, p[i + 1].h), d2 = Q(p[i].a * hw, hd, p[i].h);
            TriC(*o, a, b, c2, in, roof), TriC(*o, a, c2, d2, in, roof);
        }
        for (float e : {-1.0f, 1.0f}) { // gables, set back to the wall face
            const float al = e * (hd - ov);
            for (int i = 1; i + 1 < n; ++i) TriC(*o, Q(p[0].a * (hw - ov), al, 0), Q(p[i].a * (hw - ov), al, p[i].h), Q(p[i + 1].a * (hw - ov), al, p[i + 1].h), in, gable);
        }
    }
    void Gable(float x, float z, float w, float d, float y0, float rise, bool ridgeZ, Vec3 roof, Vec3 gable, float ov = 0.2f) const {
        const PF p[3] = {{1, 0}, {0, rise}, {-1, 0}};
        ProfRoof(x, z, w, d, y0, p, 3, ridgeZ, roof, gable, ov);
    }
};
Frame FrameOf(const tbd::city::IndSite& s, std::vector<Vertex>& o) {
    const Vec3 fz{s.front.x, 0, s.front.z}, fx{s.Side().x, 0, s.Side().z};
    return {{s.c.x, s.y, s.c.z}, fz, fx, s.y, &o};
}

// Real-world, weathered tones (paint fades, steel greys, nothing fully saturated).
const Vec3 kWhite{0.83f, 0.82f, 0.78f}, kCream{0.8f, 0.76f, 0.66f}, kRed{0.52f, 0.2f, 0.16f}, kRedD{0.4f, 0.15f, 0.12f}, kRoof{0.33f, 0.35f, 0.37f},
    kRoofR{0.45f, 0.21f, 0.18f}, kMetal{0.66f, 0.68f, 0.69f}, kSteel{0.45f, 0.47f, 0.49f}, kSteelD{0.27f, 0.29f, 0.3f}, kYellow{0.8f, 0.6f, 0.17f},
    kBlueD{0.17f, 0.24f, 0.32f}, kWood{0.62f, 0.5f, 0.36f}, kBark{0.34f, 0.26f, 0.19f}, kConc{0.66f, 0.65f, 0.61f}, kBrick{0.52f, 0.29f, 0.22f},
    kOre{0.42f, 0.27f, 0.21f}, kGreen{0.24f, 0.36f, 0.22f}, kGravel{0.56f, 0.53f, 0.48f}, kTyre{0.09f, 0.09f, 0.1f}, kGlass{0.1f, 0.13f, 0.16f},
    kShingle{0.24f, 0.24f, 0.25f}, kAsph{0.27f, 0.27f, 0.28f}, kRim{0.55f, 0.56f, 0.57f}, kDirt{0.46f, 0.4f, 0.32f};

// A corrugated grain bin / silo: wall, a few stiffener rings, a shallow cone roof with its eave.
void Silo(const Frame& F, float x, float z, float r, float h, float roofH, Vec3 wall, Vec3 ring, int rings) {
    (void)rings;
    F.Cyl(x, z, r, 0, h, wall, 8, false);
    F.Cone(x, z, r * 1.06f, h, roofH, wall * 1.05f, 8);
    F.Cyl(x, z, r * 0.16f, h + roofH * 0.82f, 0.25f, ring, 4); // vent cap
}

// The hub building (static parts). Walls and flat roofs are SPOM slabs; triangles only for the silhouette (pitched roofs,
// tanks, towers, structure).
void Hub(const tbd::city::IndSite& s, std::vector<Vertex>& o) {
    const Frame F = FrameOf(s, o);
    using K = tbd::city::IndKind;
    // yards are what real ones are: farm gravel and trodden dirt, mill yards of bark and grit, oil-field caliche, mine
    // fines, landfill hardstanding
    const Vec3 pad = s.kind == K::Landfill ? Vec3{0.36f, 0.35f, 0.34f} : s.kind == K::Oil ? Vec3{0.62f, 0.58f, 0.5f} : s.kind == K::Ore ? Vec3{0.45f, 0.41f, 0.38f}
                   : s.kind == K::Timber ? Vec3{0.43f, 0.36f, 0.28f} : Vec3{0.5f, 0.47f, 0.41f};
    F.Box(0, -8, 12, 16, -8.0f, 8.06f, pad); // deep skirt: the foundation fills the slope below
    F.Box(0, -0.9f, 3.0f, 1.8f, 0.03f, 0.01f, kAsph); // the drive in from the road
    if (s.kind == K::Grain || s.kind == K::Livestock) { // a mown strip of lawn round the house, worn wheel tracks across the yard
        F.Box(-3.6f, -3.6f, 5.4f, 4.6f, 0.035f, 0.01f, {0.33f, 0.4f, 0.24f});
        for (float x : {1.7f, 3.1f}) F.Box(x, -2.8f, 0.3f, 2.8f, 0.04f, 0.005f, kDirt);
    }
    switch (s.kind) {
    case K::Grain: {
        // farmhouse: two storeys of lap siding, steep shingle roof, porch, chimney
        F.SlabBox(-3.4f, -3.8f, 3.8f, 3.4f, 0.03f, 2.6f, 2, 3.6f, 2, 3.6f);
        F.Gable(-3.4f, -3.8f, 3.8f, 3.4f, 2.63f, 1.5f, false, kShingle, kCream);
        F.Box(-3.4f, -1.75f, 2.6f, 0.75f, 0.03f, 0.1f, kWood), F.Box(-3.4f, -1.55f, 2.8f, 0.85f, 1.45f, 0.08f, kShingle);
        for (float x : {-4.6f, -2.2f}) F.Box(x, -1.3f, 0.08f, 0.08f, 0.1f, 1.35f, kWhite);
        F.Box(-4.6f, -4.2f, 0.5f, 0.5f, 2.6f, 1.9f, kBrick);
        // gambrel barn, ridge along the lot
        F.SlabBox(2.4f, -9.0f, 5, 7, 0.03f, 3.5f, 1, 0, 0, 7);
        const PF gam[5] = {{1, 0}, {0.62f, 1.25f}, {0, 2.0f}, {-0.62f, 1.25f}, {-1, 0}};
        F.ProfRoof(2.4f, -9.0f, 5, 7, 3.53f, gam, 5, true, kRoof, kRed, 0.25f);
        F.Box(2.4f, -9.0f, 0.5f, 6.0f, 5.53f, 0.18f, kSteelD); // ridge cap
        F.Cyl(2.4f, -9.0f, 0.35f, 5.6f, 0.6f, kWhite, 6), F.Cone(2.4f, -9.0f, 0.5f, 6.2f, 0.45f, kRoof, 4); // cupola
        // grain bins and the bucket elevator leg with its spouts
        Silo(F, -4.0f, -12.8f, 1.15f, 6.0f, 1.0f, kMetal, kSteel, 3);
        Silo(F, -1.5f, -14.0f, 1.0f, 5.4f, 0.9f, kMetal, kSteel, 3);
        Silo(F, -4.6f, -9.3f, 0.8f, 3.6f, 0.7f, kMetal, kSteel, 2);
        F.Box(-2.9f, -11.4f, 0.45f, 0.45f, 0, 9.6f, kSteel);
        F.Box(-2.9f, -11.4f, 0.9f, 0.9f, 9.6f, 0.7f, kSteelD);
        F.Beam({-2.9f, 9.9f, -11.4f}, {-4, 7.4f, -12.8f}, 0.12f, kSteelD);
        F.Beam({-2.9f, 9.9f, -11.4f}, {-1.5f, 6.7f, -14}, 0.12f, kSteelD);
        F.Beam({-2.9f, 9.9f, -11.4f}, {-4.6f, 4.5f, -9.3f}, 0.1f, kSteelD);
        F.Box(-2.0f, -10.6f, 1.6f, 1.2f, 0, 1.6f, kSteel), F.Cyl(-1.4f, -10.6f, 0.25f, 1.6f, 2.0f, kMetal, 6); // dryer + its stack
        break;
    }
    case K::Livestock: {
        F.SlabBox(-3.8f, -3.4f, 3.4f, 3.0f, 0.03f, 2.3f, 2, 3.4f, 2, 3.4f);
        F.Gable(-3.8f, -3.4f, 3.4f, 3.0f, 2.33f, 1.3f, false, kShingle, kWhite);
        F.Box(-4.9f, -3.9f, 0.45f, 0.45f, 2.3f, 1.8f, kBrick);
        // free-stall barn: open curtain sides, low pitched roof with an open ridge
        F.SlabBox(1.2f, -9.6f, 9.6f, 4.4f, 0.03f, 2.5f, 0, 4, 1, 0);
        F.Gable(1.2f, -9.6f, 9.6f, 4.4f, 2.53f, 1.1f, false, {0.42f, 0.45f, 0.47f}, {0.56f, 0.2f, 0.16f}, 0.35f);
        F.Box(1.2f, -9.6f, 9.0f, 0.5f, 3.63f, 0.22f, kSteelD);
        for (float x : {-1.4f, 3.8f}) F.Box(x, -9.6f, 0.7f, 0.7f, 3.85f, 0.6f, kWhite), F.Cone(x, -9.6f, 0.62f, 4.45f, 0.6f, kRoof, 4);
        // milk house
        F.SlabBox(4.3f, -4.4f, 2.2f, 2.0f, 0.03f, 1.6f, 2, 3.4f, 2, 3.4f, 3, 5);
        F.Cyl(5.0f, -3.0f, 0.55f, 0.03f, 1.5f, kMetal, 8); // bulk milk tank
        // blue glass-lined silo + feed bins
        F.Cyl(-4.4f, -13.6f, 1.25f, 0, 7.6f, kBlueD, 12, false), F.Cone(-4.4f, -13.6f, 1.27f, 7.6f, 0.9f, kBlueD, 12);
        F.Cyl(-4.4f, -13.6f, 1.28f, 6.9f, 0.3f, kWhite, 12, false);
        F.Box(-4.4f, -12.3f, 0.12f, 0.08f, 0, 7.6f, kWhite); // chute
        for (float x : {-1.8f, -0.6f}) { F.Cyl(x, -14.6f, 0.45f, 1.0f, 2.2f, kMetal, 8, false), F.Cone(x, -14.6f, 0.47f, 3.2f, 0.4f, kMetal, 8), F.Cone(x, -14.6f, 0.45f, 1.0f, -0.45f, kMetal, 8); }
        for (float x : {-1.8f, -0.6f})
            for (float a : {-0.35f, 0.35f}) F.Box(x + a, -14.6f, 0.05f, 0.05f, 0, 1.0f, kSteelD);
        // windmill tower: one SPOM volume carries the four tapering legs, the X bracing on every face and the girts
        SpomVolume(o, F.At(4.8f, -14.4f, 0), F.fx, F.fz, indspom::WINDMILL_TOWER);
        F.Box(4.8f, -14.6f, 0.25f, 0.6f, 7.0f, 0.25f, kSteelD), F.Box(4.8f, -15.4f, 0.05f, 0.7f, 6.9f, 0.5f, kSteel); // head + tail vane
        F.Cyl(3.8f, -12.8f, 0.9f, 0, 0.35f, kSteel, 8, false); // stock tank
        { // corral by the barn: four split-rail fence volumes
            const Vec3 c[4] = {F.At(0.6f, -1.2f, 0), F.At(3.4f, -1.2f, 0), F.At(3.4f, -5.0f, 0), F.At(0.6f, -5.0f, 0)};
            for (int i = 0; i < 4; ++i) FenceRun(o, c[i], c[(i + 1) % 4], indspom::FENCE_RAIL);
        }
        break;
    }
    case K::Timber: {
        // log office
        F.SlabBox(-3.8f, -3.0f, 3.0f, 2.6f, 0.03f, 2.2f, 2, 3, 2, 3);
        F.Gable(-3.8f, -3.0f, 3.0f, 2.6f, 2.23f, 1.1f, false, {0.3f, 0.36f, 0.3f}, {0.48f, 0.34f, 0.2f}, 0.35f);
        F.Box(-3.8f, -1.45f, 3.2f, 0.5f, 1.3f, 0.06f, {0.3f, 0.36f, 0.3f});
        // the sawmill shed: log infeed at the back, lumber out the front
        F.SlabBox(1.6f, -8.6f, 7.0f, 6.0f, 0.03f, 4.0f, 1, 0, 0, 6, 3, 6, false);
        F.Cone(4.4f, -13.4f, 1.6f, 0, 1.4f, {0.86f, 0.71f, 0.5f}, 8);       // sawdust pile
        F.Beam({4.0f, 4.0f, -10.0f}, {4.4f, 3.6f, -13.2f}, 0.12f, kSteel); // blower pipe
        F.Beam({4.4f, 3.6f, -13.2f}, {4.4f, 2.6f, -13.4f}, 0.12f, kSteel);
        F.Cyl(-0.8f, -11.0f, 0.3f, 0, 5.4f, kSteelD, 6); // dust collector
        F.Cone(-0.8f, -11.0f, 0.6f, 4.6f, -0.6f, kSteel, 6), F.Cyl(-0.8f, -11.0f, 0.62f, 4.6f, 1.2f, kSteel, 8);
        // log decks: each a triangular stack of bark with cut ends
        for (float z0 : {-12.4f, -14.8f}) {
            const Vec3 g = F.At(-3.2f, z0, 0);
            const PF tri[3] = {{-1.6f, 0}, {1.6f, 0}, {0.0f, 2.4f}};
            Extrude(o, g, F.fz, F.fx, tri, 3, -4.6f, 4.6f, kBark, false);
            for (float sx : {-4.6f, 4.6f}) {
                const Vec3 in = g + F.fx * (sx * 0.9f) + Vec3{0, 0.8f, 0};
                TriC(o, g + F.fx * sx + F.fz * -1.6f, g + F.fx * sx + F.fz * 1.6f, g + F.fx * sx + Vec3{0, 2.4f, 0}, in, {0.83f, 0.68f, 0.45f});
            }
        }
        // stickered lumber stacks by the front, wrapped in white
        for (float x : {3.4f, 5.0f})
            for (float z : {-3.0f, -4.6f}) F.Box(x, z, 0.65f, 0.55f, 0, 0.75f, z < -4 ? kWood : Vec3{0.92f, 0.91f, 0.86f});
        break;
    }
    case K::Oil: {
        // control building + compressor house + lab kiosk
        F.SlabBox(-3.2f, -2.8f, 4.0f, 2.4f, 0.03f, 2.6f, 0, 4, 0, 4, 3, 6);
        F.SlabBox(3.4f, -6.4f, 2.6f, 2.0f, 0.03f, 2.8f, 1, 6, 1, 6, 3, 6);
        F.SlabBox(3.8f, -2.4f, 1.3f, 1.1f, 0.03f, 1.7f, 2, 3, 2, 3, 3, 6);
        F.Cyl(4.6f, -6.6f, 0.3f, 2.8f, 4.2f, kBrick, 6); // heater stack
        // crude storage tanks: wall, wind girder, roof, a stair up the side
        for (float z : {-8.6f, -13.2f}) {
            F.Cyl(-3.3f, z, 1.8f, 0, 2.9f, kWhite, 14, false);
            F.Cyl(-3.3f, z, 1.83f, 2.5f, 0.12f, kSteel, 14, false);
            F.Cyl(-3.3f, z, 1.83f, 0.0f, 0.2f, kRed, 14, false);
            F.Cone(-3.3f, z, 1.86f, 2.9f, 0.3f, kMetal, 14);
            F.Beam({-1.55f, 0, z + 0.6f}, {-1.55f, 2.9f, z - 1.0f}, 0.12f, kSteelD);
        }
        F.Box(-3.6f, -10.9f, 2.6f, 0.3f, 0, 0.5f, kConc); // bund wall between tanks
        // separator, stabiliser column with platforms, flare stack
        F.Cyl(2.6f, -11, 0.75f, 0, 11.4f, kMetal, 10, false), F.Cone(2.6f, -11, 0.76f, 11.4f, 0.6f, kMetal, 10);
        for (float y : {3.0f, 6.0f, 9.0f}) F.Cyl(2.6f, -11, 1.15f, y, 0.1f, kYellow, 10);
        F.Beam({3.4f, 0, -11.6f}, {3.4f, 9.1f, -11.6f}, 0.06f, kYellow); // ladder
        F.Cyl(0.2f, -14.6f, 1.2f, 1.6f, 1.0f, kWhite, 10, false), F.Cone(0.2f, -14.6f, 1.2f, 2.6f, 0.8f, kWhite, 10), F.Cone(0.2f, -14.6f, 1.2f, 1.6f, -0.5f, kWhite, 10);
        for (float a : {-0.8f, 0.8f})
            for (float b : {-0.8f, 0.8f}) F.Box(0.2f + a, -14.6f + b, 0.12f, 0.12f, 0, 1.6f, kSteelD);
        F.Cyl(4.5f, -13.8f, 0.55f, 0, 7.4f, kMetal, 8);
        F.Cyl(5.2f, -15.2f, 0.2f, 0, 12, kSteelD, 6), F.Cyl(5.2f, -15.2f, 0.3f, 11.6f, 0.4f, kSteel, 6);
        for (float a : {-1.0f, 1.0f}) F.Beam({5.2f + a * 1.6f, 0, -15.2f}, {5.2f, 8.0f, -15.2f}, 0.05f, kSteel); // guy wires
        // pipe rack: bents and three lines
        SpomVolume(o, F.At(-1.55f, -10.4f, 0) - F.fz * 0.6f, F.fx, F.fz, indspom::PIPE_RACK); // one volume: bents + three lines
        F.Box(-0.6f, -6.0f, 1.2f, 0.8f, 0, 0.9f, kYellow); // pig launcher skid
        break;
    }
    case K::Ore: {
        // brick hoist house under the headframe, pitched roof
        F.SlabBox(-2.6f, -4.6f, 4.2f, 3.2f, 0.03f, 3.3f, 1, 3.2f, 1, 3.2f);
        F.Gable(-2.6f, -4.6f, 4.2f, 3.2f, 3.33f, 1.3f, false, kRoof, {0.55f, 0.28f, 0.2f});
        F.Cyl(-4.2f, -5.4f, 0.32f, 3.3f, 2.4f, kBrick, 6);
        // steel headframe: four legs, two girts, the back-stay legs, the sheave deck
        const float hx = -2.6f, hz = -11.2f, Hh = 10;
        for (float a : {-1.0f, 1.0f})
            for (float c : {-1.0f, 1.0f}) F.Beam({hx + a * 1.3f, 0, hz + c * 1.3f}, {hx + a * 0.55f, Hh, hz + c * 0.55f}, 0.13f, kSteelD);
        for (float y : {3.3f, 6.6f}) {
            const float k = 1.3f - 0.75f * y / Hh;
            F.Beam({hx - k, y, hz - k}, {hx + k, y, hz - k}, 0.07f, kSteelD), F.Beam({hx + k, y, hz - k}, {hx + k, y, hz + k}, 0.07f, kSteelD);
            F.Beam({hx + k, y, hz + k}, {hx - k, y, hz + k}, 0.07f, kSteelD), F.Beam({hx - k, y, hz + k}, {hx - k, y, hz - k}, 0.07f, kSteelD);
        }
        for (float sd : {-1.0f, 1.0f}) F.Beam({hx + sd * 0.55f, Hh - 0.4f, hz + 0.55f}, {hx + sd * 1.2f, 0, hz + 4.6f}, 0.15f, kSteelD);
        F.Box(hx, hz, 1.9f, 1.9f, Hh, 0.25f, kSteelD);
        for (float sd : {-1.0f, 1.0f}) F.Beam({hx + sd * 0.45f, Hh + 1.2f, hz}, {-2.6f + sd * 0.3f, 6.4f, -6.2f}, 0.03f, kSteel); // hoist ropes
        F.Box(hx, hz, 1.4f, 1.4f, 0, 1.0f, kConc); // shaft collar
        // processing plant, conveyor gallery up to it, ore stockpile under the stacker
        F.SlabBox(3.0f, -9.2f, 4.0f, 4.2f, 0.03f, 6.4f, 0, 8, 0, 8, 3, 8);
        F.Box(3.0f, -9.2f, 1.2f, 1.2f, 6.83f, 0.9f, kSteel); // head house on the roof
        F.Beam({-1.4f, 3.0f, -11.2f}, {1.0f, 5.6f, -10.0f}, 0.45f, kSteel); // gallery from the headframe
        for (float t : {0.3f, 0.65f}) F.Beam({-1.4f + 2.4f * t, 0, -11.2f + 1.2f * t}, {-1.4f + 2.4f * t, 3.0f + 2.6f * t, -11.2f + 1.2f * t}, 0.06f, kSteelD);
        F.Beam({3.2f, 6.0f, -11.4f}, {3.6f, 3.4f, -14.8f}, 0.4f, kSteel);
        F.Beam({3.6f, 3.4f, -14.8f}, {3.6f, 0, -14.8f}, 0.08f, kSteelD);
        F.Cone(3.6f, -14.6f, 2.3f, 0, 2.6f, kOre, 9);
        // mine office at the front
        F.SlabBox(3.6f, -2.6f, 2.6f, 1.6f, 0.03f, 2.0f, 2, 3.6f, 2, 3.6f, 3, 8);
        break;
    }
    case K::Landfill: { // site office, weighbridge + gatehouse, leachate tank, gas flare + engine container, skips
        F.SlabBox(-3.4f, -3.4f, 4.2f, 2.6f, 0.03f, 1.8f, 0, 3, 0, 3, 3, 6);
        F.Box(2.6f, -3.0f, 2.0f, 7.0f, 0.05f, 0.1f, kSteel), F.Box(2.6f, -3.0f, 2.3f, 7.3f, 0.0f, 0.06f, kConc); // weighbridge
        for (float z : {-6.6f, 0.6f}) F.Box(1.6f, z, 0.06f, 0.06f, 0, 0.5f, kYellow), F.Box(3.6f, z, 0.06f, 0.06f, 0, 0.5f, kYellow);
        F.SlabBox(4.7f, -1.7f, 1.2f, 1.2f, 0.03f, 1.4f, 1, 2.4f, 1, 2.4f, 3, 6);
        F.Box(1.0f, 0.6f, 0.08f, 0.08f, 0, 0.6f, kYellow), F.Box(1.0f + 1.4f, 0.6f, 1.4f, 0.04f, 0.55f, 0.05f, {0.9f, 0.25f, 0.2f}); // barrier arm
        F.Cyl(-3.6f, -10.4f, 1.6f, 0, 2.6f, kSteel, 12, false), F.Cone(-3.6f, -10.4f, 1.62f, 2.6f, 0.3f, kSteelD, 12); // leachate tank
        F.Cyl(-3.6f, -10.4f, 1.64f, 2.2f, 0.1f, kYellow, 12, false);
        F.SlabBox(-3.0f, -14.2f, 4.6f, 2.0f, 0.03f, 1.6f, 2, 6, 2, 6, 3, 6); // gas engine container
        F.Cyl(-1.6f, -14.2f, 0.18f, 1.6f, 1.4f, kSteelD, 6);                 // its exhaust
        F.Cyl(1.6f, -14.0f, 0.55f, 0.5f, 6.5f, kMetal, 8), F.Cyl(1.6f, -14.0f, 0.57f, 6.0f, 0.5f, {0.25f, 0.22f, 0.2f}, 8); // flare
        for (float a : {-0.4f, 0.4f}) F.Beam({1.6f + a, 0, -14.0f}, {1.6f + a * 0.6f, 0.6f, -14.0f}, 0.1f, kSteelD);
        F.Beam({1.6f, 0.4f, -14.0f}, {-0.7f, 0.4f, -14.2f}, 0.1f, kYellow);
        for (float z : {-8.0f, -10.8f}) { // roll-off skips: tapered steel box with ribs
            const Vec3 g = F.At(3.6f, z, 0);
            const PF sk[4] = {{-3.0f, 0}, {3.0f, 0}, {3.4f, 1.8f}, {-3.4f, 1.8f}};
            Extrude(o, g, F.fz, F.fx, sk, 4, -1.6f, 1.6f, kYellow);
            for (float r = -2.4f; r <= 2.41f; r += 1.2f) OBox(o, g + F.fz * r, F.fz, F.fx, 0.08f, 1.68f, 0.15f, 1.6f, Vec3{0.8f, 0.6f, 0.12f});
        }
        break;
    }
    default: break;
    }
}

// Pumpjack (static half): skid, samson post A-frame, gear box, motor, wellhead. g is the skid centre; it faces +x.
void PumpjackBase(std::vector<Vertex>& o, Vec3 g) {
    const Vec3 X{1, 0, 0}, Z{0, 0, 1};
    OBox(o, g, X, Z, 2.4f, 1.0f, -3.0f, 0.0f, kGravel);  // footing reaching into the slope (the pad's top is in the volume)
    SpomVolume(o, g, X, Z, indspom::PUMPJACK);         // skid, samson post, gear box, motor, wellhead: one volume
}
// Pumpjack (moving half) at phase a: walking beam with its horsehead, the pitman arms down to the turning cranks and
// counterweights, the polished rod hanging from the head.
void PumpjackBeam(std::vector<Vertex>& o, Vec3 g, float ph, Vec3 col) {
    const float a = std::sin(ph) * 0.24f;
    const Vec3 piv = g + Vec3{0.7f, 5.4f, 0}, X{std::cos(a), std::sin(a), 0}, U{-std::sin(a), std::cos(a), 0}, Z{0, 0, 1};
    // a prism in the beam's frame (X along it, U up, Z across): profile points (along, up), half width w
    auto prism = [&](const PF* pr, int n, float w, Vec3 c) {
        Vec3 pts[16];
        float ca = 0, cu = 0;
        for (int i = 0; i < n; ++i) pts[i] = piv + X * pr[i].a + U * pr[i].h, ca += pr[i].a, cu += pr[i].h;
        const Vec3 in = piv + X * (ca / n) + U * (cu / n);
        for (float sz : {-w, w})
            for (int i = 1; i + 1 < n; ++i) TriC(o, pts[0] + Z * sz, pts[i] + Z * sz, pts[i + 1] + Z * sz, in, c);
        for (int i = 0; i < n; ++i) {
            const Vec3 p = pts[i], q = pts[(i + 1) % n];
            TriC(o, p - Z * w, q - Z * w, q + Z * w, in, c), TriC(o, p - Z * w, q + Z * w, p + Z * w, in, c);
        }
    };
    // walking beam: a slim I-beam (the flanges read as the darker top and bottom bands), the saddle bearing on the post
    const PF web[4] = {{-2.55f, -0.24f}, {2.75f, -0.24f}, {2.75f, 0.24f}, {-2.55f, 0.24f}};
    prism(web, 4, 0.06f, col * 0.85f);
    const PF flTop[4] = {{-2.55f, 0.17f}, {2.75f, 0.17f}, {2.75f, 0.26f}, {-2.55f, 0.26f}};
    const PF flBot[4] = {{-2.55f, -0.26f}, {2.75f, -0.26f}, {2.75f, -0.17f}, {-2.55f, -0.17f}};
    prism(flTop, 4, 0.16f, col), prism(flBot, 4, 0.16f, col);
    // horsehead: a curved face on a radius about the saddle, so the bridle hangs plumb over the well all through the stroke
    PF hh[9];
    int n = 0;
    hh[n++] = {2.55f, 0.3f};
    for (int i = 0; i <= 5; ++i) {
        const float ang = 0.16f - i * (0.62f / 5);
        hh[n++] = {3.95f * std::cos(ang), 3.95f * std::sin(ang)};
    }
    hh[n++] = {3.05f, -1.5f};
    hh[n++] = {2.55f, -0.35f};
    prism(hh, n, 0.22f, col);
    const PF tailW[4] = {{-2.75f, -0.3f}, {-2.3f, -0.3f}, {-2.3f, 0.3f}, {-2.75f, 0.3f}};
    prism(tailW, 4, 0.2f, col * 0.8f);                                                   // tail bearing block
    // polished rod from the horsehead down to the wellhead
    const Vec3 head = piv + X * 3.95f * std::cos(-0.25f) + U * 3.95f * std::sin(-0.25f);
    Beam(o, head, Vec3{g.x + 3.9f, g.y + 2.1f, g.z}, 0.05f, kMetal);                          // bridle + polished rod
    // cranks turn with the stroke; pitman arms from the beam's tail to the crank pins
    const Vec3 crankC = g + Vec3{-1.3f, 1.25f, 0}, tail = piv + X * -2.4f;
    for (float sz : {-0.62f, 0.62f}) {
        const Vec3 pin = crankC + Vec3{std::cos(ph) * 0.9f, std::sin(ph) * 0.9f, sz};
        Beam(o, crankC + Vec3{0, 0, sz}, pin, 0.3f, kSteelD);
        Beam(o, pin - Vec3{std::cos(ph) * 0.5f, std::sin(ph) * 0.5f, 0}, pin, 0.75f, kRed); // counterweight
        Beam(o, pin, tail + Vec3{0, 0, sz}, 0.12f, kSteelD);
    }
    Beam(o, tail + Vec3{0, 0, -0.62f}, tail + Vec3{0, 0, 0.62f}, 0.16f, kSteelD); // equalizer
}

// A machine standing on a slope: built level at the origin (+x forward, +z left), then turned onto the ground plane
// sampled under its wheels (halfL ahead / behind, halfW either side) and set down at g. SPOM volumes lean with it.
template <class Build>
void OnGround(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd, float halfL, float halfW, Build&& build) {
    static std::vector<Vertex> tmp;
    tmp.clear();
    build(tmp, Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    auto h = [&](Vec3 q) { return IndGround(q.x, q.z); };
    const float hf = h(g + fd * halfL), hb = h(g - fd * halfL), hl = h(g + sd * halfW), hr = h(g - sd * halfW);
    const Vec3 F = tbd::Normalize(fd * (2 * halfL) + Vec3{0, hf - hb, 0});
    Vec3 S = tbd::Normalize(sd * (2 * halfW) + Vec3{0, hl - hr, 0});
    const Vec3 U = tbd::Normalize(tbd::Cross(S, F));
    S = tbd::Normalize(tbd::Cross(F, U));
    const float y = std::max(g.y, 0.25f * (hf + hb + hl + hr)); // sit on the mean of the four contact points
    const Vec3 base{g.x, y, g.z};
    for (Vertex v : tmp) {
        const Vec3 p = base + F * v.px + U * v.py + S * v.pz, n = F * v.nx + U * v.ny + S * v.nz;
        v.px = p.x, v.py = p.y, v.pz = p.z, v.nx = n.x, v.ny = n.y, v.nz = n.z;
        o.push_back(v);
    }
}
// Vehicles: a plain core between two SPOM side volumes that carry everything with detail (wheels, cab glass and pillars,
// rails, ladders, ribs, lights). The core spans only the width the volumes leave between them. g on the ground, fd forward,
// sd to the left.
void Combine(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd, float t) {
    const Vec3 body{0.55f, 0.14f, 0.11f}, dark{0.16f, 0.16f, 0.17f};
    const PF hull[6] = {{-4.3f, 1.0f}, {-4.4f, 2.7f}, {-3.6f, 3.5f}, {1.1f, 3.5f}, {1.6f, 1.7f}, {1.1f, 1.0f}};
    Extrude(o, g, fd, sd, hull, 6, -0.85f, 0.85f, body);
    const PF tank[4] = {{-3.0f, 3.5f}, {0.6f, 3.5f}, {0.7f, 4.85f}, {-3.1f, 4.85f}};
    Extrude(o, g, fd, sd, tank, 4, -0.85f, 0.85f, body * 1.05f);
    OBox(o, g + fd * 1.7f, fd, sd, 0.85f, 0.85f, 3.45f, 5.19f, {0.12f, 0.16f, 0.2f});            // cab core (glass)
    const PF feed[4] = {{1.4f, 1.15f}, {3.55f, 0.5f}, {3.55f, 1.35f}, {1.6f, 2.5f}};
    Extrude(o, g, fd, sd, feed, 4, -0.7f, 0.7f, body * 0.85f);
    OBox(o, g + fd * 3.35f, fd, sd, 0.2f, 3.9f, 0.3f, 1.3f, body * 0.8f);                       // header back (behind its volume)
    SpomSides(o, g, fd, sd, indspom::COMBINE_L, indspom::COMBINE_R);
    SpomVolume(o, g, sd, fd, indspom::HEADER);
    for (int i = 0; i < 4; ++i) { // the reel turns: four bats as thin cards
        const float a = t * 3.0f + i * 1.5708f;
        const Vec3 p = g + fd * (4.1f + std::cos(a) * 0.55f) + Vec3{0, 1.65f + std::sin(a) * 0.55f, 0};
        Card(o, p - sd * 3.7f, p + sd * 3.7f, 0.1f, {0.72f, 0.58f, 0.2f});
    }
    (void)dark;
}
void HaulTruck(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd, Vec3 col) {
    const Vec3 dark{0.2f, 0.2f, 0.21f};
    OBox(o, g + fd * 0.3f, fd, sd, 3.6f, 1.4f, 1.0f, 1.9f, dark);                               // chassis
    const PF hood[4] = {{2.6f, 1.6f}, {4.4f, 1.6f}, {4.2f, 3.4f}, {2.6f, 3.4f}};
    Extrude(o, g, fd, sd, hood, 4, -1.2f, 1.2f, col);
    OBox(o, g + fd * 4.45f, fd, sd, 0.06f, 1.0f, 1.8f, 3.1f, dark);                              // grille
    OBox(o, g + fd * 3.4f, fd, sd, 1.0f, 1.4f, 3.3f, 3.5f, col);                                // deck centre
    const PF bed[6] = {{-4.4f, 2.6f}, {-3.4f, 1.9f}, {2.3f, 2.2f}, {2.8f, 3.6f}, {3.9f, 5.6f}, {-4.6f, 4.4f}};
    Extrude(o, g, fd, sd, bed, 6, -1.4f, 1.4f, col * 0.92f);
    const Vec3 a = g + fd * -4.2f + sd * -2.3f + Vec3{0, 4.25f, 0}, b = g + fd * 2.4f + sd * -2.3f + Vec3{0, 4.5f, 0};
    const Vec3 c = g + fd * 2.4f + sd * 2.3f + Vec3{0, 4.5f, 0}, d = g + fd * -4.2f + sd * 2.3f + Vec3{0, 4.25f, 0};
    const Vec3 peak = a + (b - a) * 0.3f + (d - a) * 0.5f + Vec3{0, 0.9f, 0};                      // the heaped load
    zfx::Tri(o, peak, a, d, {0, 1, 0}, kOre), zfx::Tri(o, peak, b, a, {0, 1, 0}, kOre * 1.05f);
    zfx::Tri(o, peak, c, b, {0, 1, 0}, kOre * 0.95f), zfx::Tri(o, peak, d, c, {0, 1, 0}, kOre);
    SpomSides(o, g, fd, sd, indspom::HAUL_L, indspom::HAUL_R);
}
void Compactor(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd) {
    const Vec3 col{0.76f, 0.58f, 0.17f}, steel{0.38f, 0.39f, 0.4f};
    const PF body[6] = {{-3.2f, 1.0f}, {-3.4f, 2.6f}, {-1.4f, 2.8f}, {1.2f, 2.4f}, {1.6f, 1.2f}, {-1.0f, 0.9f}};
    Extrude(o, g, fd, sd, body, 6, -0.8f, 0.8f, col);
    OBox(o, g + fd * -0.3f, fd, sd, 0.9f, 0.8f, 2.6f, 4.0f, {0.12f, 0.16f, 0.2f});
    OBox(o, g + fd * -0.3f, fd, sd, 1.0f, 0.86f, 4.0f, 4.14f, col);
    const PF blade[4] = {{3.3f, 0.2f}, {3.6f, 0.2f}, {3.9f, 2.1f}, {3.4f, 2.2f}};
    Extrude(o, g, fd, sd, blade, 4, -0.8f, 0.8f, col * 0.9f);
    for (float a : {-2.6f, 2.2f}) Wheel(o, g, fd, sd, a, 0.95f, 0.9f, -0.8f, 0.8f, steel, 6);      // drum centres
    SpomSides(o, g, fd, sd, indspom::COMPACTOR_L, indspom::COMPACTOR_R);
}
void Excavator(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd, Vec3 col) {
    const Vec3 dark{0.2f, 0.2f, 0.21f};
    OBox(o, g, fd, sd, 1.8f, 0.55f, 0.35f, 0.95f, dark);                                       // car body
    const PF hs[5] = {{-2.2f, 1.1f}, {-2.3f, 2.4f}, {0.0f, 2.5f}, {0.6f, 2.3f}, {0.6f, 1.1f}};
    Extrude(o, g, fd, sd, hs, 5, -0.55f, 0.55f, col);
    const Vec3 b0 = g + fd * 0.6f + sd * 0.1f + Vec3{0, 1.8f, 0}, b1 = g + fd * 4.6f + sd * 0.1f + Vec3{0, 4.4f, 0}, b2 = g + fd * 6.6f + sd * 0.1f + Vec3{0, 0.9f, 0};
    Beam(o, b0, b1, 0.55f, col), Beam(o, b1, b2, 0.4f, col);
    const PF bk[4] = {{5.9f, 0.0f}, {7.0f, 0.0f}, {7.2f, 1.1f}, {6.2f, 1.3f}};
    Extrude(o, g + sd * 0.1f, fd, sd, bk, 4, -0.55f, 0.55f, dark);
    SpomSides(o, g, fd, sd, indspom::EXCAVATOR_L, indspom::EXCAVATOR_R);
}
void Forwarder(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd) {
    const Vec3 col{0.7f, 0.46f, 0.17f};
    const PF fr[5] = {{1.4f, 1.0f}, {3.6f, 1.0f}, {3.7f, 2.0f}, {2.4f, 2.3f}, {1.4f, 2.2f}};
    Extrude(o, g, fd, sd, fr, 5, -0.5f, 0.5f, col);
    OBox(o, g + fd * 2.15f, fd, sd, 0.65f, 0.5f, 2.2f, 3.6f, {0.12f, 0.16f, 0.2f});
    const PF logs[4] = {{-3.95f, 1.0f}, {1.0f, 1.0f}, {1.0f, 2.55f}, {-3.95f, 2.55f}};
    Extrude(o, g, fd, sd, logs, 4, -0.5f, 0.5f, kBark);
    Beam(o, g + fd * 1.0f + Vec3{0, 2.4f, 0}, g + fd * -0.6f + Vec3{0, 4.2f, 0}, 0.26f, col); // crane
    Beam(o, g + fd * -0.6f + Vec3{0, 4.2f, 0}, g + fd * -2.4f + Vec3{0, 3.0f, 0}, 0.2f, col);
    SpomSides(o, g, fd, sd, indspom::FORWARDER_L, indspom::FORWARDER_R);
}
// A row-crop tractor: nearly all of it is the two side volumes (wheels, fenders, cab, hood louvres); the core is the hood's
// middle strip.
void Tractor(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd, Vec3 col) {
    const PF hood[5] = {{-0.1f, 1.0f}, {2.6f, 1.0f}, {2.75f, 1.45f}, {2.55f, 1.85f}, {-0.1f, 2.05f}};
    Extrude(o, g, fd, sd, hood, 5, -0.12f, 0.12f, col);
    OBox(o, g + fd * -0.9f, fd, sd, 0.85f, 0.12f, 1.0f, 3.1f, {0.12f, 0.16f, 0.2f});
    OBox(o, g + fd * -0.84f, fd, sd, 0.95f, 0.12f, 3.1f, 3.24f, col);
    SpomSides(o, g, fd, sd, indspom::TRACTOR_L, indspom::TRACTOR_R);
}

// A cow: barrel torso with a narrower spine on top (a rounded cross-section in two prisms), neck and head grazing or raised,
// four slim legs, a tail; Holsteins get patches and an udder.
void Cow(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd, float r) {
    const bool angus = r > 0.72f, graze = std::fmod(r * 13.0f, 1.0f) > 0.45f, hol = !angus && r <= 0.6f;
    const Vec3 hide = angus ? Vec3{0.1f, 0.09f, 0.08f} : r > 0.6f ? Vec3{0.46f, 0.28f, 0.17f} : Vec3{0.82f, 0.8f, 0.76f};
    const PF torso[8] = {{-0.95f, 0.82f}, {-1.05f, 1.2f}, {-0.92f, 1.42f}, {0.55f, 1.45f}, {0.85f, 1.3f}, {0.85f, 0.95f}, {0.45f, 0.75f}, {-0.5f, 0.74f}};
    Extrude(o, g, fd, sd, torso, 8, -0.32f, 0.32f, hide);
    const PF spine[4] = {{-1.0f, 1.35f}, {0.75f, 1.38f}, {0.7f, 1.55f}, {-0.95f, 1.53f}};
    Extrude(o, g, fd, sd, spine, 4, -0.2f, 0.2f, hide);
    // neck (as wide as the shoulders' top) then a small head: down to the grass, or level
    PF nk[4], hd[4];
    if (graze) {
        nk[0] = {0.6f, 1.05f}, nk[1] = {1.05f, 0.75f}, nk[2] = {1.15f, 0.95f}, nk[3] = {0.75f, 1.42f};
        hd[0] = {1.0f, 0.85f}, hd[1] = {1.28f, 0.22f}, hd[2] = {1.42f, 0.3f}, hd[3] = {1.2f, 1.0f};
    } else {
        nk[0] = {0.6f, 1.05f}, nk[1] = {1.05f, 1.15f}, nk[2] = {1.1f, 1.45f}, nk[3] = {0.7f, 1.45f};
        hd[0] = {1.0f, 1.2f}, hd[1] = {1.5f, 1.15f}, hd[2] = {1.5f, 1.35f}, hd[3] = {1.08f, 1.58f};
    }
    Extrude(o, g, fd, sd, nk, 4, -0.18f, 0.18f, hide);
    Extrude(o, g, fd, sd, hd, 4, -0.13f, 0.13f, hol ? Vec3{0.1f, 0.09f, 0.09f} : hide);
    const Vec3 leg = hol ? Vec3{0.8f, 0.78f, 0.74f} : hide;
    for (float a : {-0.78f, 0.6f})
        for (float s : {-0.19f, 0.19f}) OBox(o, g + fd * a + sd * s, fd, sd, 0.07f, 0.07f, 0.0f, 0.86f, leg);
    Beam(o, g + fd * -1.02f + Vec3{0, 1.35f, 0}, g + fd * -1.08f + Vec3{0, 0.55f, 0}, 0.05f, hide); // tail
    if (hol) {
        OBox(o, g + fd * -0.45f, fd, sd, 0.18f, 0.14f, 0.62f, 0.8f, {0.82f, 0.62f, 0.6f}); // udder
        const float k = std::fmod(r * 7.0f, 1.0f);
        OBox(o, g + fd * (k * 0.8f - 0.55f), fd, sd, 0.32f, 0.335f, 0.95f, 1.4f, {0.09f, 0.08f, 0.08f});
        OBox(o, g + fd * (0.35f - k * 0.5f), fd, sd, 0.22f, 0.205f, 1.3f, 1.56f, {0.09f, 0.08f, 0.08f});
    }
}
// A net-wrapped round bale lying on its side (1.5 m across): wrapped barrel, straw end faces showing the spiral darker.
void Bale(std::vector<Vertex>& o, Vec3 g, Vec3 fd, Vec3 sd) {
    // one SPOM volume (tools/building_gen/industry_spom.py "bale"): sagging barrel, domed ends with the straw spiral, net wrap
    SpomVolume(o, g, fd, sd, indspom::BALE);
}
// The repo's tree models (lowpoly_tree/out/obj, LOD1: 108 / 132 triangles, alpha-cut needle cards) instanced for forestry.
// Work-area ground textures (tools/building_gen/industry_ground.py): one tile per kGroundTile metres.
enum GroundTex : int { kGrGrain, kGrPasture, kGrForest, kGrOil, kGrOre, kGrRefuse, kGrCover, kGrLiner, kGrCap, kGrCount };
constexpr float kGroundTile = 8.0f;
struct TreeInst { Vec3 p; float yaw, k; int species; };
// A cow in a pasture: drawn each frame as its close-up SPOM volume near the camera, the extruded model (LOD) further away.
struct CowInst { Vec3 p, fd, sd; float r; };
inline int CowVolume(float r) { // matches Cow()'s breed and pose choice
    const bool angus = r > 0.72f, graze = std::fmod(r * 13.0f, 1.0f) > 0.45f;
    if (angus) return indspom::COW_ANGUS_G;
    if (r > 0.6f) return indspom::COW_HERE;
    return graze ? indspom::COW_HOL_G : indspom::COW_HOL;
}
// The hub's moving parts (pumpjack-free): windmill rotor, flare, headframe wheels.
void HubAnim(const tbd::city::IndSite& s, float t, std::vector<Vertex>& o) {
    const Frame F = FrameOf(s, o);
    using K = tbd::city::IndKind;
    if (s.kind == K::Livestock) { // windmill rotor facing the road
        const Vec3 hub = F.At(4.8f, -14.4f + 0.3f, 7.2f);
        for (int i = 0; i < 10; ++i) {
            const float a = i * 0.6283f + t * 2.2f;
            Beam(o, hub, hub + F.fx * (std::sin(a) * 2.2f) + Vec3{0, std::cos(a) * 2.2f, 0}, 0.3f, kWhite);
        }
    } else if (s.kind == K::Landfill) { // flame licking above the flare shroud
        const float k = 1 + 0.2f * std::sin(t * 7.1f);
        Cone(o, F.At(1.6f, -14.0f, 7.0f), 0.45f, 0.9f * k, {1.0f, 0.62f, 0.2f}, 6);
    } else if (s.kind == K::Oil) {
        const float k = 1 + 0.25f * std::sin(t * 9.3f) + 0.1f * std::sin(t * 21);
        Cone(o, F.At(5.2f, -15.2f, 12), 0.9f * (1 + 0.1f * std::sin(t * 13)), 3.2f * k, {1.0f, 0.55f, 0.15f}, 6);
    } else if (s.kind == K::Ore) {
        for (float sd : {-1.0f, 1.0f}) {
            const Vec3 c = F.At(-2.6f + sd * 0.45f, -11.2f, 11.2f);
            for (int i = 0; i < 4; ++i) {
                const float a = i * 0.785f + t * 1.6f * sd;
                const Vec3 d = F.fz * (std::cos(a) * 1.8f) + Vec3{0, std::sin(a) * 1.8f, 0};
                Beam(o, c - d, c + d, 0.12f, kRed);
            }
        }
    }
}

// The work area: terrain-draped 4 m cells plus props. Pumpjack spots go to `jacks` (animated per frame).
void Area(const tbd::city::IndSite& s, std::vector<Vertex>& o, std::vector<Vec3>& jacks, std::vector<V2>& loop, std::vector<TreeInst>& trees, std::vector<CowInst>& cows, std::vector<Vertex>* ground) {
    using K = tbd::city::IndKind;
    const std::vector<V2> poly = s.Poly();
    if (poly.size() < 3) return;
    const V2 u = s.Side(), v = s.front * -1.0f;
    float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
    for (V2 p : poly) u0 = std::min(u0, Dot2(p, u)), u1 = std::max(u1, Dot2(p, u)), v0 = std::min(v0, Dot2(p, v)), v1 = std::max(v1, Dot2(p, v));
    auto W = [&](float a, float b) { return u * a + v * b; };
    auto G = [&](V2 p, float lift) { return Vec3{p.x, IndGround(p.x, p.z) + lift, p.z}; };
    auto H = [](float a, float b, float k) { return tbd::city::resnoise::Hash((int)std::floor(a), (int)std::floor(b), (uint32_t)k); };
    const float cs = 3.0f;
    // landfill: cells fill outward from the hub; filled ground is capped and grassed, the front is open refuse,
    // the rest is lined, excavated cell waiting for waste
    V2 ba, bb;
    s.BackEdge(ba, bb);
    const float vBack = Dot2(Lerp2(ba, bb, 0.5f), v), fill = s.Fill();
    auto lfDepth = [&](float b) { return std::clamp((b - vBack) / std::max(1.0f, v1 - vBack), 0.0f, 1.0f); };
    auto edgeDist = [&](V2 m) {
        float e = 1e30f;
        for (size_t i = 0; i < poly.size(); ++i) e = std::min(e, tbd::city::SegDist(m, poly[i], poly[(i + 1) % poly.size()]));
        return e;
    };
    for (float b = std::floor(v0 / cs) * cs; b < v1; b += cs)
        for (float a = std::floor(u0 / cs) * cs; a < u1; a += cs) {
            const V2 m = W(a + cs * 0.5f, b + cs * 0.5f);
            if (!tbd::city::PolyContains(poly, m) || s.InHub(m, 0.5f)) continue;
            // the ground is a tiling texture per surface (tools/building_gen/industry_ground.py), its uv in the area's own axes so
            // crop rows run parallel to the hub; a faint per-cell tint breaks the repeat
            int gi;
            float tint = 1.0f + (H(std::floor(a / 12.0f), std::floor(b / 12.0f), 3) - 0.5f) * 0.12f;
            Vec3 tc{tint, tint, tint};
            switch (s.kind) {
            case K::Grain: {
                gi = kGrGrain;
                const float band = H(std::floor(b / 24.0f), (float)s.id, 9); // strips of different crops / ripeness
                if (band < 0.3f) tc = Vec3{tc.x * 0.78f, tc.y * 0.95f, tc.z * 0.62f};
                else if (band < 0.5f) tc = Vec3{tc.x * 1.05f, tc.y * 0.98f, tc.z * 0.88f};
                break;
            }
            case K::Livestock: gi = kGrPasture; break;
            case K::Timber: gi = kGrForest; break;
            case K::Oil: gi = kGrOil; break;
            case K::Ore: gi = kGrOre; break;
            case K::Landfill: {
                const float d = lfDepth(b) + (H(a, b, 7) - 0.5f) * 0.06f;
                gi = d < fill - 0.08f ? kGrCap : d < fill + 0.02f ? kGrRefuse : d < fill + 0.1f ? kGrCover : kGrLiner;
                break;
            }
            default: gi = kGrCover;
            }
            const V2 q[4] = {W(a, b), W(a + cs, b), W(a + cs, b + cs), W(a, b + cs)};
            const float qa[4] = {a, a + cs, a + cs, a}, qb[4] = {b, b, b + cs, b + cs};
            float lift = 0.3f;
            if (s.kind == K::Landfill) // a mound: filled cells rise, sloping down toward the area's edge
                lift += (lfDepth(b) < fill ? 1.0f : 0.25f) * std::min(edgeDist(m) * 0.25f, 9.0f) * std::min(1.0f, fill * 3.0f);
            Vec3 P4[4];
            for (int k = 0; k < 4; ++k) P4[k] = G(q[k], lift);
            Vec3 n = tbd::Cross(P4[2] - P4[0], P4[1] - P4[3]);
            n = tbd::Normalize(n.y < 0 ? n * -1.0f : n);
            auto vtx = [&](int k) {
                ground[gi].push_back({P4[k].x, P4[k].y, P4[k].z, n.x, n.y, n.z, tc.x, tc.y, tc.z, 1.0f, qa[k] / kGroundTile, qb[k] / kGroundTile});
            };
            vtx(0), vtx(1), vtx(2), vtx(0), vtx(2), vtx(3);
        }
    // a fence along one edge of the area: SPOM volumes of about 8 m, each standing on the lower ground of its two ends
    auto fencePoly = [&](V2 a, V2 b, int id) {
        const int n = std::max(1, (int)std::lround(Len(b - a) / indspom::kVol[id].L));
        for (int i = 0; i < n; ++i) {
            Vec3 p0 = G(Lerp2(a, b, (float)i / n), 0), p1 = G(Lerp2(a, b, (float)(i + 1) / n), 0);
            p0.y = p1.y = std::min(p0.y, p1.y);
            FencePiece(o, p0, p1, id);
        }
    };
    // props on a jittered grid
    auto scatter = [&](float sp, float margin, auto&& fn) {
        for (float b = std::floor(v0 / sp) * sp; b < v1; b += sp)
            for (float a = std::floor(u0 / sp) * sp; a < u1; a += sp) {
                const V2 m = W(a + sp * (0.25f + 0.5f * H(a, b, 1)), b + sp * (0.25f + 0.5f * H(a, b, 2)));
                if (!tbd::city::PolyContains(poly, m) || s.InHub(m, 2.0f)) continue;
                float e = 1e30f;
                for (size_t i = 0; i < poly.size(); ++i) e = std::min(e, tbd::city::SegDist(m, poly[i], poly[(i + 1) % poly.size()]));
                if (e < margin) continue;
                fn(m, H(a, b, 5));
            }
    };
    switch (s.kind) {
    case K::Timber:
        scatter(6.0f, 2.0f, [&](V2 m, float r) {
            const Vec3 g = G(m, 0);
            // harvest blocks: some stands are cut (stumps + a felled log), the rest grow in age classes
            const float block = H(std::floor(Dot2(m, u) / 36.0f), std::floor(Dot2(m, v) / 36.0f), 17);
            if (block < 0.18f) {
                Cyl(o, g, 0.32f, 0.5f, kBark, 5);
                if (r < 0.3f) Beam(o, g + Vec3{0.6f, 0.3f, 0}, g + Vec3{6.0f * std::cos(r * 30), 0.3f, 6.0f * std::sin(r * 30)}, 0.5f, kBark);
                return;
            }
            // the repo's Pine / Spruce models (18 m when grown) in age classes: planted, pole stage, mature
            const float k = (block < 0.4f ? 0.3f : block < 0.7f ? 0.6f : 0.9f) * (0.85f + r * 0.3f);
            trees.push_back({g, r * 40.0f, k, block > 0.55f ? 1 : 0});
        });
        { // a forwarder by the back of the hub, loaded
            V2 a, b;
            s.BackEdge(a, b);
            const V2 p = Lerp2(a, b, 0.25f) + v * 9.0f;
            OnGround(o, G(p, 0.3f), {u.x, 0, u.z}, {v.x, 0, v.z}, 3.0f, 1.2f, [&](auto& b, Vec3 q, Vec3 f, Vec3 sd) { Forwarder(b, q, f, sd); });
            const V2 q = Lerp2(a, b, 0.7f) + v * 7.0f; // a roadside log deck waiting for the trucks
            const PF tri[3] = {{-1.4f, 0}, {1.4f, 0}, {0.0f, 2.0f}};
            Extrude(o, G(q, 0), {v.x, 0, v.z}, {u.x, 0, u.z}, tri, 3, -4.0f, 4.0f, kBark);
        }
        break;
    case K::Livestock: {
        for (size_t e = 1; e < poly.size(); ++e) { // fence, open toward the barn
            fencePoly(poly[e], poly[(e + 1) % poly.size()], indspom::FENCE_RAIL); // split-rail fence volumes
        }
        scatter(12.0f, 3.0f, [&](V2 m, float r) { // cattle in loose groups (some cells empty): drawn per frame, close-up or LOD
            if (H(std::floor(Dot2(m, u) / 40.0f), std::floor(Dot2(m, v) / 40.0f), 21) < 0.35f) return;
            // each cow walks about 5.4 m along its heading and back: turn it so both ends of that route stay in the pasture
            float h = r * 40.0f;
            for (int k = 0; k < 8; ++k, h += 0.785f) {
                const V2 end = m + V2{std::cos(h), std::sin(h)} * 6.0f;
                float e = 1e30f;
                for (size_t i = 0; i < poly.size(); ++i) e = std::min(e, tbd::city::SegDist(end, poly[i], poly[(i + 1) % poly.size()]));
                if (tbd::city::PolyContains(poly, end) && e > 2.5f && !s.InHub(end, 3.0f)) break;
            }
            cows.push_back({G(m, 0), {std::cos(h), 0, std::sin(h)}, {-std::sin(h), 0, std::cos(h)}, r});
        });
        scatter(70.0f, 10.0f, [&](V2 m, float r) { // feed rings with a bale + water troughs
            const Vec3 g = G(m, 0);
            if (r < 0.5f) {
                Cyl(o, g, 1.5f, 1.0f, kSteel, 8, false);
                Bale(o, g, {1, 0, 0}, {0, 0, 1});
            } else
                OBox(o, g, {1, 0, 0}, {0, 0, 1}, 1.6f, 0.45f, 0, 0.6f, kSteel);
        });
        { // a tractor parked by the barn
            V2 a, b;
            s.BackEdge(a, b);
            OnGround(o, G(Lerp2(a, b, 0.3f) + v * 8.0f, 0.3f), {v.x, 0, v.z}, {-v.z, 0, v.x}, 1.6f, 0.9f, [&](auto& bb, Vec3 q, Vec3 f, Vec3 sd) { Tractor(bb, q, f, sd, {0.21f, 0.33f, 0.19f}); });
        }
        break;
    }
    case K::Oil: {
        V2 a, b;
        s.BackEdge(a, b);
        const Vec3 mid = G(Lerp2(a, b, 0.5f), 0.3f);
        scatter(28.0f, 5.0f, [&](V2 m, float) {
            const Vec3 g = G(m, 0);
            jacks.push_back(g);
            PumpjackBase(o, g);
            // flow line: along the ground to the hub, on little sleepers
            // a round pipe (a box, not a card) following the ground in 6 m runs: out along x, then across to the hub
            const V2 w{g.x + 3.9f, g.z + 1.4f}, corner{w.x, mid.z}, end{mid.x, mid.z};
            for (auto [p0, p1] : {std::pair{w, corner}, std::pair{corner, end}}) {
                const int k = std::max(1, (int)(Len(p1 - p0) / 6.0f));
                for (int i = 0; i < k; ++i)
                    zfx::Beam(o, G(Lerp2(p0, p1, (float)i / k), 0.42f), G(Lerp2(p0, p1, (float)(i + 1) / k), 0.42f), 0.16f, {0.24f, 0.25f, 0.26f});
            }
        });
        scatter(90.0f, 12.0f, [&](V2 m, float r) { // tank batteries: two stock tanks and a separator on a gravel pad
            if (r > 0.6f) return;
            const Vec3 g = G(m, 0);
            OBox(o, g, {1, 0, 0}, {0, 0, 1}, 5.5f, 3.5f, -2.5f, 0.3f, kGravel);
            for (float x : {-2.5f, 0.5f}) Cyl(o, g + Vec3{x, 0.3f, 0}, 1.4f, 4.8f, {0.55f, 0.5f, 0.4f}, 8);
            Cyl(o, g + Vec3{3.6f, 0.3f, 1.2f}, 0.6f, 2.6f, kWhite, 6);
            Beam(o, g + Vec3{-2.5f, 5.2f, 0}, g + Vec3{0.5f, 5.2f, 0}, 0.4f, kYellow); // walkway
        });
        break;
    }
    case K::Ore: {
        scatter(22.0f, 8.0f, [&](V2 m, float r) { // muck piles and the odd boulder
            if (r < 0.15f) { Cone(o, G(m, -0.2f), 1.2f, 1.4f, Vec3{0.5f, 0.47f, 0.43f}, 5); return; }
            Cone(o, G(m, -0.3f), 2.0f + r * 2.5f, 1.5f + r * 2.0f, r < 0.55f ? kOre : Vec3{0.55f, 0.52f, 0.48f}, 7);
        });
        V2 c{0, 0};
        for (V2 p : poly) c = c + p;
        c = c * (1.0f / poly.size());
        { // a shovel loading at the face (the pit's centre) and a drill rig on a bench
            const V2 d = Norm(poly[2 % poly.size()] - c);
            OnGround(o, G(c, 0.3f), {d.x, 0, d.z}, {-d.z, 0, d.x}, 2.2f, 1.3f, [&](auto& b, Vec3 q, Vec3 f, Vec3 sd) { Excavator(b, q, f, sd, kYellow); });
            const V2 q = c + (poly[poly.size() - 1] - c) * 0.4f;
            const Vec3 g = G(q, 0);
            for (float sg : {-1.2f, 1.2f}) {
                const PF tr[4] = {{-2.0f, 0}, {2.0f, 0}, {1.8f, 0.8f}, {-1.8f, 0.8f}};
                Extrude(o, g, {1, 0, 0}, {0, 0, 1}, tr, 4, sg - 0.4f, sg + 0.4f, {0.2f, 0.2f, 0.21f});
            }
            OBox(o, g, {1, 0, 0}, {0, 0, 1}, 2.0f, 1.2f, 0.8f, 2.0f, kYellow);
            Beam(o, g + Vec3{1.8f, 0.5f, 0}, g + Vec3{1.8f, 10.0f, 0}, 0.35f, kYellow); // mast
        }
        for (V2 p : poly) loop.push_back(c + (p - c) * 0.72f); // the haul road the trucks drive
        break;
    }
    case K::Grain: {
        scatter(40.0f, 8.0f, [&](V2 m, float) { // round bales
            const Vec3 g = G(m, 0);
            const float h = H(Dot2(m, u), Dot2(m, v), 31) * 6.28f;
            const Vec3 fd{std::cos(h), 0, std::sin(h)}, sd{-std::sin(h), 0, std::cos(h)};
            Bale(o, g, fd, sd), Bale(o, g + sd * 1.7f + fd * 0.3f, fd, sd); // they come in pairs
        });
        { // grain cart + tractor waiting at the field edge for the combine
            V2 a, b;
            s.BackEdge(a, b);
            const V2 p = Lerp2(a, b, 0.75f) + v * 10.0f;
            const Vec3 g = G(p, 0), fd{u.x, 0, u.z}, sd{v.x, 0, v.z};
            OnGround(o, g + Vec3{0, 0.3f, 0}, fd, sd, 1.6f, 0.9f, [&](auto& bb, Vec3 q, Vec3 f, Vec3 s2) { Tractor(bb, q, f, s2, {0.21f, 0.33f, 0.19f}); });
            const PF cart[4] = {{-6.4f, 1.0f}, {-2.6f, 1.0f}, {-2.2f, 3.0f}, {-6.8f, 3.0f}};
            Extrude(o, g, fd, sd, cart, 4, -1.3f, 1.3f, kRed);
            for (float sg : {-1.0f, 1.0f}) Tyre(o, g, fd, sd, -4.6f, 0.9f, 0.9f, sg > 0 ? 1.3f : -1.9f, sg > 0 ? 1.9f : -1.3f, 8);
            Beam(o, g + fd * -2.6f + Vec3{0, 1.0f, 0}, g + fd * -1.5f + Vec3{0, 0.8f, 0}, 0.15f, kSteelD);
        }
        V2 c{0, 0};
        for (V2 p : poly) c = c + p;
        c = c * (1.0f / poly.size());
        for (V2 p : poly) loop.push_back(c + (p - c) * 0.6f); // the combine's lap
        break;
    }
    case K::Landfill: {
        for (size_t e = 1; e < poly.size(); ++e) { // perimeter litter fence
            fencePoly(poly[e], poly[(e + 1) % poly.size()], indspom::FENCE_LITTER);
        }
        scatter(18.0f, 6.0f, [&](V2 m, float r) { // gas wells on the capped ground (standing on the mound)
            const float d = lfDepth(Dot2(m, v));
            if (d > fill - 0.1f || r > 0.7f) return;
            const Vec3 g = G(m, 0.3f + std::min(edgeDist(m) * 0.25f, 9.0f) * std::min(1.0f, fill * 3.0f));
            Cyl(o, g, 0.6f, 0.25f, kConc, 6);                                 // well collar
            Cyl(o, g, 0.25f, 1.6f, kWhite, 6, false);                         // riser
            Beam(o, g + Vec3{0, 1.3f, 0}, g + Vec3{1.2f, 1.3f, 0}, 0.18f, kYellow); // valve + lateral
            Cyl(o, g + Vec3{0, 1.6f, 0}, 0.4f, 0.22f, {0.25f, 0.25f, 0.25f}, 6);
        });
        V2 c{0, 0};
        for (V2 p : poly) c = c + p;
        c = c * (1.0f / poly.size());
        for (V2 p : poly) loop.push_back(c + (p - c) * 0.55f); // the compactor's round
        break;
    }
    default: break;
    }
}

// Ground-hugging heat map of one resource around a point (unlit cells; the low end is left as terrain).
void Heat(tbd::city::Res r, V2 centre, std::vector<Vertex>& o) {
    const float cs = 14.0f, R = 560.0f;
    const tbd::Vec3 hi = tbd::city::ResColor(r), lo{0.3f, 0.32f, 0.3f};
    float t, d;
    for (float z = -R; z < R; z += cs)
        for (float x = -R; x < R; x += cs) {
            const float px = centre.x + x, pz = centre.z + z;
            const float v = g_ind.field.At(r, px + cs * 0.5f, pz + cs * 0.5f);
            if (v < 0.04f) continue;
            if (g_roads.net.SegmentAt({px + cs * 0.5f, pz + cs * 0.5f}, 1.0f, t, d) >= 0) continue;
            const Vec3 col = lo * (1 - v) + hi * v;
            auto P = [&](float a, float b) { return Vec3{a, IndGround(a, b) + 0.4f, b}; };
            zfx::Quad(o, P(px, pz), P(px + cs, pz), P(px + cs, pz + cs), P(px, pz + cs), {0, 0, 0}, col);
        }
}

struct Cache {
    uint64_t ver = 0;
    std::vector<Vertex> tris;
    std::vector<Vec3> jacks;
    std::vector<V2> loop;
    std::vector<TreeInst> trees;
    std::vector<Vertex> treeTris[2]; // per species, drawn with its texture
    std::vector<CowInst> cows;
    std::vector<Vertex> ground[kGrCount]; // the area's ground, per texture
};
} // namespace ind

std::unordered_map<uint64_t, ind::Cache> g_indCache;

// ---- tool actions
void IndustryLevel(tbd::city::IndSite& s) {
    const tbd::city::V2 frontMid = s.c + s.front * (tbd::city::kHubD * 0.5f + 2.0f);
    const float target = RoadsGround(frontMid.x, frontMid.z);
    if (g_roads.flatten)
        for (int pass = 0; pass < 2; ++pass)
            for (float a = -tbd::city::kHubW * 0.5f + 2; a <= tbd::city::kHubW * 0.5f - 2 + 0.01f; a += 4)
                for (float b = -tbd::city::kHubD * 0.5f + 2; b <= tbd::city::kHubD * 0.5f - 2 + 0.01f; b += 4) {
                    const tbd::city::V2 q = s.c + s.Side() * a + s.front * b;
                    float t, d;
                    if (g_roads.net.SegmentAt(q, 7.5f, t, d) >= 0) continue;
                    g_roads.flatten(q.x, q.z, 4.5f, 7.0f, target);
                }
    s.y = RoadsGround(s.c.x, s.c.z);
}
// Remote "industry_demo": one site of every kind in a row along +x from (x, z), hubs facing -z, each with a 90 x 110 m area.
bool IndustryDemo(float x, float z, std::string& out) {
    for (int k = 0; k < (int)tbd::city::IndKind::Count; ++k) {
        tbd::city::IndSite s;
        s.id = g_ind.nextId++;
        s.kind = (tbd::city::IndKind)k;
        s.c = {x + k * 130.0f, z};
        s.front = {0, -1};
        s.y = HubHeight(s);
        tbd::city::V2 a, b;
        s.BackEdge(a, b);
        const tbd::city::V2 back{0, 1}, ab = tbd::city::Norm(b - a) * 40.0f;
        s.area = {b + ab, b + ab + back * 110.0f, a - ab + back * 110.0f, a - ab};
        if (k == (int)tbd::city::IndKind::Landfill) s.stock = 0.4 * (double)(tbd::city::PolyArea(s.Poly()) / 10000.0f) * tbd::city::kLandfillTPerHa;
        g_ind.sites.push_back(s);
        g_ind.Refresh(g_ind.sites.back());
    }
    out = "placed " + std::to_string((int)tbd::city::IndKind::Count) + " sites from " + std::to_string((int)x) + " " + std::to_string((int)z) + ", 130 m apart along +x";
    return true;
}
// Remote "industry_dump|path.js": every hub, vehicle and field prop as triangles in a JS file for industry_models.html.
// Plain triangles carry position + colour; slab triangles position + their texture uv (u along / period, v across / extent).
bool IndustryDump(const std::string& path, std::string& out) {
    using tbd::Vec3;
    std::string js = "window.IND_MODELS=[\n";
    auto emit = [&](const char* name, const char* group, const char* tex, const std::vector<tbd::Vertex>& v) {
        std::string plain, slab[4], spom[indspom::COUNT * 2];
        char b[200];
        for (const tbd::Vertex& p : v) {
            const int sv = ind::SpomTexOf(p);
            if (sv >= 0) { // SPOM volume: position, unit N, (W, H, z), uv in metres
                const float l = std::sqrt(p.nx * p.nx + p.ny * p.ny + p.nz * p.nz);
                snprintf(b, sizeof(b), "%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,", p.px, p.py, p.pz, p.nx / l, p.ny / l, p.nz / l, p.r, p.g, p.b, p.u, p.v);
                spom[sv] += b;
                continue;
            }
            const int s = ind::SlabSlot(p.a);
            if (s < 0) {
                snprintf(b, sizeof(b), "%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,", p.px, p.py, p.pz, std::clamp(p.r, 0.0f, 1.0f), std::clamp(p.g, 0.0f, 1.0f), std::clamp(p.b, 0.0f, 1.0f));
                plain += b;
            } else {
                const bool roof = p.a > 0.815f;
                snprintf(b, sizeof(b), "%.2f,%.2f,%.2f,%.4f,%.4f,", p.px, p.py, p.pz, p.u / std::max(p.g, 0.1f), roof ? 1.0f - p.v / std::max(p.b, 0.1f) : p.v / std::max(p.b, 0.1f));
                slab[s] += b;
            }
        }
        js += std::string("{name:\"") + name + "\",group:\"" + group + "\",tex:\"" + tex + "\",tris:" + std::to_string(v.size() / 3) + ",plain:[" + plain + "],slab:[";
        for (int s = 0; s < 4; ++s) js += "[" + slab[s] + "],";
        js += "],spom:{";
        for (int k = 0; k < indspom::COUNT * 2; ++k)
            if (!spom[k].empty()) js += "\"" + std::string(indspom::kVol[k / 2].file) + ((k & 1) ? "_f" : "") + "\":[" + spom[k] + "],";
        js += "}},\n";
    };
    static const char* names[] = {"grain", "livestock", "timber", "oil", "ore", "landfill"};
    static const char* titles[] = {"Grain Farm hub", "Livestock Farm hub", "Forestry sawmill hub", "Oil Drilling hub", "Ore Mine hub", "Landfill hub"};
    std::vector<tbd::Vertex> v;
    for (int k = 0; k < (int)tbd::city::IndKind::Count; ++k) {
        tbd::city::IndSite s;
        s.kind = (tbd::city::IndKind)k;
        s.c = {0, 0}, s.front = {0, 1}, s.y = 0;
        v.clear();
        ind::Hub(s, v);
        // drop the deep foundation skirt below ground (it is there for slopes)
        for (tbd::Vertex& p : v) p.py = std::max(p.py, -0.6f);
        emit(titles[k], "Hubs", names[k], v);
    }
    const Vec3 g{0, 0, 0}, fd{1, 0, 0}, sd{0, 0, 1};
    v.clear(), ind::Combine(v, g, fd, sd, 0.3f), emit("Combine harvester", "Vehicles", "", v);
    v.clear(), ind::HaulTruck(v, g, fd, sd, ind::kYellow), emit("Ore haul truck", "Vehicles", "", v);
    v.clear(), ind::Compactor(v, g, fd, sd), emit("Landfill compactor", "Vehicles", "", v);
    v.clear(), ind::Excavator(v, g, fd, sd, ind::kYellow), emit("Mining shovel", "Vehicles", "", v);
    v.clear(), ind::Forwarder(v, g, fd, sd), emit("Forwarder (loaded)", "Vehicles", "", v);
    v.clear(), ind::Tractor(v, g, fd, sd, {0.21f, 0.33f, 0.19f}), emit("Tractor", "Vehicles", "", v);
    v.clear(), ind::PumpjackBase(v, g), ind::PumpjackBeam(v, g, 0.8f, {0.18f, 0.37f, 0.54f});
    for (tbd::Vertex& p : v) p.py = std::max(p.py, -0.3f);
    emit("Pumpjack", "Field props", "", v);
    // cattle: the close-up SPOM volume and the far LOD of each breed / pose
    const struct { const char* name; float r; } cows[] = {{"Holstein", 0.08f}, {"Holstein grazing", 0.3f}, {"Angus grazing", 0.9f}, {"Hereford", 0.65f}};
    for (const auto& cw : cows) {
        v.clear(), ind::SpomVolume(v, g, fd, sd, ind::CowVolume(cw.r)), emit((std::string("Cow ") + cw.name + " (close-up)").c_str(), "Cattle", "", v);
        v.clear(), ind::Cow(v, g, fd, sd, cw.r), emit((std::string("Cow ") + cw.name + " (LOD)").c_str(), "Cattle", "", v);
    }
    v.clear(), ind::FencePiece(v, g, g + fd * 8.0f, indspom::FENCE_RAIL), emit("Split-rail fence, 8 m", "Field props", "", v);
    v.clear(), ind::FencePiece(v, g, g + fd * 8.0f, indspom::FENCE_LITTER), emit("Landfill litter fence, 8 m", "Field props", "", v);
    v.clear(), ind::Bale(v, g, fd, sd), emit("Round bale", "Field props", "", v);
    js += "];\n";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { out = "cannot write " + path; return false; }
    fwrite(js.data(), 1, js.size(), f);
    fclose(f);
    out = "wrote " + path + " (" + std::to_string(js.size() / 1024) + " KB)";
    return true;
}
void IndustrySelect(int i) { g_indTool.sel = i; }
void IndustryStartPlace(int kind) {
    if (g_indTool.mode == IndustryTool::Draw) return;
    g_indTool.kind = kind;
    g_indTool.mode = IndustryTool::Place;
    g_indTool.sel = -1;
}
void IndustryFinish() {
    auto& T = g_indTool;
    if (T.drawing < 0) return;
    tbd::city::IndSite& s = g_ind.sites[T.drawing];
    if (s.area.empty()) { // nothing drawn: the hub goes too
        g_indCache.erase(s.id);
        g_ind.sites.erase(g_ind.sites.begin() + T.drawing);
        T.sel = -1;
    } else {
        g_ind.Refresh(s);
        T.sel = T.drawing;
    }
    T.drawing = -1;
    T.mode = IndustryTool::Pick;
}
bool IndustryPointOk(const tbd::city::IndSite& s, tbd::city::V2 q) {
    float t, d;
    if (g_roads.net.SegmentAt(q, 1.0f, t, d) >= 0) return false;
    for (const auto& o : g_ind.sites)
        if (o.InHub(q, 1.0f)) return false;
    (void)s;
    return true;
}

// ---- Waste Management point buildings (incinerator, recycling center, hazardous waste site): one GLB each, snapped beside a
// road with its entrance toward it. The landfill is the hub + drawn-area site above.
void GarbagePanel(); // city_garbage.inl
struct WasteType {
    const char* name;
    const char* file;      // assets/buildings/waste/glb
    float w, d;            // footprint in metres: along the road, away from it
    float yaw;             // extra turn so the model's entrance side faces the road (model +z faces it at 0)
    const char* what;
    float tPerMonth;       // garbage processed
    float mw;              // electricity produced (incinerator)
    bool unique;
};
const WasteType kWasteTypes[] = {
    {"Incineration Plant", "incinerator.glb", 100, 64, 3.14159265f, "Burns garbage, makes electricity", 9000, 18, false},
    {"Recycling Center", "recycling_center.glb", 80, 60, 0.0f, "Sorts garbage into materials to sell", 6000, 0, false},
    {"Industrial Waste Site", "hazardous_waste.glb", 48, 64, -1.5707963f, "Takes industrial waste (one per city)", 4000, 0, true},
};
struct WasteBuilding {
    uint64_t id = 0;
    int type = 0;
    tbd::city::V2 c, front;
    float y = 0;
    std::string entity;
};
std::vector<WasteBuilding> g_wasteB;
uint64_t g_wasteNext = 1;
struct WasteGhost {
    WasteBuilding b;
    bool shown = false, ok = false;
} g_wasteGhost;

tbd::city::V2 WasteSide(const WasteBuilding& b) { return tbd::city::LeftOf(b.front); }
std::vector<tbd::city::V2> WasteRect(const WasteBuilding& b, float pad = 0) {
    const WasteType& t = kWasteTypes[b.type];
    const tbd::city::V2 sd = WasteSide(b) * (t.w * 0.5f + pad), f = b.front * (t.d * 0.5f + pad);
    return {b.c - sd - f, b.c + sd - f, b.c + sd + f, b.c - sd + f};
}
bool WasteContains(const WasteBuilding& b, tbd::city::V2 q, float pad = 0) {
    const WasteType& t = kWasteTypes[b.type];
    const tbd::city::V2 d = q - b.c;
    return std::fabs(tbd::city::Dot2(d, WasteSide(b))) < t.w * 0.5f + pad && std::fabs(tbd::city::Dot2(d, b.front)) < t.d * 0.5f + pad;
}
bool WasteSnap(tbd::city::V2 p, WasteBuilding& b) {
    float t, d;
    const int seg = g_roads.net.SegmentAt(p, 90.0f, t, d);
    if (seg < 0) return false;
    const tbd::city::Profile& pr = g_roads.net.Prof(seg);
    if (pr.cat == tbd::city::Cat::Highway || pr.cat == tbd::city::Cat::Path) return false;
    const tbd::city::Cubic cu = g_roads.net.Curve(seg);
    const tbd::city::V2 rp = cu.At(t), left = tbd::city::LeftOf(cu.Tan(t));
    const tbd::city::V2 out = tbd::city::Dot2(p - rp, left) >= 0 ? left : left * -1.0f;
    b.c = rp + out * (pr.Half() + kWasteTypes[b.type].d * 0.5f + 1.0f);
    b.front = out * -1.0f;
    b.y = RoadsGround(rp.x, rp.z);
    return true;
}
bool WasteFree(const WasteBuilding& b) {
    const WasteType& T = kWasteTypes[b.type];
    float t, d;
    for (float a = -0.5f; a <= 0.51f; a += 0.125f)
        for (float c = -0.5f; c <= 0.47f; c += 0.125f)
            if (g_roads.net.SegmentAt(b.c + WasteSide(b) * (a * T.w) + b.front * (c * T.d), 0.0f, t, d) >= 0) return false;
    for (const WasteBuilding& o : g_wasteB) {
        if (T.unique && o.type == b.type) return false;
        for (tbd::city::V2 q : WasteRect(o)) if (WasteContains(b, q)) return false;
        for (tbd::city::V2 q : WasteRect(b)) if (WasteContains(o, q)) return false;
        if (WasteContains(o, b.c)) return false;
    }
    for (const auto& s : g_ind.sites) {
        if (WasteContains(b, s.c, tbd::city::kHubD * 0.5f)) return false;
        for (tbd::city::V2 q : s.Poly()) if (s.area.size() && WasteContains(b, q)) return false;
    }
    return true;
}
void WastePlaceEntity(tbd::Entity& e, const WasteBuilding& b) {
    e.position = {b.c.x, b.y, b.c.z};
    e.rotation = tbd::QuatAxisAngle({0, 1, 0}, std::atan2(b.front.x, b.front.z) + kWasteTypes[b.type].yaw);
    e.scale = {1, 1, 1};
}
bool WasteBuild(WasteBuilding b) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path f = fs::path(tbd::ExeDirectory()) / L".." / L"assets" / L"buildings" / L"waste" / L"glb" / kWasteTypes[b.type].file;
    tbd::Entity e;
    e.mesh = tbd::MeshKind::Imported;
    e.meshPath = tbd::WideToUtf8(fs::weakly_canonical(f, ec).wstring()) + "#native";
    if (ImportedVertices(e.meshPath).empty()) return false;
    b.id = g_wasteNext++;
    b.entity = std::string(kWasteTypes[b.type].name) + " " + std::to_string(b.id);
    if (g_roads.flatten) // level the lot to the road's height so the slab meets the street
        {
            const WasteType& T = kWasteTypes[b.type];
            for (float a = -T.w * 0.5f + 4; a <= T.w * 0.5f - 4 + 0.01f; a += 8)
                for (float c = -T.d * 0.5f + 4; c <= T.d * 0.5f - 4 + 0.01f; c += 8) {
                    const tbd::city::V2 p = b.c + WasteSide(b) * a + b.front * c;
                    float t, d;
                    if (g_roads.net.SegmentAt(p, 6.0f, t, d) >= 0) continue;
                    g_roads.flatten(p.x, p.z, 6.0f, 9.0f, b.y);
                }
        }
    e.name = b.entity;
    e.color = {1, 1, 1};
    WastePlaceEntity(e, b);
    CompleteSceneLoad();
    const int keepSel = g_engine.scene.selected;
    g_engine.scene.Add(e);
    g_engine.scene.selected = keepSel;
    g_engine.meshes.push_back(UploadEntityMesh(e));
    g_wasteB.push_back(b);
    return true;
}
void WasteRemove(int i) {
    auto& ents = g_engine.scene.entities;
    for (size_t k = 0; k < ents.size(); ++k)
        if (ents[k].name == g_wasteB[(size_t)i].entity) {
            g_engine.scene.selected = (int)k;
            DeleteSelected();
            g_engine.scene.selected = -1;
            break;
        }
    g_wasteB.erase(g_wasteB.begin() + i);
}
void WasteStartPlace(int type) { // type: index into kWasteTypes, or -1 for the landfill tool
    auto& T = g_indTool;
    if (T.mode == IndustryTool::Draw) return;
    T.wsel = -1, T.sel = -1;
    if (type < 0) return (void)(T.point = 0, IndustryStartPlace((int)tbd::city::IndKind::Landfill));
    T.point = type + 1;
    T.mode = IndustryTool::Place;
}

void IndustryCursor() {
    auto& T = g_indTool;
    tbd::Vec3 hit;
    const bool overGui = g_gui.WantsMouseAt((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY);
    T.have = g_terrainEdit.mouseX >= 0 && !overGui &&
             GroundHit(g_engine.camera.ScreenRay((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY, (float)g_engine.vpW, (float)g_engine.vpH), hit);
    if (!T.have) return;
    T.cur = {hit.x, hit.z};
    if (T.mode == IndustryTool::Place && T.point) {
        g_wasteGhost.b.type = T.point - 1;
        g_wasteGhost.shown = WasteSnap(T.cur, g_wasteGhost.b);
        g_wasteGhost.ok = g_wasteGhost.shown && WasteFree(g_wasteGhost.b);
        return;
    }
    if (T.mode == IndustryTool::Place) {
        T.ghost.kind = (tbd::city::IndKind)T.kind;
        T.ghost.id = 0;
        T.ghostShown = g_ind.Snap(g_roads.net, T.cur, T.ghost);
        if (T.ghostShown) {
            T.ghost.y = HubHeight(T.ghost);
            T.ghostOk = g_ind.Free(g_roads.net, T.ghost);
        }
    }
    if (T.dragIdx >= 0 && T.sel >= 0) {
        tbd::city::IndSite& s = g_ind.sites[T.sel];
        const tbd::city::V2 old = s.area[T.dragIdx];
        s.area[T.dragIdx] = T.cur;
        if (!IndustryPointOk(s, T.cur) || !tbd::city::PolySimple(s.Poly())) s.area[T.dragIdx] = old;
        g_ind.Refresh(s);
    }
}

void IndustryPress() {
    auto& T = g_indTool;
    IndustryCursor();
    if (!T.have) return;
    if (T.mode == IndustryTool::Place && T.point) {
        if (!g_wasteGhost.shown || !g_wasteGhost.ok) {
            Toast(kWasteTypes[T.point - 1].unique && std::any_of(g_wasteB.begin(), g_wasteB.end(), [&](const WasteBuilding& o) { return o.type == T.point - 1; })
                      ? "ONLY ONE PER CITY" : "NEEDS FREE SPACE BESIDE A ROAD");
            return;
        }
        if (!WasteBuild(g_wasteGhost.b)) Toast("MODEL MISSING: ASSETS/BUILDINGS/WASTE/GLB");
        return;
    }
    if (T.waste && T.mode == IndustryTool::Pick) { // a point building under the cursor
        T.wsel = -1;
        for (size_t i = 0; i < g_wasteB.size(); ++i)
            if (WasteContains(g_wasteB[i], T.cur)) { T.wsel = (int)i, T.sel = -1; return; }
    }
    if (T.mode == IndustryTool::Place) {
        if (!T.ghostShown || !T.ghostOk) {
            Toast("NEEDS FREE SPACE BESIDE A ROAD");
            return;
        }
        tbd::city::IndSite s = T.ghost;
        s.id = g_ind.nextId++;
        s.y = HubHeight(s); // no terrain digging: the foundation skirt fills the slope
        g_ind.sites.push_back(s);
        T.drawing = (int)g_ind.sites.size() - 1;
        T.mode = IndustryTool::Draw;
        return;
    }
    if (T.mode == IndustryTool::Draw) {
        tbd::city::IndSite& s = g_ind.sites[T.drawing];
        const std::vector<tbd::city::V2> poly = s.Poly();
        if (!s.area.empty() && (tbd::city::Len(T.cur - poly.front()) < 6.0f || tbd::city::Len(T.cur - poly.back()) < 3.0f)) {
            IndustryFinish();
            return;
        }
        if (tbd::city::Len(T.cur - poly.back()) < 3.0f) return;
        if (!IndustryPointOk(s, T.cur) || !tbd::city::PolySimple(s.Poly(&T.cur))) {
            Toast("THE AREA CAN'T CROSS ITSELF, A ROAD OR A HUB");
            return;
        }
        s.area.push_back(T.cur);
        g_ind.Refresh(s);
        return;
    }
    // Pick: a corner of the selected site, an edge (adds a corner), or another site
    if (T.sel >= 0) {
        tbd::city::IndSite& s = g_ind.sites[T.sel];
        const float grab = std::max(4.0f, g_engine.camera.distance * 0.02f);
        for (size_t i = 0; i < s.area.size(); ++i)
            if (tbd::city::Len(T.cur - s.area[i]) < grab) {
                T.dragIdx = (int)i;
                return;
            }
        const std::vector<tbd::city::V2> poly = s.Poly();
        for (size_t e = 1; e < poly.size(); ++e)
            if (tbd::city::SegDist(T.cur, poly[e], poly[(e + 1) % poly.size()]) < grab * 0.6f) {
                s.area.insert(s.area.begin() + (e - 1), T.cur);
                if (!tbd::city::PolySimple(s.Poly())) {
                    s.area.erase(s.area.begin() + (e - 1));
                    break;
                }
                g_ind.Refresh(s);
                T.dragIdx = (int)(e - 1);
                return;
            }
    }
    T.sel = g_ind.SiteAt(T.cur);
    if (T.sel >= 0 && g_ind.sites[T.sel].IsLandfill() != T.waste) T.sel = -1; // each menu edits only its own sites
}
void IndustryRelease() { g_indTool.dragIdx = -1; }
// Right click: cancel placing, or finish the area being drawn.
void IndustryRightClick() {
    auto& T = g_indTool;
    if (T.mode == IndustryTool::Draw) IndustryFinish();
    else T.mode = IndustryTool::Pick, T.point = 0;
}
bool IndustryKey(uint64_t k) {
    using PK = tbd::platform::Key;
    auto& T = g_indTool;
    if (k == (uint64_t)PK::Escape) {
        if (T.mode == IndustryTool::Draw) IndustryFinish();
        else if (T.mode == IndustryTool::Place) T.mode = IndustryTool::Pick, T.point = 0;
        else T.active = false, T.sel = -1;
        return true;
    }
    if (k == (uint64_t)PK::Return && T.mode == IndustryTool::Draw) return IndustryFinish(), true;
    if (k == (uint64_t)PK::Back && T.mode == IndustryTool::Draw) {
        auto& a = g_ind.sites[T.drawing].area;
        if (!a.empty()) a.pop_back(), g_ind.Refresh(g_ind.sites[T.drawing]);
        return true;
    }
    if ((k == (uint64_t)PK::Delete || k == (uint64_t)PK::Back) && T.mode == IndustryTool::Pick && T.waste && T.wsel >= 0) {
        WasteRemove(T.wsel);
        T.wsel = -1;
        return true;
    }
    if ((k == (uint64_t)PK::Delete || k == (uint64_t)PK::Back) && T.mode == IndustryTool::Pick && T.sel >= 0) {
        g_indCache.erase(g_ind.sites[T.sel].id);
        g_ind.sites.erase(g_ind.sites.begin() + T.sel);
        T.sel = -1;
        return true;
    }
    if (T.waste && k >= '1' && k <= '4' && T.mode != IndustryTool::Draw) return WasteStartPlace((int)(k - '1') - 1), true;
    if (!T.waste && k >= '1' && k <= '5' && T.mode != IndustryTool::Draw) return IndustryStartPlace((int)(k - '1')), true;
    return false;
}

void IndustrySync(float realDt) {
    if (!g_ind.field.ground) g_ind.field.ground = RoadsGround;
    const float minutes = realDt * g_clock.Rate();
    if (minutes > 0) g_ind.Tick(minutes);
}

// ---- drawing
// SPOM slab textures per kind and slot (tools/building_gen/industry_tex.py), loaded on first draw.
tbd::TextureId g_indTex[(int)tbd::city::IndKind::Count][4];
bool g_indTexLoaded = false;
void IndustryLoadTextures(tbd::Renderer& r) {
    if (g_indTexLoaded) return;
    g_indTexLoaded = true;
    static const char* names[] = {"grain", "livestock", "timber", "oil", "ore", "landfill"};
    for (int k = 0; k < (int)tbd::city::IndKind::Count; ++k)
        for (int s = 0; s < 4; ++s) {
            g_indTex[k][s] = tbd::kNoTexture;
            std::wstring path = tbd::ExeDirectory() + L"\\..\\assets\\industry\\tex\\";
            for (const char* c = names[k]; *c; ++c) path += (wchar_t)*c;
            path += L"_" + std::to_wstring(s) + L".png";
            std::string bytes;
            std::vector<uint8_t> rgba;
            int w = 0, h = 0;
            if (tbd::ReadAllText(path, bytes) && tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), rgba, w, h)) g_indTex[k][s] = r.CreateTexture(rgba.data(), w, h);
            else tbd::Log("Industry: missing slab texture %ls", path.c_str());
        }
}
// The forestry trees: the repo's own Pine / Spruce LOD1 models (lowpoly_tree/out/obj) and their alpha-cut textures.
tbd::TextureId g_indTreeTex[2] = {tbd::kNoTexture, tbd::kNoTexture};
const std::vector<tbd::Vertex>& IndustryTreeMesh(int species) {
    static const char* names[] = {"Pine", "Spruce"};
    const std::string path = tbd::WideToUtf8(tbd::ExeDirectory()) + "\\..\\lowpoly_tree\\out\\obj\\" + names[species] + "_LOD1.obj";
    return ImportedVertices(path);
}
void IndustryLoadTreeTextures(tbd::Renderer& r) {
    static bool done = false;
    if (done) return;
    done = true;
    static const wchar_t* files[] = {L"Pine\\Pine_BaseColor.png", L"Spruce\\Spruce_BaseColor.png"};
    for (int k = 0; k < 2; ++k) {
        const std::wstring path = tbd::ExeDirectory() + L"\\..\\lowpoly_tree\\out\\cs2\\" + files[k];
        std::string bytes;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (tbd::ReadAllText(path, bytes) && tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), rgba, w, h)) g_indTreeTex[k] = r.CreateTexture(rgba.data(), w, h);
        else tbd::Log("Industry: missing tree texture %ls", path.c_str());
    }
}
// Instances the cached sites' trees: each a copy of the model, turned and scaled (static, so built once per site change).
void IndustryBuildTrees(ind::Cache& c) {
    for (auto& t : c.treeTris) t.clear();
    for (const ind::TreeInst& t : c.trees) {
        const std::vector<tbd::Vertex>& m = IndustryTreeMesh(t.species);
        if (m.empty()) continue;
        // the importer normalises the model's size: scale it back to a grown tree's 18 m, base on the ground
        float y0 = 1e30f, y1 = -1e30f;
        for (const tbd::Vertex& v : m) y0 = std::min(y0, v.py), y1 = std::max(y1, v.py);
        const float k = t.k * 18.0f / std::max(y1 - y0, 1e-3f);
        const float cs = std::cos(t.yaw), sn = std::sin(t.yaw);
        for (tbd::Vertex v : m) {
            const float x = v.px * cs - v.pz * sn, z = v.px * sn + v.pz * cs, nx = v.nx * cs - v.nz * sn, nz = v.nx * sn + v.nz * cs;
            v.px = t.p.x + x * k, v.py = t.p.y + (v.py - y0) * k, v.pz = t.p.z + z * k, v.nx = nx, v.nz = nz;
            c.treeTris[t.species].push_back(v);
        }
    }
}
// Draws a hub's triangles: plain lit colour in one call, each SPOM slab slot with its texture bound.
// ---- rigged close-up cattle (tools/building_gen/gen_cattle.py -> assets/industry/cattle/glb/COW_<breed>.cowbin + .png)
// 400 triangles, 15 bones, two looping clips (0 walk, 1 graze), skinned on the CPU each frame for the cows near the eye.
// Model space: +x forward, +y up, +z the cow's left; origin on the ground under the barrel.
struct CowRig {
    struct Clip { int frames = 0; float duration = 1; std::vector<float> q; }; // frames x bones x (x, y, z, w)
    int nv = 0, nb = 0;
    std::vector<float> vtx;  // per vertex: pos3 nrm3 uv2 joints4 weights4
    std::vector<uint32_t> idx;
    std::vector<int> parent;
    std::vector<tbd::Vec3> rest;
    Clip clips[2];
    tbd::TextureId tex = tbd::kNoTexture;
    bool ok = false;
};
CowRig g_cowRig[3]; // holstein, angus, hereford
bool g_cowRigLoaded = false;
void IndustryLoadCowRigs(tbd::Renderer& r) {
    if (g_cowRigLoaded) return;
    g_cowRigLoaded = true;
    static const wchar_t* names[] = {L"holstein", L"angus", L"hereford"};
    for (int k = 0; k < 3; ++k) {
        CowRig& R = g_cowRig[k];
        const std::wstring base = tbd::ExeDirectory() + L"\\..\\assets\\industry\\cattle\\glb\\COW_" + names[k];
        std::string b;
        if (!tbd::ReadAllText(base + L".cowbin", b) || b.size() < 20 || b.compare(0, 4, "COW1") != 0) { tbd::Log("Industry: missing cow rig %ls", base.c_str()); continue; }
        size_t at = 4;
        auto rd = [&](void* dst, size_t n) { if (at + n > b.size()) return false; std::memcpy(dst, b.data() + at, n); at += n; return true; };
        int head[4];
        if (!rd(head, sizeof(head))) continue;
        R.nv = head[0], R.nb = head[1 + 1];
        const int ni = head[1], nc = head[3];
        R.parent.resize(R.nb), R.rest.resize(R.nb);
        bool good = true;
        for (int i = 0; i < R.nb && good; ++i) {
            int p;
            float xyz[3];
            good = rd(&p, 4) && rd(xyz, 12);
            R.parent[i] = p, R.rest[i] = {xyz[0], xyz[1], xyz[2]};
        }
        R.vtx.resize((size_t)R.nv * 16), R.idx.resize(ni);
        good = good && rd(R.vtx.data(), R.vtx.size() * 4) && rd(R.idx.data(), R.idx.size() * 4);
        for (int c = 0; c < nc && c < 2 && good; ++c) {
            good = rd(&R.clips[c].frames, 4) && rd(&R.clips[c].duration, 4);
            R.clips[c].q.resize((size_t)R.clips[c].frames * R.nb * 4);
            good = good && rd(R.clips[c].q.data(), R.clips[c].q.size() * 4);
        }
        std::string png;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (tbd::ReadAllText(base + L".png", png) && tbd::DecodeImage((const uint8_t*)png.data(), png.size(), rgba, w, h)) R.tex = r.CreateTexture(rgba.data(), w, h);
        R.ok = good && R.tex != tbd::kNoTexture;
        if (!R.ok) tbd::Log("Industry: cow rig %ls is incomplete", base.c_str());
    }
}
namespace ind {
struct Q4 { float x, y, z, w; };
inline Q4 QMul(Q4 a, Q4 b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Vec3 QRot(Q4 q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = tbd::Cross(u, v) * 2.0f;
    return v + t * q.w + tbd::Cross(u, t);
}
// One cow posed at clip `clip` time t, placed at g facing fd (sd its left), into `out` (textured, lit).
void CowRigDraw(const CowRig& R, int clip, float t, Vec3 g, Vec3 fd, Vec3 sd, std::vector<Vertex>& out) {
    const CowRig::Clip& C = R.clips[clip];
    if (C.frames < 2) return;
    float f = std::fmod(t, C.duration) / C.duration * (C.frames - 1);
    if (f < 0) f += (float)(C.frames - 1);
    const int f0 = std::min((int)f, C.frames - 2);
    const float k = f - f0;
    Q4 wr[16];
    Vec3 wp[16];
    for (int b = 0; b < R.nb && b < 16; ++b) {
        const float* a = &C.q[((size_t)f0 * R.nb + b) * 4];
        const float* c = &C.q[((size_t)(f0 + 1) * R.nb + b) * 4];
        const float dot = a[0] * c[0] + a[1] * c[1] + a[2] * c[2] + a[3] * c[3], s = dot < 0 ? -1.0f : 1.0f;
        Q4 q{a[0] + (c[0] * s - a[0]) * k, a[1] + (c[1] * s - a[1]) * k, a[2] + (c[2] * s - a[2]) * k, a[3] + (c[3] * s - a[3]) * k};
        const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        q = {q.x / l, q.y / l, q.z / l, q.w / l};
        const int p = R.parent[b];
        if (p < 0) wr[b] = q, wp[b] = R.rest[b];
        else wr[b] = QMul(wr[p], q), wp[b] = wp[p] + QRot(wr[p], R.rest[b] - R.rest[p]);
    }
    auto place = [&](Vec3 m) { return g + fd * m.x + Vec3{0, m.y, 0} + sd * m.z; };
    auto turn = [&](Vec3 m) { return fd * m.x + Vec3{0, m.y, 0} + sd * m.z; };
    static std::vector<Vertex> skinned;
    skinned.resize(R.nv);
    for (int i = 0; i < R.nv; ++i) {
        const float* v = &R.vtx[(size_t)i * 16];
        const Vec3 p{v[0], v[1], v[2]}, n{v[3], v[4], v[5]};
        Vec3 sp{0, 0, 0}, sn{0, 0, 0};
        for (int j = 0; j < 4; ++j) {
            const float w = v[12 + j];
            if (w <= 0) continue;
            const int b = std::clamp((int)v[8 + j], 0, R.nb - 1);
            sp = sp + (wp[b] + QRot(wr[b], p - R.rest[b])) * w;
            sn = sn + QRot(wr[b], n) * w;
        }
        const Vec3 wpos = place(sp), wn = tbd::Normalize(turn(sn));
        skinned[i] = {wpos.x, wpos.y, wpos.z, wn.x, wn.y, wn.z, 1, 1, 1, 1, v[6], v[7]};
    }
    for (uint32_t ix : R.idx) out.push_back(skinned[ix]);
}
// Where a cow is and what it is doing at real time t (seconds): grazing, then a short walk, grazing facing back, a walk
// back; 52 s a round, each cow at its own phase. Out: ground position, heading, clip (0 walk, 1 graze), clip time.
inline void CowBehave(const CowInst& c, float t, Vec3& g, Vec3& fd, Vec3& sd, int& clip, float& ct) {
    const float P = 52.0f, v = 0.9f, ph = std::fmod(t + c.r * 977.0f, P);
    float off;
    bool back;
    if (ph < 20) off = 0, back = false, clip = 1, ct = ph;
    else if (ph < 26) off = (ph - 20) * v, back = false, clip = 0, ct = ph - 20;
    else if (ph < 46) off = 6 * v, back = true, clip = 1, ct = ph - 26;
    else off = 6 * v - (ph - 46) * v, back = true, clip = 0, ct = ph - 46;
    g = c.p + c.fd * off;
    g.y = IndGround(g.x, g.z) + 0.3f; // the area's ground cells are drawn 0.3 m above the terrain
    fd = back ? c.fd * -1.0f : c.fd;
    sd = back ? c.sd * -1.0f : c.sd;
}
inline int CowBreed(float r) { return r > 0.72f ? 1 : r > 0.6f ? 2 : 0; }
} // namespace ind

// Work-area ground textures, loaded on first draw.
tbd::TextureId g_groundTex[ind::kGrCount];
void IndustryLoadGround(tbd::Renderer& r) {
    static bool done = false;
    if (done) return;
    done = true;
    static const wchar_t* names[] = {L"grain", L"pasture", L"forest", L"oil", L"ore", L"refuse", L"cover", L"liner", L"cap"};
    for (int k = 0; k < ind::kGrCount; ++k) {
        g_groundTex[k] = tbd::kNoTexture;
        const std::wstring path = tbd::ExeDirectory() + L"\\..\\assets\\industry\\ground\\" + names[k] + L".png";
        std::string bytes;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (tbd::ReadAllText(path, bytes) && tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), rgba, w, h)) g_groundTex[k] = r.CreateTexture(rgba.data(), w, h);
        else tbd::Log("Industry: missing ground texture %ls", path.c_str());
    }
}
// SPOM volume textures (tools/building_gen/industry_spom.py): [volume * 2 + mirrored][albedo, normal], loaded on first draw.
tbd::TextureId g_spomTex[indspom::COUNT * 2][2];
void IndustryLoadSpom(tbd::Renderer& r) {
    static bool done = false;
    if (done) return;
    done = true;
    for (int k = 0; k < indspom::COUNT * 2; ++k)
        for (int m = 0; m < 2; ++m) {
            g_spomTex[k][m] = tbd::kNoTexture;
            std::wstring path = tbd::ExeDirectory() + L"\\..\\assets\\industry\\spom\\";
            for (const char* c = indspom::kVol[k / 2].file; *c; ++c) path += (wchar_t)*c;
            path += (k & 1) ? L"_f" : L"";
            path += m ? L"_n.png" : L"_a.png";
            std::string bytes;
            std::vector<uint8_t> rgba;
            int w = 0, h = 0;
            if (tbd::ReadAllText(path, bytes) && tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), rgba, w, h)) g_spomTex[k][m] = r.CreateTexture(rgba.data(), w, h);
            else tbd::Log("Industry: missing SPOM texture %ls", path.c_str());
        }
}
// Draws industry triangles: plain lit colour in one call, each SPOM slab slot with the kind's texture (kind < 0: none), and
// each SPOM volume with its own albedo + relief pair.
void IndustryDrawSplit(tbd::Renderer& r, const tbd::Mat4& viewProj, const std::vector<tbd::Vertex>& v, int kind) {
    IndustryLoadTextures(r);
    IndustryLoadTreeTextures(r);
    IndustryLoadSpom(r);
    static std::vector<tbd::Vertex> part[5], vol[indspom::COUNT * 2];
    for (auto& p : part) p.clear();
    for (auto& p : vol) p.clear();
    for (size_t k = 0; k + 2 < v.size(); k += 3) {
        const int sv = ind::SpomTexOf(v[k]);
        if (sv >= 0) { vol[sv].insert(vol[sv].end(), v.begin() + (ptrdiff_t)k, v.begin() + (ptrdiff_t)k + 3); continue; }
        const int s = kind >= 0 ? ind::SlabSlot(v[k].a) : -1;
        auto& d = part[s < 0 ? 4 : s];
        d.insert(d.end(), v.begin() + (ptrdiff_t)k, v.begin() + (ptrdiff_t)k + 3);
    }
    if (!part[4].empty()) r.DrawTransient(part[4].data(), (uint32_t)part[4].size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
    bool bound = false;
    for (int s = 0; s < 4 && kind >= 0; ++s) {
        if (part[s].empty() || g_indTex[kind][s] == tbd::kNoTexture) continue;
        r.SetMaterial(g_indTex[kind][s], tbd::kNoTexture);
        r.DrawTransient(part[s].data(), (uint32_t)part[s].size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
        bound = true;
    }
    for (int k = 0; k < indspom::COUNT * 2; ++k) {
        if (vol[k].empty() || g_spomTex[k][0] == tbd::kNoTexture) continue;
        r.SetMaterial(g_spomTex[k][0], g_spomTex[k][1]);
        r.DrawTransient(vol[k].data(), (uint32_t)vol[k].size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
        bound = true;
    }
    if (bound) r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
}
void IndustryDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    using tbd::Vec3;
    auto& T = g_indTool;
    const float time = (float)std::fmod(g_clock.minutes, 100000.0) * 0.5f;
    static std::vector<tbd::Vertex> dyn, cowTris[3];
    dyn.clear();
    for (auto& ct : cowTris) ct.clear();
    // static geometry per site, rebuilt when it changes (not while a corner is being dragged: the outline shows it)
    for (const tbd::city::IndSite& s : g_ind.sites) {
        ind::Cache& c = g_indCache[s.id];
        const bool dragging = T.dragIdx >= 0 && T.sel >= 0 && g_ind.sites[T.sel].id == s.id;
        if (c.ver != s.version && !dragging) {
            c.ver = s.version;
            c.tris.clear(), c.jacks.clear(), c.loop.clear(), c.trees.clear(), c.cows.clear();
            for (auto& gv : c.ground) gv.clear();
            ind::Hub(s, c.tris);
            ind::Area(s, c.tris, c.jacks, c.loop, c.trees, c.cows, c.ground);
            IndustryBuildTrees(c);
        }
        IndustryDrawSplit(r, viewProj, c.tris, (int)s.kind);
        IndustryLoadGround(r);
        for (int k = 0; k < ind::kGrCount; ++k)
            if (!c.ground[k].empty() && g_groundTex[k] != tbd::kNoTexture) {
                r.SetMaterial(g_groundTex[k], tbd::kNoTexture);
                r.DrawTransient(c.ground[k].data(), (uint32_t)c.ground[k].size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
                r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
            }
        for (int sp = 0; sp < 2; ++sp)
            if (!c.treeTris[sp].empty() && g_indTreeTex[sp] != tbd::kNoTexture) {
                r.SetMaterial(g_indTreeTex[sp], tbd::kNoTexture);
                r.DrawTransient(c.treeTris[sp].data(), (uint32_t)c.treeTris[sp].size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
                r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
            }
        ind::HubAnim(s, time, dyn);
        { // cattle graze and wander; within 60 m of the eye the rigged, animated cow, further out its 12-triangle SPOM volume
            IndustryLoadCowRigs(r);
            const Vec3 eye = g_engine.camera.Eye();
            const float rt = (float)std::fmod((double)tbd::platform::TickCountMs() / 1000.0, 1.0e5);
            for (const ind::CowInst& cw : c.cows) {
                Vec3 g, fd, sd;
                int clip;
                float ct;
                ind::CowBehave(cw, rt, g, fd, sd, clip, ct);
                const Vec3 d = g - eye;
                const int breed = ind::CowBreed(cw.r);
                if (tbd::Dot(d, d) < 60.0f * 60.0f && g_cowRig[breed].ok) ind::CowRigDraw(g_cowRig[breed], clip, ct, g, fd, sd, cowTris[breed]);
                else {
                    // the volume shows the cow's pose: grazing breeds have a head-down volume, walking ones stand up
                    int vol = breed == 1 ? indspom::COW_ANGUS_G : breed == 2 ? indspom::COW_HERE : (clip == 1 ? indspom::COW_HOL_G : indspom::COW_HOL);
                    ind::SpomVolume(dyn, g, fd, sd, vol);
                }
            }
        }
        for (size_t i = 0; i < c.jacks.size(); ++i) // pumpjacks nodding out of phase
            ind::PumpjackBeam(dyn, c.jacks[i], time * 1.7f + i * 1.3f, {0.18f, 0.37f, 0.54f});
        if (c.loop.size() >= 3) { // haul trucks / combine on their lap
            float tot = 0;
            for (size_t i = 0; i < c.loop.size(); ++i) tot += tbd::city::Len(c.loop[(i + 1) % c.loop.size()] - c.loop[i]);
            const int n = s.kind == tbd::city::IndKind::Ore ? 3 : 1;
            for (int k = 0; k < n; ++k) {
                float d = std::fmod(time * 6.0f + k * tot / n, tot);
                for (size_t i = 0; i < c.loop.size(); ++i) {
                    const tbd::city::V2 a = c.loop[i], b = c.loop[(i + 1) % c.loop.size()];
                    const float l = tbd::city::Len(b - a);
                    if (d > l) { d -= l; continue; }
                    const tbd::city::V2 p = tbd::city::Lerp2(a, b, l > 0 ? d / l : 0), f = tbd::city::Norm(b - a);
                    const Vec3 g{p.x, RoadsGround(p.x, p.z), p.z}, fd{f.x, 0, f.z}, sd{-f.z, 0, f.x};
                    // driven machines follow the slope under their wheels (ground cells are drawn 0.3 m up)
                    const Vec3 gg = g + Vec3{0, 0.3f, 0};
                    if (s.kind == tbd::city::IndKind::Ore) ind::OnGround(dyn, gg, fd, sd, 3.0f, 2.0f, [&](auto& b, Vec3 q, Vec3 f, Vec3 sd2) { ind::HaulTruck(b, q, f, sd2, ind::kYellow); });
                    else if (s.kind == tbd::city::IndKind::Landfill) ind::OnGround(dyn, gg, fd, sd, 2.6f, 1.5f, [&](auto& b, Vec3 q, Vec3 f, Vec3 sd2) { ind::Compactor(b, q, f, sd2); });
                    else ind::OnGround(dyn, gg, fd, sd, 2.2f, 1.7f, [&](auto& b, Vec3 q, Vec3 f, Vec3 sd2) { ind::Combine(b, q, f, sd2, time); });
                    break;
                }
            }
        }
    }
    if (!dyn.empty()) IndustryDrawSplit(r, viewProj, dyn, -1);
    for (int b = 0; b < 3; ++b) // rigged cattle, one draw per breed with its texture
        if (!cowTris[b].empty()) {
            r.SetMaterial(g_cowRig[b].tex, tbd::kNoTexture);
            r.DrawTransient(cowTris[b].data(), (uint32_t)cowTris[b].size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
            r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
        }
    if (T.active && T.mode == IndustryTool::Place && !T.point && T.ghostShown) {
        static std::vector<tbd::Vertex> ghost;
        ghost.clear();
        ind::Hub(T.ghost, ghost);
        IndustryDrawSplit(r, viewProj, ghost, (int)T.ghost.kind);
    }
    if (!T.active) return;

    // the natural-resource heat map for the picked type (CS2's infoview), cached until the view moves 80 m
    {
        static std::vector<tbd::Vertex> heat;
        static int heatRes = -1;
        static tbd::city::V2 heatAt{1e9f, 1e9f};
        const tbd::city::Res res = tbd::city::IndTypeOf((tbd::city::IndKind)T.kind).res;
        const tbd::city::V2 cam{g_engine.camera.target.x, g_engine.camera.target.z};
        if ((int)res != heatRes || tbd::city::Len(cam - heatAt) > 80.0f) {
            heatRes = (int)res, heatAt = cam, heat.clear();
            if (res != tbd::city::Res::None) ind::Heat(res, cam, heat);
        }
        if (!heat.empty()) r.DrawTransient(heat.data(), (uint32_t)heat.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
    }
    // outlines and corner handles
    static std::vector<tbd::Vertex> lines;
    lines.clear();
    auto line = [&](tbd::city::V2 a, tbd::city::V2 b, Vec3 c) {
        lines.push_back({a.x, RoadsGround(a.x, a.z) + 0.8f, a.z, 0, 0, 0, c.x, c.y, c.z, 1});
        lines.push_back({b.x, RoadsGround(b.x, b.z) + 0.8f, b.z, 0, 0, 0, c.x, c.y, c.z, 1});
    };
    auto handle = [&](tbd::city::V2 p, Vec3 c) {
        const float h = std::max(1.2f, g_engine.camera.distance * 0.008f);
        line(p + tbd::city::V2{-h, 0}, p + tbd::city::V2{0, h}, c), line(p + tbd::city::V2{0, h}, p + tbd::city::V2{h, 0}, c);
        line(p + tbd::city::V2{h, 0}, p + tbd::city::V2{0, -h}, c), line(p + tbd::city::V2{0, -h}, p + tbd::city::V2{-h, 0}, c);
    };
    auto outline = [&](const std::vector<tbd::city::V2>& p, Vec3 c, bool closed) {
        for (size_t i = 0; i + (closed ? 0 : 1) < p.size(); ++i) line(p[i], p[(i + 1) % p.size()], c);
    };
    if (T.mode == IndustryTool::Place && T.point && g_wasteGhost.shown) {
        outline(WasteRect(g_wasteGhost.b), g_wasteGhost.ok ? Vec3{0.4f, 1, 0.45f} : Vec3{1, 0.3f, 0.25f}, true);
        const tbd::city::V2 m = g_wasteGhost.b.c + g_wasteGhost.b.front * (kWasteTypes[g_wasteGhost.b.type].d * 0.5f);
        line(m, m - g_wasteGhost.b.front * 8.0f, {1, 1, 1}); // entrance arrow
    }
    if (T.waste && T.mode == IndustryTool::Pick && T.wsel >= 0 && T.wsel < (int)g_wasteB.size())
        outline(WasteRect(g_wasteB[(size_t)T.wsel], 1.0f), {1, 0.85f, 0.3f}, true);
    if (T.mode == IndustryTool::Place && !T.point && T.ghostShown) {
        const tbd::city::IndSite& g = T.ghost;
        const tbd::city::V2 sd = g.Side() * (tbd::city::kHubW * 0.5f), f = g.front * (tbd::city::kHubD * 0.5f);
        outline({g.c - sd - f, g.c + sd - f, g.c + sd + f, g.c - sd + f}, T.ghostOk ? Vec3{0.4f, 1, 0.45f} : Vec3{1, 0.3f, 0.25f}, true);
    }
    if (T.mode == IndustryTool::Draw && T.drawing >= 0) {
        const tbd::city::IndSite& s = g_ind.sites[T.drawing];
        const tbd::city::V2* ex = T.have ? &T.cur : nullptr;
        const std::vector<tbd::city::V2> p = s.Poly(ex);
        const bool ok = tbd::city::PolySimple(p) && (!ex || IndustryPointOk(s, T.cur));
        outline(p, ok ? Vec3{1, 1, 1} : Vec3{1, 0.3f, 0.25f}, p.size() >= 3);
        for (size_t i = 0; i < p.size(); ++i) handle(p[i], i < 2 ? Vec3{0.6f, 0.65f, 0.62f} : Vec3{1, 1, 1});
    }
    if (T.mode == IndustryTool::Pick && T.sel >= 0 && T.sel < (int)g_ind.sites.size()) {
        const tbd::city::IndSite& s = g_ind.sites[T.sel];
        const std::vector<tbd::city::V2> p = s.Poly();
        outline(p, {1, 0.85f, 0.3f}, true);
        for (size_t i = 2; i < p.size(); ++i) handle(p[i], {1, 1, 1});
    }
    if (!lines.empty()) r.DrawTransient(lines.data(), (uint32_t)lines.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), false);
}

// ---- GUI: a launcher beside Zoning; when open, the type list and the site's numbers
bool IndustryToolbar(float lift) {
    using namespace tbd::gui;
    auto& T = g_indTool;
    Context& g = g_gui;
    if (!T.active) {
        return false; // the city HUD's toolbar (city_hud.h) opens it (HudOpenTool)
    }
    g.BeginPanel("##industry", Anchor::BottomLeft, 8.0f, lift, 330.0f, 0.0f, 10.0f);
    g.Text(TextStyle::Bright, "%s", T.waste ? "Waste Management" : "Specialized Industry");
    if (T.waste) GarbagePanel(); // city_garbage.inl
    const int kLand = (int)tbd::city::IndKind::Landfill;
    if (T.waste) {
        char label[64];
        std::snprintf(label, sizeof(label), "%s1  Landfill", (T.mode != IndustryTool::Pick && !T.point) ? "> " : "");
        g.NextWidth(310);
        if (g.Button(label)) WasteStartPlace(-1);
        for (int k = 0; k < 3; ++k) {
            std::snprintf(label, sizeof(label), "%s%d  %s", (T.mode == IndustryTool::Place && T.point == k + 1) ? "> " : "", k + 2, kWasteTypes[k].name);
            g.NextWidth(310);
            if (g.Button(label)) WasteStartPlace(k);
        }
        const int wt = T.mode == IndustryTool::Place && T.point ? T.point - 1 : T.mode == IndustryTool::Pick && T.wsel >= 0 ? g_wasteB[(size_t)T.wsel].type : -1;
        if (wt >= 0) {
            const WasteType& w = kWasteTypes[wt];
            g.Text(TextStyle::Bright, "%s", w.name);
            g.Text(TextStyle::Muted, "%s", w.what);
            if (w.mw > 0) g.Text(TextStyle::Normal, "Processes %.0f t / month   Power %.0f MW", w.tPerMonth, w.mw);
            else g.Text(TextStyle::Normal, "Processes %.0f t / month   Lot %.0f x %.0f m", w.tPerMonth, w.w, w.d);
            g.Text(TextStyle::Small, "%s", T.mode == IndustryTool::Place ? "Click beside a road; the entrance faces it. Right click / Esc stops."
                                                                         : "Delete removes it.");
            g.NextWidth(310);
            if (g.Button("Close")) T.active = false, T.wsel = -1, T.point = 0, T.mode = IndustryTool::Pick;
            g.EndPanel();
            return false;
        }
    }
    for (int k = T.waste ? kLand : 0; k < (T.waste ? 0 : kLand); ++k) {
        const auto& t = tbd::city::IndTypeOf((tbd::city::IndKind)k);
        char label[64];
        std::snprintf(label, sizeof(label), "%s%d  %s", (T.kind == k && T.mode != IndustryTool::Pick) ? "> " : "", T.waste ? 1 : k + 1, t.name);
        g.NextWidth(310);
        if (g.Button(label)) IndustryStartPlace(k);
    }
    const tbd::city::IndSite* s = T.drawing >= 0 ? &g_ind.sites[T.drawing] : T.sel >= 0 && T.sel < (int)g_ind.sites.size() ? &g_ind.sites[T.sel] : nullptr;
    const tbd::city::IndKind kind = s ? s->kind : (tbd::city::IndKind)T.kind;
    const auto& t = tbd::city::IndTypeOf(kind);
    g.Text(TextStyle::Bright, "%s", t.name);
    g.Text(TextStyle::Muted, "Needs: %s   Produces: %s", t.resName, t.product);
    if (s) {
        float eff = s->eff, ha = s->hectares, pm = s->perMonth;
        if (T.mode == IndustryTool::Draw && T.have) g_ind.Evaluate(s->kind, s->Poly(&T.cur), eff, ha, pm);
        g.Text(TextStyle::Normal, "Area %.2f ha   Efficiency %.0f%%", ha, eff * 100.0f);
        if (s->IsLandfill())
            g.Text(TextStyle::Normal, "Takes %.0f t / month   %.0f%% full of %.0f t", pm, s->Fill() * 100.0f, ha * tbd::city::kLandfillTPerHa);
        else
            g.Text(TextStyle::Normal, "Output %.1f t / month   Stock %.1f t", pm, s->stock);
    } else if (T.mode == IndustryTool::Place && T.have) {
        g.Text(TextStyle::Normal, "Resource here: %.0f%%", g_ind.field.At(t.res, T.cur.x, T.cur.z) * 100.0f);
    }
    g.Text(TextStyle::Small, "%s",
           T.mode == IndustryTool::Place  ? "Click beside a road to place the hub. Right click / Esc cancels."
           : T.mode == IndustryTool::Draw ? "Click to add corners. First corner, Enter or right click finishes. Backspace undoes."
                                          : T.waste ? "1 landfill, 2-4 buildings. Click a site to select; drag corners, click an edge to add one. Del removes."
                                          : "Pick a type (1-5). Click a site to select; drag corners, click an edge to add one. Del removes.");
    g.NextWidth(310);
    if (g.Button("Close")) {
        if (T.mode == IndustryTool::Draw) IndustryFinish();
        T.active = false, T.sel = -1, T.mode = IndustryTool::Pick;
    }
    g.EndPanel();
    return false;
}
