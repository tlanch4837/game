// Phase 5 transit network for the sim: passenger lines as ordered stops with distances along the line, headways from
// the vehicle count, access lists (the nearest stop of each line within walking range of a point) and the best path
// between two access lists (one line, or two with one transfer). Header only, headless (tests\sim_transit_test).
//
// The host builds LineInputs from transit.h's TransitWorld (passenger routes: bus, tram, train, boat, plane, subway);
// nothing here knows the game. Minutes are game minutes; distances metres.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tbd {
namespace city {

struct LineInput {
    uint64_t id = 0;            // the route's id (TransitWorld Route::id)
    uint8_t mode = 0;           // transit.h TMode
    bool closed = false;        // a loop (else out and back)
    std::vector<float> x, z;    // stop positions, line order
    std::vector<float> legLen;  // metres stop i -> i+1 (closed: the last leg returns to stop 0)
    int vehicles = 1;
};

struct TransitNetParams {
    float walkRadius = 500.0f;     // metres from a point to a stop
    float walkMetresPerMin = 80.0f;
    float dwellMin = 0.5f;         // per stop passed
    float transferPenaltyMin = 3.0f;
    float transferRadius = 160.0f; // metres between two lines' stops to change
    float serviceHours = 18.0f;    // the lines run this many hours a day
    float maxWaitMin = 30.0f;
};

// metres per minute and seats + standing per vehicle, by transit.h TMode (Bus, Tram, Train, Boat, Plane, Subway)
inline float TransitSpeedMpm(int mode) { static const float k[] = {420, 460, 1300, 380, 3600, 900}; return k[std::clamp(mode, 0, 5)]; }
inline int TransitVehicleCap(int mode) { static const int k[] = {60, 140, 600, 250, 150, 520}; return k[std::clamp(mode, 0, 5)]; }

struct TransitLine {
    uint64_t id = 0;
    uint8_t mode = 0;
    bool closed = false;
    int vehicles = 1;
    std::vector<float> x, z, cum; // cum[i]: metres from stop 0 to stop i along the line
    float total = 0.0f;           // closed: the whole loop; open: stop 0 to the last stop
    float cycleMin = 0.0f, headwayMin = 0.0f;
    int64_t capPerDir = 0;        // riders a day past one segment in one direction
    int Stops() const { return (int)x.size(); }
    int Segs() const { return closed ? Stops() : 2 * (Stops() - 1); } // open: forward segs then backward segs
};

struct TransitAccess {
    int16_t line = -1, stop = -1;
    float walkMin = 0.0f;
};

// A trip on the lines: legs 1 or 2; leg k rides line[k] from stop on[k] to off[k].
struct TransitPath {
    float minutes = 0.0f;
    uint8_t legs = 0;
    int16_t line[2] = {-1, -1}, on[2] = {-1, -1}, off[2] = {-1, -1};
};

class TransitNet {
public:
    TransitNetParams p;
    std::vector<TransitLine> lines;
    struct Transfer { int16_t a, b; int16_t sa, sb; float walkMin; };
    std::vector<std::vector<Transfer>> transfers; // [lineA * L + lineB]: places to change from A to B
    uint64_t version = 0;

    void Build(const std::vector<LineInput>& in) {
        lines.clear();
        for (const LineInput& li : in) {
            const int n = (int)li.x.size();
            if (n < 2 || li.z.size() != li.x.size()) continue;
            TransitLine L;
            L.id = li.id, L.mode = li.mode, L.closed = li.closed, L.vehicles = std::max(1, li.vehicles);
            L.x = li.x, L.z = li.z, L.cum.assign((size_t)n, 0.0f);
            for (int i = 1; i < n; ++i) L.cum[(size_t)i] = L.cum[(size_t)i - 1] + Leg(li, i - 1);
            L.total = L.cum[(size_t)n - 1] + (li.closed ? Leg(li, n - 1) : 0.0f);
            const float run = (li.closed ? L.total : 2.0f * L.total) / TransitSpeedMpm(li.mode);
            L.cycleMin = run + p.dwellMin * (float)(li.closed ? n : 2 * (n - 1));
            L.headwayMin = std::max(1.0f, L.cycleMin / (float)L.vehicles);
            const double runsPerDay = p.serviceHours * 60.0 / std::max(1.0f, L.cycleMin);
            L.capPerDir = (int64_t)std::llround(runsPerDay * L.vehicles * TransitVehicleCap(li.mode));
            lines.push_back(std::move(L));
        }
        BuildTransfers();
        ++version;
    }
    // The nearest stop of each line within walking range of (x, z), in line order.
    void AccessAt(float x, float z, std::vector<TransitAccess>& out) const {
        out.clear();
        for (size_t l = 0; l < lines.size(); ++l) {
            const TransitLine& L = lines[l];
            int best = -1;
            float bd = p.walkRadius * p.walkRadius;
            for (int s = 0; s < L.Stops(); ++s) {
                const float dx = L.x[(size_t)s] - x, dz = L.z[(size_t)s] - z, d = dx * dx + dz * dz;
                if (d < bd) bd = d, best = s;
            }
            if (best >= 0) out.push_back({(int16_t)l, (int16_t)best, std::sqrt(bd) * 1.25f / p.walkMetresPerMin});
        }
    }
    // A stable fingerprint of an access list and the lines it touches (headways included): equal = same answers.
    uint32_t AccessSig(const std::vector<TransitAccess>& a) const {
        uint32_t h = 2166136261u;
        auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };
        for (const TransitAccess& e : a) {
            mix((uint32_t)e.line), mix((uint32_t)e.stop), mix((uint32_t)std::lround(e.walkMin * 10.0f));
            mix((uint32_t)(lines[(size_t)e.line].id * 2654435761u)), mix((uint32_t)std::lround(lines[(size_t)e.line].headwayMin * 10.0f));
        }
        return h;
    }
    float WaitMin(int line) const { return std::min(p.maxWaitMin, lines[(size_t)line].headwayMin * 0.5f); }
    // Metres riding line l from stop a to stop b (open lines: the direction follows; closed: forward round the loop).
    float RideMetres(int l, int a, int b) const {
        const TransitLine& L = lines[(size_t)l];
        const float d = L.cum[(size_t)b] - L.cum[(size_t)a];
        return L.closed ? (d >= 0.0f ? d : d + L.total) : std::fabs(d);
    }
    int StopsPassed(int l, int a, int b) const {
        const TransitLine& L = lines[(size_t)l];
        const int d = b - a;
        return L.closed ? (d >= 0 ? d : d + L.Stops()) : std::abs(d);
    }
    float RideMin(int l, int a, int b) const {
        return RideMetres(l, a, b) / TransitSpeedMpm(lines[(size_t)l].mode) + p.dwellMin * (float)std::max(0, StopsPassed(l, a, b) - 1);
    }

    bool Best(const std::vector<TransitAccess>& A, const std::vector<TransitAccess>& B, TransitPath& out) const;

private:
    static float Leg(const LineInput& li, int i) {
        if ((size_t)i < li.legLen.size() && li.legLen[(size_t)i] > 0.0f) return li.legLen[(size_t)i];
        const size_t j = ((size_t)i + 1) % li.x.size();
        return std::hypot(li.x[j] - li.x[(size_t)i], li.z[j] - li.z[(size_t)i]) * 1.2f;
    }
    void BuildTransfers();
};

} // namespace city
} // namespace tbd

#include "sim_transit_path.h"
