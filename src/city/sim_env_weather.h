// Phase 3 weather and seasons (CS2 sim roadmap, Phase 3). Header only, headless (tests\sim_env_weather_test).
//
// The season comes from SimCalendar (4 x 3 life months). The weather is a seeded Markov chain stepped every 3 game hours:
// state n = Next(state n-1, SimSeed(seed, n)), with season-weighted transitions. It is pure: the state at any step can be
// replayed from step 0 (WeatherChain::Replay), so a save only stores it to skip the replay (WTHR). Temperature, rain,
// snow, wind and the solar / wind output factors (for utilities.h, read through a getter) follow from the state, the
// calendar and per-step hashes. Everything is integer or quantised: the same seed gives the same weather on any machine.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "sim.h"
#include "sim_calendar.h"

namespace tbd {
namespace city {

enum WeatherKind : uint8_t { kWxClear = 0, kWxCloudy = 1, kWxFog = 2, kWxRain = 3, kWxStorm = 4, kWxKinds = 5 };
inline const char* WeatherName(int k, bool snow = false) {
    static const char* const n[kWxKinds] = {"Clear", "Cloudy", "Fog", "Rain", "Storm"};
    if (snow && (k == kWxRain || k == kWxStorm)) return k == kWxStorm ? "Blizzard" : "Snow";
    return k >= 0 && k < kWxKinds ? n[k] : "";
}
constexpr int64_t kWeatherStepMinutes = 180;

// The chain's state after a step: kind and wind direction (0..7, 45 degree steps, 0 = towards +X, 2 = towards +Z).
struct WeatherChain {
    int64_t step = 0;
    uint8_t kind = kWxClear;
    uint8_t windDir = 4; // towards -X: the prevailing wind comes from the east of the map in winter, veers with fronts

    // Season-weighted transition weights [season][from][to].
    static int Weight(int season, int from, int to) {
        static const uint8_t base[kWxKinds][kWxKinds] = {
            {60, 25, 6, 8, 1}, {25, 40, 7, 24, 4}, {35, 30, 30, 5, 0}, {10, 30, 4, 46, 10}, {5, 25, 0, 45, 25}};
        static const uint8_t bySeason[kSeasons][kWxKinds] = { // multiplier (x / 4) per target kind
            {4, 4, 4, 5, 3}, {6, 3, 2, 3, 5}, {3, 5, 6, 6, 4}, {3, 5, 5, 5, 3}};
        return base[from][to] * bySeason[season][to];
    }
    static WeatherChain Next(const WeatherChain& s, uint64_t seed, const SimCalendar& cal) {
        WeatherChain n = s;
        n.step = s.step + 1;
        const uint64_t h = SimSeed(seed, (uint64_t)n.step, 0x5745); // "WE"
        const int season = cal.SeasonOf(n.step * kWeatherStepMinutes);
        int sum = 0;
        for (int k = 0; k < kWxKinds; ++k) sum += Weight(season, s.kind, k);
        int pick = (int)(h % (uint64_t)std::max(1, sum));
        for (int k = 0; k < kWxKinds; ++k) {
            pick -= Weight(season, s.kind, k);
            if (pick < 0) { n.kind = (uint8_t)k; break; }
        }
        const int veer = (int)((h >> 32) % 8u); // 0..7: back one, steady (x5), veer one, or (on a front change) two
        int d = s.windDir + (veer == 0 ? -1 : veer == 7 ? 1 : 0);
        if (n.kind != s.kind && (n.kind == kWxStorm || n.kind == kWxRain)) d += (int)((h >> 40) % 3u) - 1;
        n.windDir = (uint8_t)((d % 8 + 8) % 8);
        return n;
    }
    // The state at `step`, from step 0 (or from `from` when it is not past `step`).
    static WeatherChain Replay(int64_t step, uint64_t seed, const SimCalendar& cal, const WeatherChain* from = nullptr) {
        WeatherChain s = from && from->step <= step ? *from : WeatherChain();
        while (s.step < step) s = Next(s, seed, cal);
        return s;
    }
};

// The weather at one minute (derived, never stored).
struct EnvWeather {
    int season = kSpring;
    uint8_t kind = kWxClear;
    bool snow = false;
    float tempC = 10.0f;    // 0.1 degree steps
    float rainMmH = 0.0f;   // precipitation, mm per hour (snow counts as its water)
    float cloud = 0.0f;     // 0..1
    float windMs = 4.0f;    // 0.1 m/s steps
    int windDir = 4;        // 0..7
    int windX = 0, windZ = 0; // the direction as whole tiles (-1..1), for the drift of air pollution
    float daylight = 0.0f;
    float solar = 0.0f;     // solar output factor 0..1 (0 at night)
    float windPower = 0.0f; // wind turbine output factor 0..1
};

inline float EnvQuant(float v, float step) { return std::round(v / step) * step; }

inline EnvWeather WeatherAt(const WeatherChain& c, int64_t minute, uint64_t seed, const SimCalendar& cal) {
    EnvWeather w;
    const double kTwoPi = 6.28318530717958647692;
    const SimDate d = cal.At(minute);
    w.season = d.season, w.kind = c.kind, w.windDir = c.windDir, w.daylight = d.daylight;
    const uint64_t h = SimSeed(seed, (uint64_t)c.step, 0x5458); // per step noise
    const float n1 = (float)(h % 1000u) / 999.0f, n2 = (float)((h >> 20) % 1000u) / 999.0f;
    static const float kOff[kWxKinds] = {1.0f, -1.0f, -1.5f, -3.0f, -4.5f};
    static const float kCloud[kWxKinds] = {0.1f, 0.65f, 0.8f, 0.85f, 1.0f};
    static const float kWind[kWxKinds] = {4.0f, 5.5f, 1.5f, 6.5f, 14.0f};
    const double month = d.monthOfYear * 12.0 / std::max(1, cal.lifeMonthsPerYear);
    const double mean = 10.0 - 12.0 * std::cos(kTwoPi * month / 12.0); // Jan -2, Jul 22
    const double daily = 4.0 * std::cos(kTwoPi * (d.minuteOfDay / 60.0 - 15.0) / 24.0); // peak at 15:00
    w.tempC = EnvQuant((float)(mean + daily) + kOff[c.kind] + (n1 - 0.5f) * 3.0f, 0.1f);
    w.cloud = kCloud[c.kind];
    w.rainMmH = c.kind == kWxRain ? EnvQuant(2.0f + 3.0f * n2, 0.1f) : c.kind == kWxStorm ? EnvQuant(10.0f + 15.0f * n2, 0.1f) : 0.0f;
    w.snow = w.rainMmH > 0.0f && w.tempC <= 0.5f;
    const float seasonWind = d.season == kWinter ? 1.5f : d.season == kSummer ? -1.0f : 0.0f;
    w.windMs = EnvQuant(std::max(0.0f, kWind[c.kind] + seasonWind + (n2 - 0.5f) * 6.0f), 0.1f);
    static const int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, kDz[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    w.windX = w.windMs >= 2.0f ? kDx[w.windDir & 7] : 0, w.windZ = w.windMs >= 2.0f ? kDz[w.windDir & 7] : 0;
    w.solar = EnvQuant(w.daylight * (1.0f - 0.75f * w.cloud) * (w.snow ? 0.7f : 1.0f), 0.01f);
    const float v = w.windMs;
    w.windPower = v < 3.0f || v > 25.0f ? 0.0f : EnvQuant(std::min(1.0f, std::pow((v - 3.0f) / 10.0f, 3.0f)), 0.01f);
    return w;
}

} // namespace city
} // namespace tbd
