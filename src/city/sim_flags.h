// Per-building problem flags of the city sim (sim gameplay pass, phase A). One byte per zoned lot, zones.lots order.
// Header only, headless (tests\sim_utilities_test.cpp). The notification icons and the info card read these, nothing else
// decides them:
//
//   NoPower / NoWater / NoSewage  the utilities solve (utilities.h, via sim_coverage.h LotUtil) could not serve the lot
//   NoWorkers    a workplace whose cell has 40 % or more of its jobs unfilled
//   NoCustomers  a shop while commercial demand is strongly negative (more shops than the residents spend in)
//   Abandoned    a level 1 lot that has been in bad conditions for over half of its patience: sim_growth.h abandons
//                (demolishes) it soon unless things improve
//
// Efficiency: a cell without power works at 50 %, without water at 70 % (both: 35 %); sim_economy.h scales its tax by it.
#pragma once

#include <cstdint>
#include <vector>

#include "sim_cells.h"
#include "sim_coverage.h"
#include "sim_growth.h"

namespace tbd {
namespace city {

enum SimFlag : uint8_t {
    kFlagNoPower = 1 << 0,
    kFlagNoWater = 1 << 1,
    kFlagNoSewage = 1 << 2,
    kFlagNoWorkers = 1 << 3,
    kFlagNoCustomers = 1 << 4,
    kFlagAbandoned = 1 << 5,
    kFlagCount = 6,
};
inline const char* SimFlagText(int bit) {
    static const char* const k[kFlagCount] = {"No electricity", "No water", "No sewage", "Not enough workers", "Not enough customers", "Being abandoned"};
    return bit >= 0 && bit < kFlagCount ? k[bit] : "";
}

inline int64_t CellEfficiencyPermille(const CellSim& c) { return (c.powerOk ? 1000 : 500) * (c.waterOk ? 1000 : 700) / 1000; }

// lotUtil: sim_coverage.h LotUtil per lot (empty = all fine). commercialDemand: SimGrowth::demand[Commercial].
inline void ComputeSimFlags(const ZoneGrid& z, const CitySim& s, const SimGrowth& g, const SimTuning& t, const std::vector<uint8_t>& lotUtil,
                            std::vector<uint8_t>& out) {
    out.assign(z.lots.size(), 0);
    const auto& types = ZoneTypes();
    const float abandonAt = SimGrowth::DownMinutes(t) * 0.5f;
    const double comm = g.demand[(int)ZoneCat::Commercial];
    for (size_t i = 0; i < z.lots.size(); ++i) {
        const Lot& l = z.lots[i];
        if (!l.alive || !l.Built() || l.type < 0 || l.type >= (int)types.size()) continue;
        uint8_t f = 0;
        if (i < lotUtil.size()) {
            const uint8_t u = lotUtil[i];
            if (!(u & kLotPower)) f |= kFlagNoPower;
            if (!(u & kLotWater)) f |= kFlagNoWater;
            if (!(u & kLotSewage)) f |= kFlagNoSewage;
        }
        const ZoneCat cat = types[l.type].cat;
        const int c = s.cells.Find(CellKeyAt(l.c.x, l.c.z));
        if (c >= 0 && cat != ZoneCat::Residential) {
            const CellSim& cs = s.sim[(size_t)c];
            const uint32_t cap = (uint32_t)cs.jobCap[0] + cs.jobCap[1] + cs.jobCap[2];
            const uint32_t vac = (uint32_t)cs.vacancies[0] + cs.vacancies[1] + cs.vacancies[2];
            if (cap > 0 && vac * 10 >= cap * 4) f |= kFlagNoWorkers;
        }
        if (cat == ZoneCat::Commercial && comm < -0.3) f |= kFlagNoCustomers;
        if (l.level <= 1 && g.BadMinutes(l.id) >= abandonAt) f |= kFlagAbandoned;
        out[i] = f;
    }
}
} // namespace city
} // namespace tbd
