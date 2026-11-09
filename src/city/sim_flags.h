// Per-building problem flags of the city sim. Header only, headless (tests\sim_utilities_test.cpp). The notification
// icons, overlays and the info card read these, nothing else decides them.
//
// CS2 sim roadmap 3.5 (Phase 0; frozen interface): 32 bits, ALL reserved now. Each bit has one owning phase and a phase
// writes only its own bits, in its own ComputeXFlags, through SimFlagTable::Write (which masks to the caller's bits).
// The table is keyed by BuildingId (sim_buildings.h), so a flag survives a lots compaction; ComputeSimFlags below is the
// current sim's per-lot pass (zones.lots order) and SimFlagTable::FromLots moves its bits onto BuildingIds.
//
//   NoPower / NoWater / NoSewage  the utilities solve (utilities.h, via sim_coverage.h LotUtil) could not serve the lot
//   NoWorkers    a workplace whose cell has 40 % or more of its jobs unfilled
//   NoCustomers  a shop while commercial demand is strongly negative (more shops than the residents spend in)
//   Abandoned    a level 1 lot that has been in bad conditions for over half of its patience: sim_growth.h abandons
//                (demolishes) it soon unless things improve
//   the rest     reserved for Phases 1 to 7 (see SimFlagOwner); nothing sets them yet
//
// Efficiency: a cell without power works at 50 %, without water at 70 % (both: 35 %); sim_economy.h scales its tax by it.
#pragma once

#include <cstdint>
#include <vector>

#include "sim_buildings.h"
#include "sim_cells.h"
#include "sim_coverage.h"
#include "sim_growth.h"

