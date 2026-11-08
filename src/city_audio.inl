// ------------------------------------------------------------------- the city view's sound: music, ambience, event sfx
// Included by game_core.inl after glass_dock_host.inl; CityAudioSync runs once per frame after RadioSync. It only reads the
// city (no tool or sim code calls into it): what the view looks at goes to tbd::audio::SetCityScene (the audio thread
// plays the music playlist and the ambience beds from it, src/audio_director.h), and what changed since the last frame
// becomes a sound:
//   build_place     something was placed: a road segment, a service / transport building, a utility building or line,
//                   a waste building, an industry site (at most one every 0.2 s)
//   demolish        any of those removed (bulldozer, undo; at most one every 0.3 s)
//   cash_register   a loan taken, a milestone reached, the treasury passing $100k / 250k / 500k / 1M / 2.5M / 5M / 10M
//                   upward (at most one every 15 s)
//   siren_pass      a fire, police or health building finished; and now and then while the camera is low over a city
//                   that has one (every 2-5 real minutes, never with the clock paused)
// A load or a new map is taken as it stands (nothing plays for what was already there). Nothing here runs in a TBD_SHOT /
// TBD_CAPTURE run: audio::Start did not start, so every call is a no-op.

namespace city_audio {

struct State {
    uint64_t generation = ~0ull;
    double sinceLoad = 0.0;
    size_t segs = 0, transport = 0, utilB = 0, utilL = 0, waste = 0, sites = 0, loans = 0;
    int milestone = 0;
    int treasuryMark = 0; // the highest $ mark passed
    std::set<uint64_t> servicesDone;
    double lastBuild = -1e9, lastDemolish = -1e9, lastCash = -1e9, lastSiren = -1e9, now = 0.0;
    double nextAmbientSiren = 180.0;
    float sample = 0.0f; // seconds to the next look at the view (built / traffic / water)
    tbd::audio::CityScene scene;
    bool haveService = false;
    uint32_t rng = 0x6a09e667u;
};
State g;

float Rand01() {
    g.rng ^= g.rng << 13, g.rng ^= g.rng >> 17, g.rng ^= g.rng << 5;
    return (float)(g.rng & 0xffffffu) / 16777216.0f;
}

constexpr long long kMarks[] = {100000LL, 250000LL, 500000LL, 1000000LL, 2500000LL, 5000000LL, 10000000LL}; // dollars

int TreasuryMark() {
    const long long dollars = (long long)(g_simEcon.treasury / 100);
    int m = 0;
    for (long long v : kMarks) m += dollars >= v ? 1 : 0;
    return m;
}

bool IsService(const tbd::city::BuildingDef& d) {
    using tbd::city::TTab;
    return d.tab == TTab::Fire || d.tab == TTab::Police || d.tab == TTab::Health;
}

struct Counts { size_t segs = 0, transport = 0, utilB = 0, utilL = 0, waste = 0, sites = 0; };
Counts Count() {
    Counts c;
    for (const auto& s : g_roads.net.segs) c.segs += s.alive ? 1 : 0;
    for (const auto& b : g_transport.items) c.transport += b.alive ? 1 : 0;
    for (const auto& u : g_util.items) c.utilB += u.alive ? 1 : 0;
    for (const auto& l : g_util.lines) c.utilL += l.alive ? 1 : 0;
    c.waste = g_wasteB.size();
    c.sites = g_ind.sites.size();
    return c;
}

void Baseline() {
    const Counts c = Count();
    g.segs = c.segs, g.transport = c.transport, g.utilB = c.utilB, g.utilL = c.utilL, g.waste = c.waste, g.sites = c.sites;
    g.loans = g_simBudget.loans.size();
    g.milestone = g_simMilestones.Level();
    g.treasuryMark = TreasuryMark();
    g.servicesDone.clear();
    for (const auto& b : g_transport.items)
        if (b.alive && b.state == tbd::city::BState::Done && IsService(tbd::city::Defs()[b.def])) g.servicesDone.insert(b.id);
}

// What the view looks at, a couple of times a second: built-up share, road length and life, water near.
void SampleView(float height) {
    using namespace tbd::city;
    const tbd::Vec3 focus = g_engine.camera.target;
    const float R = std::clamp(height * 1.2f, 120.0f, 1500.0f), R2 = R * R, area = 3.14159265f * R2;
    float lotArea = 0.0f;
    for (const Lot& l : g_zones.lots) {
        if (!l.alive || !l.Built()) continue;
        const float dx = l.c.x - focus.x, dz = l.c.z - focus.z;
        if (dx * dx + dz * dz <= R2) lotArea += (float)(l.w * l.d) * kCell * kCell;
    }
    g.scene.built = std::clamp(lotArea / area * 2.5f, 0.0f, 1.0f); // ~40% of the ground under buildings reads as full
    float road = 0.0f;
    const auto& net = g_roads.net;
    for (const auto& s : net.segs) {
        if (!s.alive || s.a < 0 || s.b < 0) continue;
        const tbd::Vec3 a = net.nodes[(size_t)s.a].p, b = net.nodes[(size_t)s.b].p;
        const float mx = 0.5f * (a.x + b.x) - focus.x, mz = 0.5f * (a.z + b.z) - focus.z;
        if (mx * mx + mz * mz <= R2) road += std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    }
    const float life = 0.25f + 0.75f * std::clamp((float)g_simTotals.population / 3000.0f, 0.0f, 1.0f); // empty roads are quiet
    g.scene.traffic = std::clamp(road / area / 0.015f, 0.0f, 1.0f) * life; // 15 km of road per km^2 reads as busy
    float wet = 0.0f;
    if (g_engine.terrain.params.enabled && g_engine.terrain.params.mapKind >= 1) {
        int n = 0, hits = 0;
        for (int ring = 0; ring < 2; ++ring)
            for (int k = 0; k < 8; ++k, ++n) {
                const float r = ring == 0 ? R * 0.35f : R * 0.75f, a = (float)k * 0.785398f + (float)ring * 0.39f;
                hits += g_engine.terrain.HeightAt(focus.x + r * std::cos(a), focus.z + r * std::sin(a)) < tbd::IslandMap::kSeaLevel ? 1 : 0;
            }
        hits += g_engine.terrain.HeightAt(focus.x, focus.z) < tbd::IslandMap::kSeaLevel ? 3 : 0;
        wet = std::clamp((float)hits / (float)(n + 3) * 2.5f, 0.0f, 1.0f);
    }
    g.scene.water = wet;
    g.haveService = false;
    for (const auto& b : g_transport.items)
        if (b.alive && b.state == BState::Done && IsService(Defs()[b.def])) { g.haveService = true; break; }
}

} // namespace city_audio

