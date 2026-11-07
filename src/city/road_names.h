// Road naming: turns the node / segment graph into named road chains.
//
//  * A chain is a run of segments of one class that continue through intersections (always through a plain bend, or at a
//    junction when the two arms are within ~30 degrees of straight). One name per chain, not per segment.
//  * Class decides the form: highways are numbered routes (I-5, I-12), arterials / collectors / locals are "Base Suffix",
//    quays and paths have their own suffix sets.
//  * Suffix follows geometry: median -> Boulevard, dead end -> Court / Place, curls back -> Crescent / Loop,
//    spans over / under the ground -> Bridge / Tunnel.
//  * Grid districts number their straight axis-aligned streets (N 3rd Street, E Avenue B); other districts draw from a
//    themed pool (trees, founders, nature, trades, gems) chosen per district cell.
//  * Generation is deterministic (seeded from the chain's lowest segment id), the player can override any chain, and
//    house numbers grow outward from the city centre (odd left, even right, +100 per block).
//
// Usage:  RoadNames names; names.Update(net);  names.Label(seg);  names.HouseNumber(net, seg, t, +1);
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "road_network.h"

namespace tbd {
namespace city {

enum class RoadClass : uint8_t { Local, Collector, Arterial, Highway, Quay, Path };

struct RoadName {
    std::string base;     // "Oak", "3rd", "Avenue B", or "I-5" (highways)
    std::string suffix;   // "Street", "Boulevard", ...   (empty for highways)
    std::string prefix;   // "N", "S", "E", "W" (grid districts only)
    int route = 0;        // highways: the route number
    bool player = false;  // set by the player: kept across rebuilds
    std::string Full() const {
        std::string s = prefix.empty() ? base : prefix + " " + base;
        return suffix.empty() ? s : s + " " + suffix;
    }
};

struct RoadChain {
    std::vector<int> segs; // in walk order, from the end nearest the city centre outward
    std::vector<char> rev; // rev[i]: segs[i] is walked b -> a
    float length = 0;
    RoadClass cls = RoadClass::Local;
    RoadName name;
};

namespace names_detail {

inline uint32_t Hash(uint32_t x) {
    x ^= x >> 16, x *= 0x7feb352dU, x ^= x >> 15, x *= 0x846ca68bU, x ^= x >> 16;
    return x;
}
inline std::string Ordinal(int n) {
    const int m = n % 100;
    const char* s = (m >= 11 && m <= 13) ? "th" : n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
    return std::to_string(n) + s;
}
inline std::string Letter(int i) { // A..Z, then AA, AB, ...
    std::string s;
    for (++i; i > 0; i = (i - 1) / 26) s.insert(s.begin(), (char)('A' + (i - 1) % 26));
    return s;
}

inline const std::vector<const char*>& Pool(int theme) {
    static const std::vector<const char*> trees = {"Oak", "Elm", "Maple", "Cedar", "Birch", "Willow", "Pine", "Aspen", "Hickory", "Linden", "Sycamore", "Poplar", "Chestnut", "Walnut", "Spruce", "Magnolia", "Juniper", "Alder", "Hazel", "Laurel"};
    static const std::vector<const char*> founders = {"Adams", "Baker", "Carter", "Dalton", "Everett", "Fulton", "Grant", "Hale", "Irving", "Jasper", "Kendall", "Lowell", "Mercer", "Norton", "Osborne", "Prescott", "Quincy", "Rowan", "Sutton", "Thornton", "Upton", "Vance", "Whitman", "Yates"};
    static const std::vector<const char*> nature = {"Ridge", "Hillcrest", "Riverside", "Meadow", "Brook", "Summit", "Valley", "Lakeview", "Stone", "Fern", "Heron", "Falcon", "Harbor", "Cliff", "Prairie", "Glen", "Sunset", "Dawn", "Orchard", "Highland"};
    static const std::vector<const char*> trades = {"Foundry", "Market", "Mill", "Cooper", "Forge", "Tannery", "Depot", "Quarry", "Anvil", "Weaver", "Brewer", "Mason", "Cannery", "Granary", "Dock", "Rail", "Commerce", "Freight", "Kiln", "Smith"};
    static const std::vector<const char*> gems = {"Amber", "Garnet", "Jade", "Opal", "Onyx", "Topaz", "Pearl", "Coral", "Ruby", "Sapphire", "Emerald", "Agate", "Beryl", "Jasper", "Quartz", "Zircon"};
    switch (theme % 5) {
    case 0: return trees;
    case 1: return founders;
    case 2: return nature;
    case 3: return trades;
    default: return gems;
    }
}

inline RoadClass ClassOf(Cat c) {
    switch (c) {
    case Cat::Small: return RoadClass::Local;
    case Cat::Medium: return RoadClass::Collector;
    case Cat::Large: return RoadClass::Arterial;
    case Cat::Highway: return RoadClass::Highway;
    case Cat::Quay: return RoadClass::Quay;
    default: return RoadClass::Path;
    }
}

} // namespace names_detail

class RoadNames {
public:
    float cellSize = 700.0f; // a district: one theme, grid or organic
    float block = 120.0f;    // grid spacing used to number streets
    V2 centre{0, 0};         // the city centre: baselines for N/S/E/W and where house numbers start

