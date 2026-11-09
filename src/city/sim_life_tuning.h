// Phase 1 tuning (CS2 sim roadmap, Phase 1): every value the citizen sim reads, keyed "life.<name>" in tuning.json.
// Header only, headless. Set() ignores keys of other phases (land., env., svc., move., goods., gov.) and unprefixed ones,
// so this struct can be fed the same key/value list as everyone else's (sim_tuning_table.h wires it in later).
// Rates are per life month unless the name says otherwise. Values follow docs/city_sim_design.md section 11.
#pragma once

#include <cstdint>
#include <cstring>

namespace tbd {
namespace city {

#define TBD_LIFE_TUNING(X)                                                                                       \
    X(int, instantFill, 0)           /* 1: free homes fill at once (tests, old saves) */                        \
    X(int, recordCap, 120000)        /* citizens with a record; past it homes fill with statistical residents */ \
    X(float, moveInRate, 0.12f)      /* share of free home capacity that fills per month at attractiveness 1 */  \
    X(float, attractHappy, 64.0f)    /* happiness points (over 128) that double attractiveness */               \
    X(float, attractJobs, 4.0f)      /* weight of (vacancies - seekers) / adults on attractiveness */           \
    X(float, fertility, 1.55f)       /* children per woman over ages 18..44 */                                  \
    X(float, mortA, 0.00005f)        /* Gompertz yearly death hazard a * exp(b * age) */                        \
    X(float, mortB, 0.09f)                                                                                      \
    X(float, mortFloor, 0.0006f)     /* yearly background hazard at any age */                                  \
    X(int, maxAge, 105)                                                                                          \
    X(float, partnerRate, 0.04f)     /* chance a single adult 20..45 pairs up per month (needs a match) */      \
    X(float, leaveHomeRate, 0.02f)   /* chance an adult child 18..34 wants an own home per month */             \
    X(float, eduElementary, 0.97f)   /* chance to finish elementary at 12 with no school coverage: x(0.5+cov) */\
    X(float, eduHigh, 0.80f)         /* finish high school at 18 */                                             \
    X(float, eduCollege, 0.45f)      /* start college at 18 (with high school) */                               \
    X(float, eduUniversity, 0.40f)   /* go on to university after college */                                    \
    X(int, matchHoods, 8)            /* nearest hoods a job seeker looks in (own first) */                       \
    X(int, maxCommute, 75)           /* minutes: no job further than this */                                    \
    X(int, quitCommute, 50)          /* minutes: over this a worker may quit to look again */                   \
    X(float, quitRate, 0.08f)        /* per month, over quitCommute or overqualified */                         \
    X(int, matchPerPass, 3000)       /* seekers handled per 10-minute job pass */                               \
    X(int, retireAge, 65)                                                                                        \
    X(int, wage0, 1100) X(int, wage1, 1500) X(int, wage2, 2000) X(int, wage3, 2700) X(int, wage4, 3500)          \
    X(int, pension, 1200)                                                                                        \
    X(int, rentPerPerson, 330)       /* per month at level 1 and neutral rent burden */                         \
    X(int, spendPerPerson, 200)                                                                                  \
    X(float, spendShare, 0.30f)      /* of what is left after rent */                                           \
    X(int, unhappyBelow, 85)         /* household mean happiness under this counts an unhappy month */          \
    X(int, leaveUnhappyMonths, 6)                                                                                \
    X(float, leaveUnhappyRate, 0.30f)                                                                            \
    X(int, leavePoorMonths, 3)       /* months with savings below zero */                                      \
    X(float, leavePoorRate, 0.45f)                                                                               \
    X(float, moveRate, 0.25f)        /* chance a looking household moves per month when a better home is free */ \
    X(int, movesPerMonth, 400)                                                                                   \
    X(float, sickBelow, 90.0f)       /* health under this may turn sick */                                     \
    X(float, happyDrift, 0.25f)      /* share of the gap to the target closed per month */                     \
    X(float, healthDrift, 0.20f)

struct LifeTuning {
#define TBD_LIFE_FIELD(T, n, d) T n = d;
    TBD_LIFE_TUNING(TBD_LIFE_FIELD)
#undef TBD_LIFE_FIELD

    // "life.fertility" = 1.8 etc. Returns false for a key this struct does not own.
    bool Set(const char* key, double v) {
        if (!key || std::strncmp(key, "life.", 5) != 0) return false;
        const char* n = key + 5;
#define TBD_LIFE_SET(T, f, d) if (std::strcmp(n, #f) == 0) { f = (T)v; return true; }
        TBD_LIFE_TUNING(TBD_LIFE_SET)
#undef TBD_LIFE_SET
        return false;
    }
    int Wage(int level) const {
        const int w[5] = {wage0, wage1, wage2, wage3, wage4};
        return w[level < 0 ? 0 : level > 4 ? 4 : level];
    }
};

} // namespace city
} // namespace tbd
