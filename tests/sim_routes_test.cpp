// Headless checks for routes (src/city/sim_routes.h):
//   a highway run becomes one pipe link per direction; route-tree next-hops reach the destination and add up to the tree's
//   minutes; the cache keeps one tree per destination, evicts the least recent, and drops everything when the network
//   version changes; alternatives come out as distinct routes (highway first, then local); the logit split moves toward the
//   faster alternative and does not flip-flop over 20 windows, even when the loads follow the shares.
// Build and run: tests\sim_routes_test.bat
#include <cstdio>
#include <vector>

#include "city/sim_routes.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static int Seg(Network& net, int a, int b, int prof) {
    const V2 pa{net.nodes[a].p.x, net.nodes[a].p.z}, pb{net.nodes[b].p.x, net.nodes[b].p.z};
    return net.AddSeg(a, b, pa + (pb - pa) * (1.0f / 3.0f), pa + (pb - pa) * (2.0f / 3.0f), prof);
}

struct Scene {
    Network net;
    FlowGraph g;
    LinkTable t;
    LinkBuild lb;
    int A, R1, M, R2, D, L;
    Scene() {
        const int local = ProfileIndex("Two-Lane Road"), medium = ProfileIndex("Four-Lane Road"), hwy = ProfileIndex("Four-Lane Two-Way Highway");
        A = net.AddNode({0, 0, 0}, true), R1 = net.AddNode({800, 0, 0}, true), M = net.AddNode({3000, 0, 0}, true);
        R2 = net.AddNode({5200, 0, 0}, true), D = net.AddNode({6000, 0, 0}, true), L = net.AddNode({3000, 0, 900}, true);
        Seg(net, A, R1, local);
        Seg(net, R1, M, hwy);
        Seg(net, M, R2, hwy);
        Seg(net, R1, L, medium);
        Seg(net, L, R2, medium);
        Seg(net, R2, D, local);
        g = FlowGraph::Build(net);
        BuildLinkTable(net, g, t, &lb);
    }
    int Pipe(bool forward) const {
        for (size_t i = 0; i < t.links.size(); ++i)
            if (t.links[i].pipe && (t.links[i].from == R1) == forward) return (int)i;
        return -1;
    }
};

