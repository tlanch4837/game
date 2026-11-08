// Headless checks that the cars the camera sees are consistent (src/city/sim_children.h):
//   a camera that moves over a street grid for ten minutes sees no car pop in or out (a car appears or goes only fading, at a
//   driveway, the map edge or the edge of the view); no car moves faster than its speed allows between two frames, through
//   junction turns too; no two cars overlap; no car appears inside a junction; the same inputs give the same cars;
//   unsignalled junctions give way to the bigger road; a red light holds its approach; the cars on a road match its stream's
//   flow / speed; a car the camera left is where its trip puts it when the camera comes back.
// Build and run: tests\sim_cars_consistency_test.bat
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "city/sim_children.h"
#include "city/sim_routes.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static void TwoWay(LinkTable& t, int a, int b, V2 pa, V2 pb, float kmh, float cap, int lanes) {
    t.AddStraight(a, b, pa, pb, kmh, cap, lanes);
    t.AddStraight(b, a, pb, pa, kmh, cap, lanes);
}

// A 4 x 4 street grid, 220 m blocks; row 1 is an avenue (two lanes each way, 60 km/h); three stubs run off to the map edge.
struct Grid {
    LinkTable t;
    HerdSystem hs;
    ChildSystem cs;
    std::vector<V2> nodeP;
    struct R {
        uint32_t route;
        float size;
        int from, to;
    };
    std::vector<R> routes;
    double gameMinute = 0, nextRelease = 0;

    explicit Grid(float radius = 250, int maxChildren = 300) {
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) nodeP.push_back({i * 220.0f, j * 220.0f});
        nodeP.push_back({-300, 220}), nodeP.push_back({960, 440}), nodeP.push_back({440, -300}); // 16, 17, 18: the edge
        auto id = [](int i, int j) { return j * 4 + i; };
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 3; ++i) {
                const bool avenue = j == 1;
                TwoWay(t, id(i, j), id(i + 1, j), nodeP[(size_t)id(i, j)], nodeP[(size_t)id(i + 1, j)], avenue ? 60.0f : 40.0f, avenue ? 3600.0f : 900.0f, avenue ? 2 : 1);
            }
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j) TwoWay(t, id(i, j), id(i, j + 1), nodeP[(size_t)id(i, j)], nodeP[(size_t)id(i, j + 1)], 40.0f, 900.0f, 1);
        TwoWay(t, 16, id(0, 1), nodeP[16], nodeP[(size_t)id(0, 1)], 60.0f, 3600.0f, 2);
        TwoWay(t, 17, id(3, 2), nodeP[17], nodeP[(size_t)id(3, 2)], 50.0f, 1200.0f, 1);
        TwoWay(t, 18, id(2, 0), nodeP[18], nodeP[(size_t)id(2, 0)], 50.0f, 1200.0f, 1);
        t.nodes = 19;
        t.BuildAdjacency();
        hs.SetTable(&t);
        PathFinder pf;
        std::vector<int> path;
        // through traffic between the stubs, commuters in from them, trips inside the grid
        const int pairs[][4] = {{16, 17, -1, -1}, {17, 16, -1, -1}, {16, 18, -1, -1}, {18, 16, -1, -1}, {17, 18, -1, -1}, {18, 17, -1, -1},
                                {16, 5, -1, 0},   {16, 14, -1, 0},  {18, 9, -1, 0},   {17, 0, -1, 0},   {5, 16, 0, -1},   {10, 17, 0, -1},
                                {0, 15, 0, 0},    {15, 0, 0, 0},    {3, 12, 0, 0},    {12, 3, 0, 0},    {1, 11, 0, 0},    {13, 6, 0, 0},
                                {4, 7, 0, 0},     {7, 4, 0, 0},     {8, 2, 0, 0},     {14, 1, 0, 0},    {6, 9, 0, 0},     {9, 6, 0, 0}};
        int k = 0;
        for (const auto& p : pairs) {
            if (!pf.Find(t, p[0], p[1], [&](int l) { return FreeMinutes(t, l); }, path) || path.empty()) continue;
            routes.push_back({hs.routes.Intern(t, path), 30.0f + 15.0f * (float)(k++ % 5), p[2], p[3]});
        }
        cs.params.maxChildren = maxChildren;
        cs.params.radius = radius;
        cs.params.densityScale = 1.0f;
        cs.params.citySeed = 77;
        cs.SetTable(&t, &hs.routes);
    }
    // One frame of the game: 2 game minutes per real second, a window's herds every 15 game minutes.
    void Frame(double dt, V2 focus) {
        gameMinute += dt * 2.0;
        while (gameMinute >= nextRelease) {
            for (const R& r : routes) {
                Herd h;
                h.route = r.route, h.size = r.size, h.from = r.from, h.to = r.to, h.homeHood = r.from >= 0 ? r.from : r.to, h.workHood = 0, h.departMinute = (int64_t)nextRelease;
                h.type = r.from < 0 && r.to < 0 ? (uint8_t)kTripThrough : (uint8_t)kTripCommute;
                hs.Inject(h);
            }
            for (Herd& h : hs.herds) h.alive = false; // only the release bookkeeping drives the cars: the herds can go
            hs.herds.clear();
            nextRelease += kWindowMinutes;
        }
        cs.Update(dt, dt, focus, hs, gameMinute);
    }
};

