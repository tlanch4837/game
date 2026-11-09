// Headless checks for the city sim's calendar (src/city/sim_calendar.h):
//   day / hour boundaries at 1439 / 1440; one game day is one life month, a life year is 12 game days; month and year
//   starts; negative minutes floor; weekdays and weekends with a start weekday; all 12 months map to the right season and
//   a 4-month year scales onto 12; daylight is 0 at midnight, 1 at noon, rises monotonically through the morning and is
//   symmetric about noon; ages in life years; At() agrees with the single getters; a 2-day life month.
// Build and run: tests\sim_calendar_test.bat
#include <cmath>
#include <cstdio>
#include <cstring>

#include "city/sim_calendar.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static void TestBoundaries() {
    const SimCalendar cal;
    CHECK(cal.Day(0) == 0 && cal.Day(1439) == 0 && cal.Day(1440) == 1);
    const SimDate a = cal.At(1439), b = cal.At(1440);
    CHECK(a.hour == 23 && a.minuteOfDay == 1439 && b.hour == 0 && b.minuteOfDay == 0);
    CHECK(a.lifeMonth == 0 && b.lifeMonth == 1 && a.monthOfYear == 0 && b.monthOfYear == 1);
    CHECK(cal.IsMonthStart(0) && cal.IsMonthStart(1440) && !cal.IsMonthStart(1439) && !cal.IsMonthStart(1441));
    // A life year is 12 game days.
    const int64_t year1 = 12 * kMinutesPerDay;
    CHECK(cal.Year(year1 - 1) == 0 && cal.Year(year1) == 1 && cal.MonthOfYear(year1) == 0 && cal.MonthOfYear(year1 - 1) == 11);
    CHECK(cal.IsYearStart(0) && cal.IsYearStart(year1) && !cal.IsYearStart(kMinutesPerDay) && !cal.IsYearStart(year1 + 1));
    CHECK(cal.MonthStartMinute(13) == 13 * kMinutesPerDay && cal.LifeMonth(cal.MonthStartMinute(13)) == 13);
    // Before minute 0 the calendar floors (no -0 day).
    CHECK(cal.Day(-1) == -1 && cal.LifeMonth(-1) == -1 && cal.MonthOfYear(-1) == 11 && cal.Year(-1) == -1);
    CHECK(cal.At(-1).hour == 23 && cal.At(-1).minuteOfDay == 1439);
    CHECK(SimCalendar::FloorDiv(-1440, 1440) == -1 && SimCalendar::FloorDiv(-1441, 1440) == -2 && SimCalendar::Mod(-1, 7) == 6);
    // Far future (a long-lived city): 1000 life years.
    const int64_t far = 1000ll * 12 * kMinutesPerDay + 5;
    CHECK(cal.Year(far) == 1000 && cal.MonthOfYear(far) == 0 && cal.Day(far) == 12000);
}

static void TestWeekdays() {
    SimCalendar cal;
    for (int d = 0; d < 14; ++d) {
        const int64_t m = d * kMinutesPerDay + 600;
        CHECK(cal.Weekday(m) == d % 7);
        CHECK(cal.Weekend(m) == (d % 7 >= 5));
    }
    cal.startWeekday = 5; // day 0 is a Saturday
    CHECK(cal.Weekday(0) == 5 && cal.Weekend(0) && cal.Weekend(kMinutesPerDay) && !cal.Weekend(2 * kMinutesPerDay));
    CHECK(cal.Weekday(-kMinutesPerDay) == 4);
}

