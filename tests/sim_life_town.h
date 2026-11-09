// Shared synthetic town for the Phase 1 citizen tests (sim_citizens_test, sim_lifecycle_test, sim_jobs_test,
// sim_migration_test): a square of CitySim cells, each with residential and/or workplace buildings in a BuildingRegistry,
// hoods of 4 x 4 cells. Header only.
#pragma once

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "city/sim_life.h"
#include "city/sim_life_save.h"
#include "city/sim_life_views.h"

namespace lifetest {
using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                            \
    do {                                                                                    \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

struct Town {
    BuildingRegistry reg;
    LifeSim sim;
    int side = 0;
    uint64_t nextId = 1;
    std::vector<uint64_t> ids; // origin ids of the buildings, in creation order

    // side x side cells; per cell `homes` residents of capacity (0: none) and `jobs` workplace slots.
    void Build(int sideCells, uint32_t homes, uint32_t jobs, int cat = 1) {
        side = sideCells;
        reg.BeginSync();
        for (const uint64_t id : ids) Keep(id);
        for (int z = 0; z < side; ++z)
            for (int x = 0; x < side; ++x) {
                if (homes) Add(x, z, (uint16_t)homes, 0, cat);
                if (jobs) Add(x, z, 0, jobs, cat);
            }
        reg.EndSync();
        sim.SyncBuildings(reg);
    }
    BuildingRow Row(int x, int z, uint16_t homes, uint32_t jobs, int cat) const {
        BuildingRow r;
        r.cellX = x, r.cellZ = z, r.cell = z * 64 + x, r.hood = (z / 4) * 16 + x / 4, r.level = 1, r.homes = homes;
        r.kind = cat;
        if (jobs) SplitJobSlots(jobs, cat, 1, r.jobSlots);
        return r;
    }
    uint64_t Add(int x, int z, uint16_t homes, uint32_t jobs, int cat = 1) {
        const uint64_t id = nextId++;
        rows_.push_back(Row(x, z, homes, jobs, cat));
        ids.push_back(id);
        reg.Upsert(BOrigin::Lot, id, rows_.back());
        return id;
    }
    void Keep(uint64_t id) {
        const size_t k = (size_t)(id - 1);
        if (k < rows_.size() && alive_(k)) reg.Upsert(BOrigin::Lot, id, rows_[k]);
    }
    // Re-sync with only the buildings `keep` says yes to (removal), or with changed rows.
    template <class F>
    void Resync(F&& keep) {
        reg.BeginSync();
        for (const uint64_t id : ids) {
            const size_t k = (size_t)(id - 1);
            if (!alive_(k)) continue;
            BuildingRow r = rows_[k];
            if (keep(id, r)) rows_[k] = r, reg.Upsert(BOrigin::Lot, id, r);
            else dead_.resize(rows_.size(), 0), dead_[k] = 1;
        }
        reg.EndSync();
        sim.SyncBuildings(reg);
    }
    void RunMinutes(int64_t from, int64_t n) { for (int64_t m = from; m < from + n; ++m) sim.Tick(m); }
    void RunDays(int64_t& minute, int days) { RunMinutes(minute, (int64_t)days * 1440), minute += (int64_t)days * 1440; }

private:
    bool alive_(size_t k) const { return k >= dead_.size() || !dead_[k]; }
    std::vector<BuildingRow> rows_;
    std::vector<uint8_t> dead_;
};

inline uint64_t LifeHash(const LifeWorld& w) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull; };
    auto vec = [&](const auto& v) { if (!v.empty()) mix(v.data(), v.size() * sizeof(v[0])); };
    vec(w.cit.household), vec(w.cit.work), vec(w.cit.bornMonth), vec(w.cit.edu), vec(w.cit.jobLevel), vec(w.cit.health);
    vec(w.cit.happy), vec(w.cit.flags), vec(w.cit.stage), vec(w.cit.commuteMin), vec(w.cit.seed), vec(w.cit.nextInHousehold);
    vec(w.hh.home), vec(w.hh.firstMember), vec(w.hh.savings), vec(w.hh.size), vec(w.hh.wealth), vec(w.cit.pool.Gens());
    return h;
}

} // namespace lifetest
