// Phase 5 transit world: zones (the host's supercells plus the outside connections), trip links between them, the
// mode choice of every link, ridership on the lines, taxis, parking pressure. Header only, headless
// (tests\sim_transit_test). Deferred simulation (the user's rule for every phase):
//   - a link is evaluated only when it is dirty: new, or one of its zones changed (population or wealth past a
//     threshold, its stop access, its parking level), or a city-wide input changed (fare, taxis); clean links cost 0
//   - dirty links are drained in index order, at most linksPerTick per game minute; zone access is recomputed in
//     groups of zonesPerTick after the lines change (round robin, no spike)
//   - totals (mode trips, riders, parking arrivals) are integers kept incrementally from the link results; Verify()
//     recomputes everything from scratch and counts what differs (the stale-state check)
// Order of zones and links only depends on the inputs, so two worlds fed the same give the same bytes.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sim_modes.h"
#include "sim_transit_ridership.h"

namespace tbd {
namespace city {

struct TransitZoneIn {
    uint64_t key = 0;          // stable id (the host packs a supercell key, or an outside connection's id | bit 63)
    float x = 0, z = 0;
    uint32_t pop = 0, jobs = 0;
    float wealth[4] = {0, 1, 0, 0}; // residents (or visitors) by band
    float carOwn = 1.0f;       // x ModeParams::carOwn (0: visitors without a car)
};
struct TransitLinkIn {
    uint64_t from = 0, to = 0; // zone keys
    float n = 0;               // people a day who go from -> to and come back
};

struct TransitParams {
    ModeParams mode;
    TransitNetParams net;
    float fare = 1.0f;          // per boarding (currency)
    float fareMul = 1.0f;       // the policy multiplier (PolicyMods::transitFare)
    int taxis = 0;              // taxis in service (the depots' vehicles)
    int tripsPerTaxi = 28;      // a taxi's trips a day
    int linksPerTick = 192;     // dirty links evaluated per game minute at most
    int zonesPerTick = 48;      // zone access lists recomputed per game minute at most
    float popDirty = 0.08f;     // a zone's population or wealth mix moving this much re-evaluates its links
    int popDirtyMin = 25;
    float linkDirty = 0.06f;    // a link's people a day moving less than this (and linkDirtyMin) keeps the old count
    int linkDirtyMin = 3;
    float parkPerJob = 0.6f, parkPerHome = 0.5f, parkBase = 150.0f; // spaces a zone has
};

struct TransitZone {
    uint64_t key = 0;
    float x = 0, z = 0;
    uint32_t pop = 0, jobs = 0;
    float wealth[4] = {0, 1, 0, 0};
    float carOwn = 1.0f;
    uint32_t evalPop = 0;          // pop and wealth the zone's links were last evaluated with (the dirty threshold)
    float evalWealth[4] = {0, 1, 0, 0};
    uint32_t accessSig = 0;
    uint8_t parking = 0;           // 0 fine .. 3 none to be had (minutes added to drivers arriving)
    std::vector<TransitAccess> access;
};

struct TransitLink {
    int32_t from = 0, to = 0;      // zone indices
    int32_t n = 0;                 // people a day (each makes the trip there and back)
    uint16_t share[kModeCount] = {0, 1000, 0, 0};
    TransitPath out, back;
    uint8_t evaluated = 0;
};

class TransitSim {
public:
    TransitParams p;
    TransitNet net;
    std::vector<LineInput> lineIn;  // what net was built from (saved: the paths' line indices refer to it)
    RidershipLedger ledger;
    std::vector<TransitZone> zones;
    std::vector<TransitLink> links; // sorted by (from, to)
    std::vector<uint8_t> dirty;     // per link
    int64_t modeMille[kModeCount] = {0, 0, 0, 0}; // trips a day x 1000, by mode (both directions)
    std::vector<int64_t> parkMille;               // per zone: car arrivals a day x 1000
    int64_t evaluations = 0;        // links evaluated ever (the bench and the test watch it)
    uint64_t version = 0;           // bumps when a link result changes
    size_t accessCursor = 0;        // zones still waiting for their access list
    int dirtyCount = 0;

    void Clear() { const TransitParams keep = p; *this = TransitSim(); p = keep; }
    int Zone(uint64_t key) const { const auto it = zoneIdx_.find(key); return it == zoneIdx_.end() ? -1 : it->second; }
    int Link(uint64_t fromKey, uint64_t toKey) const;
    bool Pending() const { return dirtyCount > 0 || accessCursor < zones.size(); }

    void SetZones(const std::vector<TransitZoneIn>& in);
    void SetLinks(const std::vector<TransitLinkIn>& in);
    void SetLines(const std::vector<LineInput>& in);
    void SetFare(float fare, float mul);
    void SetTaxis(int taxis);
    void Tick(int64_t minute);       // one game minute of deferred work
    void Drain() { for (int i = 0; i < 100000 && Pending(); ++i) Tick(-1); } // tests and loads: everything due, now
    void SummarizeParking();         // hourly: parking levels from the arrivals (a changed level dirties its links)
    int Verify() const;              // full recompute vs the incremental state: mismatches (0 = nothing stale)
    void RebuildAfterLoad();         // the saved zones and links in, derived state out

    // the evaluation of one link with the current state (pure: no member changes)
    void Evaluate(const TransitLink& l, uint16_t share[kModeCount], TransitPath& out, TransitPath& back) const;
    int64_t TransitRiders(const TransitLink& l) const { return ((int64_t)l.n * l.share[kModeTransit] + 500) / 1000; }
    float Share(int mode) const {
        int64_t s = 0;
        for (int64_t v : modeMille) s += v;
        return s > 0 ? (float)((double)modeMille[mode] / (double)s) : (mode == kModeCar ? 1.0f : 0.0f);
    }

    // (save) the zone index map is derived
    void IndexZones() { zoneIdx_.clear(); for (size_t i = 0; i < zones.size(); ++i) zoneIdx_[zones[i].key] = (int)i; }

private:
    std::unordered_map<uint64_t, int> zoneIdx_;
    std::vector<std::vector<int32_t>> adj_; // per zone: links touching it (either end)
    size_t dirtyCursor_ = 0;

    void Contribute(const TransitLink& l, int sign);
    void MarkLink(size_t i) { if (i < dirty.size() && !dirty[i]) dirty[i] = 1, ++dirtyCount; }
    void MarkZone(int z) { if (z >= 0 && (size_t)z < adj_.size()) for (int32_t i : adj_[(size_t)z]) MarkLink((size_t)i); }
    void BuildAdjacency();
    void RefreshAccess(size_t z);
    bool ZoneMoved(const TransitZone& z) const;
};

} // namespace city
} // namespace tbd

#include "sim_transit_links.h"
#include "sim_transit_eval.h"
