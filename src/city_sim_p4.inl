// ------------------------------------------------------------------- city sim Phase 4: city services, coverage, crime, fire, health, death, garbage, post, telecom
// Included by city_sim_phases.inl (CS2 sim roadmap, section 4). Phase 4 owns this file, city_sim_p4_*.inl, city_sim_services.inl
// and src/city/sim_svc*.h (headless, tests\sim_svc_*). The hub calls the five hooks at the bottom (frozen signatures).
//   svc        every game minute (sim_svc.h SvcTick): the incidents that are due (a time-ordered queue: nothing else is
//              looked at), one of 96 cell groups every 15 minutes (incident rates, crime and fire factors), a bounded flags
//              flush (dirty buildings first), one school-seat slice every 10 minutes (the whole citizen store once a game
//              day), one of 60 garbage groups (every lot once a game hour), the deaths and floods of the event ring
//   coverage   on change only (services, roads, lots, funding), at most once per 10 game minutes: the snapshot is taken
//              at a 10-minute tick and handed to a worker (SvcCover + SvcPrepare); the result lands at the NEXT 10-minute
//              tick, from the worker or, if it is not done, computed here from the same snapshot (timing never matters)
// Tables P4 writes: g_cellFactors (coverage[], crime, fireRisk, telecom, parkAccess), g_simFlagTable (SimFlagPhaseMask(4)),
// g_simEvents (crime, fire, sick, disaster ...), CitizenStore::school (through g_life), CellSim::coverage (the six legacy
// slots that growth and land value read), g_garbagePile / g_garbage (city_garbage.inl draws and lists them).
// Placeholders: parks (Healthcare tab) and post (Communications tab) have no models yet (transport.h, plain shells).

// The headers (city/sim_svc.h) come from the host TUs (main.cpp, game_host.cpp, probe_game.cpp).

namespace herds_host { // city_herds.inl (included after the phase hub) owns these; P4 reads the link table
extern tbd::city::LinkTable table;
extern bool tableReady;
} // namespace herds_host

namespace sim_p4 {
using namespace tbd::city;
bool on = false;
SvcWorld w;
SvcSeatPass seats;
SvcGarbage garb;
SvcGarbLoaded garbLoaded;          // a load's piles until the lots are listed
SvcGarbStats garbHour[kGarbGroups]; // the last visit of each group (the panel sums them)
std::vector<SvcGarbFacility> garbFac;
float funding[kServicesV2] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}; // P4's own sliders (deathcare, garbage, post, telecom)
SimEventRing::Cursor events = 0;
// coverage job: the snapshot taken at one 10-minute tick lands at the next
struct CovJob {
    SvcPrepared in;  // the snapshot (read only once the job runs)
    SvcPrepared out; // the worker's result
    double ms = 0;
    std::atomic<bool> ready{false};
};
std::shared_ptr<CovJob> job;
std::shared_ptr<const SvcGraph> graph;  // of herds_host::table (rebuilt when its version moves)
uint64_t covKey = ~0ull, bldKey = ~0ull, lotsKey = ~0ull;
uint64_t lastBldMs = 0;
bool syncNext = false;              // a load: the first coverage pass runs on the main thread at once
uint64_t loadMs = 0;                // when (real ms) the load's Load hook ran
std::shared_ptr<SvcGarbage::Prepared> lotsJob; // the garbage re-list being prepared
std::atomic<bool> lotsReady{false};
double covMs = 0, installMs = 0, bldMs = 0, tickAvgMs = 0, tickMaxMs = 0;
int covRuns = 0, covFallbacks = 0, verifyBad = 0;
std::vector<tbd::city::NodeLink> links; // yesterday's vehicle trips as herd node links (SimServiceVehicleLinks)
uint64_t linksVersion = ~0ull;
size_t linksTrips = ~(size_t)0;
std::unordered_map<uint64_t, float> delay; // PackKey -> traffic delay (x free-flow), from the flow solve
uint64_t delayFrom = ~0ull;

double Now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t Seed() { return (uint64_t)(uint32_t)g_engine.terrain.params.seed ^ 0x5E4Cull; }
} // namespace sim_p4
float SimGovServiceCapacity(int slot); // city_sim_p7_ledger.inl: policy x Budget panel slider per slot, -1 while P7 is off
namespace sim_p4 {
// The funding of a slot: with P7 on, its Budget panel slider (all twelve) times the city's service capacity policy;
// else the budget's six sliders for the legacy services (schools of all levels share one), P4's own for the rest.
float FundingOf(int slot) {
    const float gov = SimGovServiceCapacity(slot);
    if (gov >= 0.0f) return gov;
    if (slot == kSvcHighSchool || slot == kSvcUniversity) return g_simFunding[kSchool];
    return slot >= 0 && slot < kServices ? g_simFunding[slot] : slot >= 0 && slot < kServicesV2 ? funding[slot] : 1.0f;
}
bool GarbageOwned() { return on && g_simPhaseTune.svc.ownGarbage != 0.0; }
} // namespace sim_p4

// city_sim_services.inl and city_garbage.inl ask these (declared there, before this file).
bool SimP4CoverageOwned() { return sim_p4::on; }
bool SimP4GarbageOwned() { return sim_p4::GarbageOwned(); }

#include "city_sim_p4_input.inl" // the building list, the coverage snapshot, the garbage lots and facilities
#include "city_sim_p4_tick.inl"  // the svc system: coverage job, incidents, seats, garbage, events, vehicles
#include "city_sim_p4_ui.inl"    // overlays, panels, the inspector's lines
#include "city_sim_p4_hooks.inl" // the five hooks
