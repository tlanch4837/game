// The city sim's demand, growth, level-up and abandonment (Track S, phase S2). Header only, headless
// (tests\sim_growth_test.cpp).
//
//   Demand     one value in [-1, 1] per zone category (Residential, Commercial, Office, Industrial), smoothed toward the
//              raw value over about a game day (SimTuning::demandTauMinutes):
//                R  a start-up pull while the city is tiny, plus vacancies minus unemployed adults;
//                C  residents' spending (commercialPerResident jobs per resident) against commercial jobs;
//                I, Office  unemployed minus vacancies, split by the share of higher-educated adults, plus the goods
//                   commercial needs from industry.
//   Growth     the zone grid's spawns are gated on demand: a category above growThreshold grows, weighted by how far above.
//   Levels     a built lot whose cell has land value >= levelUpLandValue, all utilities, and positive demand for its category
//              for levelUpDays in a row swaps to a same-footprint model one level up (construction starts again); one with
//              bad conditions (demand below badDemand, no power or water) for levelDownDays steps down a level, or is
//              abandoned (demolished, the zone stays painted) at level 1. No same-footprint model: skipped, logged once.
//   Land value coarse, per cell from the 3 x 3 cells around: jobs and residents raise it, industry lowers it, services
//              (S3 coverage) raise it.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sim.h"
#include "sim_cells.h"
#include "sim_tuning.h"
#include "zoning.h"

namespace tbd {
namespace city {

class SimGrowth {
public:
    static constexpr int kCats = (int)ZoneCat::Count; // Residential, Commercial, Office, Industrial
    double demand[kCats] = {}, raw[kCats] = {};
    double offset[kCats] = {}; // added to the raw demand (S4: tax rates away from 10 %)
    std::vector<std::string> events;  // what happened since the host last emptied it (level ups, abandonments), for the log
    uint64_t levelUps = 0, levelDowns = 0, abandoned = 0;

    void Clear() {
        for (int c = 0; c < kCats; ++c) demand[c] = raw[c] = 0;
        state_.clear();
        noModel_.clear();
        events.clear();
        levelUps = levelDowns = abandoned = 0;
    }

    // Raw demand from the cells' counters, then smoothed over `minutes`. With the zone grid, what is under construction
    // counts as if it were built (its homes as future unemployed adults, its jobs as future vacancies): without it growth
    // overshoots by everything in the pipeline and then abandons in waves.
    void UpdateDemand(const CitySim& s, const SimTuning& t, double minutes, const ZoneGrid* pipeline = nullptr) {
        uint64_t pop = 0, adults = 0, working = 0, cap[3] = {}, emp[3] = {}, higher = 0;
        for (const CellSim& c : s.sim) {
            pop += c.Population();
            adults += c.residents[kAdults];
            working += c.working;
            higher += c.edu[2];
            for (int k = 0; k < 3; ++k) cap[k] += c.jobCap[k], emp[k] += c.employed[k];
        }
        uint64_t pendHomes = 0, pendJobs[3] = {};
        if (pipeline)
            for (const Lot& l : pipeline->lots)
                if (l.alive && !l.Built()) {
                    const LotCap c = LotCapacity(l);
                    pendHomes += c.homes;
                    for (int k = 0; k < 3; ++k) pendJobs[k] += c.jobs[k];
                }
        pop += pendHomes;
        const uint64_t pendAdults = pendHomes * 65 / 100;
        adults += pendAdults;
        for (int k = 0; k < 3; ++k) cap[k] += pendJobs[k];
        const double A = std::max<double>((double)adults, 50.0);
        const double U = (double)(adults - std::min(adults, working));
        const double V = (double)((cap[0] - emp[0]) + (cap[1] - emp[1]) + (cap[2] - emp[2]));
        const double P = (double)pop;
        const double h = adults ? (double)higher / (double)adults : 0.2;
        const double start = t.demandStartPull * std::max(0.0, 1.0 - P / std::max(1.0, t.demandStartPopulation));
        const double wantComm = P * t.commercialPerResident, comm = (double)cap[kJobCommercial];
        const double wantGoods = comm * t.industryPerCommercial, ind = (double)cap[kJobIndustrial];
        const double jobs = t.demandUnemployedWeight * (U - V) / A;
        const double office = std::clamp(t.demandOfficeShare * (0.5 + 2.0 * h), 0.0, 1.0);
        raw[(int)ZoneCat::Residential] = start + t.demandBaseResidential + t.demandJobsWeight * (V - U) / A;
        raw[(int)ZoneCat::Commercial] = t.demandCommercialWeight * (wantComm - comm) / std::max(wantComm, 20.0);
        raw[(int)ZoneCat::Office] = jobs * office;
        raw[(int)ZoneCat::Industrial] = jobs * (1.0 - office) + t.demandGoodsWeight * (wantGoods - ind) / std::max(wantGoods, 20.0);
        const double k = std::clamp(minutes / std::max(1.0, t.demandTauMinutes), 0.0, 1.0);
        for (int c = 0; c < kCats; ++c) {
            raw[c] = std::clamp(raw[c] + offset[c], -1.0, 1.0);
            demand[c] += (raw[c] - demand[c]) * k;
        }
    }

