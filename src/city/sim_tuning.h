// Every constant the city sim balances on, in one place. Compiled-in defaults; assets\sim\tuning.json (a flat object of
// "name": number pairs, the same names as below) overrides any of them at load, so tuning needs no rebuild. Unknown
// names are reported, missing ones keep their default. Headless: tests use the defaults.
#pragma once

#include "../platform/platform.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace tbd {
namespace city {

struct SimTuning {
#define TBD_SIM_TUNING(X)                                                                                                   \
    /* demand (S2): smoothed toward the raw value with this time constant */                                                \
    X(demandTauMinutes, 1440.0)        /* about a game day */                                                               \
    X(demandStartPull, 0.6)            /* residential pull of a city with no one in it yet, fading out by startPopulation */ \
    X(demandStartPopulation, 400.0)                                                                                         \
    X(demandBaseResidential, 0.15)     /* people who want to move in anyway: R settles at ~base/jobsWeight unemployment */ \
    X(demandJobsWeight, 2.0)           /* R: (vacancies - unemployed) / adults, times this */                               \
    X(demandUnemployedWeight, 2.5)     /* I / Office: (unemployed - vacancies) / adults, times this */                      \
    X(demandOfficeShare, 0.35)         /* of the job demand, the office part (rest industrial), scaled by higher education */ \
    X(commercialPerResident, 0.12)     /* commercial jobs a resident's spending supports */                                 \
    X(demandCommercialWeight, 1.5)                                                                                          \
    X(industryPerCommercial, 0.4)      /* industrial jobs (goods) a commercial job wants */                                 \
    X(demandGoodsWeight, 0.5)                                                                                               \
    /* growth (S2) */                                                                                                       \
    X(growThreshold, 0.01)             /* a category grows only above this smoothed demand (0.05 left a dead zone: every   \
                                          category settled at ~0.04 and the city stalled) */                                \
    X(levelUpLandValue, 100.0)         /* 0..255 cell land value needed to level up */                                      \
    X(levelUpDays, 3.0)                /* days of good conditions in a row */                                               \
    X(levelDownDays, 6.0)              /* days of bad conditions in a row before a downgrade / abandonment */               \
    X(badDemand, -0.35)                /* category demand below this counts as bad */                                       \
    X(utilitiesRequired, 1.0)          /* 1: no power or water counts as bad (downgrade, abandon); 0: utilities ignored */  \
    X(serviceRadiusBase, 450.0)        /* metres of coverage radius of a service building, plus perVehicle per vehicle */  \
    X(serviceRadiusPerVehicle, 150.0)                                                                                       \
    X(serviceRadiusMax, 2000.0)                                                                                             \
    /* land value (S2, coarse): base, plus neighbours */                                                                    \
    X(landBase, 60.0)                                                                                                       \
    X(landPerJobNear, 0.06)            /* per job within the 3x3 cells around */                                            \
    X(landPerResidentNear, 0.03)                                                                                            \
    X(landPerIndustrialNear, -0.08)    /* industrial jobs pull it down */                                                   \
    X(landPerCoverage, 0.25)           /* per point of average service coverage (S3) */                                     \
    /* flow traffic (S5): vehicles per hour at the peak */                                                                  \
    X(flowCarShare, 0.6)               /* commuters who drive */                                                            \
    X(flowPeakShare, 0.35)             /* of a day's commuters, the share on the road in the peak hour */                   \
    X(flowShopPerResident, 0.03)       /* shopping car trips per resident in that hour */                                   \
    X(flowShopDecayMetres, 1500.0)     /* gravity: weight exp(-distance / this) */                                          \
    X(flowSolveMinutes, 30.0)          /* re-solve at least this often (game minutes), and on any change */                 \
    /* economy (S4), cents */                                                                                               \
    X(taxResidentialPerResidentDay, 40.0)                                                                                   \
    X(taxCommercialPerJobDay, 60.0)                                                                                         \
    X(taxIndustrialPerJobDay, 50.0)                                                                                         \
    X(taxOfficePerJobDay, 80.0)                                                                                             \
    X(startingMoney, 150000000.0)      /* cents: 1,500,000, a CS2-sized start: a first neighbourhood's roads, power, water,  \
                                          sewage and a service or two before taxes come in (a small coal plant is 300,000;  \
                                          construction is charged on placement) */                                          \
    X(budgetNewsGraceDays, 1.0)        /* ledger days before the budget news / radio (deficit, bankrupt) may speak: a fresh \
                                          city is never "in deficit" on its first day */                                    \
    X(serviceUpkeepScale, 1.0)                                                                                              \
    X(taxDemandPerPoint, 0.04)         /* demand change per tax point away from 10 % (higher tax, lower demand) */

#define TBD_SIM_FIELD(name, def) double name = def;
    TBD_SIM_TUNING(TBD_SIM_FIELD)
#undef TBD_SIM_FIELD

    bool Set(const std::string& key, double v) {
#define TBD_SIM_SET(name, def) \
    if (key == #name) {        \
        name = v;              \
        return true;           \
    }
        TBD_SIM_TUNING(TBD_SIM_SET)
#undef TBD_SIM_SET
        return false;
    }

    // Reads "name": number pairs out of a JSON object (nesting, strings and other values are skipped). Returns how many
    // were applied; `unknown` collects names that are not tuning fields.
    int ParseJson(const std::string& text, std::vector<std::string>* unknown = nullptr) {
        int applied = 0;
        size_t i = 0;
        while ((i = text.find('"', i)) != std::string::npos) {
            const size_t end = text.find('"', i + 1);
            if (end == std::string::npos) break;
            const std::string key = text.substr(i + 1, end - i - 1);
            size_t j = end + 1;
            while (j < text.size() && std::isspace((unsigned char)text[j])) ++j;
            if (j >= text.size() || text[j] != ':') { i = end + 1; continue; } // a string value, not a key
            ++j;
            while (j < text.size() && std::isspace((unsigned char)text[j])) ++j;
            char* stop = nullptr;
            const double v = std::strtod(text.c_str() + j, &stop);
            if (stop && stop != text.c_str() + j) {
                if (Set(key, v)) ++applied;
                else if (unknown) unknown->push_back(key);
                i = (size_t)(stop - text.c_str());
            } else {
                i = j;
            }
        }
        return applied;
    }
    bool LoadFile(const wchar_t* path, std::vector<std::string>* unknown, int* applied) {
        FILE* f = tbd::platform::WOpen(path, L"rb");
        if (!f) return false;
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
        const int a = ParseJson(text, unknown);
        if (applied) *applied = a;
        return true;
    }
};

} // namespace city
} // namespace tbd
