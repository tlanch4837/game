// ------------------------------------------------------------------- specialized industry (src/city/industry.h)
// Included by main.cpp after city_zones.inl. An "Industry" launcher beside Zoning opens the tool:
//   pick a type      grain, livestock, timber, oil, ore, landfill; the natural-resource heat map for it covers the terrain;
//   place the hub    it snaps beside the nearest road (3 x 4 cells), green when free;
//   draw the area    the polygon starts at the hub's back edge; click to add corners, click the first corner,
//                    the last one again, Enter or right click to finish; Backspace removes a corner;
//   edit             click a site to select it, drag its corners, Delete removes it.
// Efficiency = the mean resource value under the area, so the heat map decides where a site produces well.
// city/industry.h is included at the top of main.cpp (outside its anonymous namespace)

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

inline void TriN(std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c, Vec3 col) {
    Vec3 n = tbd::Cross(b - a, c - a);
    const float l = std::sqrt(tbd::Dot(n, n));
    n = l > 1e-6f ? n * (1.0f / l) : Vec3{0, 1, 0};
    if (n.y < -0.2f) n = n * -1.0f;
    zfx::Tri(o, a, b, c, n, col);
}
inline void QuadN(std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 col) { TriN(o, a, b, c, col), TriN(o, a, c, d, col); }
inline void Cyl(std::vector<Vertex>& o, Vec3 base, float r, float h, Vec3 col, int seg = 10) {
    for (int i = 0; i < seg; ++i) {
        const float a0 = i * 6.2831853f / seg, a1 = (i + 1) * 6.2831853f / seg;
        const Vec3 p0 = base + Vec3{std::cos(a0) * r, 0, std::sin(a0) * r}, p1 = base + Vec3{std::cos(a1) * r, 0, std::sin(a1) * r};
        const Vec3 n = tbd::Normalize(Vec3{std::cos((a0 + a1) * 0.5f), 0, std::sin((a0 + a1) * 0.5f)});
        zfx::Quad(o, p0, p1, p1 + Vec3{0, h, 0}, p0 + Vec3{0, h, 0}, n, col);
        zfx::Tri(o, base + Vec3{0, h, 0}, p0 + Vec3{0, h, 0}, p1 + Vec3{0, h, 0}, {0, 1, 0}, col);
    }
}
inline void Cone(std::vector<Vertex>& o, Vec3 base, float r, float h, Vec3 col, int seg = 8) {
    const Vec3 top = base + Vec3{0, h, 0};
    for (int i = 0; i < seg; ++i) {
        const float a0 = i * 6.2831853f / seg, a1 = (i + 1) * 6.2831853f / seg;
        TriN(o, base + Vec3{std::cos(a0) * r, 0, std::sin(a0) * r}, base + Vec3{std::cos(a1) * r, 0, std::sin(a1) * r}, top, col);
    }
}
// A hub-local frame in the prototype's units (industry_zones.html): x -6..6 across, z -16 (back) .. 0 (road), 1 unit = 2 m.
struct Frame {
    Vec3 c, fz, fx;
    float y;
    std::vector<Vertex>* o;
    Vec3 At(float x, float z, float h) const { return Vec3{c.x, y, c.z} + fx * (x * 2) + fz * ((z + 8) * 2) + Vec3{0, h * 2, 0}; }
    void Box(float x, float z, float w, float d, float y0, float h, Vec3 col) const { zfx::Block(*o, At(x, z, 0), fz, fx, d, w, y + y0 * 2, y + (y0 + h) * 2, col); }
    void Cyl(float x, float z, float r, float y0, float h, Vec3 col, int seg = 10) const { ind::Cyl(*o, At(x, z, y0), r * 2, h * 2, col, seg); }
    void Cone(float x, float z, float r, float y0, float h, Vec3 col, int seg = 8) const { ind::Cone(*o, At(x, z, y0), r * 2, h * 2, col, seg); }
    void Beam(Vec3 a, Vec3 b, float t, Vec3 col) const { zfx::Beam(*o, At(a.x, a.z, a.y), At(b.x, b.z, b.y), t * 2, col); }
    // Gable roof over a w x d box top at y0: ridge along the road axis (z) or across (x).
    void Roof(float x, float z, float w, float d, float y0, float rise, bool ridgeZ, Vec3 roof, Vec3 wall) const {
        const float hw = w * 0.5f + 0.15f, hd = d * 0.5f + 0.15f;
        if (ridgeZ) {
            const Vec3 a = At(x - hw, z - hd, y0), b = At(x - hw, z + hd, y0), c2 = At(x + hw, z + hd, y0), d2 = At(x + hw, z - hd, y0);
            const Vec3 r0 = At(x, z - hd, y0 + rise), r1 = At(x, z + hd, y0 + rise);
            QuadN(*o, a, b, r1, r0, roof), QuadN(*o, d2, r0, r1, c2, roof), TriN(*o, a, r0, d2, wall), TriN(*o, b, c2, r1, wall);
        } else {
            const Vec3 a = At(x - hw, z - hd, y0), b = At(x + hw, z - hd, y0), c2 = At(x + hw, z + hd, y0), d2 = At(x - hw, z + hd, y0);
            const Vec3 r0 = At(x - hw, z, y0 + rise), r1 = At(x + hw, z, y0 + rise);
            QuadN(*o, a, b, r1, r0, roof), QuadN(*o, d2, r0, r1, c2, roof), TriN(*o, a, r0, d2, wall), TriN(*o, b, c2, r1, wall);
        }
    }
    void House(float x, float z, float w, float d, float h, Vec3 wall, Vec3 roof) const {
        Box(x, z, w + 0.3f, d + 0.3f, 0, 0.3f, {0.62f, 0.6f, 0.57f});
        Box(x, z, w, d, 0.3f, h, wall);
        Roof(x, z, w, d, h + 0.3f, 1.3f, false, roof, wall);
        Box(x + w * 0.22f, z + d * 0.5f + 0.02f, 0.8f, 0.08f, 0.3f, 1.5f, {0.45f, 0.32f, 0.2f});
        Box(x - w * 0.28f, z - d * 0.15f, 0.45f, 0.45f, h, 1.5f, {0.6f, 0.32f, 0.24f});
    }
};
Frame FrameOf(const tbd::city::IndSite& s, std::vector<Vertex>& o) {
    const Vec3 fz{s.front.x, 0, s.front.z}, fx{s.Side().x, 0, s.Side().z};
    return {{s.c.x, s.y, s.c.z}, fz, fx, s.y, &o};
}

