// Phase 6 (CS2 sim roadmap 4): resources, products and recipes of the production chains. Header only, headless
// (tests\sim_goods_test). Everything is integer: 1 unit = 100 kg, prices are whole currency per unit.
//
//   raw (industry hubs)    grain, livestock, timber, oil, ore
//   processed (industry)   food (grain + livestock), wood (timber), petrochemicals (oil), metals (ore)
//   manufactured           goods (wood + petrochemicals + metals)
//   retail (commercial)    shops sell food and goods to the residents
// Every recipe turns N input units into N output units, so mass is conserved through a chain: what leaves the city is
// only what the residents consume and what is exported.
#pragma once

#include <cstdint>

namespace tbd {
namespace city {

enum class Good : uint8_t { Grain, Livestock, Timber, Oil, Ore, Food, Wood, Petro, Metals, Goods, Count, None = 255 };
constexpr int kGoodCount = (int)Good::Count;

inline const char* GoodName(Good g) {
    static const char* const k[kGoodCount] = {"Grain", "Livestock", "Timber", "Crude oil", "Metal ore",
                                              "Food", "Wood", "Petrochemicals", "Metals", "Goods"};
    return (int)g < kGoodCount ? k[(int)g] : "";
}
inline bool GoodIsRaw(Good g) { return (int)g <= (int)Good::Ore; }
// Wholesale price per unit (what a buyer pays the seller, or the outside world).
inline int32_t GoodPrice(Good g) {
    static const int32_t k[kGoodCount] = {10, 12, 9, 14, 11, 26, 18, 28, 22, 80};
    return (int)g < kGoodCount ? k[(int)g] : 0;
}

// The industry hub kinds (industry.h IndKind order: Grain, Livestock, Timber, Oil, Ore; Landfill is not a company).
inline Good RawGoodOfHub(int indKind) { return indKind >= 0 && indKind <= 4 ? (Good)indKind : Good::None; }

constexpr int kMaxInputs = 3;
struct Recipe {
    const char* name;
    Good in[kMaxInputs]; // Good::None for an unused slot
    Good out;
    uint8_t inputs;      // used input slots; one unit of each input makes `inputs` units of out
};
enum RecipeId : uint8_t { kRecipeFood, kRecipeWood, kRecipePetro, kRecipeMetals, kRecipeGoods, kRecipeCount };
inline const Recipe& RecipeOf(int r) {
    static const Recipe k[kRecipeCount] = {
        {"Food processing", {Good::Grain, Good::Livestock, Good::None}, Good::Food, 2},
        {"Sawmill", {Good::Timber, Good::None, Good::None}, Good::Wood, 1},
        {"Refinery", {Good::Oil, Good::None, Good::None}, Good::Petro, 1},
        {"Smelter", {Good::Ore, Good::None, Good::None}, Good::Metals, 1},
        {"Manufacturing", {Good::Wood, Good::Petro, Good::Metals}, Good::Goods, 3},
    };
    return k[r < kRecipeCount ? r : 0];
}
// An industrial lot's recipe, picked once from its id (weights 2 food, 1 wood, 1 petro, 1 metals, 2 goods).
inline int RecipeForLot(uint64_t lotId) {
    uint64_t h = lotId * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    static const uint8_t pick[7] = {kRecipeFood, kRecipeFood, kRecipeWood, kRecipePetro, kRecipeMetals, kRecipeGoods, kRecipeGoods};
    return pick[h % 7];
}

// What shops sell: food and goods, in this share of the residents' spending (percent).
constexpr Good kRetailGoods[2] = {Good::Food, Good::Goods};
constexpr int kRetailShare[2] = {40, 60};

// Tuning (compiled; the goods. block of sim_tuning_table.h carries only `enabled` for now).
struct GoodsParams {
    int32_t workerMilliPerHour = 500;   // a worker makes (or sells) 0.5 units an hour at full efficiency
    int32_t rawScale = 10;              // a hub's IndSite::perMonth tonnes are made per game day (x 10 units per tonne)
    int32_t stockHours = 12;            // input target stock: this many hours of full-rate consumption
    int32_t outCapHours = 24;           // output stock cap: a day of full-rate production
    int32_t truckUnits = 200;           // one truck carries 20 t
    int32_t truckMetresPerMinute = 600; // 36 km/h door to door, plus loading
    int32_t loadMinutes = 10;
    int32_t matchRadiusM = 6000;        // a local seller farther than this loses to an import
    int32_t exportAtPercent = 75;       // an output stock this full ships its surplus out of the city
    int32_t residentMilliPerHour = 40;  // each resident buys 0.04 units an hour (about one unit a day)
    int32_t wagePerDay = 60;            // per worker
    int32_t retailMarkupPercent = 140;  // shops sell at 1.4 x the wholesale price
    int32_t officeValuePerDay = 140;    // an office worker's output (offices have no goods)
    int32_t bankruptDays = 4;           // days in a row with a loss (and cash < 0) before bankruptcy
    int32_t reopenDays = 3;             // a bankrupt company's lot reopens with a new company after this
    int32_t noCustomersPercent = 35;    // a shop whose customers buy under this share of what it could sell
    int32_t noGoodsPercent = 15;        // an input stock under this share of the target
    int32_t startCash = 2000;
};

} // namespace city
} // namespace tbd