    // Rebuild chains and names when the network changed.
    void Update(const Network& net) {
        if (builtVersion_ == net.version) return;
        builtVersion_ = net.version;
        Build(net);
    }
    void Invalidate() { builtVersion_ = 0; }

    const RoadChain* ChainOf(int seg) const {
        if (seg < 0 || seg >= (int)segChain_.size() || segChain_[seg] < 0) return nullptr;
        return &chains_[segChain_[seg]];
    }
    // "Oak Street", "I-5", "N 3rd Street"; empty for rail and dead ids.
    std::string Label(int seg) const {
        const RoadChain* c = ChainOf(seg);
        return c ? c->name.Full() : std::string();
    }
    // "Street" / "Highway" / ... for HUD subtitles.
    static const char* ClassLabel(RoadClass c) {
        static const char* k[] = {"Local street", "Collector", "Arterial", "Highway", "Quay", "Path"};
        return k[(int)c];
    }
    // Player override for the whole chain the segment belongs to (empty text clears it back to generated).
    void Rename(const Network& net, int seg, const std::string& text) {
        if (text.empty()) overrides_.erase(seg); else overrides_[seg] = text;
        if (const RoadChain* c = ChainOf(seg)) { // drop other keys on the same chain so one entry rules it
            for (int s : c->segs)
                if (s != seg) overrides_.erase(s);
        }
        Invalidate();
        Update(net);
    }
    // House number at parameter t of a segment (a -> b); side +1 = left of a -> b, -1 = right.
    int HouseNumber(const Network& net, int seg, float t, int side) const {
        const RoadChain* c = ChainOf(seg);
        if (!c) return 0;
        float dist = 0;
        for (size_t i = 0; i < c->segs.size(); ++i) {
            const float len = net.Curve(c->segs[i]).Length();
            if (c->segs[i] == seg) {
                const float along = c->rev[i] ? (1 - t) : t;
                dist += len * along;
                if (c->rev[i]) side = -side; // walking the chain the other way swaps left and right
                break;
            }
            dist += len;
        }
        const int blockNo = (int)(dist / 100.0f);
        const int within = (int)(std::fmod(dist, 100.0f) / 8.0f); // ~12 plots per block
        return blockNo * 100 + within * 2 + (side > 0 ? 1 : 2);
    }

    int ChainCount() const { return (int)chains_.size(); }

private:
    struct Link { int seg = -1, end = 0; };

    uint64_t builtVersion_ = 0;
    std::vector<RoadChain> chains_;
    std::vector<int> segChain_;
    std::map<int, std::string> overrides_;

