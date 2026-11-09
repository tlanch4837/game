// The city sim's aggregate tier (Track S, phase S1): per-CityCell counters for homes, jobs, residents and employment.
// Header only, no renderer or scene dependencies (tests\sim_cells_test.cpp).
//
//   Capacity   LotCapacity(lot) is the one place homes and jobs per lot are decided (TransitDemand uses it too).
//   Residents  a built residential lot moves its households in at once (S2 adds demand-driven move-in); the split into
//              kids / adults / seniors, education and wealth bands is fixed per lot and recorded, so demolishing the lot
//              takes away exactly what it brought.
//   Jobs       counter-based matching, no per-person search: unemployed adults in a supercell (4 x 4 cells) fill the
//              vacancies of their own supercell first, then the rings around it (kMatchRings). Who works where is kept
//              only as supercell-to-supercell link counts, which is enough to fire / quit exactly: a job cell losing
//              capacity lets go of workers through its links, a home cell losing adults quits their jobs through its links.
//
// Invariants (checked by the test): per cell residents[adult] >= working, employed[k] <= jobCap[k],
// vacancies[k] == jobCap[k] - employed[k]; city-wide sum(working) == sum(employed) == sum(links); per supercell
// sum(working) == its home links and sum(employed) == its job links.
//
// externalPeople (CS2 sim roadmap Phase 1): the citizen records (sim_life*.h) are the one source of who lives and works
// where. Add() then gives a lot's empty homes and job slots only, Remove() takes capacity only, Match() does nothing,
// and the host writes every cell's people with SetCellPeople and the commute links with SetLinks (sim_life_cells.h).
// The supercell links come from the real job assignments, so ForEachLink, herds and hoods work unchanged.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "city_cells.h"
#include "zoning.h"

namespace tbd {
namespace city {

enum JobKind : int { kJobIndustrial = 0, kJobCommercial = 1, kJobOffice = 2, kJobKinds = 3 };
enum AgeBand : int { kKids = 0, kAdults = 1, kSeniors = 2, kAgeBands = 3 };

struct LotCap {
    uint16_t homes = 0;           // residents the lot houses at full occupancy
    uint16_t jobs[kJobKinds] = {}; // workplaces by kind
    uint16_t Jobs() const { return (uint16_t)(jobs[0] + jobs[1] + jobs[2]); }
};

// Homes and jobs of a lot, from its footprint (w x d cells) and zone type. The same per-area factors TransitDemand used:
// homes 0.8 / 1.4 / 2.5 / 5.0 per cell (low, row, medium, high density); jobs 2.0 industrial, 1.5 commercial, 2.5 office.
// Rounded to whole people. A higher level (the sim levels lots up, S2) houses or employs more on the same footprint:
// x1.0 at L1, x1.5 at L2, x2.0 at L3 and above.
inline float LevelFactor(int level) { return level <= 1 ? 1.0f : level == 2 ? 1.5f : 2.0f; }
inline LotCap LotCapacity(int type, int w, int d, int level = 1) {
    LotCap c;
    const auto& types = ZoneTypes();
    if (type < 0 || type >= (int)types.size() || w <= 0 || d <= 0) return c;
    const float area = (float)(w * d) * LevelFactor(level);
    static const float kHome[4] = {0.8f, 1.4f, 2.5f, 5.0f};
    const auto round16 = [](float v) { return (uint16_t)std::clamp(std::lround(v), 0L, 65535L); };
    switch (types[type].cat) {
    case ZoneCat::Residential: c.homes = round16(area * kHome[std::min(3, type)]); break;
    case ZoneCat::Industrial: c.jobs[kJobIndustrial] = round16(area * 2.0f); break;
    case ZoneCat::Commercial: c.jobs[kJobCommercial] = round16(area * 1.5f); break;
    case ZoneCat::Office: c.jobs[kJobOffice] = round16(area * 2.5f); break;
    default: break;
    }
    return c;
}
inline LotCap LotCapacity(const Lot& l) { return LotCapacity(l.type, l.w, l.d, l.level); }

struct CellSim {
    uint16_t homeCap = 0, jobCap[kJobKinds] = {};
    uint16_t residents[kAgeBands] = {};      // kids, adults, seniors
    uint16_t working = 0;                    // adults living here who have a job (anywhere)
    uint16_t employed[kJobKinds] = {};       // jobs here that are filled (by anyone)
    uint16_t vacancies[kJobKinds] = {};      // jobCap - employed
    uint16_t edu[3] = {};                    // adults by education: basic, skilled, higher
    uint16_t wealth[4] = {};                 // residents by wealth: low, modest, comfortable, rich
    uint8_t happiness = 128, landValue = 0;  // filled by S2 / S3
    uint8_t powerOk : 1, waterOk : 1, sewageOk : 1, garbageOk : 1;
    uint8_t coverage[6] = {};                // fire, police, health, school, park, transit (S3)
    // tracked people (S6) are still counted above; these say how many of them are named, so the anonymous matching
    // never hires or fires a named person (they change jobs only through SimPeople)
    uint16_t named[kAgeBands] = {}, namedWorking = 0;
    uint16_t namedLost = 0;                  // named workers here whose job went (capacity loss); SimPeople flips them
    CellSim() : powerOk(1), waterOk(1), sewageOk(1), garbageOk(1) {}
    uint32_t Population() const { return (uint32_t)residents[0] + residents[1] + residents[2]; }
    uint16_t AnonAdults() const { return residents[kAdults] > named[kAdults] ? (uint16_t)(residents[kAdults] - named[kAdults]) : 0; }
    uint16_t AnonWorking() const { return working > namedWorking ? (uint16_t)(working - namedWorking) : 0; }
    // Anonymous adults without a job: all jobless adults less the named jobless ones. Never more than adults - working,
    // so hiring them can never push working past the adults (whatever the named counts say).
    uint16_t Unemployed() const {
        const int jobless = (int)residents[kAdults] - (int)working;
        const int namedJobless = std::max(0, (int)named[kAdults] - (int)namedWorking);
        return (uint16_t)std::max(0, jobless - namedJobless);
    }
};
// Changed from the brief's proposal: `working` (home side) is separate from `employed` (job side), so a cell's residents
// and its workplaces can be counted without knowing who works where.

class CitySim {
public:
    static constexpr int kSuper = 4;      // cells per supercell side
    static constexpr int kMatchRings = 2; // supercell rings searched for jobs beyond the own one (5 x 5 = 2.5 km)