int main() {
    Scene s;
    // ---- the highway run is one pipe per direction
    {
        int pipes = 0;
        for (const LinkInfo& l : s.t.links) pipes += l.pipe;
        CHECK(pipes == 2);
        CHECK(s.t.links.size() == s.g.edges.size() - 2);
        const LinkInfo& f = s.t.links[(size_t)s.Pipe(true)];
        CHECK(f.from == s.R1 && f.to == s.R2 && std::fabs(f.len - 4400.0f) < 5.0f);
        CHECK(std::fabs(f.freeMpm - 90.0f * 1000 / 60) < 1.0f);
        CHECK(f.cap > 3000);
        V2 p, d; // the polyline runs from one end to the other
        float y;
        CHECK(s.t.PointAt(s.Pipe(true), 0, p, y, d) && std::fabs(p.x - 800) < 1 && d.x > 0.99f);
        CHECK(s.t.PointAt(s.Pipe(true), 1e9f, p, y, d) && std::fabs(p.x - 5200) < 1);
        CHECK(s.t.PointAt(s.Pipe(true), 2200, p, y, d) && std::fabs(p.x - 3000) < 2);
        for (int e : s.lb.edgeLink) CHECK(e >= 0);
    }

    // ---- route tree: next-hops reach the destination and add up
    {
        RouteTree tree;
        RouteTree::Build(s.t, s.D, tree);
        std::vector<int> route;
        int reached = 0;
        for (int n = 0; n < s.t.nodes; ++n) {
            if (!std::isfinite(tree.minutes[(size_t)n])) continue;
            CHECK(tree.Follow(s.t, n, route));
            float sum = 0;
            int at = n;
            for (int l : route) {
                CHECK(s.t.links[(size_t)l].from == at);
                at = s.t.links[(size_t)l].to;
                sum += FreeMinutes(s.t, l);
            }
            CHECK(at == s.D && std::fabs(sum - tree.minutes[(size_t)n]) < 1e-3f);
            ++reached;
        }
        CHECK(reached >= 5);
        tree.Follow(s.t, s.A, route);
        CHECK(std::any_of(route.begin(), route.end(), [&](int l) { return s.t.links[(size_t)l].pipe != 0; })); // the fast way is the highway
    }

    // ---- cache: one tree per destination, LRU, dropped on a new network version
    {
        RouteCache c;
        c.capacity = 2;
        c.Ensure(s.t, s.D);
        c.Ensure(s.t, s.D);
        CHECK(c.builds == 1 && c.Size() == 1);
        c.Ensure(s.t, s.A);
        c.Get(s.t.version, s.D); // D is now the more recent
        c.Ensure(s.t, s.R2);     // evicts A
        CHECK(c.Size() == 2 && c.Get(s.t.version, s.A) == nullptr && c.Get(s.t.version, s.D) != nullptr);
        CHECK(c.Get(s.t.version + 1, s.D) == nullptr && c.Size() == 0 && c.drops > 0);
        LinkTable t2 = s.t;
        t2.version += 5;
        c.Ensure(t2, s.D);
        CHECK(c.Get(t2.version, s.D) != nullptr && c.Get(s.t.version, s.D) == nullptr);
    }

    // ---- alternatives
    {
        PathFinder pf;
        std::vector<std::vector<int>> alts;
        const int n = Alternatives(s.t, pf, s.A, s.D, [&](int l) { return FreeMinutes(s.t, l); }, alts);
        CHECK(n >= 2);
        auto usesPipe = [&](const std::vector<int>& r) { return std::any_of(r.begin(), r.end(), [&](int l) { return s.t.links[(size_t)l].pipe != 0; }); };
        CHECK(usesPipe(alts[0]) && !usesPipe(alts[1]));
        CHECK(alts[0] != alts[1]);
        CHECK(alts[0].front() == alts[1].front()); // both start on the only street out of A
        CHECK(Alternatives(s.t, pf, s.D, s.A, [&](int l) { return FreeMinutes(s.t, l); }, alts) >= 1);
    }

    // ---- logit split with smoothing
    {
        HerdSystem hs;
        hs.SetTable(&s.t);
        RoutePicker rp;
        rp.herds = &hs;
        rp.SetTable(&s.t);
        const int pipe = s.Pipe(true);
        int64_t w = 0;
        auto pick = [&]() {
            rp.BeginWindow(w++);
            RouteChoice ch[kMaxAlternatives];
            const int n = rp.Pick(s.A, s.D, ch);
            CHECK(n >= 2);
            float sum = 0;
            for (int i = 0; i < n; ++i) sum += ch[i].share;
            CHECK(std::fabs(sum - 1.0f) < 1e-4f);
            return ch[0].share;
        };
        float prev = pick(); // free roads: the highway has the larger share
        for (int i = 0; i < 3; ++i) prev = pick();
        const float free = prev;
        CHECK(free > 0.5f);
        hs.loadPrev[(size_t)pipe] = s.t.links[(size_t)pipe].cap * 0.9f; // the pipe jams: its share falls window by window and ends well below
        bool fell = true;
        for (int i = 0; i < 20; ++i) {
            const float v = pick();
            fell &= v <= prev + 1e-6f;
            prev = v;
        }
        CHECK(fell && prev < free * 0.6f);
        const float jammed = prev;
        hs.loadPrev[(size_t)pipe] = 0; // smoothed: one clear window does not flip it back, twenty bring it home
        const float one = pick();
        CHECK(one > jammed && one < free * 0.8f + 0.05f);
        bool rose = true;
        prev = one;
        for (int i = 0; i < 20; ++i) {
            const float v = pick();
            rose &= v >= prev - 1e-6f;
            prev = v;
        }
        CHECK(rose && prev > jammed && std::fabs(prev - free) < 0.05f);

        // loads follow the shares (each alternative carries what it was given): it settles instead of oscillating
        for (float demand : {1500.0f, 4000.0f, 9000.0f}) {
            RoutePicker rp2;
            HerdSystem h2;
            h2.SetTable(&s.t);
            rp2.herds = &h2;
            rp2.SetTable(&s.t);
            float share[40];
            for (int i = 0; i < 40; ++i) {
                rp2.BeginWindow(i);
                RouteChoice ch[kMaxAlternatives];
                const int n = rp2.Pick(s.A, s.D, ch);
                share[i] = ch[0].share;
                std::fill(h2.loadPrev.begin(), h2.loadPrev.end(), 0.0f);
                for (int k = 0; k < n; ++k) {
                    const RouteStore::Rec& r = h2.routes.recs[ch[k].route];
                    for (uint32_t j = 0; j < r.n; ++j) h2.loadPrev[(size_t)h2.routes.links[r.off + j]] += demand * ch[k].share / 4.0f; // vehicles per window
                }
            }
            float lo = 1, hi = 0;
            int flips = 0;
            for (int i = 20; i < 40; ++i) {
                lo = std::min(lo, share[i]), hi = std::max(hi, share[i]);
                if (i > 21 && (share[i] - share[i - 1]) * (share[i - 1] - share[i - 2]) < -1e-9f) ++flips;
            }
            CHECK(hi - lo < 0.12f);
            CHECK(flips <= 4);
        }
    }

    if (failures == 0) std::printf("sim_routes_test: all checks passed (%zu links)\n", s.t.links.size());
    return failures ? 1 : 0;
}