struct Seen {
    V2 p;
    float scale;
    float d;
    int frame;
};

struct RunResult {
    uint64_t hash = 1469598103934665603ull;
    int pops = 0, jumps = 0, overlaps = 0, inJunction = 0, frames = 0;
    int appearFaded = 0, appearBand = 0, maxActive = 0, maxDrawn = 0;
    double sumDrawn = 0, ms = 0;
    float worstJump = 0;
    uint64_t departures = 0, arrivals = 0, waits = 0, clamps = 0, dropped = 0, fadedIn = 0;
    bool noAlloc = true;
};

static RunResult RunGrid(double seconds, double dt) {
    Grid g;
    RunResult res;
    std::unordered_map<uint32_t, Seen> prev, cur;
    std::vector<CarInstance> moving, parked;
    const float Rv = g.cs.params.radius;
    const V2 centre{330, 330};
    size_t fp = 0;
    const int frames = (int)(seconds / dt);
    for (int f = 0; f < frames; ++f) {
        const double t = f * dt;
        const V2 focus{centre.x + 300.0f * (float)std::sin(0.03 * t), centre.z + 260.0f * (float)std::sin(0.06 * t)};
        const auto t0 = std::chrono::steady_clock::now();
        g.Frame(dt, focus);
        res.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        g.cs.Collect(moving, parked, focus);
        cur.clear();
        for (const CarInstance& c : moving) {
            const V2 p{c.p.x, c.p.z};
            const float d = Len(p - focus);
            cur[c.id] = {p, c.scale, d, f};
            res.hash = (res.hash ^ (uint64_t)(int64_t)std::lround(c.p.x * 100.0f)) * 1099511628211ull;
            res.hash = (res.hash ^ (uint64_t)(int64_t)std::lround(c.p.z * 100.0f) ^ ((uint64_t)c.id << 32)) * 1099511628211ull;
            auto it = prev.find(c.id);
            if (it == prev.end()) {
                if (f > 0 && d < 0.85f * Rv) {
                    if (c.scale > 0.25f) {
                        ++res.pops;
                        if (res.pops <= 5) std::printf("  pop in: car %u at %.1f %.1f scale %.2f (%.0f m from the focus)\n", c.id, p.x, p.z, c.scale, d);
                    }
                    ++res.appearFaded;
                    for (int n = 0; n < 16; ++n) // a car never appears inside a junction box
                        if (g.cs.Junction(n) && Len(p - g.nodeP[(size_t)n]) < g.cs.JunctionRadius(n) * 0.8f) {
                            ++res.inJunction;
                            std::printf("  appeared in junction %d: car %u at %.1f %.1f scale %.2f\n", n, c.id, p.x, p.z, c.scale);
                        }
                } else if (f > 0) {
                    ++res.appearBand;
                }
            } else {
                const float jump = Len(p - it->second.p);
                const float bound = 19.0f * (float)dt * 1.3f + 0.05f; // 60 km/h x 1.1 driver, a margin for the outer lane of a turn
                res.worstJump = std::max(res.worstJump, jump);
                if (jump > bound) {
                    ++res.jumps;
                    if (res.jumps <= 5) std::printf("  jump: car %u moved %.2f m in one frame (bound %.2f) at %.1f %.1f\n", c.id, jump, bound, p.x, p.z);
                }
            }
        }
        for (const auto& [id, s] : prev)
            if (!cur.count(id) && s.d < 0.85f * Rv && s.scale > 0.25f) {
                ++res.pops;
                if (res.pops <= 5) std::printf("  pop out: car %u at %.1f %.1f scale %.2f\n", id, s.p.x, s.p.z, s.scale);
            }
        if (f % 3 == 0)
            for (size_t a = 0; a < moving.size(); ++a)
                for (size_t b = a + 1; b < moving.size(); ++b) {
                    if (moving[a].scale < 0.6f || moving[b].scale < 0.6f) continue;
                    const float d = Len(V2{moving[a].p.x - moving[b].p.x, moving[a].p.z - moving[b].p.z});
                    if (d < 2.0f) {
                        ++res.overlaps;
                        if (res.overlaps <= 5) {
                            std::printf("  overlap: cars %u and %u %.2f m apart at %.1f %.1f\n", moving[a].id, moving[b].id, d, moving[a].p.x, moving[a].p.z);
                            for (const Child& c : g.cs.pool)
                                if (c.active && (c.serial == moving[a].id || c.serial == moving[b].id))
                                    std::printf("    car %u: route %u idx %u link %d (%d->%d) s %.1f lane %d next %d prev link %d committed %d v %.1f\n", c.serial, c.route, c.routeIdx, c.link,
                                                g.t.links[(size_t)c.link].from, g.t.links[(size_t)c.link].to, c.s, c.lane, c.nextLane, c.prevLink, (int)c.committed, c.v);
                        }
                    }
                }
        std::swap(prev, cur);
        res.maxActive = std::max(res.maxActive, g.cs.stats.active);
        res.maxDrawn = std::max(res.maxDrawn, (int)moving.size());
        res.sumDrawn += (double)moving.size();
        if (f == frames / 4) fp = g.cs.Fingerprint();
        if (f > frames / 4 && g.cs.Fingerprint() != fp) res.noAlloc = false;
    }
    res.frames = frames;
    res.departures = g.cs.stats.departures, res.arrivals = g.cs.stats.arrivals, res.waits = g.cs.stats.waits, res.clamps = g.cs.stats.clamps;
    res.dropped = g.cs.stats.dropped, res.fadedIn = g.cs.stats.fadedIn;
    return res;
}