    CityCells cells;
    std::vector<CellSim> sim; // parallel to cells.cells
    bool externalPeople = false; // Phase 1: people come from the citizen records (see the header comment); Clear keeps it

    // externalPeople: one cell's people as the records count them (saturating to the uint16 counters).
    void SetCellPeople(int cell, const uint32_t res[kAgeBands], uint32_t working, const uint32_t employed[kJobKinds], const uint32_t edu[3],
                       const uint32_t wealth[4]) {
        if (cell < 0 || (size_t)cell >= sim.size()) return;
        CellSim& c = sim[(size_t)cell];
        const auto sat = [](uint32_t v) { return (uint16_t)std::min<uint32_t>(v, 65535u); };
        bool diff = c.working != sat(working);
        for (int a = 0; a < kAgeBands; ++a) diff |= c.residents[a] != sat(res[a]), c.residents[a] = sat(res[a]);
        c.working = sat(working);
        for (int k = 0; k < kJobKinds; ++k) {
            diff |= c.employed[k] != sat(employed[k]);
            c.employed[k] = sat(employed[k]);
            c.vacancies[k] = Sub(c.jobCap[k], c.employed[k]);
        }
        for (int e = 0; e < 3; ++e) c.edu[e] = sat(edu[e]);
        for (int w = 0; w < 4; ++w) c.wealth[w] = sat(wealth[w]);
        if (diff) cells.Touch(cell, kDirtySim);
    }
    // externalPeople: the commute links from scratch. each(add) calls add(homeCell, jobCell, workers) for every pair.
    template <class F>
    void SetLinks(F&& each) {
        byJob_.clear(), byHome_.clear();
        each([this](int h, int j, int32_t n) {
            if (n <= 0 || h < 0 || j < 0 || (size_t)h >= superOf_.size() || (size_t)j >= superOf_.size()) return;
            Link(superOf_[(size_t)h], superOf_[(size_t)j], n);
        });
    }

