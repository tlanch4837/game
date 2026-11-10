// Rail junction planes in the game (src/city/rail_junction_place.h): the switch / diamond / crossover / level crossing
// boxes drawn by JunctionPS (mesh.hlsl, vertex alpha 0.84), their props, and the level crossings' gates.
//   Draws      one static mesh per layout in use (every junction of that layout in it, one texture bind), one mesh of
//              props (Prop atlas, assets/rail/junction/tex/props_albedo.png) and one transient draw of the gate booms.
//   Textures   a layout's texture (albedo, height in alpha: <id>_albedo.jpg + <id>_height.png) is read and uploaded
//              (mipmapped) only once a junction of that layout exists, and kept for the session.
//   Gates      down while a train is within kGateWarn seconds of the crossing or on it (rail_yard.h trains), up after.
// RoadsSync calls RailJunctionsPlan / RailJunctionsRebuild whenever the network changed; the scene draws
// RailJunctionsDraw with the rail. (city/rail_junction_place.h: the hosts take it through city/transport_models.h)

namespace rj {
struct Gpu {
    std::vector<tbd::TextureId> tex;
    std::vector<char> tried;
    std::vector<tbd::MeshId> mesh;
    std::vector<uint32_t> verts;
    tbd::TextureId props = tbd::kNoTexture;
    bool propsTried = false;
    tbd::MeshId propMesh = tbd::kInvalidMesh;
    std::vector<tbd::city::JunctionSite> sites;
    std::vector<float> gate; // per site: 0 down .. 1 up (level crossings)
    double lastMs = 0;
    float forceGate = -1; // rail_demo: the booms held at this (0 .. 1), -1 the trains decide
} g;
constexpr float kGateWarn = 9.0f;  // seconds of approach with the gates down
constexpr float kGateSpeed = 0.35f; // of the full swing per second (about 3 s)

std::wstring Dir() { return tbd::ExeDirectory() + L"\\..\\assets\\rail\\junction\\tex\\"; }
std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// One layout's texture: albedo with the height field in alpha (as TrackSpomTexture).
tbd::TextureId LoadLayout(tbd::Renderer& r, int layout) {
    const std::string& id = tbd::city::JunctionLayouts()[(size_t)layout].id;
    std::string a, h;
    std::vector<uint8_t> rgba, hgt;
    int w = 0, ht = 0, w2 = 0, h2 = 0;
    if (!tbd::ReadAllText(Dir() + W(id) + L"_albedo.jpg", a) || !tbd::DecodeImage((const uint8_t*)a.data(), a.size(), rgba, w, ht) ||
        !tbd::ReadAllText(Dir() + W(id) + L"_height.png", h) || !tbd::DecodeImage((const uint8_t*)h.data(), h.size(), hgt, w2, h2) || w != w2 || ht != h2) {
        tbd::Log("Rail junctions: missing texture %s in %ls", id.c_str(), Dir().c_str());
        return tbd::kNoTexture;
    }
    for (size_t i = 0; i < (size_t)w * ht; ++i) rgba[i * 4 + 3] = hgt[i * 4];
    return r.CreateTexture(rgba.data(), w, ht);
}
} // namespace rj

// A new renderer: every id is gone with the old one.
void RailJunctionsForget() {
    rj::g.tex.clear(), rj::g.tried.clear(), rj::g.mesh.clear(), rj::g.verts.clear();
    rj::g.props = tbd::kNoTexture, rj::g.propsTried = false, rj::g.propMesh = tbd::kInvalidMesh;
}

// The sites of the network as it is now, and what they cut out of its mesh (before BuildNetwork).
tbd::city::RailCuts RailJunctionsPlan() {
    static const bool off = tbd::platform::GetEnvA("TBD_NOJUNCTIONS", nullptr, 0) > 0; // A/B: the old junction fills, uncut track
    rj::g.sites = off ? std::vector<tbd::city::JunctionSite>{} : tbd::city::FindJunctionSites(g_roads.net, RoadsGround);
    rj::g.gate.assign(rj::g.sites.size(), 1.0f);
    return tbd::city::RailCutsFor(g_roads.net, rj::g.sites);
}