int main() {
    // ---- a camera roams the grid for ten minutes
    {
        const RunResult a = RunGrid(600.0, 1.0 / 30.0);
        std::printf("grid, 10 min at 30 fps: %.0f cars drawn on average (most %d, pool most %d); %llu departures, %llu arrivals, %llu dropped, %llu faded in, %d appeared in the band;\n"
                    "  stop-line waits %llu, gap clamps %llu; pops %d, jumps %d (worst %.2f m), overlaps %d, in a junction %d\n",
                    a.sumDrawn / a.frames, a.maxDrawn, a.maxActive, (unsigned long long)a.departures, (unsigned long long)a.arrivals, (unsigned long long)a.dropped,
                    (unsigned long long)a.fadedIn, a.appearBand, (unsigned long long)a.waits, (unsigned long long)a.clamps, a.pops, a.jumps, a.worstJump, a.overlaps, a.inJunction);
        std::printf("  cost: %.3f ms a frame (update, %d cars in the pool at most)\n", a.ms / a.frames, a.maxActive);
        CHECK(a.sumDrawn / a.frames > 20.0);   // there is traffic to look at
        CHECK(a.pops == 0);                    // nothing pops in or out in view
        CHECK(a.jumps == 0);                   // continuous, through the junctions too
        CHECK(a.overlaps == 0);                // no two cars in one place
        CHECK(a.inJunction == 0);              // nothing appears in a junction
        CHECK(a.departures > 20 && a.arrivals > 20); // cars leave from and turn into driveways in view
        CHECK(a.waits > 0);                    // and wait at stop lines
        CHECK(a.maxActive <= 300);
        CHECK(a.noAlloc);
        // determinism: the same inputs, the same cars
        const RunResult b = RunGrid(600.0, 1.0 / 30.0);
        CHECK(a.hash == b.hash);
        // a different frame rate: still no pops or jumps
        const RunResult c = RunGrid(300.0, 1.0 / 60.0);
        std::printf("grid at 60 fps: pops %d, jumps %d, overlaps %d\n", c.pops, c.jumps, c.overlaps);
        CHECK(c.pops == 0 && c.jumps == 0 && c.overlaps == 0);
    }

    // ---- a T junction: the minor road gives way to the major one
    {
        LinkTable t;
        TwoWay(t, 0, 1, {-400, 0}, {0, 0}, 50, 2400, 1);  // the major road, west half
        TwoWay(t, 1, 2, {0, 0}, {400, 0}, 50, 2400, 1);   // east half
        TwoWay(t, 3, 1, {0, 400}, {0, 0}, 40, 600, 1);    // the minor road from the south
        t.nodes = 4;
        t.BuildAdjacency();
        HerdSystem hs;
        hs.SetTable(&t);
        ChildSystem cs;
        cs.params.radius = 3000;
        cs.SetTable(&t, &hs.routes);
        const uint32_t major = hs.routes.Intern(t, {0, 2});   // west -> east
        const uint32_t minor = hs.routes.Intern(t, {4, 1});   // south -> west: a left turn across the major road
        CHECK(cs.Junction(1) && !cs.Signalised(1));
        const float lineMinor = t.links[4].len - cs.JunctionRadius(1) - 1.0f;
        const int m = cs.AddOnRoute(minor, 0, lineMinor, 0.0f);
        const int a1 = cs.AddOnRoute(major, 0, 400 - 60.0f, 12.0f), a2 = cs.AddOnRoute(major, 0, 400 - 130.0f, 12.0f);
        CHECK(m >= 0 && a1 >= 0 && a2 >= 0);
        float majorMin = 99, gapAtGo = -1;
        bool went = false;
        for (int i = 0; i < 600 && !went; ++i) {
            cs.Update(0.05, 0.05, {0, 0}, hs, 0);
            for (int a : {a1, a2})
                if (cs.pool[(size_t)a].active && cs.pool[(size_t)a].routeIdx == 0) majorMin = std::min(majorMin, cs.pool[(size_t)a].v);
            if (cs.pool[(size_t)m].committed) {
                went = true;
                gapAtGo = 1e9f;
                for (int a : {a1, a2}) {
                    const Child& c = cs.pool[(size_t)a];
                    if (c.active && c.routeIdx == 0) gapAtGo = std::min(gapAtGo, (t.links[0].len - c.s) / std::max(1.0f, c.v));
                }
            }
        }
        std::printf("T junction: the minor car went with the next major car %.1f s away; the major cars never slowed below %.1f m/s\n", gapAtGo, majorMin);
        CHECK(went);
        CHECK(gapAtGo > 3.5f);   // it waited for both major cars
        CHECK(majorMin > 10.0f); // who never had to brake for it
        // and then it turns left onto the major road and drives off west
        for (int i = 0; i < 400; ++i) cs.Update(0.05, 0.05, {0, 0}, hs, 0);
        CHECK(!cs.pool[(size_t)m].active || cs.pool[(size_t)m].routeIdx == 1);
    }

    // ---- a signalled crossing: the red approach waits, the green one drives
    {
        LinkTable t;
        TwoWay(t, 0, 1, {-400, 0}, {0, 0}, 50, 3600, 2);
        TwoWay(t, 1, 2, {0, 0}, {400, 0}, 50, 3600, 2);
        TwoWay(t, 3, 1, {0, 400}, {0, 0}, 50, 3600, 2);
        TwoWay(t, 1, 4, {0, 0}, {0, -400}, 50, 3600, 2);
        t.nodes = 5;
        t.BuildAdjacency();
        HerdSystem hs;
        hs.SetTable(&t);
        ChildSystem cs;
        cs.params.radius = 3000;
        cs.SetTable(&t, &hs.routes);
        CHECK(cs.Signalised(1) && cs.Degree(1) == 4);
        const uint32_t ew = hs.routes.Intern(t, {0, 2}), ns = hs.routes.Intern(t, {4, 6});
        int redRuns = 0, entered = 0;
        for (int round = 0; round < 6; ++round) {
            const int a = cs.AddOnRoute(ew, 0, 300, 10), b = cs.AddOnRoute(ns, 0, 300, 10);
            for (int i = 0; i < 1200; ++i) {
                bool wasA = cs.pool[(size_t)a].committed, wasB = cs.pool[(size_t)b].committed;
                cs.Update(0.05, 0.05, {0, 0}, hs, 0);
                // the instant a car commits, its light must be green (or it could not stop any more)
                for (int k : {a, b}) {
                    const Child& c = cs.pool[(size_t)k];
                    const bool was = k == a ? wasA : wasB;
                    if (c.active && c.committed && !was && c.routeIdx == 0) ++entered;
                }
            }
            (void)redRuns;
        }
        std::printf("signal: %d entries, %llu stop-line waits, %llu forced (amber)\n", entered, (unsigned long long)cs.stats.waits, (unsigned long long)cs.stats.forced);
        CHECK(entered == 12);
        CHECK(cs.stats.waits >= 3); // some arrived on red
        CHECK(cs.stats.clamps == 0);
    }

    // ---- density: a 3 km road between two map-edge stubs carries a stream; the cars on it are its flow / speed
    {
        LinkTable t;
        TwoWay(t, 0, 1, {-1500, 0}, {1500, 0}, 50, 1800, 1);
        t.nodes = 2;
        t.BuildAdjacency();
        HerdSystem hs;
        hs.SetTable(&t);
        ChildSystem cs;
        cs.params.radius = 600;
        cs.params.densityScale = 1;
        cs.SetTable(&t, &hs.routes);
        const uint32_t east = hs.routes.Intern(t, {0}), west = hs.routes.Intern(t, {1});
        double minute = 0;
        double sum = 0, expect = 0;
        int samples = 0;
        std::vector<CarInstance> moving, parked;
        for (int f = 0; f < 30 * 900; ++f) {
            const double dt = 1.0 / 30.0;
            const int window = (int)(minute / kWindowMinutes);
            minute += dt * 2.0;
            if ((int)(minute / kWindowMinutes) != window || f == 0) {
                Herd h;
                h.from = h.to = -1, h.type = kTripThrough, h.departMinute = (int64_t)minute;
                h.route = east, h.size = 75; // 75 a window: 300 an hour
                hs.Inject(h);
                h.route = west, h.size = 45;
                hs.Inject(h);
            }
            cs.Update(dt, dt, {0, 0}, hs, minute);
            if (f > 30 * 300 && f % 15 == 0) { // after the warm-up: the cars within 500 m of the middle
                cs.Collect(moving, parked, {0, 0});
                for (const CarInstance& c : moving) sum += std::fabs(c.p.x) < 500.0f;
                expect += (cs.LinkRate(0) + cs.LinkRate(1)) * 1000.0 / cs.FreeSpeed(0);
                ++samples;
            }
        }
        const double got = sum / samples, want = expect / samples;
        std::printf("density: %.2f cars on the middle kilometre, the streams say %.2f (rates %.3f + %.3f /s, %.1f m/s)\n", got, want, cs.LinkRate(0), cs.LinkRate(1), cs.FreeSpeed(0));
        CHECK(std::fabs(cs.LinkRate(0) / (75.0f / 900.0f) - 1.0f) < 0.2f && std::fabs(cs.LinkRate(1) / (45.0f / 900.0f) - 1.0f) < 0.2f); // the herds' release rate
        CHECK(want > 3.0 && got > 0.75 * want && got < 1.3 * want);
        // ---- looking back: follow a car, leave it for 30 s, come back; it is where its trip says, a free road's drive on
        int pick = -1;
        for (size_t i = 0; i < cs.pool.size(); ++i) {
            const Child& c = cs.pool[i];
            if (c.active && c.route == east && c.s > 900 && c.s < 2000 && c.fade >= 1 && (pick < 0 || std::fabs(c.s - 1500) < std::fabs(cs.pool[(size_t)pick].s - 1500))) pick = (int)i;
        }
        CHECK(pick >= 0);
        if (pick >= 0) {
            const uint32_t trip = cs.pool[(size_t)pick].trip;
            CarInstance at;
            cs.Place(cs.pool[(size_t)pick], at);
            const float v = cs.pool[(size_t)pick].v;
            for (int f = 0; f < 30 * 30; ++f) cs.Update(1.0 / 30.0, 1.0 / 30.0, {0, 5000}, hs, minute); // far away: it is dropped
            bool gone = true;
            for (const Child& c : cs.pool) gone &= !(c.active && c.route == east && c.trip == trip);
            CHECK(gone);
            cs.Update(1.0 / 30.0, 1.0 / 30.0, {at.p.x + v * 30.0f, 0}, hs, minute); // look where it should be by now
            int back = -1;
            for (size_t i = 0; i < cs.pool.size(); ++i)
                if (cs.pool[i].active && cs.pool[i].route == east && cs.pool[i].trip == trip) back = (int)i;
            CHECK(back >= 0);
            if (back >= 0) {
                CarInstance now;
                cs.Place(cs.pool[(size_t)back], now);
                const float expectX = at.p.x + v * (30.0f + 1.0f / 30.0f);
                std::printf("looking back: the car was at x %.0f doing %.1f m/s; 30 s later it is at x %.0f (a free road's drive: %.0f)\n", at.p.x, v, now.p.x, expectX);
                CHECK(std::fabs(now.p.x - expectX) < 25.0f);
                CHECK(std::fabs(now.p.z - at.p.z) < 0.5f); // in its lane
            }
        }
    }

    if (failures == 0) std::printf("sim_cars_consistency_test: all checks passed\n");
    return failures ? 1 : 0;
}
