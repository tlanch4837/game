// The city sim's calendar (CS2 sim roadmap, Phase 0; frozen interface). Header only, headless (tests\sim_calendar_test).
//
// One sim tick is one game minute (sim.h). A game day is 1440 minutes. The life calendar runs faster than the clock, as in
// CS2: one game day is one life month (lifeDaysPerMonth = 1), lifeMonthsPerYear = 12, so a life year passes in 12 game
// days (about 16 real minutes at the fastest speed). Ages, births, seasons and yearly reports count life months.
//   day        game days since minute 0 (0-based)
//   lifeMonth  life months since minute 0 (0-based); month of year = lifeMonth % 12 (0 = January)
//   year       life years since minute 0 (0-based); the HUD shows year + 1
//   weekday    0 Monday .. 6 Sunday, of the game day (commutes, weekend leisure)
//   season     0 spring (Mar-May), 1 summer (Jun-Aug), 2 autumn (Sep-Nov), 3 winter (Dec-Feb): 4 x 3 life months
//   daylight   0 night .. 1 full day, from the hour the same way sun.h's Sun::Direction() puts the sun up (06 to 18)
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tbd {
namespace city {

constexpr int64_t kMinutesPerDay = 1440;

enum Season : int { kSpring = 0, kSummer = 1, kAutumn = 2, kWinter = 3, kSeasons = 4 };

struct SimDate {
    int64_t minute = 0;   // the tick
    int64_t day = 0;      // game days since 0
    int minuteOfDay = 0;  // 0..1439
    int hour = 0;         // 0..23
    int weekday = 0;      // 0 Monday .. 6 Sunday
    int64_t lifeMonth = 0;// life months since 0
    int monthOfYear = 0;  // 0..11
    int64_t year = 0;     // life years since 0
    int season = kSpring;
    float daylight = 0.0f;
};

struct SimCalendar {
    int lifeDaysPerMonth = 1;   // game days per life month (the roadmap's decision: 1 game day = 1 life month)
    int lifeMonthsPerYear = 12; // tuning `lifeDaysPerYear` in the roadmap: life months in a life year
    int startWeekday = 0;       // weekday of day 0
    float sunTiltDeg = 22.4f;   // sun.h Sun::tiltDeg default

    static int64_t FloorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }
    static int64_t Mod(int64_t a, int64_t b) { const int64_t m = a % b; return m < 0 ? m + b : m; }

    int64_t Day(int64_t minute) const { return FloorDiv(minute, kMinutesPerDay); }
    int64_t LifeMonth(int64_t minute) const { return FloorDiv(Day(minute), std::max(1, lifeDaysPerMonth)); }
    int64_t Year(int64_t minute) const { return FloorDiv(LifeMonth(minute), std::max(1, lifeMonthsPerYear)); }
    int MonthOfYear(int64_t minute) const { return (int)Mod(LifeMonth(minute), std::max(1, lifeMonthsPerYear)); }
    int Weekday(int64_t minute) const { return (int)Mod(Day(minute) + startWeekday, 7); }
    bool Weekend(int64_t minute) const { return Weekday(minute) >= 5; }
    // Season from the month of a 12-month year; other year lengths are scaled onto 12.
    int SeasonOf(int64_t minute) const {
        const int n = std::max(1, lifeMonthsPerYear);
        const int m12 = (int)(MonthOfYear(minute) * 12 / n);
        return ((m12 + 10) % 12) / 3; // Mar..May 0, Jun..Aug 1, Sep..Nov 2, Dec..Feb 3
    }
    // 0..1: the sun's height through the day (sun.h: elevation = cos((hour - 12) * 15 deg) * cos(tilt)), eased so that the
    // light is full a little after sunrise.
    float Daylight(int64_t minute) const {
        const double hour = (double)Mod(minute, kMinutesPerDay) / 60.0;
        const double kPi = 3.14159265358979323846;
        const double up = std::cos((hour - 12.0) * 15.0 * kPi / 180.0) * std::cos(sunTiltDeg * kPi / 180.0);
        const double t = std::clamp((up + 0.05) / 0.35, 0.0, 1.0);
        return (float)(t * t * (3.0 - 2.0 * t));
    }
    // Minutes of the first tick of a life month (for "once a month" systems).
    int64_t MonthStartMinute(int64_t lifeMonth) const { return lifeMonth * std::max(1, lifeDaysPerMonth) * kMinutesPerDay; }
    bool IsMonthStart(int64_t minute) const { return Mod(minute, kMinutesPerDay * std::max(1, lifeDaysPerMonth)) == 0; }
    bool IsYearStart(int64_t minute) const { return IsMonthStart(minute) && MonthOfYear(minute) == 0; }
    // Age in whole life years of someone born in life month bornMonth.
    int AgeYears(int32_t bornMonth, int64_t nowMonth) const { return (int)FloorDiv(nowMonth - bornMonth, std::max(1, lifeMonthsPerYear)); }

    SimDate At(int64_t minute) const {
        SimDate d;
        d.minute = minute;
        d.day = Day(minute);
        d.minuteOfDay = (int)Mod(minute, kMinutesPerDay);
        d.hour = d.minuteOfDay / 60;
        d.weekday = Weekday(minute);
        d.lifeMonth = LifeMonth(minute);
        d.monthOfYear = MonthOfYear(minute);
        d.year = Year(minute);
        d.season = SeasonOf(minute);
        d.daylight = Daylight(minute);
        return d;
    }
};

inline const char* SeasonName(int s) {
    static const char* const k[kSeasons] = {"Spring", "Summer", "Autumn", "Winter"};
    return s >= 0 && s < kSeasons ? k[s] : "";
}
inline const char* MonthName(int m) {
    static const char* const k[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return m >= 0 && m < 12 ? k[m] : "";
}

} // namespace city
} // namespace tbd