// The meshes (after BuildNetwork, before the road meshes are made: the crossings' ramps and markings go into `out`).
void RailJunctionsRebuild(tbd::Renderer& r, tbd::city::MeshOut& out) {
    const size_t L = tbd::city::JunctionLayouts().size();
    rj::g.tex.resize(L, tbd::kNoTexture), rj::g.tried.resize(L, 0), rj::g.mesh.resize(L, tbd::kInvalidMesh), rj::g.verts.resize(L, 0);
    tbd::city::JunctionDraws d;
    tbd::city::BuildJunctionDraws(g_roads.net, rj::g.sites, d, &out);
    for (size_t l = 0; l < L; ++l) {
        if (rj::g.mesh[l] != tbd::kInvalidMesh) r.RetireMesh(rj::g.mesh[l]);
        rj::g.mesh[l] = tbd::kInvalidMesh, rj::g.verts[l] = 0;
        const std::vector<tbd::Vertex>& v = d.byLayout[l];
        if (v.empty()) continue;
        if (!rj::g.tried[l]) rj::g.tried[l] = 1, rj::g.tex[l] = rj::LoadLayout(r, (int)l); // lazily: only layouts in use
        rj::g.mesh[l] = r.CreateMesh(v.data(), (uint32_t)v.size(), tbd::Topology::Triangles);
        rj::g.verts[l] = (uint32_t)v.size();
    }
    if (rj::g.propMesh != tbd::kInvalidMesh) r.RetireMesh(rj::g.propMesh);
    rj::g.propMesh = d.props.empty() ? tbd::kInvalidMesh : r.CreateMesh(d.props.data(), (uint32_t)d.props.size(), tbd::Topology::Triangles);
    if (!d.props.empty() && !rj::g.propsTried) {
        rj::g.propsTried = true;
        std::string bytes;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (tbd::ReadAllText(rj::Dir() + L"props_albedo.png", bytes) && tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), rgba, w, h))
            rj::g.props = r.CreateTexture(rgba.data(), w, h);
        else tbd::Log("Rail junctions: missing props_albedo.png in %ls", rj::Dir().c_str());
    }
    if (d.sites) tbd::Log("Rail junctions: %d sites, %zu props triangles", d.sites, d.props.size() / 3);
}

// Gates: the target of each level crossing from the trains (down while one approaches within kGateWarn s or is on it).
void RailJunctionsGates(float dt) {
    for (size_t i = 0; i < rj::g.sites.size(); ++i) {
        const tbd::city::JunctionSite& S = rj::g.sites[i];
        if (S.type != tbd::city::JType::LevelCrossing) continue;
        bool down = false;
        for (const tbd::city::YardTrain& t : g_railYards.trains) {
            if (!t.alive || t.phase == 3 || t.path.size() < 2 || down) continue;
            float best = 9.0f, sx = -1; // the crossing's place along the train's path (within 9 m of it)
            for (size_t k = 1; k < t.path.size(); ++k) {
                const tbd::city::V2 a = t.path[k - 1], b = t.path[k], ab = b - a;
                const float l2 = std::max(tbd::city::Dot2(ab, ab), 1e-6f), u = std::clamp(tbd::city::Dot2(S.o - a, ab) / l2, 0.0f, 1.0f);
                const float dd = tbd::city::Len(a + ab * u - S.o);
                if (dd < best) best = dd, sx = t.cum[k - 1] + (t.cum[k] - t.cum[k - 1]) * u;
            }
            if (sx < 0) continue;
            const float ahead = sx - t.s; // > 0: the head has yet to reach it
            down = ahead > -(tbd::city::TrainLength() + 3.0f) && ahead < std::max(t.speed, 4.0f) * rj::kGateWarn + 12.0f;
        }
        const float want = rj::g.forceGate >= 0 ? rj::g.forceGate : down ? 0.0f : 1.0f;
        rj::g.gate[i] += std::clamp(want - rj::g.gate[i], -rj::kGateSpeed * dt, rj::kGateSpeed * dt);
    }
}

