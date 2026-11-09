// LifeSim: the Phase 1 citizen sim as one object the host drives (CS2 sim roadmap Phase 1). Header only, headless
// (tests\sim_citizens_test, sim_lifecycle_test, sim_jobs_test, sim_migration_test).
//
//   SyncBuildings  after the BuildingRegistry changed (BuildingRegistry::version).
//   Tick(minute)   every sim tick (1 game minute). Due work only:
//                    month start   close the month's stats, migration numbers and quota, the moving pass
//                    every 15 min  one slice of the month: citizens and households with index % slices == slice get
//                                  their monthly visit (96 slices per game day = per life month), and the slice's share
//                                  of the immigration quota arrives
//                    every 10 min  job matching (seekers only)
//   WriteFactors   CellFactor::unemployment and ::commuteMin (this phase's only factor writes).
// Everything is deterministic from (city seed, tick): no shared random stream, records visited in index order.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sim_migration.h"

namespace tbd {
namespace city {

class LifeSim {
public:
    LifeWorld w;
    LifeMigrationState mig;
    int64_t lastMonth = INT64_MIN;

    void Reset(uint64_t citySeed) {
        LifeTuning t = w.tune;
        SimCalendar c = w.cal;
        SimEventRing* ev = w.events;
        const CellFactors* f = w.factors;
        LifeTravelFn tr = w.travel;
        w = LifeWorld(), mig = LifeMigrationState(), lastMonth = INT64_MIN;
        w.tune = t, w.cal = c, w.events = ev, w.factors = f, w.travel = tr, w.citySeed = citySeed;
    }
    void SyncBuildings(const BuildingRegistry& reg, const LifeJobKindFn& jobKindOf = nullptr) {
        LifeSyncBuildings(w, reg, jobKindOf);
        LifeComputeNear(w);
    }
    // Hood travel times changed (RouteCache rebuilt): the seekers' search order follows.
    void TravelChanged() { LifeComputeNear(w); }
    void Bootstrap() { LifeBootstrap(w); }

    int SlicesPerMonth() const { return 96 * std::max(1, w.cal.lifeDaysPerMonth); }
    void Tick(int64_t minute) {
        w.minute = minute;
        w.month = w.cal.LifeMonth(minute);
        if (w.month != lastMonth) MonthStart();
        if (minute % 15 == 0) Slice((int)((minute - w.cal.MonthStartMinute(w.month)) / 15));
        if (minute % 10 == 0) LifeMatchJobs(w);
    }

    void MonthStart() {
        if (lastMonth != INT64_MIN) {
            LifeMonthStats& s = w.cur;
            s.population = (int32_t)w.Population(), s.households = (int32_t)w.hh.Live(), s.workers = (int32_t)w.Working();
            s.unemployed = mig.seekers;
            w.history.push_back(s);
            if (w.history.size() > 120) w.history.erase(w.history.begin());
        }
        w.cur = LifeMonthStats();
        lastMonth = w.month;
        LifeMigrationMonth(w, mig);
        LifeMovePass(w, mig);
        if (w.tune.instantFill) LifeImmigrate(w, mig, mig.quota);
    }

    void Slice(int slice) {
        const uint32_t n = (uint32_t)SlicesPerMonth();
        if (slice < 0 || (uint32_t)slice >= n) return;
        for (uint32_t ci = (uint32_t)slice; ci < w.cit.Capacity(); ci += n)
            if (ci && w.cit.At(ci).Valid()) LifeCitizenMonth(w, ci);
        for (uint32_t hi = (uint32_t)slice; hi < w.hh.Capacity(); hi += n)
            if (hi && w.hh.At(hi).Valid()) LifeHouseholdVisit(w, hi);
        const int left = mig.quota - mig.admitted, slicesLeft = (int)n - slice;
        if (left > 0) LifeImmigrate(w, mig, (left + slicesLeft - 1) / slicesLeft);
    }

    // This phase's factor writes: unemployment (share of the cell's working-age adults without a job, 255 = all) and
    // the mean commute of the cell's workers in whole minutes.
    void WriteFactors(CellFactors& f) const {
        f.Fit(w.cells.size());
        for (size_t i = 0; i < w.cells.size(); ++i) {
            const LifeCell& c = w.cells[i];
            CellFactor& o = f.cells[i];
            const uint32_t able = c.residents[1] > c.students ? c.residents[1] - c.students : 0;
            o.unemployment = (uint8_t)(able ? std::min<uint32_t>(255, (able - std::min(able, c.working)) * 255u / able) : 0);
            o.commuteMin = (uint16_t)(c.working ? (c.commuteSum + c.working / 2) / c.working : 0);
        }
        ++f.version;
    }

    // The citizen in a building's slot (walkers and cars ask for the person they show): slot counts the building's
    // record residents in household order; past them (statistical residents) there is nobody.
    CitizenId TenantOf(uint32_t building, uint32_t slot) const {
        const uint32_t i = LifeWorld::Ix(building);
        if (!building || i >= w.bld.size() || w.bld[i].id != building) return CitizenId();
        for (uint32_t h = w.bld[i].firstHousehold; h; h = w.hh.nextInHome[LifeWorld::Ix(h)])
            for (uint32_t c = w.hh.firstMember[LifeWorld::Ix(h)]; c; c = w.cit.nextInHousehold[LifeWorld::Ix(c)])
                if (slot-- == 0) return CitizenId(c);
        return CitizenId();
    }
    // A worker of a workplace (cars going to work show them). O(citizens): for clicks, not per frame.
    CitizenId WorkerOf(uint32_t building, uint32_t slot) const {
        for (uint32_t ci = 1; ci < w.cit.Capacity(); ++ci)
            if (w.cit.work[ci] == building && w.cit.At(ci).Valid() && slot-- == 0) return w.cit.At(ci);
        return CitizenId();
    }
    // Pins a citizen (the player follows them: Phase 1's sim_people view keeps them named).
    void Pin(CitizenId c, bool on) { if (w.cit.Alive(c)) w.cit.flags[c.Index()] = (uint8_t)(on ? w.cit.flags[c.Index()] | kCitPinned : w.cit.flags[c.Index()] & ~kCitPinned); }
};

} // namespace city
} // namespace tbd