void CityAudioSync(float realDt) {
    using namespace city_audio;
    namespace au = tbd::audio;
    g.now += realDt;
    au::CityScene& sc = g.scene;
    sc.active = g_terrainSandbox && !g_play.active;
    if (!sc.active) { au::SetCityScene(sc); return; }
    sc.menu = gdock::g_state.menu;
    sc.hour = (float)std::fmod(g_clock.minutes / 60.0, 24.0);
    const tbd::Vec3 eye = g_engine.camera.Eye();
    sc.height = std::max(0.0f, eye.y - CityCamGround(eye.x, eye.z));
    if ((g.sample -= realDt) <= 0.0f) g.sample = 0.5f, SampleView(sc.height);
    au::SetCityScene(sc);

    if (g.generation != g_cityGeneration) { // a new map or a load: what is there was not just built
        g.generation = g_cityGeneration;
        g.sinceLoad = 0.0;
        Baseline();
        return;
    }
    g.sinceLoad += realDt;
    if (g.sinceLoad < 2.0) { Baseline(); return; }

    // placed / removed
    const Counts c = Count();
    const bool more = c.segs > g.segs || c.transport > g.transport || c.utilB > g.utilB || c.utilL > g.utilL || c.waste > g.waste || c.sites > g.sites;
    const bool fewer = c.segs < g.segs || c.transport < g.transport || c.utilB < g.utilB || c.utilL < g.utilL || c.waste < g.waste || c.sites < g.sites;
    // a road laid across another splits it (one fewer, two more): a net gain is a build, only a net loss a demolition
    const long long net = (long long)(c.segs + c.transport + c.utilB + c.utilL + c.waste + c.sites) -
                          (long long)(g.segs + g.transport + g.utilB + g.utilL + g.waste + g.sites);
    if (more && net >= 0 && g.now - g.lastBuild >= 0.2) g.lastBuild = g.now, au::PlayUi("sfx/build_place", 0.7f, 0.96f + 0.08f * Rand01());
    else if (fewer && net < 0 && g.now - g.lastDemolish >= 0.3) g.lastDemolish = g.now, au::PlayUi("sfx/demolish", 0.6f, 0.95f + 0.1f * Rand01());
    g.segs = c.segs, g.transport = c.transport, g.utilB = c.utilB, g.utilL = c.utilL, g.waste = c.waste, g.sites = c.sites;

    // money
    const size_t loans = g_simBudget.loans.size();
    const int milestone = g_simMilestones.Level(), mark = TreasuryMark();
    const bool cash = loans > g.loans || milestone > g.milestone || mark > g.treasuryMark;
    if (cash && g.now - g.lastCash >= 15.0) g.lastCash = g.now, au::PlayUi("sfx/cash_register", 0.55f);
    g.loans = loans, g.milestone = milestone;
    g.treasuryMark = std::max(g.treasuryMark, mark); // each mark once: dipping under and back does not ring again

    // sirens
    const float closeK = au::Nearness(sc.height);
    for (const auto& b : g_transport.items) {
        if (!b.alive || b.state != tbd::city::BState::Done || !IsService(tbd::city::Defs()[b.def])) continue;
        if (g.servicesDone.insert(b.id).second && g.now - g.lastSiren >= 30.0)
            g.lastSiren = g.now, au::PlayUi("sfx/siren_pass", 0.25f + 0.25f * closeK);
    }
    if (g.now >= g.nextAmbientSiren) {
        g.nextAmbientSiren = g.now + 120.0 + 180.0 * Rand01();
        if (g.haveService && !g_clock.paused && !sc.menu && sc.height < 350.0f && g.now - g.lastSiren >= 60.0)
            g.lastSiren = g.now, au::PlayUi("sfx/siren_pass", 0.1f + 0.25f * closeK, 0.94f + 0.12f * Rand01());
    }
}