    // Diffs the zone grid's lots against what the sim knows: new lots join their cell (render dirty), lots that finished
    // building add capacity and residents, lots that are gone take theirs away. Then fixes over-employment and matches.
    // Cheap to call every tick: it returns at once while `version` is unchanged. Returns true when anything changed.
    bool Sync(const std::vector<Lot>& lots, uint64_t version) {
        if (version == syncedVersion_) return false;
        syncedVersion_ = version;
        ++generation_;
        bool changed = false;
        for (const Lot& l : lots) {
            if (!l.alive) continue;
            auto it = recs_.find(l.id);
            if (it == recs_.end()) {
                Rec r;
                r.cell = CellFor(l.c.x, l.c.z);
                r.type = l.type, r.w = l.w, r.d = l.d, r.level = l.level;
                r.path = l.path;
                cells.AddLot(r.cell, l.id);
                cells.Touch(r.cell, kDirtyRender | kDirtySim);
                it = recs_.emplace(l.id, r).first;
                changed = true;
            }
            Rec& r = it->second;
            r.gen = generation_;
            if (r.path != l.path || r.type != l.type || r.w != l.w || r.d != l.d || r.level != l.level) { // a level change swapped the model
                if (r.built) Remove(r);
                r.type = l.type, r.w = l.w, r.d = l.d, r.level = l.level, r.path = l.path;
                cells.Touch(r.cell, kDirtyRender | kDirtySim);
                changed = true;
            }
            if (!r.built && l.Built()) {
                Add(r);
                changed = true;
            }
        }
        for (auto it = recs_.begin(); it != recs_.end();) { // gone: demolished (alive false) or compacted away
            if (it->second.gen != generation_) {
                if (it->second.built) Remove(it->second);
                cells.RemoveLot(it->second.cell, it->first);
                cells.Touch(it->second.cell, kDirtyRender | kDirtySim);
                it = recs_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) Match();
        return changed;
    }

    // Unemployed adults take vacancies: own supercell first, then ring by ring.
    void Match() {
        if (externalPeople) return; // the records match jobs (sim_jobs.h)
        const int nsc = (int)supers_.size();
        std::vector<uint32_t> scVac((size_t)nsc, 0), scUnemp((size_t)nsc, 0);
        for (size_t c = 0; c < sim.size(); ++c) {
            const int s = superOf_[c];
            scUnemp[(size_t)s] += sim[c].Unemployed();
            for (int k = 0; k < kJobKinds; ++k) scVac[(size_t)s] += sim[c].vacancies[k];
        }
        for (int sh = 0; sh < nsc; ++sh) {
            if (!scUnemp[(size_t)sh]) continue;
            const CellKey hk = supers_[(size_t)sh].key;
            for (int ring = 0; ring <= kMatchRings && scUnemp[(size_t)sh]; ++ring)
                for (int dz = -ring; dz <= ring && scUnemp[(size_t)sh]; ++dz)
                    for (int dx = -ring; dx <= ring && scUnemp[(size_t)sh]; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;
                        const auto it = superIndex_.find(PackKey({hk.x + dx, hk.z + dz}));
                        if (it == superIndex_.end() || !scVac[(size_t)it->second]) continue;
                        const int sj = it->second;
                        const uint32_t moved = Hire(sh, sj, std::min(scUnemp[(size_t)sh], scVac[(size_t)sj]));
                        scUnemp[(size_t)sh] -= moved;
                        scVac[(size_t)sj] -= moved;
                    }
        }
    }

    // ---- totals (O(cells); the host caches them per tick)
    uint64_t Population() const { uint64_t n = 0; for (const CellSim& c : sim) n += c.Population(); return n; }
    uint64_t Adults() const { uint64_t n = 0; for (const CellSim& c : sim) n += c.residents[kAdults]; return n; }
    uint64_t Working() const { uint64_t n = 0; for (const CellSim& c : sim) n += c.working; return n; }
    uint64_t Employed() const { uint64_t n = 0; for (const CellSim& c : sim) n += (uint64_t)c.employed[0] + c.employed[1] + c.employed[2]; return n; }
    uint64_t JobCap() const { uint64_t n = 0; for (const CellSim& c : sim) n += (uint64_t)c.jobCap[0] + c.jobCap[1] + c.jobCap[2]; return n; }
    uint64_t HomeCap() const { uint64_t n = 0; for (const CellSim& c : sim) n += c.homeCap; return n; }
    uint64_t Links() const { uint64_t n = 0; for (const auto& l : byJob_) n += (uint64_t)l.second; return n; }
    size_t Lots() const { return recs_.size(); }

    // Per supercell link sums, for the test's invariant check: (home links, job links) of supercell s.
    void SuperLinks(int s, uint64_t& home, uint64_t& job) const {
        home = job = 0;
        for (auto it = byHome_.lower_bound((uint64_t)s << 32); it != byHome_.end() && (int)(it->first >> 32) == s; ++it) home += (uint64_t)it->second;
        for (auto it = byJob_.lower_bound((uint64_t)s << 32); it != byJob_.end() && (int)(it->first >> 32) == s; ++it) job += (uint64_t)it->second;
    }
    int SuperOf(int cell) const { return superOf_[(size_t)cell]; }
    int Supercells() const { return (int)supers_.size(); }
    CellKey SuperKey(int s) const { return supers_[(size_t)s].key; }
    const std::vector<int>& SuperCellList(int s) const { return supers_[(size_t)s].cells; }
    // ---- tracked people (S6). All keep the invariants above; false (and nothing changes) when it cannot be done.
    struct LotView {
        int cell = -1;
        bool built = false;
        uint16_t homes = 0;
        uint16_t res[kAgeBands] = {};
    };
    bool FindLot(uint64_t id, LotView& out) const {
        const auto it = recs_.find(id);
        if (it == recs_.end()) return false;
        out.cell = it->second.cell, out.built = it->second.built, out.homes = it->second.cap.homes;
        for (int a = 0; a < kAgeBands; ++a) out.res[a] = it->second.res[a];
        return true;
    }
    void Name(int cell, int band, bool working, int delta) {
        CellSim& c = sim[(size_t)cell];
        c.named[band] = (uint16_t)std::max(0, (int)c.named[band] + delta);
        if (working) c.namedWorking = (uint16_t)std::max(0, (int)c.namedWorking + delta);
    }
    // A worker of home cell h with a job of kind k in cell j stops working there.
    bool DropJob(int h, int j, int k, bool named) {
        CellSim& H = sim[(size_t)h];
        CellSim& J = sim[(size_t)j];
        const int sh = superOf_[(size_t)h], sj = superOf_[(size_t)j];
        const auto it = byJob_.find(((uint64_t)sj << 32) | (uint32_t)sh);
        if (!H.working || !J.employed[k] || it == byJob_.end() || it->second <= 0) return false;
        if (named && !H.namedWorking) return false;
        --H.working;
        if (named) --H.namedWorking;
        --J.employed[k];
        J.vacancies[k] = Sub(J.jobCap[k], J.employed[k]);
        Link(sh, sj, -1);
        return true;
    }
    // An unemployed adult of home cell h takes a vacancy of kind k in cell j.
    bool TakeJob(int h, int j, int k, bool named) {
        CellSim& H = sim[(size_t)h];
        CellSim& J = sim[(size_t)j];
        if (!J.vacancies[k] || H.working >= H.residents[kAdults]) return false;
        ++H.working;
        if (named) ++H.namedWorking;
        ++J.employed[k];
        J.vacancies[k] = Sub(J.jobCap[k], J.employed[k]);
        Link(superOf_[(size_t)h], superOf_[(size_t)j], 1);
        return true;
    }
    // A named resident of band `band` moves from lot `from` to lot `to` (which must have a free home); a worker keeps
    // their job in job cell j (-1 none). Lot records move with them, so demolishing either lot later stays exact.
    bool MoveResident(uint64_t from, uint64_t to, int band, bool working, int j) {
        const auto fi = recs_.find(from), ti = recs_.find(to);
        if (fi == recs_.end() || ti == recs_.end() || from == to || !fi->second.built || !ti->second.built) return false;
        Rec& F = fi->second;
        Rec& T = ti->second;
        if (!F.res[band] || (uint32_t)T.res[0] + T.res[1] + T.res[2] >= T.cap.homes) return false;
        CellSim& A = sim[(size_t)F.cell];
        CellSim& B = sim[(size_t)T.cell];
        if (working && j >= 0) { // the commute they keep must still be there (a capacity loss may have taken it)
            const auto it = byJob_.find(((uint64_t)superOf_[(size_t)j] << 32) | (uint32_t)superOf_[(size_t)F.cell]);
            if (it == byJob_.end() || it->second <= 0 || !A.working) return false;
        }
        if (band == kAdults && !(working && j >= 0) && A.working >= A.residents[kAdults]) return false; // no jobless adult to move
        if (band == kAdults && !(working && j >= 0) && B.working > B.residents[kAdults]) return false;
        --F.res[band], ++T.res[band];
        A.residents[band] = Sub(A.residents[band], 1), B.residents[band] = AddSat(B.residents[band], 1);
        A.named[band] = Sub(A.named[band], 1), B.named[band] = AddSat(B.named[band], 1);
        if (working && j >= 0) {
            A.working = Sub(A.working, 1), B.working = AddSat(B.working, 1);
            A.namedWorking = Sub(A.namedWorking, 1), B.namedWorking = AddSat(B.namedWorking, 1);
            const int sj = superOf_[(size_t)j];
            Link(superOf_[(size_t)F.cell], sj, -1);
            Link(superOf_[(size_t)T.cell], sj, 1);
        }
        cells.Touch(F.cell, kDirtySim), cells.Touch(T.cell, kDirtySim);
        return true;
    }
    int32_t LinkCount(int sh, int sj) const {
        const auto it = byJob_.find(((uint64_t)sj << 32) | (uint32_t)sh);
        return it == byJob_.end() ? 0 : it->second;
    }
    // Links of home supercell sh: f(jobSuper, workers).
    template <class F>
    void ForEachJobOf(int sh, F&& f) const {
        for (auto it = byHome_.lower_bound((uint64_t)sh << 32); it != byHome_.end() && (int)(it->first >> 32) == sh; ++it) f((int)(uint32_t)it->first, it->second);
    }
    // Every commute link: f(homeSuper, jobSuper, workers), in a fixed order.
    template <class F>
    void ForEachLink(F&& f) const {
        for (const auto& l : byHome_) f((int)(l.first >> 32), (int)(uint32_t)l.first, l.second);
    }

    uint64_t Hash() const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        for (size_t i = 0; i < sim.size(); ++i) {
            const CellSim& c = sim[i];
            mix(PackKey(cells.cells[i].key));
            mix(c.homeCap), mix(c.working);
            for (int k = 0; k < 3; ++k) mix(c.residents[k]), mix(c.employed[k]), mix(c.jobCap[k]);
        }
        for (const auto& l : byJob_) mix(l.first), mix((uint64_t)l.second);
        return h;
    }

