// Jobs (CS2 sim roadmap Phase 1): workplace slots by education level, a seeker queue per hood, local-first matching,
// overqualified workers, changing jobs over long commutes. Header only, headless (tests\sim_jobs_test).
//
//   Slots     LifeBuilding::slots[level] from BuildingRow::jobSlots; filled[level] counts holders (no per-job record).
//   Seekers   working-age adults without a job are queued in their home hood by their monthly visit; a worker who is
//             overqualified or commutes over life.quitCommute is queued too, but only takes a better job.
//   Matching  every 10 game minutes (LifeMatchJobs): own hood first, then the life.matchHoods nearest hoods by travel
//             time (hood-to-hood route costs through LifeWorld::travel, else straight line), never over life.maxCommute.
//             Within a hood the highest level <= the seeker's education wins: an overqualified worker is allowed and
//             pays for it in happiness (sim_wellbeing.h). The same order as sim_hoods.h MatchLocalFirst, per person.
// Retirement (65) and the job loss of a building that shrank live in sim_lifecycle.h and sim_life_boot.h.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "sim_wellbeing.h"

namespace tbd {
namespace city {

// Hood centres from their buildings, then for each hood the hoods to search, own first, nearest first.
inline void LifeComputeNear(LifeWorld& w) {
    const int n = std::max(1, w.hoodCount);
    std::vector<double> sx((size_t)n, 0.0), sz((size_t)n, 0.0), cnt((size_t)n, 0.0);
    for (const LifeBuilding& L : w.bld)
        if (L.id) sx[(size_t)L.hood] += L.cellX, sz[(size_t)L.hood] += L.cellZ, cnt[(size_t)L.hood] += 1.0;
    w.hoodNear.assign((size_t)n, {});
    std::vector<std::pair<float, int>> d;
    for (int a = 0; a < n; ++a) {
        d.clear();
        if (cnt[(size_t)a] == 0) { w.hoodNear[(size_t)a].push_back((uint16_t)a); continue; }
        for (int b = 0; b < n; ++b) {
            if (cnt[(size_t)b] == 0) continue;
            float m = 0.0f;
            if (a != b) {
                const double dx = sx[(size_t)a] / cnt[(size_t)a] - sx[(size_t)b] / cnt[(size_t)b];
                const double dz = sz[(size_t)a] / cnt[(size_t)a] - sz[(size_t)b] / cnt[(size_t)b];
                m = (float)(std::sqrt(dx * dx + dz * dz) * 128.0 / 500.0) + 4.0f;
                if (w.travel) m = std::max(m, w.travel(a, b));
            }
            if (m <= (float)w.tune.maxCommute) d.push_back({a == b ? -1.0f : m, b});
        }
        std::sort(d.begin(), d.end());
        const size_t keep = std::min(d.size(), (size_t)std::max(1, w.tune.matchHoods));
        for (size_t k = 0; k < keep; ++k) w.hoodNear[(size_t)a].push_back((uint16_t)d[k].second);
    }
    if (w.seekers.size() < (size_t)n) w.seekers.resize((size_t)n);
}

inline int LifeHomeHood(const LifeWorld& w, uint32_t ci) { const LifeBuilding* L = LifeHomeOf(w, ci); return L ? L->hood : -1; }
inline void LifeQueueSeeker(LifeWorld& w, uint32_t ci) {
    const int h = LifeHomeHood(w, ci);
    if (h >= 0 && (size_t)h < w.seekers.size()) w.seekers[(size_t)h].push_back(w.cit.At(ci).v);
}

// One seeker: true when hired (or moved to a better job).
inline bool LifeTryHire(LifeWorld& w, uint32_t ci, std::vector<uint32_t>& cursor) {
    const LifeBuilding* home = LifeHomeOf(w, ci);
    if (!home || (size_t)home->hood >= w.hoodNear.size()) return false;
    const bool employed = w.cit.work[ci] != 0;
    const int minLevel = employed ? (w.cit.commuteMin[ci] > w.tune.quitCommute ? w.cit.jobLevel[ci] : w.cit.jobLevel[ci] + 1) : 0;
    for (uint16_t nh : w.hoodNear[(size_t)home->hood]) {
        if (nh >= w.hoodJobs.size()) continue;
        for (int l = w.cit.edu[ci]; l >= minLevel; --l) {
            if (w.hoodVac[nh][(size_t)l] <= 0) continue;
            const std::vector<uint32_t>& list = w.hoodJobs[nh];
            for (size_t k = 0; k < list.size(); ++k) {
                const uint32_t i = (uint32_t)((cursor[nh] + k) % list.size());
                const LifeBuilding& J = w.bld[list[i]];
                if (J.filled[l] >= J.slots[l]) continue;
                const uint16_t m = w.CommuteBetween(*home, J);
                if (m > w.tune.maxCommute || (employed && l == w.cit.jobLevel[ci] && m + 10 > w.cit.commuteMin[ci])) continue;
                cursor[nh] = i;
                if (employed) w.cur.quit++;
                w.SetWork(ci, J.id, l);
                w.cur.hired++;
                w.Event(SimEventType::Hired, J.id, w.cit.At(ci).v, l);
                return true;
            }
        }
    }
    return false;
}

// The 10-minute job pass: at most life.matchPerPass seekers, hoods in turn from a rotating start. A seeker that finds
// nothing is dropped and queued again by its next monthly visit.
inline void LifeMatchJobs(LifeWorld& w) {
    std::vector<uint32_t>& cursor = w.jobCursor;
    uint32_t& hoodStart = w.matchStart;
    cursor.resize(w.hoodJobs.size(), 0);
    int budget = w.tune.matchPerPass;
    const uint32_t n = (uint32_t)w.seekers.size();
    for (uint32_t k = 0; k < n && budget > 0; ++k) {
        const uint32_t h = (hoodStart + k) % n;
        std::vector<uint32_t>& q = w.seekers[h];
        size_t done = 0;
        for (; done < q.size() && budget > 0; ++done, --budget) {
            const CitizenId c(q[done]);
            if (!w.cit.Alive(c)) continue;
            const uint32_t ci = c.Index();
            if (!LifeIsWorkingAge(w, ci) || LifeHomeHood(w, ci) != (int)h) continue;
            LifeTryHire(w, ci, cursor);
        }
        q.erase(q.begin(), q.begin() + (long)done);
        if (budget <= 0) hoodStart = h;
    }
}

} // namespace city
} // namespace tbd