    static V2 Out(const Network& net, int s, int end) { // direction leaving the node at this end of segment s
        const Cubic c = net.Curve(s);
        return end == 0 ? c.Tan(0) : c.Tan(1) * -1.0f;
    }

    void Build(const Network& net) {
        using namespace names_detail;
        const int ns = (int)net.segs.size();
        std::vector<std::array<Link, 2>> link(ns);
        auto live = [&](int s) { return net.segs[s].alive && !net.IsRailSeg(s); };

        // 1. link continuing arms at every node
        for (int n = 0; n < (int)net.nodes.size(); ++n) {
            if (!net.nodes[n].alive) continue;
            struct Arm { int seg, end; RoadClass cls; V2 d; };
            std::vector<Arm> arms;
            for (int s : net.nodes[n].segs) {
                if (!live(s)) continue;
                const Segment& g = net.segs[s];
                const int end = g.a == n ? 0 : 1;
                arms.push_back({s, end, ClassOf(net.Prof(s).cat), Out(net, s, end)});
            }
            struct Pair { float dot; int i, j; };
            std::vector<Pair> pairs;
            for (int i = 0; i < (int)arms.size(); ++i)
                for (int j = i + 1; j < (int)arms.size(); ++j) {
                    if (arms[i].cls != arms[j].cls || arms[i].seg == arms[j].seg) continue;
                    const float d = Dot2(arms[i].d, arms[j].d);
                    if (arms.size() == 2 || d < -0.866f) pairs.push_back({d, i, j}); // plain bend always; junction only if near straight
                }
            std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.dot < b.dot; });
            std::vector<char> used(arms.size(), 0);
            for (const Pair& p : pairs) {
                if (used[p.i] || used[p.j]) continue;
                used[p.i] = used[p.j] = 1;
                link[arms[p.i].seg][arms[p.i].end] = {arms[p.j].seg, arms[p.j].end};
                link[arms[p.j].seg][arms[p.j].end] = {arms[p.i].seg, arms[p.i].end};
            }
        }

        // 2. walk chains: from open ends first, then whatever cycles are left
        chains_.clear();
        segChain_.assign(ns, -1);
        std::vector<char> seen(ns, 0);
        auto walk = [&](int start, int startEnd) { // startEnd: the end we enter by (the one that is open / arbitrary)
            RoadChain ch;
            int s = start, enter = startEnd;
            while (s >= 0 && !seen[s]) {
                seen[s] = 1;
                ch.segs.push_back(s);
                ch.rev.push_back(enter == 1); // entering by b means walking b -> a
                const int exitEnd = 1 - enter;
                const Link nx = link[s][exitEnd];
                s = nx.seg, enter = nx.end;
            }
            return ch;
        };
        for (int s = 0; s < ns; ++s) {
            if (!live(s) || seen[s]) continue;
            for (int end = 0; end < 2 && !seen[s]; ++end)
                if (link[s][end].seg < 0) { chains_.push_back(walk(s, end)); }
        }
        for (int s = 0; s < ns; ++s)
            if (live(s) && !seen[s]) chains_.push_back(walk(s, 0)); // a closed loop

