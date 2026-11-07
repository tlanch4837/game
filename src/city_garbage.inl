// ------------------------------------------------------------------- garbage: the Waste Management simulation
// Included by main.cpp after city_industry.inl (which hosts the landfill sites and the waste point buildings).
//
//   Every built zone lot produces garbage per 8 m cell per game month (industry the most, offices the least). It piles
//   up on the lot until a facility collects it. A facility collects from lots on the same connected road network within
//   its range, nearest first, up to its monthly capacity:
//     Landfill            takes in its intake rate (scales with the drawn area) until its cells are full;
//     Incineration Plant  burns what it collects and turns it into electricity (MW shown, scaled by how busy it is);
//     Recycling Center    sorts what it collects into materials (tonnes stockpiled);
//     Industrial Waste    takes industrial lots' garbage first, with double the range.
//   A lot whose pile passes three months of its own output is "piling up": a garbage heap grows beside it and it shows on
//   the garbage info view (green clean -> brown dirty) while the Waste menu is open.

namespace garbage {
using tbd::city::V2;

constexpr float kRange = 2200.0f;    // metres a facility's trucks reach (as the crow flies, on the same road network)
constexpr float kPileMonths = 3.0f;  // a pile bigger than this many months of output is "piling up"

// tonnes per game month per 8 m cell, by zone type (ZoneTypes() order)
inline float PerCell(int zoneType) {
    static const float t[] = {0.6f, 0.8f, 1.2f, 2.0f, 1.5f, 1.0f, 3.0f};
    return t[std::clamp(zoneType, 0, 6)];
}
inline bool Industrial(int zoneType) { return zoneType == 6; }

struct Facility {
    enum Kind { Landfill, Incinerator, Recycling, Hazardous } kind;
    int index;         // into g_ind.sites (landfill) or g_wasteB
    V2 at;             // where its trucks leave from
    int comp = -1;     // road network component
    float capacity;    // tonnes per game month it can take (0: full / closed)
    float range;
    float took = 0;    // tonnes taken this tick
};

struct Stats {
    float producedPerMonth = 0;  // all built lots
    float capacityPerMonth = 0;  // all facilities that are open
    float collectedPerMonth = 0; // smoothed rate actually collected
    double uncollected = 0;      // tonnes lying on lots
    int piling = 0, lots = 0, unserved = 0;
    float powerMW = 0;           // incinerators
    double materials = 0;        // recycling stockpile (tonnes)
    double landfillUsed = 0, landfillCap = 0;
};
} // namespace garbage

std::unordered_map<uint64_t, double> g_garbagePile; // lot id -> tonnes waiting
std::unordered_map<uint64_t, float> g_wasteBusy;    // waste building id -> 0..1 how busy it was (smoothed)
garbage::Stats g_garbage;

namespace garbage {
// Connected components of the road network (segments joined through their nodes).
inline void Components(const tbd::city::Network& net, std::vector<int>& segComp) {
    const int S = (int)net.segs.size(), N = (int)net.nodes.size();
    std::vector<int> p((size_t)(S + N));
    std::iota(p.begin(), p.end(), 0);
    auto find = [&](int x) {
        while (p[(size_t)x] != x) x = p[(size_t)x] = p[(size_t)p[(size_t)x]];
        return x;
    };
    for (int s = 0; s < S; ++s)
        if (net.segs[s].alive) p[(size_t)find(s)] = find(S + net.segs[s].a), p[(size_t)find(s)] = find(S + net.segs[s].b);
    segComp.assign((size_t)S, -1);
    for (int s = 0; s < S; ++s)
        if (net.segs[s].alive) segComp[(size_t)s] = find(s);
}
inline int CompAt(const tbd::city::Network& net, const std::vector<int>& segComp, V2 p, float reach) {
    float t, d;
    const int s = net.SegmentAt(p, reach, t, d);
    return s >= 0 && s < (int)segComp.size() ? segComp[(size_t)s] : -1;
}
} // namespace garbage

