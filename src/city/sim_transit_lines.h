// Phase 5 line book: the player's per-line settings (vehicle count) and each day's riders, fares and upkeep per line.
// Header only, headless (tests\sim_transit_test). Money in cents (the budget's unit).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "sim_transit_ridership.h"

namespace tbd {
namespace city {

inline int64_t LineUpkeepPerVehicleDay(int mode) { static const int64_t k[6] = {8000, 14000, 40000, 30000, 80000, 35000}; return k[std::clamp(mode, 0, 5)]; }

struct LineSetting {
    uint64_t id = 0;
    int32_t vehicles = 0; // 0: as many as the line's length asks for (transit.h BuildLegs)
};
struct LineDay {
    uint64_t id = 0;
    int32_t mode = 0, vehicles = 0;
    int64_t riders = 0, served = 0;   // wanting to ride, and carried (the busiest segment's capacity caps it)
    int64_t fares = 0, upkeep = 0;    // cents
};

class LineBook {
public:
    std::vector<LineSetting> settings; // sorted by id
    std::vector<LineDay> last;         // yesterday, net line order
    std::vector<int64_t> ridersDays;   // carried a day, city-wide, oldest first (30 days)
    int64_t faresTotal = 0, upkeepTotal = 0;

    int32_t Setting(uint64_t id) const {
        const auto it = std::lower_bound(settings.begin(), settings.end(), id, [](const LineSetting& s, uint64_t k) { return s.id < k; });
        return it != settings.end() && it->id == id ? it->vehicles : 0;
    }
    void Set(uint64_t id, int32_t vehicles) {
        auto it = std::lower_bound(settings.begin(), settings.end(), id, [](const LineSetting& s, uint64_t k) { return s.id < k; });
        if (it != settings.end() && it->id == id) it->vehicles = vehicles;
        else settings.insert(it, LineSetting{id, vehicles});
    }
    // Drop the settings of lines that no longer exist.
    void Keep(const TransitNet& net) {
        settings.erase(std::remove_if(settings.begin(), settings.end(), [&](const LineSetting& s) {
                           for (const TransitLine& L : net.lines) if (L.id == s.id) return false;
                           return true;
                       }), settings.end());
    }
    // The day's book from the ledger: fare per boarding (currency, policy included).
    void Day(const TransitNet& net, const RidershipLedger& led, float fare) {
        last.assign(net.lines.size(), LineDay());
        int64_t carried = 0;
        for (size_t l = 0; l < net.lines.size() && l < led.lines.size(); ++l) {
            const TransitLine& L = net.lines[l];
            LineDay& d = last[l];
            d.id = L.id, d.mode = L.mode, d.vehicles = L.vehicles;
            d.riders = led.lines[l].riders;
            d.served = (int64_t)std::floor((double)d.riders * RidershipLedger::Served(L, led.lines[l]));
            d.fares = (int64_t)std::llround((double)d.served * fare * 100.0);
            d.upkeep = LineUpkeepPerVehicleDay(L.mode) * L.vehicles;
            carried += d.served, faresTotal += d.fares, upkeepTotal += d.upkeep;
        }
        ridersDays.push_back(carried);
        if (ridersDays.size() > 30) ridersDays.erase(ridersDays.begin());
    }
    int64_t FaresYesterday() const { int64_t s = 0; for (const LineDay& d : last) s += d.fares; return s; }
    int64_t UpkeepYesterday() const { int64_t s = 0; for (const LineDay& d : last) s += d.upkeep; return s; }
    void Clear() { *this = LineBook(); }
};

} // namespace city
} // namespace tbd