const Vec3 kWhite{0.95f, 0.93f, 0.9f}, kCream{0.94f, 0.88f, 0.77f}, kRed{0.75f, 0.29f, 0.23f}, kRedD{0.56f, 0.2f, 0.16f}, kRoof{0.36f, 0.38f, 0.41f},
    kRoofR{0.56f, 0.23f, 0.19f}, kMetal{0.78f, 0.8f, 0.82f}, kSteel{0.49f, 0.52f, 0.55f}, kSteelD{0.31f, 0.34f, 0.36f}, kYellow{0.94f, 0.7f, 0.16f},
    kBlueD{0.17f, 0.29f, 0.43f}, kWood{0.82f, 0.64f, 0.42f}, kBark{0.43f, 0.3f, 0.19f}, kConc{0.81f, 0.8f, 0.76f}, kBrick{0.65f, 0.34f, 0.25f},
    kOre{0.48f, 0.27f, 0.21f}, kGreen{0.25f, 0.56f, 0.27f}, kGravel{0.7f, 0.67f, 0.61f};

// The hub building (static parts).
void Hub(const tbd::city::IndSite& s, std::vector<Vertex>& o) {
    const Frame F = FrameOf(s, o);
    using K = tbd::city::IndKind;
    const Vec3 pad = s.kind == K::Landfill ? Vec3{0.42f, 0.42f, 0.42f} : s.kind == K::Oil ? kConc : s.kind == K::Ore ? Vec3{0.54f, 0.5f, 0.47f} : s.kind == K::Timber ? Vec3{0.66f, 0.54f, 0.38f} : Vec3{0.47f, 0.67f, 0.28f};
    F.Box(0, -8, 12, 16, -8.0f, 8.06f, pad); // deep skirt: the foundation fills the slope below
    switch (s.kind) {
    case K::Grain:
        F.House(-3.4f, -3.8f, 3.8f, 3.4f, 2.4f, kCream, kRoofR);
        F.Box(2.4f, -9, 5, 7, 0, 3.5f, kRed);
        F.Roof(2.4f, -9, 5, 7, 3.5f, 2.4f, true, kRoof, kRed);
        F.Box(2.4f, -5.45f, 2.4f, 0.1f, 0.3f, 2.6f, kRedD);
        for (auto [x, z, r] : {std::tuple{-4.0f, -12.8f, 1.15f}, std::tuple{-1.5f, -14.0f, 1.0f}}) {
            F.Cyl(x, z, r, 0, 6.7f, kMetal, 14);
            F.Cone(x, z, r * 1.08f, 6.7f, 1.1f, kMetal, 14);
            for (float y = 1.4f; y < 6.6f; y += 1.05f) F.Cyl(x, z, r + 0.03f, y, 0.06f, kSteel, 14);
        }
        F.Box(-2.9f, -11.4f, 0.5f, 0.5f, 0, 9.6f, kSteel);
        F.Box(-2.9f, -11.4f, 1, 1, 9.6f, 0.8f, kSteel);
        F.Beam({-2.9f, 9.8f, -11.4f}, {-4, 8.1f, -12.8f}, 0.12f, kSteelD);
        F.Beam({-2.9f, 9.8f, -11.4f}, {-1.5f, 8.1f, -14}, 0.12f, kSteelD);
        F.Box(4.6f, -2.5f, 1, 2.2f, 0.1f, 0.7f, kGreen), F.Box(4.6f, -3, 0.95f, 0.9f, 0.8f, 1.1f, {0.2f, 0.28f, 0.37f});
        break;
    case K::Livestock:
        F.House(-3.8f, -3.4f, 3.4f, 3, 2.2f, kWhite, kRoof);
        F.Box(1.2f, -9.6f, 9.6f, 4.4f, 0, 2.5f, {0.71f, 0.32f, 0.23f});
        F.Box(1.2f, -9.6f, 9.62f, 4.46f, 1.25f, 0.55f, {0.23f, 0.21f, 0.2f});
        F.Roof(1.2f, -9.6f, 9.6f, 4.4f, 2.5f, 1.5f, false, {0.43f, 0.51f, 0.56f}, {0.71f, 0.32f, 0.23f});
        for (float x : {-1.4f, 3.8f}) F.Box(x, -9.6f, 0.7f, 0.7f, 3.9f, 0.7f, kWhite), F.Cone(x, -9.6f, 0.62f, 4.6f, 0.6f, kRoof, 4);
        F.Cyl(-4.4f, -13.6f, 1.25f, 0, 7.6f, kBlueD, 14), F.Cone(-4.4f, -13.6f, 1.26f, 7.6f, 0.9f, kBlueD, 14), F.Cyl(-4.4f, -13.6f, 1.28f, 6.6f, 0.4f, kWhite, 14);
        for (float a : {-0.8f, 0.8f})
            for (float b : {-0.8f, 0.8f}) F.Beam({4.8f + a, 0, -14.4f + b}, {4.8f + a * 0.18f, 7, -14.4f + b * 0.18f}, 0.06f, kSteel);
        for (auto [x, z] : {std::pair{0.8f, -1.0f}, std::pair{5.6f, -1.0f}, std::pair{5.6f, -5.0f}, std::pair{0.8f, -5.0f}}) F.Box(x, z, 0.14f, 0.14f, 0, 1.1f, kWood);
        F.Beam({0.8f, 0.85f, -1}, {5.6f, 0.85f, -1}, 0.07f, kWood), F.Beam({5.6f, 0.85f, -1}, {5.6f, 0.85f, -5}, 0.07f, kWood);
        F.Beam({5.6f, 0.85f, -5}, {0.8f, 0.85f, -5}, 0.07f, kWood), F.Beam({0.8f, 0.85f, -5}, {0.8f, 0.85f, -1}, 0.07f, kWood);
        break;
    case K::Timber:
        F.House(-3.8f, -3, 3, 2.6f, 2.2f, kWood, kRoof);
        for (float x : {-1.7f, 0.5f, 2.7f, 4.9f})
            for (float z : {-11.2f, -5.6f}) F.Box(x, z, 0.25f, 0.25f, 0, z < -8 ? 4.4f : 3.6f, kSteelD);
        F.Beam({1.6f, 4.35f, -11.4f}, {1.6f, 3.65f, -5.4f}, 0.2f, {0.43f, 0.51f, 0.56f});
        F.Box(1.6f, -8.4f, 7.6f, 6.4f, 4.0f, 0.15f, {0.43f, 0.51f, 0.56f});
        F.Box(1.6f, -11.25f, 7, 0.15f, 0, 3.4f, {0.55f, 0.39f, 0.25f});
        F.Box(1.6f, -8.2f, 6, 0.9f, 0, 0.45f, kSteel), F.Box(2.8f, -8.2f, 1, 1.4f, 0, 2.2f, kRed);
        for (float z0 : {-11.6f, -14.6f})
            for (int row = 0; row < 4; ++row)
                for (int i = 0; i < 4 - row; ++i) {
                    const float x = -3.4f + (i - (3 - row) * 0.5f) * 0.56f, y = 0.28f + row * 0.48f;
                    F.Beam({x, y, z0 - 2.2f}, {x, y, z0 + 2.2f}, 0.5f, kBark);
                }
        for (float x : {3.6f, 0.6f})
            for (int k = 0; k < 3; ++k) F.Box(x, -14, 2.8f, 1.6f, k * 0.55f, 0.45f, kWood);
        F.Cone(4.6f, -4.4f, 1.4f, 0, 1.2f, {0.9f, 0.75f, 0.56f}, 7);
        break;
    case K::Oil:
        F.Box(-3.2f, -2.7f, 4, 2.4f, 0, 2.6f, kWhite), F.Box(-3.2f, -2.7f, 4.2f, 2.6f, 2.6f, 0.3f, {0.66f, 0.64f, 0.6f});
        for (float z : {-8.4f, -13.2f}) {
            F.Cyl(-3.3f, z, 1.8f, 0, 2.9f, kWhite, 16);
            F.Cyl(-3.3f, z, 1.82f, 1.9f, 0.3f, kRed, 16);
            F.Cone(-3.3f, z, 1.85f, 2.9f, 0.35f, kMetal, 16);
        }
        F.Cyl(2.6f, -11, 0.75f, 0, 11.4f, kMetal, 12), F.Cone(2.6f, -11, 0.76f, 11.4f, 0.6f, kMetal, 12);
        for (float y : {3.0f, 6.0f, 9.0f}) F.Cyl(2.6f, -11, 1.2f, y, 0.12f, kYellow, 12);
        F.Cyl(4.5f, -13.8f, 0.55f, 0, 7.4f, kMetal, 10);
        F.Box(3.3f, -6.4f, 2.8f, 2, 0, 2.2f, kSteel), F.Cyl(4.4f, -6.4f, 0.3f, 0, 7, kBrick, 8);
        F.Cyl(0.2f, -14.6f, 1.25f, 1.6f, 1.0f, kWhite, 12), F.Cone(0.2f, -14.6f, 1.25f, 2.6f, 0.9f, kWhite, 12);
        for (float a : {-0.8f, 0.8f})
            for (float b : {-0.8f, 0.8f}) F.Box(0.2f + a, -14.6f + b, 0.15f, 0.15f, 0, 1.6f, kSteelD);
        for (int i = 0; i < 3; ++i) F.Beam({-1.5f, 2.3f, -10.4f + i * 0.35f}, {1.9f, 2.3f, -10.4f + i * 0.35f}, 0.12f, i == 0 ? kSteel : i == 1 ? kYellow : kBlueD);
        F.Cyl(5.2f, -15.2f, 0.22f, 0, 12, kSteelD, 8);
        break;
    case K::Ore: {
        F.Box(-2.6f, -4.4f, 4.2f, 3.2f, 0, 3.3f, kBrick);
        F.Roof(-2.6f, -4.4f, 4.2f, 3.2f, 3.3f, 1.3f, false, kRoof, kBrick);
        const float hx = -2.6f, hz = -11.2f, H = 10;
        for (float a : {-1.0f, 1.0f})
            for (float c : {-1.0f, 1.0f}) F.Beam({hx + a * 1.3f, 0, hz + c * 1.3f}, {hx + a * 0.55f, H, hz + c * 0.55f}, 0.12f, kSteelD);
        for (float y : {3.3f, 6.6f}) {
            const float k = 1.3f - 0.75f * y / H;
            F.Beam({hx - k, y, hz - k}, {hx + k, y, hz - k}, 0.07f, kSteelD), F.Beam({hx + k, y, hz - k}, {hx + k, y, hz + k}, 0.07f, kSteelD);
            F.Beam({hx + k, y, hz + k}, {hx - k, y, hz + k}, 0.07f, kSteelD), F.Beam({hx - k, y, hz + k}, {hx - k, y, hz - k}, 0.07f, kSteelD);
        }
        for (float sd : {-1.0f, 1.0f}) F.Beam({hx + sd * 0.55f, H - 0.4f, hz + 0.55f}, {hx + sd * 1.2f, 0, hz + 4.2f}, 0.14f, kSteelD);
        F.Box(hx, hz, 2, 2, H, 0.3f, kSteelD);
        F.Box(3, -9, 4, 4.2f, 0, 6.4f, kConc), F.Box(3, -9, 4.4f, 4.6f, 6.4f, 0.2f, {0.43f, 0.51f, 0.56f});
        F.Beam({3.2f, 5.9f, -11}, {3.6f, 3.1f, -14.8f}, 0.5f, kSteel);
        F.Cone(3.6f, -14.6f, 2.3f, 0, 2.7f, kOre, 9);
        F.Box(3.6f, -2.6f, 3.6f, 1.9f, 0.1f, 1.6f, kYellow);
        break;
    }
    case K::Landfill: { // site office, weighbridge + gatehouse, leachate tank, gas flare + engine container, skips
        F.Box(-3.4f, -3.4f, 4.2f, 2.6f, 0, 1.8f, kWhite), F.Box(-3.4f, -3.4f, 4.4f, 2.8f, 1.8f, 0.15f, kSteelD);
        F.Box(-3.4f, -2.08f, 3.2f, 0.06f, 0.6f, 0.7f, {0.2f, 0.28f, 0.37f});
        F.Box(2.6f, -3.0f, 2.0f, 7.0f, 0.05f, 0.12f, kSteel), F.Box(2.6f, -3.0f, 2.3f, 7.3f, 0.0f, 0.06f, kConc); // weighbridge
        F.Box(4.6f, -1.6f, 1.2f, 1.2f, 0, 1.4f, kWhite), F.Box(4.6f, -1.6f, 1.4f, 1.4f, 1.4f, 0.1f, kSteelD);
        F.Box(1.0f, 0.6f, 0.15f, 0.15f, 0, 0.7f, kYellow);
        F.Cyl(-3.6f, -10.4f, 1.6f, 0, 2.6f, kSteel, 14), F.Cone(-3.6f, -10.4f, 1.62f, 2.6f, 0.3f, kSteelD, 14); // leachate tank
        F.Cyl(-3.6f, -10.4f, 1.64f, 2.2f, 0.12f, kYellow, 14);
        F.Box(-3.0f, -14.2f, 4.6f, 2.0f, 0, 1.6f, kGreen); // gas engine container
        F.Cyl(1.6f, -14.0f, 0.55f, 0.5f, 6.5f, kMetal, 10), F.Cyl(1.6f, -14.0f, 0.57f, 6.0f, 0.5f, {0.25f, 0.22f, 0.2f}, 10); // flare
        for (float a : {-0.4f, 0.4f}) F.Beam({1.6f + a, 0, -14.0f}, {1.6f + a * 0.6f, 0.6f, -14.0f}, 0.1f, kSteelD);
        F.Beam({1.6f, 0.4f, -14.0f}, {-0.7f, 0.4f, -14.2f}, 0.1f, kYellow);
        for (float z : {-8.0f, -10.8f}) { // roll-off skips
            F.Box(3.6f, z, 1.6f, 3.2f, 0, 0.9f, kYellow);
            for (float r = -1.2f; r <= 1.21f; r += 0.6f) F.Box(3.6f, z + r, 1.66f, 0.08f, 0.1f, 0.75f, Vec3{0.8f, 0.6f, 0.12f});
        }
        break;
    }
    default: break;
    }
}
// The hub's moving parts (pumpjack-free): windmill rotor, flare, headframe wheels.
void HubAnim(const tbd::city::IndSite& s, float t, std::vector<Vertex>& o) {
    const Frame F = FrameOf(s, o);
    using K = tbd::city::IndKind;
    if (s.kind == K::Livestock) { // windmill rotor facing the road
        const Vec3 hub = F.At(4.8f, -14.4f + 0.3f, 7.2f);
        for (int i = 0; i < 10; ++i) {
            const float a = i * 0.6283f + t * 2.2f;
            zfx::Beam(o, hub, hub + F.fx * (std::sin(a) * 2.2f) + Vec3{0, std::cos(a) * 2.2f, 0}, 0.3f, kWhite);
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
                zfx::Beam(o, c - d, c + d, 0.12f, kRed);
            }
        }
    }
}

