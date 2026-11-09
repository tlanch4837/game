// Phase 1 host glue (CS2 sim roadmap Phase 1): the citizen records into the aggregate tier, and the registry rows the
// citizen sim needs. Header only, headless (tests\sim_cells_test, tests\sim_person_test).
//
//   LifeFillRows     a lot's homes, job slots, CitySim cell and hood into its BuildingRegistry row (P0 leaves those to the
//                    phases): homes and jobs from LotCapacity once the lot is built, split over education levels.
//   LifeJobKind      sim_cells.h JobKind of a lot's zone type (the LifeJobKindFn for LifeSim::SyncBuildings).
//   LifeApplyCells   every CitySim cell's people from LifeWorld::cells, and the supercell links from the commute matrix
//                    (CitySim::externalPeople): the counters are a view of the records, never the other way round.
//   LifePinSaved     an old save's named people (Q records, by home lot) onto the bootstrapped records: the k-th Q person
//                    of a lot is that building's TenantOf(k), pinned.
#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "sim_cells.h"
#include "sim_life.h"

namespace tbd {
namespace city {

inline int LifeJobKind(int zoneType) {
    const auto& t = ZoneTypes();
    if (zoneType < 0 || zoneType >= (int)t.size()) return -1;
    switch (t[(size_t)zoneType].cat) {
    case ZoneCat::Industrial: return kJobIndustrial;
    case ZoneCat::Commercial: return kJobCommercial;
    case ZoneCat::Office: return kJobOffice;
    default: return -1;
    }
}
// SplitJobSlots' cat: 0 residential, 1 commercial, 2 industrial, 3 office.
inline int LifeSlotCat(int jobKind) { return jobKind == kJobIndustrial ? 2 : jobKind == kJobOffice ? 3 : 1; }

// hoodOf(cell): the hood of a CitySim cell, -1 none. Rows of other origins (transport, utility, industry) get the cell
// and hood but no homes or jobs (CitySim does not count them either). Rows reserved from a load are left alone.
inline void LifeFillRows(BuildingRegistry& reg, const CitySim& s, const std::vector<Lot>& lots, const std::function<int(int)>& hoodOf) {
    int lastCell = -2, lastHood = -1; // rows come in runs of the same cell
    int32_t lastX = 0, lastZ = 0;
    for (uint32_t i = 1; i < reg.Capacity(); ++i) {
        BuildingRow* r = reg.Get(reg.At(i));
        if (!r || r->alive != 1) continue;
        if (lastCell == -2 || r->cellX != lastX || r->cellZ != lastZ) {
            lastX = r->cellX, lastZ = r->cellZ, lastCell = s.cells.Find({r->cellX, r->cellZ});
            lastHood = lastCell >= 0 && hoodOf ? hoodOf(lastCell) : -1;
        }
        r->cell = lastCell, r->hood = lastHood, r->homes = 0;
        for (int e = 0; e < kEduLevels; ++e) r->jobSlots[e] = 0;
    }
    for (const Lot& l : lots) { // homes and jobs of the built lots
        if (!l.alive || !l.Built()) continue;
        BuildingRow* r = reg.Get(reg.Find(BOrigin::Lot, l.id));
        if (!r || r->alive != 1) continue;
        const LotCap cap = LotCapacity(l);
        r->homes = cap.homes;
        const int k = LifeJobKind(l.type);
        if (cap.Jobs() && k >= 0) SplitJobSlots(cap.Jobs(), LifeSlotCat(k), l.level, r->jobSlots);
    }
}

// The records' education levels (0 none .. 4 university) on CellSim's three bands: basic, skilled, higher.
inline int LifeEduBand(int level) { return level <= 1 ? 0 : level == 2 ? 1 : 2; }

inline void LifeApplyCells(const LifeWorld& w, CitySim& s) {
    static const LifeCell kEmpty;
    for (size_t c = 0; c < s.sim.size(); ++c) {
        const LifeCell& L = c < w.cells.size() ? w.cells[c] : kEmpty;
        uint32_t edu[3] = {};
        for (int e = 0; e < kEduLevels; ++e) edu[LifeEduBand(e)] += L.edu[e];
        s.SetCellPeople((int)c, L.residents, L.working, L.employed, edu, L.wealth);
    }
    s.SetLinks([&](auto&& add) {
        for (const auto& [k, n] : w.commute) add((int)(k >> 32), (int)(uint32_t)k, n);
    });
}

// homeLots: one entry per saved person, in save order. Returns how many were pinned.
inline int LifePinSaved(LifeSim& life, const BuildingRegistry& reg, const std::vector<uint64_t>& homeLots) {
    std::unordered_map<uint64_t, uint32_t> next; // lot -> slots taken so far
    int pinned = 0;
    for (const uint64_t lot : homeLots) {
        const BuildingId b = reg.Find(BOrigin::Lot, lot);
        if (!b.Valid()) continue;
        const CitizenId c = life.TenantOf(b.v, next[lot]++);
        if (!c.Valid()) continue;
        life.Pin(c, true);
        ++pinned;
    }
    return pinned;
}

} // namespace city
} // namespace tbd