static void TestSeasons() {
    const SimCalendar cal;
    const int expect[12] = {kWinter, kWinter, kSpring, kSpring, kSpring, kSummer, kSummer, kSummer, kAutumn, kAutumn, kAutumn, kWinter};
    for (int m = 0; m < 12; ++m) {
        const int64_t minute = cal.MonthStartMinute(m) + 720;
        CHECK(cal.SeasonOf(minute) == expect[m]);
        CHECK(cal.SeasonOf(minute + 12 * kMinutesPerDay) == expect[m]); // the next year repeats
    }
    // The season changes exactly at a month boundary (Feb -> Mar).
    CHECK(cal.SeasonOf(cal.MonthStartMinute(2) - 1) == kWinter && cal.SeasonOf(cal.MonthStartMinute(2)) == kSpring);
    int count[kSeasons] = {};
    for (int m = 0; m < 12; ++m) ++count[cal.SeasonOf(cal.MonthStartMinute(m))];
    CHECK(count[0] == 3 && count[1] == 3 && count[2] == 3 && count[3] == 3);
    // A 4-month year scales onto 12: Jan, Apr, Jul, Oct.
    SimCalendar q;
    q.lifeMonthsPerYear = 4;
    CHECK(q.SeasonOf(q.MonthStartMinute(0)) == kWinter && q.SeasonOf(q.MonthStartMinute(1)) == kSpring);
    CHECK(q.SeasonOf(q.MonthStartMinute(2)) == kSummer && q.SeasonOf(q.MonthStartMinute(3)) == kAutumn);
    CHECK(q.Year(q.MonthStartMinute(4)) == 1);
    CHECK(std::strcmp(SeasonName(kAutumn), "Autumn") == 0 && SeasonName(4)[0] == 0 && SeasonName(-1)[0] == 0);
    CHECK(std::strcmp(MonthName(11), "Dec") == 0 && MonthName(12)[0] == 0);
}

static void TestDaylight() {
    const SimCalendar cal;
    CHECK(cal.Daylight(0) == 0.0f);                    // midnight
    CHECK(cal.Daylight(12 * 60) == 1.0f);              // noon
    CHECK(cal.Daylight(3 * 60) == 0.0f);               // night
    CHECK(cal.Daylight(21 * 60) == 0.0f);
    CHECK(cal.Daylight(6 * 60) > 0.0f && cal.Daylight(6 * 60) < 1.0f); // dawn is partial
    float prev = -1.0f;
    for (int m = 0; m <= 12 * 60; m += 5) { // rises monotonically to noon
        const float d = cal.Daylight(m);
        CHECK(d >= prev && d >= 0.0f && d <= 1.0f);
        prev = d;
    }
    for (int m = 0; m <= 720; m += 7) CHECK(std::fabs(cal.Daylight(720 - m) - cal.Daylight(720 + m)) < 1e-5f); // symmetric
    CHECK(cal.Daylight(5 * kMinutesPerDay + 600) == cal.Daylight(600)); // same every day
    CHECK(cal.Daylight(-kMinutesPerDay + 600) == cal.Daylight(600));
}

static void TestAgesAndAt() {
    const SimCalendar cal;
    CHECK(cal.AgeYears(0, 0) == 0 && cal.AgeYears(0, 11) == 0 && cal.AgeYears(0, 12) == 1 && cal.AgeYears(5, 5 + 12 * 30) == 30);
    CHECK(cal.AgeYears(10, 9) == -1); // not born yet
    for (int64_t m = -3000; m < 40000; m += 997) {
        const SimDate d = cal.At(m);
        CHECK(d.minute == m && d.day == cal.Day(m) && d.lifeMonth == cal.LifeMonth(m) && d.year == cal.Year(m));
        CHECK(d.monthOfYear == cal.MonthOfYear(m) && d.weekday == cal.Weekday(m) && d.season == cal.SeasonOf(m));
        CHECK(d.daylight == cal.Daylight(m) && d.hour == d.minuteOfDay / 60 && d.minuteOfDay >= 0 && d.minuteOfDay < 1440);
    }
}

static void TestLongMonths() {
    SimCalendar cal;
    cal.lifeDaysPerMonth = 2;
    CHECK(cal.LifeMonth(2 * kMinutesPerDay - 1) == 0 && cal.LifeMonth(2 * kMinutesPerDay) == 1);
    CHECK(cal.IsMonthStart(2 * kMinutesPerDay) && !cal.IsMonthStart(kMinutesPerDay));
    CHECK(cal.MonthStartMinute(3) == 6 * kMinutesPerDay && cal.Year(24 * kMinutesPerDay) == 1);
    cal.lifeDaysPerMonth = 0; // a bad value is clamped to 1, not a division by zero
    CHECK(cal.LifeMonth(kMinutesPerDay) == 1 && cal.IsMonthStart(kMinutesPerDay));
}

int main() {
    TestBoundaries();
    TestWeekdays();
    TestSeasons();
    TestDaylight();
    TestAgesAndAt();
    TestLongMonths();
    if (failures) {
        std::printf("sim_calendar_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("sim_calendar_test: all passed\n");
    return 0;
}
