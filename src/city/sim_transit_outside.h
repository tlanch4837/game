// Phase 5 outside connections (road, rail, sea, air): who and what comes through each one a day. Header only,
// headless (tests\sim_transit_test).
//   tourists    made here: by the connection kind, the city's shops (attraction) and size; they become trip links from
//               the connection to the shopping zones (the host feeds them to TransitSim like any other trip)
//   immigrants  and emigrants: Phase 1 decides how many (sim_migration.h); this splits the day's count over the
//               connections by kind (the arrival points P1 can read: SimTransitArrivalWeights)
//   cargo       Phase 6 decides imports and exports (sim_goods_world.h ledger); this splits them over the connections by
//               freight capacity, and exposes the capacity (SimTransitCargoCapacity) for P6 to read
// Integer and largest-remainder splits: deterministic, and the parts always add up to the whole.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace tbd {
namespace city {

struct OutsideIn {
    uint64_t id = 0;
    uint8_t kind = 0;     // transit.h OutKind: 0 road, 1 rail, 2 sea, 3 air
    bool linked = false;  // reaches the city's network (TransitWorld Outside::linked)
    bool terminal = false;// rail: a train station, sea: a passenger harbour, air: an airfield (passengers can land)
    float x = 0, z = 0;
};
struct OutsideDayIn {
    int64_t residents = 0, shopJobs = 0;
    int64_t immigrants = 0, emigrants = 0;   // P1's yesterday
    int64_t cargoImport = 0, cargoExport = 0; // P6's yesterday (0.1 t units)
};
struct OutsideFlow {
    uint64_t id = 0;
    uint8_t kind = 0;
    float x = 0, z = 0;
    int32_t tourists = 0, immigrants = 0, emigrants = 0;
    int64_t cargoIn = 0, cargoOut = 0, cargoCap = 0; // 0.1 t a day
};

inline int64_t OutsideCargoCap(int kind) { static const int64_t k[4] = {6000, 25000, 60000, 800}; return k[std::clamp(kind, 0, 3)]; }
inline float OutsidePeopleWeight(const OutsideIn& o) {
    static const float k[4] = {1.0f, 1.6f, 0.6f, 1.2f};
    if (!o.linked) return 0.0f;
    return o.kind == 0 || o.terminal ? k[std::clamp((int)o.kind, 0, 3)] : 0.0f;
}
inline int32_t OutsideTouristRate(int kind) { static const int32_t k[4] = {30, 90, 60, 150}; return k[std::clamp(kind, 0, 3)]; }

// total split by weights, largest remainder (ties to the lower index). All weights 0: nothing is split.
inline void Apportion(int64_t total, const std::vector<double>& w, std::vector<int64_t>& out) {
    out.assign(w.size(), 0);
    double ws = 0;
    for (double v : w) ws += std::max(0.0, v);
    if (ws <= 0 || total <= 0) return;
    int64_t given = 0;
    std::vector<double> rem(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
        const double v = (double)total * std::max(0.0, w[i]) / ws;
        out[i] = (int64_t)v, rem[i] = v - (double)out[i], given += out[i];
    }
    while (given < total) {
        size_t b = 0;
        for (size_t i = 1; i < w.size(); ++i) if (rem[i] > rem[b]) b = i;
        ++out[b], rem[b] = -1.0, ++given;
    }
}

class OutsideWorld {
public:
    std::vector<OutsideFlow> flows;        // yesterday's, one per connection (input order)
    std::vector<int32_t> touristDays;      // tourists a day, oldest first (30 days)
    int64_t touristsTotal = 0, immigrantsTotal = 0, emigrantsTotal = 0, cargoTotal = 0;

    void Day(const std::vector<OutsideIn>& conns, const OutsideDayIn& d) {
        flows.assign(conns.size(), OutsideFlow());
        std::vector<double> wp(conns.size()), wc(conns.size());
        const double attract = std::clamp((double)d.shopJobs / 1500.0, 0.0, 2.0) * (1.0 + (double)d.residents / 20000.0);
        int32_t tourists = 0;
        for (size_t i = 0; i < conns.size(); ++i) {
            const OutsideIn& o = conns[i];
            OutsideFlow& f = flows[i];
            f.id = o.id, f.kind = o.kind, f.x = o.x, f.z = o.z;
            wp[i] = OutsidePeopleWeight(o);
            f.cargoCap = o.linked ? OutsideCargoCap(o.kind) : 0, wc[i] = (double)f.cargoCap;
            f.tourists = wp[i] > 0 ? (int32_t)((double)OutsideTouristRate(o.kind) * attract) : 0;
            tourists += f.tourists;
        }
        std::vector<int64_t> part;
        Apportion(d.immigrants, wp, part);
        for (size_t i = 0; i < flows.size(); ++i) flows[i].immigrants = (int32_t)part[i];
        Apportion(d.emigrants, wp, part);
        for (size_t i = 0; i < flows.size(); ++i) flows[i].emigrants = (int32_t)part[i];
        Apportion(d.cargoImport, wc, part);
        for (size_t i = 0; i < flows.size(); ++i) flows[i].cargoIn = part[i];
        Apportion(d.cargoExport, wc, part);
        for (size_t i = 0; i < flows.size(); ++i) flows[i].cargoOut = part[i];
        touristDays.push_back(tourists);
        if (touristDays.size() > 30) touristDays.erase(touristDays.begin());
        touristsTotal += tourists, immigrantsTotal += d.immigrants, emigrantsTotal += d.emigrants, cargoTotal += d.cargoImport + d.cargoExport;
    }
    void Clear() { *this = OutsideWorld(); }
};

} // namespace city
} // namespace tbd