namespace tbd {
namespace city {

using SimFlags = uint32_t;

enum SimFlag : uint32_t {
    // the current sim (ComputeSimFlags); bit positions are stable: saves and tests know them
    kFlagNoPower = 1u << 0,
    kFlagNoWater = 1u << 1,
    kFlagNoSewage = 1u << 2,
    kFlagNoWorkers = 1u << 3,
    kFlagNoCustomers = 1u << 4,
    kFlagAbandoned = 1u << 5,
    // reserved (roadmap 3.5): the phase that owns each is in SimFlagOwner
    kFlagHighRent = 1u << 6,         // P2 rent burden over the household's means
    kFlagSick = 1u << 7,             // P4 sick residents or workers waiting for care
    kFlagCrime = 1u << 8,            // P4 crime above the threshold
    kFlagFire = 1u << 9,             // P4 on fire
    kFlagGarbage = 1u << 10,         // P4 garbage piling up
    kFlagNoSchool = 1u << 11,        // P4 children with no school place
    kFlagNoGoods = 1u << 12,         // P6 a shop or factory without input goods
    kFlagNoStudents = 1u << 13,      // P4 a school with too few students
    kFlagPolluted = 1u << 14,        // P3 air, ground or water pollution over the limit
    kFlagFlooded = 1u << 15,         // P3 under water
    kFlagNoTelecom = 1u << 16,       // P4 no network coverage (offices suffer)
    kFlagNoHealthcare = 1u << 17,    // P4 no clinic or hospital in reach
    kFlagDeadUncollected = 1u << 18, // P4 bodies waiting for a hearse
    kFlagNoMail = 1u << 19,          // P4 mail piling up
    kFlagNoisy = 1u << 20,           // P3 noise over the limit
    kFlagNoTransit = 1u << 21,       // P5 no stop in walking range
    kFlagNoEducatedWorkers = 1u << 22, // P1 jobs open for want of schooling
    kFlagNoParking = 1u << 23,       // P5 drivers cannot park
    kFlagDestroyed = 1u << 24,       // P4 burned down or hit by a disaster (rubble)
    kFlagNoFreight = 1u << 25,       // P6 output stuck: no truck, rail or port to ship it
    kFlagCount = 26,                 // named bits; 26 .. 31 spare (still Phase 0's to hand out)
};
constexpr SimFlags kFlagCurrentMask = kFlagNoPower | kFlagNoWater | kFlagNoSewage | kFlagNoWorkers | kFlagNoCustomers | kFlagAbandoned;

inline const char* SimFlagText(int bit) {
    static const char* const k[kFlagCount] = {
        "No electricity", "No water", "No sewage", "Not enough workers", "Not enough customers", "Being abandoned",
        "Rent too high", "Sick citizens", "High crime", "On fire", "Garbage piling up", "No school places",
        "No goods", "Not enough students", "Polluted", "Flooded", "No network coverage", "No healthcare",
        "Dead not collected", "Mail piling up", "Too noisy", "No public transport", "Not enough educated workers",
        "No parking", "Destroyed", "Cannot ship goods"};
    return bit >= 0 && bit < kFlagCount ? k[bit] : "";
}

// The phase (0 = the current sim / Phase 0, 1 .. 7) that alone writes a bit. -1 for a spare bit. No workers (P1), no
// customers (P6) and abandoned (P2) are written by ComputeSimFlags (kFlagCurrentMask) until their phase takes them over.
inline int SimFlagOwner(int bit) {
    static const int8_t k[kFlagCount] = {0, 0, 0, 1, 6, 2, 2, 4, 4, 4, 4, 4, 6, 4, 3, 3, 4, 4, 4, 4, 3, 5, 1, 5, 4, 6};
    return bit >= 0 && bit < kFlagCount ? k[bit] : -1;
}
// Every bit a phase owns.
inline SimFlags SimFlagPhaseMask(int phase) {
    SimFlags m = 0;
    for (int b = 0; b < kFlagCount; ++b) if (SimFlagOwner(b) == phase) m |= 1u << b;
    return m;
}

// The flags of every building, indexed by BuildingId::Index() and checked against its generation: a stale id reads 0
// and is never written. Grows on demand; Prune clears the rows of buildings the registry freed.
class SimFlagTable {
public:
    SimFlags Get(BuildingId b) const {
        const uint32_t i = b.Index();
        return b.Valid() && i < bits_.size() && gen_[i] == b.Gen() ? bits_[i] : 0u;
    }
    bool Has(BuildingId b, SimFlags f) const { return (Get(b) & f) != 0; }
    // Replaces the `mask` bits of b with `bits & mask`; the other bits stay. False if nothing changed.
    bool Write(BuildingId b, SimFlags mask, SimFlags bits) {
        if (!b.Valid()) return false;
        const uint32_t i = b.Index();
        if (i >= bits_.size()) bits_.resize(i + 1, 0), gen_.resize(i + 1, 0);
        if (gen_[i] != b.Gen()) gen_[i] = b.Gen(), bits_[i] = 0; // a new occupant of the slot starts clean
        const SimFlags next = (bits_[i] & ~mask) | (bits & mask);
        if (next == bits_[i]) return false;
        bits_[i] = next, ++version;
        return true;
    }
    // Clears `mask` on every building (a phase's pass starts here, then Writes what it found).
    void ClearAll(SimFlags mask) {
        bool any = false;
        for (SimFlags& f : bits_) any |= (f & mask) != 0, f &= ~mask;
        if (any) ++version;
    }
    void Prune(const BuildingRegistry& reg) {
        for (uint32_t i = 1; i < bits_.size(); ++i)
            if (bits_[i] && reg.At(i).Gen() != gen_[i]) bits_[i] = 0, ++version;
    }
    // The current sim's per-lot flags (zones.lots order) onto the lots' BuildingIds, `mask` bits only.
    void FromLots(const std::vector<Lot>& lots, const BuildingRegistry& reg, const std::vector<SimFlags>& perLot, SimFlags mask) {
        for (size_t i = 0; i < lots.size(); ++i) {
            if (!lots[i].alive) continue;
            const BuildingId b = reg.Find(BOrigin::Lot, lots[i].id);
            if (b.Valid()) Write(b, mask, i < perLot.size() ? perLot[i] : 0u);
        }
    }
    // OR of every building's flags (the alerts count, the HUD's problem kinds).
    SimFlags Any() const { SimFlags a = 0; for (SimFlags f : bits_) a |= f; return a; }
    uint32_t Size() const { return (uint32_t)bits_.size(); }
    void Clear() { bits_.clear(), gen_.clear(), ++version; }
    uint64_t version = 0; // bumps on every change

private:
    std::vector<SimFlags> bits_;
    std::vector<uint8_t> gen_;
};

inline int64_t CellEfficiencyPermille(const CellSim& c) { return (c.powerOk ? 1000 : 500) * (c.waterOk ? 1000 : 700) / 1000; }

// The current sim's pass: kFlagCurrentMask per lot, zones.lots order. lotUtil: sim_coverage.h LotUtil per lot (empty =
// all fine). commercialDemand: SimGrowth::demand[Commercial].
inline void ComputeSimFlags(const ZoneGrid& z, const CitySim& s, const SimGrowth& g, const SimTuning& t, const std::vector<uint8_t>& lotUtil,
                            std::vector<SimFlags>& out) {
    out.assign(z.lots.size(), 0);
    const auto& types = ZoneTypes();
    const float abandonAt = SimGrowth::DownMinutes(t) * 0.5f;
    const double comm = g.demand[(int)ZoneCat::Commercial];
    for (size_t i = 0; i < z.lots.size(); ++i) {
        const Lot& l = z.lots[i];
        if (!l.alive || !l.Built() || l.type < 0 || l.type >= (int)types.size()) continue;
        SimFlags f = 0;
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