    void Clear() {
        cells.Clear();
        sim.clear();
        recs_.clear();
        supers_.clear();
        superIndex_.clear();
        superOf_.clear();
        byJob_.clear();
        byHome_.clear();
        syncedVersion_ = ~0ull;
    }

private:
    struct Rec {
        int cell = -1;
        bool built = false;
        int type = 0, w = 0, d = 0, level = 1;
        std::string path;
        uint32_t gen = 0;
        LotCap cap;
        uint16_t res[kAgeBands] = {}, edu[3] = {}, wealth[4] = {};
    };
    struct Super {
        CellKey key;
        std::vector<int> cells; // in the order they were made
    };

    std::unordered_map<uint64_t, Rec> recs_;
    std::vector<Super> supers_;
    std::unordered_map<uint64_t, int> superIndex_;
    std::vector<int> superOf_; // cell -> supercell
    // Who works where, at supercell resolution: byJob_[(jobSuper << 32) | homeSuper] = byHome_[(homeSuper << 32) | jobSuper].
    std::map<uint64_t, int32_t> byJob_, byHome_;
    uint64_t syncedVersion_ = ~0ull;
    uint32_t generation_ = 0;

    static int FloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

    int CellFor(float x, float z) {
        bool made = false;
        const int c = cells.Get(CellKeyAt(x, z), &made);
        if (made) {
            sim.emplace_back();
            const CellKey k = cells.cells[(size_t)c].key;
            const CellKey sk{FloorDiv(k.x, kSuper), FloorDiv(k.z, kSuper)};
            auto it = superIndex_.find(PackKey(sk));
            if (it == superIndex_.end()) {
                supers_.push_back({sk, {}});
                it = superIndex_.emplace(PackKey(sk), (int)supers_.size() - 1).first;
            }
            supers_[(size_t)it->second].cells.push_back(c);
            superOf_.push_back(it->second);
        }
        return c;
    }

