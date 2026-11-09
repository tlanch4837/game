// Per-phase tuning (CS2 sim roadmap 3.5, Phase 0; frozen interface). Header only, headless (tests\sim_save_test).
//
// The same assets\sim\tuning.json that SimTuning (sim_tuning.h) reads, but with prefixed names, one prefix per phase:
//   life. (P1)  land. (P2)  env. (P3)  svc. (P4)  move. (P5)  goods. (P6)  gov. (P7)
// e.g. { "demandTauMinutes": 1440, "life.instantFill": 1, "env.windTiles": 1 }. Each struct reads only names with its
// own prefix and ignores the rest silently (other phases' names, SimTuning's unprefixed names); a name with its prefix
// that it does not know is reported as unknown. SimTuning stays the unprefixed set: its ParseJson reports prefixed names
// as unknown, so the host filters its list with IsPhaseTuningKey before logging.
//
// One template for all seven: a phase's values are an X-macro list X(name, default) in its own block below, and only that
// phase edits its block. TBD_SIM_TUNING_TABLE turns a list into a struct of doubles with Set / ParseJson / LoadFile /
// ForEach. Every phase starts with `enabled` (0 switches the phase's systems off: a dev switch for bisecting).
#pragma once

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../platform/platform.h"

namespace tbd {
namespace city {

constexpr const char* kPhaseTuningPrefixes[] = {"life.", "land.", "env.", "svc.", "move.", "goods.", "gov."};

inline bool HasPrefix(const std::string& s, const char* p) { return s.compare(0, std::char_traits<char>::length(p), p) == 0; }
inline bool IsPhaseTuningKey(const std::string& key) {
    for (const char* p : kPhaseTuningPrefixes) if (HasPrefix(key, p)) return true;
    return false;
}

// Calls f(key, value) for every "key": number pair of a JSON text, the way SimTuning::ParseJson scans it (nesting,
// strings and other values are skipped).
template <typename F>
void ForEachJsonNumber(const std::string& text, F&& f) {
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
            f(key, v);
            i = (size_t)(stop - text.c_str());
        } else {
            i = j;
        }
    }
}

// The shared half of every table: prefix handling, JSON and file reading. Self supplies kPrefix and SetLocal(name, v).
template <typename Self>
struct SimTuningTable {
    // A full key ("life.instantFill"). False for another prefix or an unknown name.
    bool Set(const std::string& key, double v) {
        if (!HasPrefix(key, Self::kPrefix)) return false;
        return static_cast<Self*>(this)->SetLocal(key.substr(std::char_traits<char>::length(Self::kPrefix)), v);
    }
    // Returns how many values were applied; `unknown` collects names with this prefix that are not in the list.
    int ParseJson(const std::string& text, std::vector<std::string>* unknown = nullptr) {
        int applied = 0;
        ForEachJsonNumber(text, [&](const std::string& key, double v) {
            if (!HasPrefix(key, Self::kPrefix)) return;
            if (Set(key, v)) ++applied;
            else if (unknown) unknown->push_back(key);
        });
        return applied;
    }
    // The same file SimTuning::LoadFile reads (a game path; FsPath maps it for the OS).
    bool LoadFile(const std::wstring& path, std::vector<std::string>* unknown, int* applied) {
        std::ifstream f(tbd::platform::FsPath(path), std::ios::binary);
        if (!f) return false;
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const int a = ParseJson(text, unknown);
        if (applied) *applied = a;
        return true;
    }
};

#define TBD_SIM_TT_FIELD(name, def) double name = def;
#define TBD_SIM_TT_SET(name, def) \
    if (k == #name) {             \
        name = v;                 \
        return true;              \
    }
#define TBD_SIM_TT_EACH(name, def) f(#name, name, (double)(def));

// Name: the struct; prefix: a string literal ending in '.'; LIST: an X-macro list of X(name, default).
#define TBD_SIM_TUNING_TABLE(Name, prefix, LIST)                                                 \
    struct Name : ::tbd::city::SimTuningTable<Name> {                                            \
        static constexpr const char* kPrefix = prefix;                                           \
        LIST(TBD_SIM_TT_FIELD)                                                                   \
        bool SetLocal(const std::string& k, double v) {                                          \
            LIST(TBD_SIM_TT_SET)                                                                 \
            return false;                                                                        \
        }                                                                                        \
        /* f(name without prefix, current value, compiled default) */                            \
        template <typename F>                                                                    \
        void ForEach(F&& f) const {                                                              \
            LIST(TBD_SIM_TT_EACH)                                                                \
        }                                                                                        \
    };

// ---- P1 citizens, households, jobs, migration (life.) -----------------------------------------------------------------
#define TBD_SIM_TUNING_LIFE(X)                                                                                              \
    X(enabled, 1.0)                                                                                                         \
    X(instantFill, 0.0)                /* 1: a finished home fills at once (tests) */
TBD_SIM_TUNING_TABLE(LifePhaseTuning, "life.", TBD_SIM_TUNING_LIFE) // not LifeTuning: that is sim_life_tuning.h's full set

// ---- P2 growth, land value, rent, demand (land.) ----------------------------------------------------------------------
#define TBD_SIM_TUNING_LAND(X)                                                                                              \
    X(enabled, 1.0)
TBD_SIM_TUNING_TABLE(LandTuning, "land.", TBD_SIM_TUNING_LAND)

