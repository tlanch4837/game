// Phase 5 mode choice (CS2 sim roadmap, Phase 5): walk, car, transit or taxi for one origin-destination trip, by a
// multinomial logit on generalised cost (minutes + money / value of time) per wealth band, with car ownership by band.
// Header only, headless (tests\sim_transit_test). Pure: the same inputs give the same shares on any machine.
//
//   ModeTrip     what the trip looks like: straight-line metres, car minutes (0: from the distance), the transit option
//                (sim_transit_net.h TransitNet::Best), whether taxis exist, the parking penalty at the destination
//   ChooseModes  shares per mode (sum 1). Car owners choose among all four, the others among walk, transit and taxi.
//   Quantise     shares to permille summing to exactly 1000 (largest remainder): what the world stores
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tbd {
namespace city {

enum TravelMode : int { kModeWalk = 0, kModeCar = 1, kModeTransit = 2, kModeTaxi = 3, kModeCount = 4 };
inline const char* TravelModeName(int m) {
    static const char* const k[kModeCount] = {"Walk or bike", "Car", "Public transport", "Taxi"};
    return m >= 0 && m < kModeCount ? k[m] : "";
}

struct ModeParams {
    float walkMetresPerMin = 90.0f;   // walking and cycling mixed (about 5.4 km/h)
    float maxWalkMetres = 4500.0f;    // past this nobody walks
    float carKmh = 32.0f;             // door to door when no route minutes are known
    float carFixedMin = 4.0f;         // to the car, parking, to the door
    float carCostPerKm = 0.25f;       // fuel and wear (currency)
    float parkingCost = 1.0f;         // per trip at the destination
    float taxiWaitMin = 6.0f;
    float taxiBase = 3.0f, taxiPerKm = 1.6f;
    float valueOfTime[4] = {0.15f, 0.22f, 0.35f, 0.65f}; // currency per minute, by wealth band (low .. rich)
    float carOwn[4] = {0.15f, 0.85f, 0.95f, 1.0f};        // households with a car, by band (P1 gives band 0 none)
    float beta = 0.10f;               // utility per generalised minute
    float asc[kModeCount] = {-0.4f, 0.0f, -0.7f, -1.6f}; // mode constants (comfort, habit)
};

struct ModeTrip {
    float metres = 0.0f;       // straight line
    float carMin = 0.0f;       // by road; 0: from the distance
    bool transit = false;      // a line reaches (walk + wait + ride + walk)
    float transitMin = 0.0f;
    float fare = 0.0f;         // currency, already scaled by the fare policy
    bool taxi = false;         // the city has taxis
    float parkingMin = 0.0f;   // extra minutes to park at the destination (no parking: high)
};

struct ModeShares {
    float s[kModeCount] = {0, 0, 0, 0};
};

inline float CarMinutes(const ModeParams& p, const ModeTrip& t) {
    const float road = t.metres * 1.3f; // streets are not straight
    const float m = t.carMin > 0.0f ? t.carMin : road / (p.carKmh * 1000.0f / 60.0f);
    return m + p.carFixedMin + t.parkingMin;
}

// Shares for a trip made by people of the given wealth mix (residents per band; any scale, all zero = modest).
inline ModeShares ChooseModes(const ModeParams& p, const ModeTrip& t, const float wealth[4]) {
    ModeShares out;
    float wsum = 0.0f;
    for (int b = 0; b < 4; ++b) wsum += std::max(0.0f, wealth[b]);
    const float km = t.metres * 1.3f / 1000.0f;
    const float carMin = CarMinutes(p, t);
    const float walkMin = t.metres * 1.25f / p.walkMetresPerMin;
    for (int b = 0; b < 4; ++b) {
        const float wb = wsum > 0.0f ? std::max(0.0f, wealth[b]) / wsum : (b == 1 ? 1.0f : 0.0f);
        if (wb <= 0.0f) continue;
        const float vot = std::max(0.01f, p.valueOfTime[b]);
        float cost[kModeCount];
        bool on[kModeCount] = {t.metres <= p.maxWalkMetres, true, t.transit, t.taxi};
        cost[kModeWalk] = walkMin;
        cost[kModeCar] = carMin + (km * p.carCostPerKm + p.parkingCost) / vot;
        cost[kModeTransit] = t.transitMin + t.fare / vot;
        cost[kModeTaxi] = carMin - p.carFixedMin + p.taxiWaitMin + (p.taxiBase + km * p.taxiPerKm) / vot;
        for (int own = 0; own < 2; ++own) { // own = 1: the household has a car
            const float wo = own ? p.carOwn[b] : 1.0f - p.carOwn[b];
            if (wo <= 0.0f) continue;
            float u[kModeCount], best = -1e30f;
            for (int m = 0; m < kModeCount; ++m) {
                const bool avail = on[m] && (m != kModeCar || own);
                u[m] = avail ? p.asc[m] - p.beta * cost[m] : -1e30f;
                best = std::max(best, u[m]);
            }
            if (best <= -1e29f) { out.s[kModeTaxi] += wb * wo; continue; } // nothing else: someone drives them
            float e[kModeCount], es = 0.0f;
            for (int m = 0; m < kModeCount; ++m) e[m] = u[m] <= -1e29f ? 0.0f : std::exp(u[m] - best), es += e[m];
            for (int m = 0; m < kModeCount; ++m) out.s[m] += wb * wo * e[m] / es;
        }
    }
    return out;
}

// Permille per mode, summing to exactly 1000 (largest remainder, ties to the lower mode index).
inline void QuantiseShares(const ModeShares& s, uint16_t out[kModeCount]) {
    float sum = 0.0f;
    for (float v : s.s) sum += std::max(0.0f, v);
    if (sum <= 0.0f) { out[0] = 0, out[1] = 1000, out[2] = out[3] = 0; return; }
    int total = 0;
    float rem[kModeCount];
    for (int m = 0; m < kModeCount; ++m) {
        const float v = std::max(0.0f, s.s[m]) * 1000.0f / sum;
        out[m] = (uint16_t)std::floor(v), rem[m] = v - (float)out[m], total += out[m];
    }
    while (total < 1000) {
        int bi = 0;
        for (int m = 1; m < kModeCount; ++m) if (rem[m] > rem[bi]) bi = m;
        ++out[bi], rem[bi] = -1.0f, ++total;
    }
}

} // namespace city
} // namespace tbd