// Runs once per game hour on the minutes gathered since (it used to run every frame, looping lots x facilities): the
// same totals per game month, collected in hourly batches instead of per frame. Each lot only looks at the facilities on
// its own road network component.
void GarbageSync(float realDt) {
    using namespace garbage;
    static float pending = 0;
    pending += realDt * g_clock.Rate() * g_clockBoost;
    if (pending < 60.0f) return;
    const float minutes = pending;
    pending = 0;
    const float months = minutes / (30.0f * 1440.0f);
    const auto& net = g_roads.net;
    static std::vector<int> segComp;
    static uint64_t compFrom = ~0ull;
    if (compFrom != net.version) compFrom = net.version, Components(net, segComp);

    // ---- facilities
    std::vector<Facility> fac;
    Stats st;
    st.materials = g_garbage.materials;
    for (int i = 0; i < (int)g_ind.sites.size(); ++i) {
        tbd::city::IndSite& s = g_ind.sites[(size_t)i];
        if (!s.IsLandfill() || s.area.empty()) continue;
        st.landfillUsed += s.stock, st.landfillCap += s.Capacity();
        const V2 gate = s.c + s.front * (tbd::city::kHubD * 0.5f + 2.0f);
        const float cap = s.Fill() >= 1.0f ? 0.0f : s.perMonth;
        fac.push_back({Facility::Landfill, i, gate, CompAt(net, segComp, gate, 12.0f), cap, kRange});
    }
    for (int i = 0; i < (int)g_wasteB.size(); ++i) {
        const WasteBuilding& b = g_wasteB[(size_t)i];
        const WasteType& T = kWasteTypes[b.type];
        const V2 gate = b.c + b.front * (T.d * 0.5f + 2.0f);
        const Facility::Kind k = b.type == 0 ? Facility::Incinerator : b.type == 1 ? Facility::Recycling : Facility::Hazardous;
        fac.push_back({k, i, gate, CompAt(net, segComp, gate, 12.0f), T.tPerMonth, k == Facility::Hazardous ? kRange * 2 : kRange});
    }
    for (const Facility& f : fac) st.capacityPerMonth += f.capacity;
    std::unordered_map<int, std::vector<int>> facByComp; // road component -> its open facilities
    for (int f = 0; f < (int)fac.size(); ++f)
        if (fac[(size_t)f].capacity > 0 && fac[(size_t)f].comp >= 0) facByComp[fac[(size_t)f].comp].push_back(f);

    // ---- lots produce; each lot is served by the nearest open facility on its road network (industrial waste site
    //      first for industry), facilities fill up in order of distance
    struct Want { int lot; int fac; float dist; };
    std::vector<Want> wants;
    for (int z = 0; z < (int)g_zones.lots.size(); ++z) {
        const tbd::city::Lot& l = g_zones.lots[(size_t)z];
        if (!l.alive || !l.Built()) continue;
        const float rate = PerCell(l.type) * (float)(l.w * l.d);
        st.producedPerMonth += rate, ++st.lots;
        double& pile = g_garbagePile[l.id];
        pile += rate * months;
        const int comp = l.frontSeg >= 0 && l.frontSeg < (int)segComp.size() ? segComp[(size_t)l.frontSeg] : -1;
        int best = -1;
        float bestD = 1e30f;
        const auto fc = facByComp.find(comp);
        static const std::vector<int> kNone;
        for (int f : fc == facByComp.end() ? kNone : fc->second) {
            const Facility& F = fac[(size_t)f];
            float d = tbd::city::Len(F.at - l.c);
            if (d > F.range) continue;
            if (F.kind == Facility::Hazardous) d = Industrial(l.type) ? d * 0.25f : d * 4.0f; // industry prefers it, others avoid it
            if (d < bestD) bestD = d, best = f;
        }
        if (best < 0) ++st.unserved;
        else wants.push_back({z, best, bestD});
        if (pile > rate * kPileMonths) ++st.piling;
    }
    std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) { return a.dist < b.dist; });
    std::vector<float> budget(fac.size());
    for (size_t f = 0; f < fac.size(); ++f) budget[f] = fac[f].capacity * months;
    double collected = 0;
    for (const Want& w : wants) {
        double& pile = g_garbagePile[g_zones.lots[(size_t)w.lot].id];
        const float take = (float)std::min<double>(pile, budget[(size_t)w.fac]);
        if (take <= 0) continue;
        pile -= take, budget[(size_t)w.fac] -= take, fac[(size_t)w.fac].took += take, collected += take;
    }
    // ---- facilities process what they took
    for (const Facility& F : fac) {
        const float busy = F.capacity > 0 && months > 0 ? std::min(1.0f, F.took / (F.capacity * months)) : 0.0f;
        if (F.kind == Facility::Landfill) {
            tbd::city::IndSite& s = g_ind.sites[(size_t)F.index];
            const int step = (int)(s.Fill() * 40);
            s.stock = std::min<double>(s.stock + F.took, s.Capacity());
            if ((int)(s.Fill() * 40) != step) ++s.version, ++g_ind.version;
        } else {
            const WasteBuilding& b = g_wasteB[(size_t)F.index];
            float& sm = g_wasteBusy[b.id];
            if (months > 0) sm += (busy - sm) * std::min(1.0f, minutes / 600.0f); // eases over ~10 game hours
            if (F.kind == Facility::Incinerator) st.powerMW += kWasteTypes[b.type].mw * sm;
            if (F.kind == Facility::Recycling) st.materials += F.took * 0.8; // 20% rejects
        }
    }
    for (const auto& pr : g_garbagePile) st.uncollected += pr.second;
    // forget demolished lots
    if (g_garbagePile.size() > g_zones.lots.size() + 64) {
        std::unordered_map<uint64_t, double> keep;
        for (const auto& l : g_zones.lots)
            if (l.alive && g_garbagePile.count(l.id)) keep[l.id] = g_garbagePile[l.id];
        g_garbagePile.swap(keep);
    }
    const float rate = months > 0 ? (float)(collected / months) : g_garbage.collectedPerMonth;
    st.collectedPerMonth = g_garbage.collectedPerMonth + (rate - g_garbage.collectedPerMonth) * std::min(1.0f, minutes / 600.0f);
    g_garbage = st;
}