// ---- P3 pollution, weather, water (env.) ------------------------------------------------------------------------------
// Defaults are sim_env.h EnvParams' compiled values; city_sim_p3.inl copies them in (SimP3ApplyTuning).
#define TBD_SIM_TUNING_ENV(X)                                                                                               \
    X(enabled, 1.0)                                                                                                         \
    X(airKeep, 0.75)                   /* share of air pollution kept per game hour */                                      \
    X(groundKeep, 0.985)               /* ground pollution kept per game hour (lingers) */                                  \
    X(waterKeep, 0.80)                 /* water pollution kept per game hour */                                             \
    X(airSteps, 2.0)                   /* most downwind steps an hour */                                                    \
    X(waterSteps, 3.0)                                                                                                      \
    X(windStepMs, 5.0)                 /* one downwind step per this many m/s */                                            \
    X(airLimit, 90.0)                  /* flag thresholds, factor steps 0..255 */                                           \
    X(groundLimit, 80.0)                                                                                                    \
    X(waterLimit, 60.0)                                                                                                     \
    X(noiseLimit, 55.0)                                                                                                     \
    X(waterNatural, 6000.0)            /* groundwater table with no pumping (0..10000) */                                   \
    X(waterRelax, 0.03)                /* share of the gap to natural closed per hour */                                    \
    X(rechargePerMm, 40.0)                                                                                                  \
    X(summerDryPerHour, 12.0)                                                                                               \
    X(infilMmH, 6.0)                                                                                                        \
    X(drainMmH, 3.0)                   /* storm drains per tile per hour */                                                 \
    X(drainShare, 0.04)                                                                                                     \
    X(floodMm, 300.0)                  /* depth that counts as flooded */                                                   \
    X(pumpDrawPerUnit, 0.005)
TBD_SIM_TUNING_TABLE(EnvTuning, "env.", TBD_SIM_TUNING_ENV)

// ---- P4 services, incidents, garbage (svc.) ---------------------------------------------------------------------------
#define TBD_SIM_TUNING_SVC(X)                                                                                               \
    X(enabled, 1.0)
TBD_SIM_TUNING_TABLE(SvcTuning, "svc.", TBD_SIM_TUNING_SVC)

// ---- P5 transit lines, modes, outside connections (move.) -------------------------------------------------------------
#define TBD_SIM_TUNING_MOVE(X)                                                                                              \
    X(enabled, 1.0)
TBD_SIM_TUNING_TABLE(MoveTuning, "move.", TBD_SIM_TUNING_MOVE)

// ---- P6 companies, goods, trade (goods.) ------------------------------------------------------------------------------
// Defaults are sim_goods.h GoodsParams' compiled values; city_sim_p6.inl copies them in (SimP6ApplyTuning).
#define TBD_SIM_TUNING_GOODS(X)                                                                                             \
    X(enabled, 1.0)                                                                                                         \
    X(workerMilliPerHour, 500.0)       /* units x 1000 a worker makes an hour */                                            \
    X(rawScale, 10.0)                                                                                                       \
    X(stockHours, 12.0)                                                                                                     \
    X(outCapHours, 24.0)                                                                                                    \
    X(truckUnits, 200.0)               /* one truck: 20 t */                                                                \
    X(truckMetresPerMinute, 600.0)                                                                                          \
    X(loadMinutes, 10.0)                                                                                                    \
    X(matchRadiusM, 6000.0)                                                                                                 \
    X(exportAtPercent, 75.0)                                                                                                \
    X(residentMilliPerHour, 40.0)                                                                                           \
    X(wagePerDay, 60.0)                                                                                                     \
    X(retailMarkupPercent, 140.0)                                                                                           \
    X(officeValuePerDay, 140.0)                                                                                             \
    X(bankruptDays, 4.0)                                                                                                    \
    X(reopenDays, 3.0)                                                                                                      \
    X(noCustomersPercent, 35.0)                                                                                             \
    X(noGoodsPercent, 15.0)                                                                                                 \
    X(startCash, 2000.0)                                                                                                    \
    X(truckDrawShare, 1.0)             /* share of yesterday's trucks the herds draw as vehicles */
TBD_SIM_TUNING_TABLE(GoodsTuning, "goods.", TBD_SIM_TUNING_GOODS)

// ---- P7 budget, policies, districts, milestones (gov.) ----------------------------------------------------------------
#define TBD_SIM_TUNING_GOV(X)                                                                                               \
    X(enabled, 1.0)
TBD_SIM_TUNING_TABLE(GovTuning, "gov.", TBD_SIM_TUNING_GOV)

// All seven, read from one text. `unknown` gets the prefixed names no table knows (names with no phase prefix are
// SimTuning's business and are not reported here).
struct PhaseTuning {
    LifePhaseTuning life;
    LandTuning land;
    EnvTuning env;
    SvcTuning svc;
    MoveTuning move;
    GoodsTuning goods;
    GovTuning gov;

    int ParseJson(const std::string& text, std::vector<std::string>* unknown = nullptr) {
        int n = life.ParseJson(text, unknown); // one statement each: `unknown` fills in phase order
        n += land.ParseJson(text, unknown);
        n += env.ParseJson(text, unknown);
        n += svc.ParseJson(text, unknown);
        n += move.ParseJson(text, unknown);
        n += goods.ParseJson(text, unknown);
        n += gov.ParseJson(text, unknown);
        return n;
    }
    bool LoadFile(const std::wstring& path, std::vector<std::string>* unknown, int* applied) {
        std::ifstream f(tbd::platform::FsPath(path), std::ios::binary);
        if (!f) return false;
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const int a = ParseJson(text, unknown);
        if (applied) *applied = a;
        return true;
    }
};

} // namespace city
} // namespace tbd