    // Fixed shares, largest remainder so the parts always add up to the whole.
    template <int N>
    static void Split(uint32_t total, const int (&pct)[N], uint16_t (&out)[N]) {
        uint32_t used = 0;
        int rem[N];
        for (int i = 0; i < N; ++i) {
            out[i] = (uint16_t)(total * (uint32_t)pct[i] / 100u);
            rem[i] = (int)(total * (uint32_t)pct[i] % 100u);
            used += out[i];
        }
        while (used < total) {
            int best = 0;
            for (int i = 1; i < N; ++i)
                if (rem[i] > rem[best]) best = i;
            ++out[best];
            rem[best] = -1;
            ++used;
        }
    }

    static uint16_t Sub(uint16_t a, uint16_t b) { return a > b ? (uint16_t)(a - b) : 0; }
    static uint16_t AddSat(uint16_t a, uint32_t b) { return (uint16_t)std::min<uint32_t>(65535u, a + b); }

    void Add(Rec& r) {
        r.built = true;
        r.cap = LotCapacity(r.type, r.w, r.d, r.level);
        static const int kAge[3] = {20, 65, 15}, kEdu[3] = {30, 50, 20}, kWealth[4] = {25, 35, 30, 10};
        // 5% of homes stay free (at least one in a lot of two or more): people moving house need somewhere to go
        const uint32_t occupied = externalPeople ? 0u : r.cap.homes >= 2 ? r.cap.homes - std::max<uint32_t>(1u, r.cap.homes / 20u) : r.cap.homes;
        Split(occupied, kAge, r.res);
        Split(r.res[kAdults], kEdu, r.edu);
        Split(occupied, kWealth, r.wealth);
        CellSim& c = sim[(size_t)r.cell];
        c.homeCap = AddSat(c.homeCap, r.cap.homes);
        for (int k = 0; k < kJobKinds; ++k) {
            c.jobCap[k] = AddSat(c.jobCap[k], r.cap.jobs[k]);
            c.vacancies[k] = Sub(c.jobCap[k], c.employed[k]);
        }
        for (int a = 0; a < kAgeBands; ++a) c.residents[a] = AddSat(c.residents[a], r.res[a]);
        for (int e = 0; e < 3; ++e) c.edu[e] = AddSat(c.edu[e], r.edu[e]);
        for (int w = 0; w < 4; ++w) c.wealth[w] = AddSat(c.wealth[w], r.wealth[w]);
        cells.Touch(r.cell, kDirtySim);
    }