// 0 (clean) .. 1 (piling up) for one lot.
float GarbageLotLevel(uint64_t lotId, int type, int w, int d) {
    const auto it = g_garbagePile.find(lotId);
    if (it == g_garbagePile.end()) return 0.0f;
    return (float)std::clamp(it->second / (garbage::PerCell(type) * w * d * garbage::kPileMonths), 0.0, 1.0);
}

// Heaps beside dirty lots (always), and the garbage info view tint over every lot while the Waste menu is open.
void GarbageDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    using tbd::city::V2;
    using tbd::Vec3;
    static std::vector<tbd::Vertex> o;
    o.clear();
    const bool view = g_indTool.active && g_indTool.waste;
    for (const tbd::city::Lot& l : g_zones.lots) {
        if (!l.alive || !l.Built()) continue;
        const float k = GarbageLotLevel(l.id, l.type, l.w, l.d);
        const V2 side = tbd::city::LeftOf(l.front);
        if (k >= 1.0f || (k > 0.6f && !view)) { // heap of black bags on the kerb corner
            const V2 p = l.c + l.front * (l.d * tbd::city::kCell * 0.5f - 1.6f) + side * (l.w * tbd::city::kCell * 0.5f - 1.8f);
            const float y = RoadsGround(p.x, p.z);
            const float h = 0.5f + 0.7f * std::min(1.0f, k);
            ind::Cone(o, {p.x, y, p.z}, 1.3f, h, {0.12f, 0.12f, 0.13f}, 7);
            ind::Cone(o, {p.x + side.x * 0.9f, y, p.z + side.z * 0.9f}, 0.8f, h * 0.8f, {0.45f, 0.36f, 0.22f}, 6);
        }
        if (view) {
            const Vec3 col = Vec3{0.30f, 0.80f, 0.35f} * (1 - k) + Vec3{0.55f, 0.33f, 0.12f} * k;
            const float hw = l.w * tbd::city::kCell * 0.5f - 0.5f, hd = l.d * tbd::city::kCell * 0.5f - 0.5f;
            auto P = [&](float a, float b) {
                const V2 q = l.c + side * a + l.front * b;
                return Vec3{q.x, RoadsGround(q.x, q.z) + 0.6f, q.z};
            };
            zfx::Quad(o, P(-hw, -hd), P(hw, -hd), P(hw, hd), P(-hw, hd), {0, 0, 0}, col);
        }
    }
    if (view) // facility range rings
        for (const WasteBuilding& b : g_wasteB) {
            const float R = b.type == 2 ? garbage::kRange * 2 : garbage::kRange;
            for (int i = 0; i < 96; ++i) {
                const float a0 = i * 6.2831853f / 96, a1 = (i + 1) * 6.2831853f / 96;
                const V2 p0 = b.c + V2{std::cos(a0), std::sin(a0)} * R, p1 = b.c + V2{std::cos(a1), std::sin(a1)} * R;
                const Vec3 c{0.95f, 0.75f, 0.25f};
                zfx::Beam(o, {p0.x, RoadsGround(p0.x, p0.z) + 1.0f, p0.z}, {p1.x, RoadsGround(p1.x, p1.z) + 1.0f, p1.z}, 1.2f, c);
            }
        }
    if (!o.empty()) r.DrawTransient(o.data(), (uint32_t)o.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
}

// City-wide numbers at the top of the Waste Management panel.
void GarbagePanel() {
    using namespace tbd::gui;
    Context& g = g_gui;
    const garbage::Stats& s = g_garbage;
    g.Text(TextStyle::Normal, "Garbage %.0f t / month   Capacity %.0f", s.producedPerMonth, s.capacityPerMonth);
    g.Text(TextStyle::Normal, "Collected %.0f t / month   Waiting %.0f t", s.collectedPerMonth, s.uncollected);
    if (s.unserved || s.piling) g.Text(TextStyle::Muted, "%d of %d buildings unserved, %d piling up", s.unserved, s.lots, s.piling);
    if (s.landfillCap > 0) g.Text(TextStyle::Normal, "Landfills %.0f%% full   Power %.1f MW", s.landfillUsed / s.landfillCap * 100.0, s.powerMW);
    else if (s.powerMW > 0) g.Text(TextStyle::Normal, "Incineration power %.1f MW", s.powerMW);
    if (s.materials > 0) g.Text(TextStyle::Normal, "Recycled materials %.0f t", s.materials);
}