// The work area: terrain-draped 4 m cells plus props. Pumpjack spots go to `jacks` (animated per frame).
void Area(const tbd::city::IndSite& s, std::vector<Vertex>& o, std::vector<Vec3>& jacks, std::vector<V2>& loop) {
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
            Vec3 col;
            const float h = H(a, b, 3) * 0.06f - 0.03f;
            switch (s.kind) {
            case K::Grain: {
                static const Vec3 pal[] = {{0.91f, 0.77f, 0.36f}, {0.86f, 0.68f, 0.27f}, {0.94f, 0.84f, 0.53f}, {0.66f, 0.77f, 0.31f}};
                const int band = (int)std::floor(b / 24.0f);
                col = ((int)std::floor(b / cs) & 1) ? pal[(int)(H((float)band, (float)s.id, 9) * 4) & 3] : Vec3{0.48f, 0.35f, 0.23f};
                break;
            }
            case K::Livestock: col = Vec3{0.58f, 0.78f, 0.38f} + Vec3{h, h * 1.5f, h}; break;
            case K::Timber: col = Vec3{0.36f, 0.47f, 0.24f} + Vec3{h, h, h}; break;
            case K::Oil: col = Vec3{0.8f, 0.71f, 0.52f} + Vec3{h, h, h}; break;
            case K::Ore: {
                float e = 1e30f;
                for (size_t i = 0; i < poly.size(); ++i) e = std::min(e, tbd::city::SegDist(m, poly[i], poly[(i + 1) % poly.size()]));
                const float k = std::min(1.0f, e / 30.0f);
                col = Vec3{0.6f, 0.55f, 0.5f} * (1 - k) + Vec3{0.47f, 0.3f, 0.23f} * k + Vec3{h, h, h};
                break;
            }
            case K::Landfill: {
                const float d = lfDepth(b) + (H(a, b, 7) - 0.5f) * 0.06f;
                if (d < fill - 0.08f) col = Vec3{0.42f, 0.52f, 0.27f} + Vec3{h, h * 1.4f, h}; // capped, grassed
                else if (d < fill + 0.02f) {                                                    // the working face
                    static const Vec3 pal[] = {{0.36f, 0.31f, 0.25f}, {0.55f, 0.52f, 0.47f}, {0.33f, 0.38f, 0.5f}, {0.6f, 0.5f, 0.35f}};
                    col = pal[(int)(H(a, b, 11) * 4) & 3];
                } else if (d < fill + 0.1f) col = Vec3{0.47f, 0.37f, 0.25f} + Vec3{h, h, h};   // daily cover soil
                else col = Vec3{0.14f, 0.14f, 0.15f} + Vec3{h, h, h} * 0.5f;                   // lined, empty cell
                break;
            }
            default: col = {0.5f, 0.5f, 0.5f};
            }
            const V2 q[4] = {W(a, b), W(a + cs, b), W(a + cs, b + cs), W(a, b + cs)};
            float lift = 0.3f;
            if (s.kind == K::Landfill) // a mound: filled cells rise, sloping down toward the area's edge
                lift += (lfDepth(b) < fill ? 1.0f : 0.25f) * std::min(edgeDist(m) * 0.25f, 9.0f) * std::min(1.0f, fill * 3.0f);
            zfx::Quad(o, G(q[0], lift), G(q[1], lift), G(q[2], lift), G(q[3], lift), {0, 1, 0}, col);
        }
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
            if (r < 0.12f) { Cyl(o, G(m, 0), 0.35f, 0.6f, kBark, 6); return; } // a stump
            const float k = 0.7f + r * 0.7f;
            const Vec3 g = G(m, 0);
            static const Vec3 pines[] = {{0.25f, 0.48f, 0.29f}, {0.29f, 0.54f, 0.31f}, {0.21f, 0.42f, 0.27f}, {0.35f, 0.58f, 0.32f}};
            const Vec3 c = pines[(int)(r * 40) & 3];
            Cyl(o, g, 0.3f * k, 2.2f * k, kBark, 5);
            Cone(o, g + Vec3{0, 1.6f * k, 0}, 2.2f * k, 4.0f * k, c, 7);
            Cone(o, g + Vec3{0, 4.0f * k, 0}, 1.5f * k, 3.4f * k, c * 1.08f, 7);
        });
        break;
    case K::Livestock: {
        for (size_t e = 1; e < poly.size(); ++e) { // fence, open toward the barn
            const V2 a = poly[e], b = poly[(e + 1) % poly.size()];
            const int n = std::max(1, (int)(Len(b - a) / 4.0f));
            for (int i = 0; i <= n; ++i) {
                const Vec3 p = G(Lerp2(a, b, (float)i / n), 0);
                zfx::Beam(o, p, p + Vec3{0, 1.3f, 0}, 0.2f, kWood);
            }
            for (float y : {0.6f, 1.1f}) zfx::Beam(o, G(a, y), G(b, y), 0.1f, kWood);
        }
        scatter(14.0f, 3.0f, [&](V2 m, float r) { // cows
            const Vec3 g = G(m, 0);
            const V2 d = V2{std::cos(r * 40), std::sin(r * 40)};
            const Vec3 fd{d.x, 0, d.z}, sd{-d.z, 0, d.x};
            zfx::Block(o, g, fd, sd, 1.2f, 0.55f, g.y + 0.9f, g.y + 2.0f, kWhite);
            zfx::Block(o, g + fd * 1.5f, fd, sd, 0.35f, 0.3f, g.y + 1.4f, g.y + 2.1f, kWhite);
            zfx::Block(o, g - fd * 0.2f, fd, sd, 0.4f, 0.57f, g.y + 1.2f, g.y + 2.02f, {0.18f, 0.17f, 0.16f});
            for (float a : {-0.9f, 0.9f})
                for (float b : {-0.35f, 0.35f}) zfx::Block(o, g + fd * a + sd * b, fd, sd, 0.12f, 0.12f, g.y, g.y + 0.95f, kWhite);
        });
        break;
    }
    case K::Oil: {
        V2 a, b;
        s.BackEdge(a, b);
        const Vec3 mid = G(Lerp2(a, b, 0.5f), 0.3f);
        scatter(28.0f, 5.0f, [&](V2 m, float) {
            const Vec3 g = G(m, 0);
            jacks.push_back(g);
            zfx::Block(o, g, {1, 0, 0}, {0, 0, 1}, 4.6f, 1.8f, g.y - 3.0f, g.y + 0.35f, kGravel);
            zfx::Block(o, g + Vec3{0.6f, 0, 0}, {1, 0, 0}, {0, 0, 1}, 3.6f, 0.8f, g.y + 0.25f, g.y + 0.7f, kSteelD);
            for (float z : {-0.75f, 0.75f}) {
                zfx::Beam(o, g + Vec3{-0.4f, 0.6f, z}, g + Vec3{0.7f, 5.2f, 0}, 0.24f, kSteelD);
                zfx::Beam(o, g + Vec3{1.8f, 0.6f, z}, g + Vec3{0.7f, 5.2f, 0}, 0.24f, kSteelD);
            }
            zfx::Block(o, g + Vec3{4.2f, 0, 0}, {1, 0, 0}, {0, 0, 1}, 0.9f, 0.7f, g.y + 0.6f, g.y + 2.0f, kSteel);
            zfx::Beam(o, g + Vec3{-4.1f, 0.3f, 0}, mid, 0.28f, kSteelD); // flow line to the hub
        });
        break;
    }
    case K::Ore: {
        scatter(16.0f, 6.0f, [&](V2 m, float r) { Cone(o, G(m, 0), 2.0f + r * 2.5f, 1.5f + r * 2.5f, r < 0.5f ? kOre : Vec3{0.55f, 0.52f, 0.48f}, 7); });
        V2 c{0, 0};
        for (V2 p : poly) c = c + p;
        c = c * (1.0f / poly.size());
        for (V2 p : poly) loop.push_back(c + (p - c) * 0.72f); // the haul road the trucks drive
        break;
    }
    case K::Grain: {
        scatter(40.0f, 8.0f, [&](V2 m, float) { // round bales
            const Vec3 g = G(m, 0);
            zfx::Beam(o, g + Vec3{-0.9f, 1.1f, 0}, g + Vec3{0.9f, 1.1f, 0}, 2.2f, {0.89f, 0.76f, 0.42f});
        });
        V2 c{0, 0};
        for (V2 p : poly) c = c + p;
        c = c * (1.0f / poly.size());
        for (V2 p : poly) loop.push_back(c + (p - c) * 0.6f); // the combine's lap
        break;
    }
    case K::Landfill: {
        for (size_t e = 1; e < poly.size(); ++e) { // perimeter litter fence
            const V2 a = poly[e], b = poly[(e + 1) % poly.size()];
            const int n = std::max(1, (int)(Len(b - a) / 5.0f));
            for (int i = 0; i <= n; ++i) {
                const Vec3 p = G(Lerp2(a, b, (float)i / n), 0);
                zfx::Beam(o, p, p + Vec3{0, 3.0f, 0}, 0.15f, kSteelD);
            }
            zfx::Beam(o, G(a, 2.9f), G(b, 2.9f), 0.06f, kSteel);
        }
        scatter(18.0f, 6.0f, [&](V2 m, float r) { // gas wells on the capped ground (standing on the mound)
            const float d = lfDepth(Dot2(m, v));
            if (d > fill - 0.1f || r > 0.7f) return;
            const Vec3 g = G(m, 0.3f + std::min(edgeDist(m) * 0.25f, 9.0f) * std::min(1.0f, fill * 3.0f));
            Cyl(o, g, 0.3f, 2.2f, kWhite, 6);
            Cyl(o, g + Vec3{0, 2.2f, 0}, 0.45f, 0.3f, {0.25f, 0.25f, 0.25f}, 6);
            Cyl(o, g + Vec3{0, 2.5f, 0}, 0.15f, 0.3f, kYellow, 5);
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
bool IndustryKey(WPARAM k) {
    auto& T = g_indTool;
    if (k == VK_ESCAPE) {
        if (T.mode == IndustryTool::Draw) IndustryFinish();
        else if (T.mode == IndustryTool::Place) T.mode = IndustryTool::Pick, T.point = 0;
        else T.active = false, T.sel = -1;
        return true;
    }
    if (k == VK_RETURN && T.mode == IndustryTool::Draw) return IndustryFinish(), true;
    if (k == VK_BACK && T.mode == IndustryTool::Draw) {
        auto& a = g_ind.sites[T.drawing].area;
        if (!a.empty()) a.pop_back(), g_ind.Refresh(g_ind.sites[T.drawing]);
        return true;
    }
    if ((k == VK_DELETE || k == VK_BACK) && T.mode == IndustryTool::Pick && T.waste && T.wsel >= 0) {
        WasteRemove(T.wsel);
        T.wsel = -1;
        return true;
    }
    if ((k == VK_DELETE || k == VK_BACK) && T.mode == IndustryTool::Pick && T.sel >= 0) {
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
void IndustryDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    using tbd::Vec3;
    auto& T = g_indTool;
    const float time = (float)std::fmod(g_clock.minutes, 100000.0) * 0.5f;
    static std::vector<tbd::Vertex> dyn;
    dyn.clear();
    // static geometry per site, rebuilt when it changes (not while a corner is being dragged: the outline shows it)
    for (const tbd::city::IndSite& s : g_ind.sites) {
        ind::Cache& c = g_indCache[s.id];
        const bool dragging = T.dragIdx >= 0 && T.sel >= 0 && g_ind.sites[T.sel].id == s.id;
        if (c.ver != s.version && !dragging) {
            c.ver = s.version;
            c.tris.clear(), c.jacks.clear(), c.loop.clear();
            ind::Hub(s, c.tris);
            ind::Area(s, c.tris, c.jacks, c.loop);
        }
        if (!c.tris.empty()) r.DrawTransient(c.tris.data(), (uint32_t)c.tris.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
        ind::HubAnim(s, time, dyn);
        for (size_t i = 0; i < c.jacks.size(); ++i) { // pumpjack walking beams
            const Vec3 g = c.jacks[i];
            const float a = std::sin(time * 1.7f + i * 1.3f) * 0.27f;
            const Vec3 piv = g + Vec3{0.7f, 5.4f, 0}, d{std::cos(a) * 3.7f, std::sin(a) * 3.7f, 0};
            zfx::Beam(dyn, piv - d, piv + d, 0.6f, {0.18f, 0.37f, 0.54f});
            zfx::Beam(dyn, piv - d, piv - d - Vec3{0, 1.6f, 0}, 0.7f, {0.18f, 0.37f, 0.54f});
        }
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
                    const Vec3 body = s.kind == tbd::city::IndKind::Ore ? ind::kYellow : s.kind == tbd::city::IndKind::Landfill ? Vec3{0.85f, 0.75f, 0.12f} : Vec3{0.82f, 0.29f, 0.2f};
                    zfx::Block(dyn, g, fd, sd, 3.2f, 1.6f, g.y + 0.8f, g.y + 3.0f, body);
                    zfx::Block(dyn, g + fd * 2.4f, fd, sd, 0.8f, 0.8f, g.y + 3.0f, g.y + 4.4f, {0.2f, 0.28f, 0.37f});
                    if (s.kind == tbd::city::IndKind::Grain) zfx::Block(dyn, g + fd * 3.6f, fd, sd, 0.6f, 3.4f, g.y + 0.3f, g.y + 1.0f, ind::kYellow);
                    break;
                }
            }
        }
    }
    if (T.active && T.mode == IndustryTool::Place && !T.point && T.ghostShown) ind::Hub(T.ghost, dyn);
    if (!dyn.empty()) r.DrawTransient(dyn.data(), (uint32_t)dyn.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
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
        g.BeginPanel("##industry launcher", Anchor::BottomCenter, 300.0f, lift, 0.0f, 0.0f, 6.0f);
        g.NextWidth(132);
        const bool open = g.Button("Industry", Icon::Zones);
        g.EndPanel();
        g.BeginPanel("##waste launcher", Anchor::BottomCenter, 450.0f, lift, 0.0f, 0.0f, 6.0f);
        g.NextWidth(132);
        const bool openW = g.Button("Waste", Icon::Zones);
        g.EndPanel();
        if (open || openW) T.active = true, T.waste = openW, T.point = 0, T.wsel = -1, T.mode = IndustryTool::Pick, T.kind = openW ? (int)tbd::city::IndKind::Landfill : 0;
        return open || openW;
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