    void Remove(Rec& r) {
        CellSim& c = sim[(size_t)r.cell];
        c.homeCap = Sub(c.homeCap, r.cap.homes);
        for (int a = 0; a < kAgeBands; ++a) c.residents[a] = Sub(c.residents[a], r.res[a]);
        for (int e = 0; e < 3; ++e) c.edu[e] = Sub(c.edu[e], r.edu[e]);
        for (int w = 0; w < 4; ++w) c.wealth[w] = Sub(c.wealth[w], r.wealth[w]);
        if (!externalPeople && c.working > c.residents[kAdults]) Quit(r.cell, (uint32_t)(c.working - c.residents[kAdults]));
        for (int k = 0; k < kJobKinds; ++k) {
            c.jobCap[k] = Sub(c.jobCap[k], r.cap.jobs[k]);
            if (!externalPeople && c.employed[k] > c.jobCap[k]) Fire(r.cell, k, (uint32_t)(c.employed[k] - c.jobCap[k]));
            c.vacancies[k] = Sub(c.jobCap[k], c.employed[k]);
        }
        r.built = false;
        r.cap = {};
        cells.Touch(r.cell, kDirtySim);
    }

    void Link(int sh, int sj, int32_t n) {
        const uint64_t kj = ((uint64_t)sj << 32) | (uint32_t)sh, kh = ((uint64_t)sh << 32) | (uint32_t)sj;
        if ((byJob_[kj] += n) <= 0) byJob_.erase(kj);
        if ((byHome_[kh] += n) <= 0) byHome_.erase(kh);
    }