    // The zone grid grows only what is in demand.
    void ApplyGate(ZoneGrid& z, const SimTuning& t) const {
        z.gateSpawns = true;
        for (int c = 0; c < kCats; ++c) z.catWeight[c] = (float)std::max(0.0, demand[c] - t.growThreshold);
    }

    // Coarse land value per cell, 0..255.
    void UpdateLand(CitySim& s, const SimTuning& t) const {
        std::vector<uint8_t> out(s.sim.size());
        for (size_t i = 0; i < s.sim.size(); ++i) {
            const CellKey k = s.cells.cells[i].key;
            double jobs = 0, res = 0, ind = 0;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int j = s.cells.Find({k.x + dx, k.z + dz});
                    if (j < 0) continue;
                    const CellSim& n = s.sim[(size_t)j];
                    jobs += (double)n.jobCap[0] + n.jobCap[1] + n.jobCap[2];
                    res += n.Population();
                    ind += n.jobCap[kJobIndustrial];
                }
            const CellSim& c = s.sim[i];
            double cov = 0;
            for (uint8_t v : c.coverage) cov += v;
            cov /= 6.0;
            const double lv = t.landBase + t.landPerJobNear * jobs + t.landPerResidentNear * res + t.landPerIndustrialNear * ind + t.landPerCoverage * cov;
            out[i] = (uint8_t)std::clamp(lv, 0.0, 255.0);
        }
        for (size_t i = 0; i < s.sim.size(); ++i) s.sim[i].landValue = out[i];
    }