// The boxes (one draw per layout in use), the props and the booms.
void RailJunctionsDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    const double now = (double)tbd::platform::TickCountMs();
    const float dt = rj::g.lastMs > 0 ? (float)std::clamp((now - rj::g.lastMs) * 0.001, 0.0, 0.25) : 0.0f;
    rj::g.lastMs = now;
    RailJunctionsGates(dt);
    bool bound = false;
    for (size_t l = 0; l < rj::g.mesh.size(); ++l) {
        if (rj::g.mesh[l] == tbd::kInvalidMesh || rj::g.tex[l] == tbd::kNoTexture) continue;
        r.SetMaterial(rj::g.tex[l], tbd::kNoTexture), bound = true;
        r.Draw(rj::g.mesh[l], viewProj, tbd::Mat4::Identity(), true, true);
    }
    static std::vector<tbd::Vertex> booms;
    booms.clear();
    for (size_t i = 0; i < rj::g.sites.size(); ++i)
        if (rj::g.sites[i].type == tbd::city::JType::LevelCrossing) tbd::city::CrossingBooms(rj::g.sites[i], rj::g.gate[i], booms);
    if (rj::g.props != tbd::kNoTexture && (rj::g.propMesh != tbd::kInvalidMesh || !booms.empty())) {
        r.SetMaterial(rj::g.props, tbd::kNoTexture), bound = true;
        if (rj::g.propMesh != tbd::kInvalidMesh) r.Draw(rj::g.propMesh, viewProj, tbd::Mat4::Identity(), true, true);
        if (!booms.empty()) r.DrawTransient(booms.data(), (uint32_t)booms.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
    }
    if (bound) r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
}

// Remote "rail_demo|x z|gate" (main.cpp), for captures: around (x, z) a single track line with a level crossing (a two-lane
// road across it), a right-hand 12 deg turnout, a crossover (left-hand, to a parallel track at the layout's 5 m) and a
// 45 deg diamond. gate: -1 (default) the trains decide, else the booms held there (0 down .. 1 up).
bool RailJunctionDemo(float x, float z, float gate, std::string& out) {
    using tbd::city::Cubic;
    using tbd::city::V2;
    bool mirror = false;
    const int rail = tbd::city::ProfileIndex("Single Track Railway"), road = tbd::city::ProfileIndex("Two-Lane Road");
    auto line = [&](V2 a, V2 b, int prof) {
        a = {a.x + x, a.z + z}, b = {b.x + x, b.z + z};
        g_roads.BuildRoad(Cubic::Line(a, b), RoadsGround(a.x, a.z), RoadsGround(b.x, b.z), prof);
    };
    const tbd::city::JunctionParams xo = tbd::city::AdjustParams(tbd::city::JunctionLayouts()[(size_t)tbd::city::LayoutFor(tbd::city::JType::Crossover, 10, tbd::city::Hand::Right, mirror)].p);
    const float run = tbd::city::CrossoverRun(xo.spacing, xo.angleDeg, xo.radius), d12 = tbd::city::Rad(12.0f);
    line({-420, 0}, {320, 0}, rail);                                       // the main line
    line({-300, -70}, {-300, 70}, road);                                   // a level crossing
    line({-150, 0}, {-150 + 130 * std::cos(d12), 130 * std::sin(d12)}, rail); // a turnout, diverging right
    line({-60, -xo.spacing}, {170, -xo.spacing}, rail);                    // a parallel track ...
    line({0, 0}, {run, -xo.spacing}, rail);                                // ... and a crossover to it (left-hand)
    line({240 - 70 * 0.7071f, -70 * 0.7071f}, {240 + 70 * 0.7071f, 70 * 0.7071f}, rail); // a 45 deg diamond
    rj::g.forceGate = gate;
    char b[96];
    std::snprintf(b, sizeof(b), "rail demo at %.0f %.0f: %zu segments", x, z, g_roads.net.segs.size());
    out = b;
    return true;
}