    // Up to n unemployed adults of supercell sh take vacancies in supercell sj. Returns how many.
    uint32_t Hire(int sh, int sj, uint32_t n) {
        uint32_t done = 0;
        auto& homes = supers_[(size_t)sh].cells;
        auto& jobs = supers_[(size_t)sj].cells;
        size_t hi = 0;
        for (int jc : jobs) {
            CellSim& J = sim[(size_t)jc];
            for (int k = 0; k < kJobKinds && done < n; ++k) {
                while (J.vacancies[k] && done < n) {
                    while (hi < homes.size() && !sim[(size_t)homes[hi]].Unemployed()) ++hi;
                    if (hi == homes.size()) goto out;
                    CellSim& H = sim[(size_t)homes[hi]];
                    const uint32_t t = std::min<uint32_t>({J.vacancies[k], H.Unemployed(), n - done});
                    J.employed[k] = (uint16_t)(J.employed[k] + t);
                    J.vacancies[k] = (uint16_t)(J.vacancies[k] - t);
                    H.working = (uint16_t)(H.working + t);
                    done += t;
                }
            }
            if (done >= n) break;
        }
    out:
        if (done) Link(sh, sj, (int32_t)done);
        return done;
    }

    // Job cell `cell` lets n workers of kind k go: through its supercell's links, from home cells there that have workers.
    void Fire(int cell, int k, uint32_t n) {
        const int sj = superOf_[(size_t)cell];
        CellSim& J = sim[(size_t)cell];
        n = std::min<uint32_t>(n, J.employed[k]);
        J.employed[k] = (uint16_t)(J.employed[k] - n);
        while (n) {
            auto it = byJob_.lower_bound((uint64_t)sj << 32);
            if (it == byJob_.end() || (int)(it->first >> 32) != sj) break; // cannot happen while the invariants hold
            const int sh = (int)(uint32_t)it->first;
            uint32_t m = std::min<uint32_t>(n, (uint32_t)it->second);
            const uint32_t taken = m;
            for (int pass = 0; pass < 2 && m; ++pass) // anonymous workers first; named ones only if there are no others
                for (int hc : supers_[(size_t)sh].cells) {
                    CellSim& H = sim[(size_t)hc];
                    const uint32_t avail = pass == 0 ? H.AnonWorking() : H.namedWorking;
                    const uint32_t t = std::min<uint32_t>(m, avail);
                    H.working = (uint16_t)(H.working - t);
                    if (pass == 1) H.namedWorking = (uint16_t)(H.namedWorking - t), H.namedLost = AddSat(H.namedLost, t);
                    m -= t;
                    if (!m) break;
                }
            Link(sh, sj, -(int32_t)taken);
            n -= taken;
        }
    }

    // Home cell `cell` loses n working adults: they leave jobs in the supercells their links point to.
    void Quit(int cell, uint32_t n) {
        const int sh = superOf_[(size_t)cell];
        CellSim& H = sim[(size_t)cell];
        n = std::min<uint32_t>(n, H.working);
        H.working = (uint16_t)(H.working - n);
        while (n) {
            auto it = byHome_.lower_bound((uint64_t)sh << 32);
            if (it == byHome_.end() || (int)(it->first >> 32) != sh) break;
            const int sj = (int)(uint32_t)it->first;
            uint32_t m = std::min<uint32_t>(n, (uint32_t)it->second);
            const uint32_t taken = m;
            for (int jc : supers_[(size_t)sj].cells) {
                CellSim& J = sim[(size_t)jc];
                for (int k = 0; k < kJobKinds && m; ++k) {
                    const uint32_t t = std::min<uint32_t>(m, J.employed[k]);
                    J.employed[k] = (uint16_t)(J.employed[k] - t);
                    J.vacancies[k] = Sub(J.jobCap[k], J.employed[k]);
                    m -= t;
                }
                if (!m) break;
            }
            Link(sh, sj, -(int32_t)taken);
            n -= taken;
        }
    }
};

} // namespace city
} // namespace tbd
