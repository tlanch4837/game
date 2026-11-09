// Phase 4 service catalog (CS2 sim roadmap Phase 4): the ServiceSpec side table. Header only, headless
// (tests\sim_svc_coverage_test). Every service building of the transport catalog (transport.h BuildingDef, by tab and
// name) gets what the old radius model never had: a capacity (people it can serve), physical units (beds, jail cells,
// body storage, school seats, letters, bandwidth), vehicles, and how far its coverage reaches in travel minutes.
//   road = 1   coverage by road travel time from the building's node (route trees, sim_svc_coverage.h)
//   road = 0   coverage as the crow flies (telecom masts, parks: kSvcCrowMetresPerMin metres count as a minute)
// Park and post buildings are placeholders (transport.h has no models for them yet; parks sit on the Healthcare tab and
// post on Communications until a TTab of their own and the approved models exist).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "sim_factors.h"

namespace tbd {
namespace city {

enum SvcUnit : uint8_t { kUnitNone = 0, kUnitBeds, kUnitCells, kUnitBodies, kUnitSeats, kUnitTonnes, kUnitLetters, kUnitBandwidth };
constexpr float kSvcCrowMetresPerMin = 250.0f;

struct SvcSpec {
    int8_t slot = -1;      // ServiceV2 (sim_factors.h); -1: not a service
    uint8_t unit = kUnitNone;
    uint8_t road = 1;      // 1 by road travel time, 0 as the crow flies
    uint8_t burns = 0;     // deathcare: a crematorium (processes bodies) rather than a cemetery (stores them for good)
    uint32_t units = 0;    // beds, cells, body places, seats, letters a day, bandwidth
    uint32_t serves = 0;   // people (residents + half the workers) it covers at full capacity
    uint16_t vehicles = 0; // engines, patrol cars, ambulances, hearses, mail vans
    float reachMin = 10;   // travel minutes where its coverage ends
    bool Valid() const { return slot >= 0; }
};

// The transport tab numbers (transport.h TTab) this catalog reads, kept here so the header stays light.
enum SvcTab : int { kTabRail = 0, kTabShip, kTabAir, kTabBus, kTabFire, kTabHealth, kTabDeath, kTabPolice, kTabEducation, kTabComms };

inline bool SvcNameHas(const char* name, const char* part) { return name && std::strstr(name, part) != nullptr; }

// The spec of a service building from its catalog entry (tab, name, workplaces, vehicles, lot size in 8 m cells).
inline SvcSpec SvcSpecFor(int tab, const char* name, int workplaces, int vehicles, int w, int d) {
    SvcSpec s;
    const uint32_t wp = (uint32_t)std::max(1, workplaces), area = (uint32_t)std::max(1, w * d);
    s.vehicles = (uint16_t)std::max(0, vehicles);
    switch (tab) {
    case kTabFire: s.slot = kSvcFire, s.serves = 4000u * std::max(1u, (uint32_t)vehicles), s.reachMin = 8; break;
    case kTabPolice:
        s.slot = kSvcPolice, s.unit = kUnitCells, s.reachMin = 8;
        s.units = SvcNameHas(name, "Jail") ? wp * 10 : wp / 2;
        s.serves = 5000u * std::max(1u, (uint32_t)vehicles);
        break;
    case kTabHealth:
        if (SvcNameHas(name, "Park") || SvcNameHas(name, "Plaza")) { // placeholder parks (no model yet)
            s.slot = kSvcPark, s.road = 0, s.serves = area * 300, s.reachMin = 700.0f / kSvcCrowMetresPerMin, s.vehicles = 0;
            break;
        }
        s.slot = kSvcHealth, s.unit = kUnitBeds, s.units = wp * 2, s.serves = wp * 250, s.reachMin = 10;
        break;
    case kTabDeath:
        s.slot = kSvcDeathcare, s.unit = kUnitBodies, s.reachMin = 12, s.serves = wp * 1500;
        s.burns = SvcNameHas(name, "Crematorium") ? 1 : 0;
        s.units = s.burns ? wp * 2 : area * 25; // places in the cold room, or graves
        break;
    case kTabEducation: {
        const bool uni = SvcNameHas(name, "College") || SvcNameHas(name, "University");
        const bool high = !uni && SvcNameHas(name, "High");
        s.slot = (int8_t)(uni ? kSvcUniversity : high ? kSvcHighSchool : kSvcSchool);
        s.unit = kUnitSeats, s.units = wp * 25, s.serves = s.units * (uni ? 12 : 6), s.reachMin = uni ? 25.0f : high ? 18.0f : 12.0f;
        break;
    }
    case kTabComms:
        if (SvcNameHas(name, "Post")) { // placeholder post buildings (no model yet)
            const bool sorting = SvcNameHas(name, "Sorting");
            s.slot = kSvcPost, s.unit = kUnitLetters, s.units = wp * 400, s.serves = sorting ? 60000 : 20000;
            s.reachMin = sorting ? 20.0f : 10.0f, s.vehicles = (uint16_t)std::max(sorting ? 10 : 4, vehicles);
            break;
        }
        s.slot = kSvcTelecom, s.road = 0, s.unit = kUnitBandwidth, s.units = wp * 100, s.serves = wp * 1500, s.vehicles = 0;
        s.reachMin = (SvcNameHas(name, "Mast") ? 4000.0f : SvcNameHas(name, "Center") ? 2500.0f : SvcNameHas(name, "Tower") ? 1500.0f : 600.0f) /
                     kSvcCrowMetresPerMin;
        break;
    case kTabBus: case kTabRail: s.slot = kSvcTransit, s.serves = 30000, s.reachMin = 6, s.vehicles = 0; break;
    default: break;
    }
    return s;
}

// Funding (the budget slider, 0 .. 1.5) scales capacity, units and vehicles (at least one vehicle while funded).
inline SvcSpec SvcFunded(SvcSpec s, float funding) {
    const float f = std::clamp(funding, 0.0f, 1.5f);
    s.serves = (uint32_t)(s.serves * f), s.units = (uint32_t)(s.units * f);
    if (s.vehicles) s.vehicles = (uint16_t)(f > 0.0f ? std::max(1, (int)(s.vehicles * f + 0.5f)) : 0);
    return s;
}

} // namespace city
} // namespace tbd