        // 3. orient each chain outward from the centre, measure, name
        std::set<std::string> taken;
        // overrides claim their names first so generated ones never collide
        for (auto& o : overrides_) taken.insert(o.second);
        // name highways first so route numbers are stable, then big roads, then small
        std::vector<int> order(chains_.size());
        for (int i = 0; i < (int)order.size(); ++i) order[i] = i;
        for (RoadChain& ch : chains_) {
            ch.cls = ClassOf(net.Prof(ch.segs[0]).cat);
            ch.length = 0;
            for (int s : ch.segs) ch.length += net.Curve(s).Length();
            const V2 first = XZ(net.nodes[ChainStartNode(net, ch, true)].p), last = XZ(net.nodes[ChainStartNode(net, ch, false)].p);
            if (Len(last - centre) < Len(first - centre)) { // reverse so the chain starts at the centre end
                std::reverse(ch.segs.begin(), ch.segs.end());
                std::reverse(ch.rev.begin(), ch.rev.end());
                for (char& r : ch.rev) r = !r;
            }
        }
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return (int)chains_[a].cls > (int)chains_[b].cls; });
        for (int i : order) NameChain(net, chains_[i], taken);
        for (int i = 0; i < (int)chains_.size(); ++i)
            for (int s : chains_[i].segs) segChain_[s] = i;
        // player overrides win (applied last because they are keyed by segment id)
        for (auto& o : overrides_) {
            const RoadChain* c = ChainOf(o.first);
            if (!c) continue;
            RoadChain& m = chains_[segChain_[o.first]];
            m.name = RoadName{};
            m.name.base = o.second;
            m.name.player = true;
        }
    }

    static int ChainStartNode(const Network& net, const RoadChain& ch, bool first) {
        const int i = first ? 0 : (int)ch.segs.size() - 1;
        const Segment& g = net.segs[ch.segs[i]];
        const bool fromA = first ? !ch.rev[i] : ch.rev[i]; // first: node we walk from; last: node we walk to
        return fromA ? g.a : g.b;
    }

    void NameChain(const Network& net, RoadChain& ch, std::set<std::string>& taken) {
        using namespace names_detail;
        const int nStart = ChainStartNode(net, ch, true), nEnd = ChainStartNode(net, ch, false);
        const V2 a = XZ(net.nodes[nStart].p), b = XZ(net.nodes[nEnd].p);
        const V2 mid = (a + b) * 0.5f;
        const V2 dir = Norm(b - a);
        const int cx = (int)std::floor(mid.x / cellSize), cz = (int)std::floor(mid.z / cellSize);
        const uint32_t cell = Hash((uint32_t)(cx * 73856093) ^ (uint32_t)(cz * 19349663));
        uint32_t seed = Hash((uint32_t)*std::min_element(ch.segs.begin(), ch.segs.end()) + 0x9e3779b9u);

        // geometry
        float turn = 0;
        for (int s : ch.segs) {
            const Cubic c = net.Curve(s);
            turn += std::acos(std::clamp(Dot2(c.Tan(0), c.Tan(1)), -1.0f, 1.0f));
        }
        const float chord = Len(b - a);
        const bool straight = turn < 0.35f && chord > ch.length * 0.95f;
        const bool loops = turn > 2.1f || (chord < ch.length * 0.45f && ch.length > 60);
        const bool nsAxis = std::fabs(dir.z) > std::fabs(dir.x); // runs mostly north-south
        const bool aligned = std::fabs(dir.x) > 0.94f || std::fabs(dir.z) > 0.94f; // within ~20 deg of an axis
        bool deadEnd = false;
        for (int n : {nStart, nEnd}) {
            int deg = 0;
            for (int s : net.nodes[n].segs) deg += net.segs[s].alive && !net.IsRailSeg(s);
            if (deg <= 1) deadEnd = true;
        }
        bool median = false;
        for (int s : ch.segs)
            for (const Strip& st : net.Prof(s).strips)
                if (st.kind == Kind::Median) median = true;
        bool spanning = false, underground = false;
        if (net.ground && ch.cls != RoadClass::Highway) {
            spanning = true;
            for (int s : ch.segs)
                if (net.Follows(net.segs[s])) spanning = false;
            if (spanning) {
                const V2 m = (a + b) * 0.5f;
                underground = net.NodeY(nStart) < net.ground(m.x, m.z) - 3.0f && net.NodeY(nEnd) < net.ground(m.x, m.z) - 3.0f;
            }
        }

        RoadName rn;
        auto unique = [&](const RoadName& r) { return !taken.count(r.Full()); };

        if (ch.cls == RoadClass::Highway) {
            // N-S routes odd, E-W routes even; longer, more central routes get lower numbers via the hash walk below
            const int parity = nsAxis ? 1 : 0;
            int num = 0;
            for (int tries = 0; tries < 200; ++tries) {
                num = (int)(Hash(seed + tries) % 48) + 1;
                num = (num / 2) * 2 + parity;
                if (num < 1) num = 1;
                RoadName t;
                t.base = "I-" + std::to_string(num);
                t.route = num;
                if (unique(t) || tries == 199) { rn = t; break; }
            }
            if (ch.length < 250) { // a short stub is a connector
                rn.suffix = "Connector";
            }
        } else {
            std::string suffix;
            switch (ch.cls) {
            case RoadClass::Arterial: {
                static const char* k[] = {"Boulevard", "Avenue", "Parkway", "Pike", "Highway"};
                suffix = median ? "Boulevard" : k[1 + Hash(seed) % 4];
                break;
            }
            case RoadClass::Collector: {
                static const char* k[] = {"Avenue", "Road", "Drive", "Way"};
                suffix = median ? "Boulevard" : k[Hash(seed) % 4];
                break;
            }
            case RoadClass::Local: {
                static const char* k[] = {"Street", "Lane", "Street", "Street"};
                suffix = k[Hash(seed) % 4];
                break;
            }
            case RoadClass::Quay: {
                static const char* k[] = {"Quay", "Wharf", "Embankment", "Esplanade"};
                suffix = k[Hash(seed) % 4];
                break;
            }
            default: {
                static const char* k[] = {"Walk", "Trail", "Greenway", "Promenade"};
                suffix = k[Hash(seed) % 4];
                break;
            }
            }
            if (ch.cls == RoadClass::Local || ch.cls == RoadClass::Collector) {
                if (deadEnd && ch.length < 160) suffix = (Hash(seed >> 3) & 1) ? "Court" : "Place";
                else if (loops) suffix = (Hash(seed >> 3) & 1) ? "Crescent" : "Loop";
            }
            if (spanning) suffix = underground ? "Tunnel" : "Bridge";

            const bool gridDistrict = (cell % 3) == 0;
            const bool numbered = gridDistrict && straight && aligned && !spanning &&
                                  (ch.cls == RoadClass::Local || ch.cls == RoadClass::Collector) && suffix != "Court" && suffix != "Place";
            if (numbered) {
                // E-W run: numbered Street by distance from the centre baseline; N-S run: lettered Avenue
                const float off = nsAxis ? mid.x - centre.x : mid.z - centre.z;
                const int idx = (int)std::floor(std::fabs(off) / block);
                RoadName t;
                if (nsAxis) { t.base = "Avenue " + Letter(idx); t.suffix = ""; t.prefix = idx > 0 ? (off < 0 ? "W" : "E") : ""; }
                else { t.base = Ordinal(idx + 1); t.suffix = "Street"; t.prefix = idx > 0 ? (off < 0 ? "N" : "S") : ""; }
                if (unique(t)) { rn = t; taken.insert(rn.Full()); ch.name = rn; return; }
            }
            const auto& pool = Pool((int)(cell >> 8)); // one theme per district
            for (int tries = 0; tries < 400; ++tries) {
                RoadName t;
                t.base = pool[(Hash(seed + tries) >> 4) % pool.size()];
                if (tries >= (int)pool.size()) { // pool exhausted around here: borrow any theme
                    const auto& other = Pool((int)(Hash(seed + tries) >> 9));
                    t.base = other[(Hash(seed * 7 + tries) >> 4) % other.size()];
                }
                t.suffix = suffix;
                if (tries >= 4 * (int)pool.size()) t.base += " " + std::to_string(tries); // last resort
                if (unique(t)) { rn = t; break; }
                if (tries == 399) rn = t;
            }
        }
        taken.insert(rn.Full());
        ch.name = rn;
    }
};

} // namespace city
} // namespace tbd