    // Level up / down / abandon, for `minutes` of game time since this lot's last visit. Lots are read and changed in
    // vector order (deterministic); with sliceCount > 1 only the lots with index % sliceCount == slice are visited.
    // lotUtil: per-lot utility flags (sim_coverage.h LotUtil, zones.lots order); without it the cells' flags are used.
    void TickLots(ZoneGrid& z, const CitySim& s, const SimTuning& t, double minutes, int slice = 0, int sliceCount = 1,
                  const std::vector<uint8_t>* lotUtil = nullptr) {
        const auto& types = ZoneTypes();
        const float upMin = (float)(t.levelUpDays * kDayMinutes), downMin = (float)(t.levelDownDays * kDayMinutes);
        if (modelsFor_ != z.models.size()) { // the catalog: which (type, w, d, level) exist
            modelsFor_ = z.models.size();
            models_.clear();
            for (const BuildingModel& m : z.models) models_.insert(Key(m.type, m.w, m.d, m.level));
        }
        const auto hasModel = [this](int type, int w, int d, int level) { return models_.count(Key(type, w, d, level)) != 0; };
        sliceCount = std::max(1, sliceCount);
        for (size_t li = (size_t)std::max(0, slice); li < z.lots.size(); li += (size_t)sliceCount) {
            Lot& l = z.lots[li];
            if (!l.alive || !l.Built() || l.type < 0 || l.type >= (int)types.size()) continue;
            const int cell = s.cells.Find(CellKeyAt(l.c.x, l.c.z));
            if (cell < 0) continue; // the cells have not caught up with this lot yet
            const CellSim& c = s.sim[(size_t)cell];
            const int cat = (int)types[l.type].cat;
            bool power = c.powerOk, water = c.waterOk, sewage = c.sewageOk;
            if (lotUtil && li < lotUtil->size()) {
                const uint8_t f = (*lotUtil)[li];
                power = (f & 1) != 0, water = (f & 2) != 0, sewage = (f & 4) != 0;
            }
            if (t.utilitiesRequired <= 0.0) power = water = sewage = true;
            const bool utilities = power && water && sewage;
            const bool bad = demand[cat] < t.badDemand || !power || !water;
            const bool good = !bad && utilities && demand[cat] > 0.0 && c.landValue >= t.levelUpLandValue;
            St& st = state_[l.id];
            if (good) {
                st.good += (float)minutes, st.bad = 0;
            } else if (bad) {
                st.bad += (float)minutes, st.good = 0;
            } else {
                st.good = 0;
                st.bad = std::max(0.0f, st.bad - (float)minutes); // neutral conditions slowly forgive
            }
            // Each lot has its own patience (1x to 2x the tuned days, fixed per lot): bad times thin a district out
            // gradually instead of emptying it on one day, and level ups spread out the same way.
            const double jitter = (double)(SimSeed(0x6C0, l.id) % 1000u) / 1000.0;
            if (st.good >= upMin * (float)(1.0 + 0.5 * jitter)) {
                st.good = 0;
                if (hasModel(l.type, l.w, l.d, l.level + 1)) {
                    const int to = l.level + 1;
                    if (z.Relevel((int)li, to)) {
                        ++levelUps;
                        events.push_back("level up: lot " + std::to_string(l.id) + " to L" + std::to_string(to));
                    }
                } else if (noModel_.insert(Key(l.type, l.w, l.d, l.level + 1)).second) {
                    events.push_back("no " + std::string(types[l.type].prefix) + "_" + std::to_string(l.w) + "x" + std::to_string(l.d) + " L" +
                                     std::to_string(l.level + 1) + " model: those lots stay at L" + std::to_string(l.level));
                }
            } else if (st.bad >= downMin * (float)(1.0 + jitter)) {
                st.bad = 0;
                if (l.level > 1 && hasModel(l.type, l.w, l.d, l.level - 1)) {
                    const int to = l.level - 1;
                    if (z.Relevel((int)li, to)) {
                        ++levelDowns;
                        events.push_back("level down: lot " + std::to_string(l.id) + " to L" + std::to_string(to));
                    }
                } else {
                    const uint64_t id = l.id;
                    z.Demolish((int)li);
                    state_.erase(id);
                    ++abandoned;
                    events.push_back("abandoned: lot " + std::to_string(id));
                }
            }
        }
        if (state_.size() > z.lots.size() * 2 + 64) { // forget lots that are gone
            std::unordered_set<uint64_t> live;
            for (const Lot& l : z.lots)
                if (l.alive) live.insert(l.id);
            for (auto it = state_.begin(); it != state_.end();) it = live.count(it->first) ? std::next(it) : state_.erase(it);
        }
    }

    // Game minutes this lot has spent in bad conditions in a row, and how long it may (before its patience jitter).
    float BadMinutes(uint64_t id) const { const auto it = state_.find(id); return it == state_.end() ? 0.0f : it->second.bad; }
    static float DownMinutes(const SimTuning& t) { return (float)(t.levelDownDays * kDayMinutes); }

private:
    struct St {
        float good = 0, bad = 0; // game minutes in a row
    };
    std::unordered_map<uint64_t, St> state_;
    std::unordered_set<uint64_t> noModel_;
    std::unordered_set<uint64_t> models_;
    size_t modelsFor_ = ~(size_t)0;
    static uint64_t Key(int type, int w, int d, int level) { return ((uint64_t)type << 24) | ((uint64_t)w << 16) | ((uint64_t)d << 8) | (uint64_t)level; }
};

} // namespace city
} // namespace tbd
