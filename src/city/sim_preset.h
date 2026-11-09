// The city sim's share of the graphics presets (CS2 sim roadmap 3.3, iPad budget): what each preset (gfx_settings.h Low,
// Med, High, Ultra; Custom uses its look tier) allows the sim per frame, and how the device's thermal state cuts it.
// Header only, headless (tests\sim_preset_test). The host (city_sim.inl SimApplyPreset) applies it when the preset or the
// thermal state changes.
//
//   budgetMs      the scheduler's main-thread budget per frame (sim.h SimScheduler::budgetMs). Past it the sim slips
//                 behind (the backlog catches up later); record ticks are never dropped below the 240-minute cap.
//   carDensity    visible cars per herd vehicle near the camera (sim_children.h ChildParams::densityScale)
//   maxParked     curbside parked cars drawn near the camera (city_herds.inl)
//   truckShare    share of Phase 6's trucks drawn as vehicles (goods.truckDrawShare is multiplied by it)
//
// Thermal state: the iOS ProcessInfo.ThermalState values (0 nominal, 1 fair, 2 serious, 3 critical); the roadmap halves
// the budget at serious. Only what is drawn and how fast the sim catches up change: the simulation's results do not
// depend on the preset (the same city on any device), only how soon they arrive.
#pragma once

#include <algorithm>

namespace tbd {
namespace city {

struct SimPresetScale {
    double budgetMs = 2.0;
    float carDensity = 1.0f;
    int maxParked = 240;
    float truckShare = 1.0f;
    bool operator==(const SimPresetScale& o) const {
        return budgetMs == o.budgetMs && carDensity == o.carDensity && maxParked == o.maxParked && truckShare == o.truckShare;
    }
};

// preset: 0 Low, 1 Med, 2 High, 3 Ultra (gfx_settings.h Preset; Custom: pass its look tier mapped onto 0..3).
// thermal: 0 nominal, 1 fair, 2 serious, 3 critical.
inline SimPresetScale SimPresetFor(int preset, int thermal) {
    static const SimPresetScale k[4] = {
        {1.0, 0.5f, 80, 0.5f},   // Low: iPhone 15 floor at 30 fps
        {1.5, 0.7f, 140, 0.75f}, // Med
        {2.0, 1.0f, 240, 1.0f},  // High
        {2.0, 1.0f, 240, 1.0f},  // Ultra (40 fps on the iPad: the frame is shorter, the budget is not raised)
    };
    SimPresetScale s = k[std::clamp(preset, 0, 3)];
    switch (std::clamp(thermal, 0, 3)) {
    case 2: s.budgetMs *= 0.5, s.carDensity *= 0.6f, s.maxParked = s.maxParked * 6 / 10, s.truckShare *= 0.5f; break;
    case 3: s.budgetMs *= 0.35, s.carDensity *= 0.35f, s.maxParked = s.maxParked * 3 / 10, s.truckShare *= 0.25f; break;
    default: break; // nominal and fair: as the preset says
    }
    return s;
}

} // namespace city
} // namespace tbd
