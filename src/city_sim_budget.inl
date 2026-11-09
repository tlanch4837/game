// ------------------------------------------------------------------- city sim: economy, budget, construction charges, milestones (S4)
// Included by city_sim.inl (moved out of it, CS2 sim roadmap Phase 0; owned by Phase 7 from here on), with the Budget
// panel's HUD fill and read.

tbd::city::SimEconomy g_simEcon; // treasury and tax rates (sim_economy.h)
tbd::city::SimBudget g_simBudget; // itemised expenses, loans, balance history, bankruptcy (sim_budget.h)
// The sim minute a fresh city (start, new map) began at: its first ledger runs a whole game day later, not on the tick
// it was made (the scheduler's minute 0 runs every system, so the first ledger used to charge day 1 at once).
int64_t g_simLedgerFrom = 0;
tbd::city::SimMilestones g_simMilestones; // population milestones and what they unlock (sim_milestones.h)
city::NewsFeed g_simNews;          // news_lines.json; sim events post triggers, the radio reads Queue()
std::string g_simBanner, g_simBannerSub; // the last milestone, for the HUD banner
uint64_t g_simBannerAt = 0;
double g_simMilestoneQuietUntil = 0; // game minutes: a load re-reaches its milestones silently, without rewards
std::vector<std::string> g_simNotes; // the Notifications panel's log, newest last (capped)

// One line in the Notifications log, stamped with the HUD's calendar date.
void SimNote(const std::string& text) {
    static const char* const kMonth[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int day0 = std::max(0, g_clock.Day() - 1);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%s %d  ", kMonth[(day0 / 30) % 12], day0 % 30 + 1);
    g_simNotes.push_back(stamp + text);
    if (g_simNotes.size() > 40) g_simNotes.erase(g_simNotes.begin());
}

// Lane-metres of every live road and track (rail counts as two lanes, a path as one), recounted when the network changes.
double SimRoadLaneMetres() {
    static uint64_t lenFor = ~0ull;
    static double sum = 0;
    if (lenFor == g_roads.net.version) return sum;
    lenFor = g_roads.net.version, sum = 0;
    for (size_t i = 0; i < g_roads.net.segs.size(); ++i) {
        if (!g_roads.net.segs[i].alive) continue;
        const tbd::city::Profile& p = g_roads.net.Prof((int)i);
        const int lanes = p.cat == tbd::city::Cat::Rail ? 2 : p.fwd + p.back;
        sum += tbd::city::RoadLaneMetres(g_roads.net.Curve((int)i).Length(8), lanes);
    }
    return sum;
}

// A sim event for the radio anchor: event driven only, one headline per event, nothing on a timer.
void RadioSimEvent(const char* trigger); // city_radio.inl
void SimNews(const char* trigger) {
    RadioSimEvent(trigger);
    std::string text, tone;
    if (g_simNews.Post(trigger, text, tone)) tbd::Log("News [%s]: %s", trigger, text.c_str());
}

// Phase 7's ledger hooks (city_sim_p7.inl; they do nothing while gov.enabled is 0).
void SimGovBeforeLedger();
void SimGovAfterLedger();
double SimGovUpkeepMul();       // the city-wide policies' and the tree's service upkeep multiplier
int64_t SimGovPolicyCostDay();  // what the policies cost today, cents (shown with the services)
int64_t SimTransitLineUpkeepDay(); // city_sim_p5.inl: the transit lines' vehicle running cost of the last day, cents
extern bool g_simHudPush;       // city_sim.inl: the rates go back into the CS2 HUD's Budget panel

// The day's upkeep split for the ledger (cents per day).
tbd::city::DayUpkeep SimDayUpkeep() {
    using namespace tbd::city;
    DayUpkeep up;
    for (const UtilBuilding& b : g_util.items)
        if (b.alive) up.utilities += UpkeepPerDay(UtilDefs()[b.def].upkeep, 1.0, 1.0);
    for (const Placed& b : g_transport.items) {
        if (!b.alive || b.state != BState::Done) continue;
        const BuildingDef& d = Defs()[b.def];
        const int kind = SimServiceKind(d);
        up.services += UpkeepPerDay(d.upkeep, kind >= 0 ? g_simFunding[kind] : 1.0, kind >= 0 ? g_simTune.serviceUpkeepScale * SimGovUpkeepMul() : 1.0);
    }
    up.services += SimGovPolicyCostDay();
    up.services += (int64_t)std::llround((double)SimTransitLineUpkeepDay() * SimGovUpkeepMul()); // P5's line vehicles
    up.roads = RoadUpkeepPerDay(PaidLaneMetres(SimRoadLaneMetres(), g_cityMapLaneMetres), kRoadUpkeepPerLaneKmDay); // not the map's corridor
    return up;
}

// ---- construction charges: what was placed since the last frame is paid for once, at its catalog cost (roads by the
// lane-metres added). A new map or a load is taken as it stands (free), like the radio's baseline, for 2 real seconds.
void SimBudgetEvents(); // below
namespace sim_build {
std::unordered_set<uint64_t> transport, util;
double laneMetres = -1.0;
uint64_t roadVersion = ~0ull, generation = ~0ull;
double settle = 0.0; // real seconds left of the baseline window
int64_t chargedTotal = 0; // cents, since the session started (dev log)
} // namespace sim_build
void SimConstructionSync(float realDt) {
    using namespace tbd::city;
    namespace sb = sim_build;
    if (sb::generation != g_cityGeneration) sb::generation = g_cityGeneration, sb::settle = 2.0, sb::laneMetres = -1.0;
    const bool settling = sb::settle > 0.0;
    if (settling) sb::settle -= realDt;
    if (settling) sb::transport.clear(), sb::util.clear();
    int64_t cost = 0;
    for (const Placed& b : g_transport.items)
        if (b.alive && sb::transport.insert(b.id).second) cost += (int64_t)Defs()[b.def].cost * 100;
    for (const UtilBuilding& b : g_util.items)
        if (b.alive && sb::util.insert(b.id).second) cost += (int64_t)UtilDefs()[b.def].cost * 100;
    if (sb::roadVersion != g_roads.net.version || sb::laneMetres < 0.0) {
        const double was = sb::laneMetres, now = SimRoadLaneMetres();
        sb::roadVersion = g_roads.net.version, sb::laneMetres = now;
        if (was >= 0.0 && now > was + 0.5) cost += RoadCost(now - was); // shortened or bulldozed roads: no refund
        if (now < g_cityMapLaneMetres) g_cityMapLaneMetres = now; // the corridor was bulldozed: what is rebuilt is the city's
    }
    // demolished ones drop out of the sets once in a while (they stay the size of the city)
    if (sb::transport.size() > g_transport.items.size() + 256) {
        sb::transport.clear();
        for (const Placed& b : g_transport.items) if (b.alive) sb::transport.insert(b.id);
    }
    if (sb::util.size() > g_util.items.size() + 256) {
        sb::util.clear();
        for (const UtilBuilding& b : g_util.items) if (b.alive) sb::util.insert(b.id);
    }
    if (settling || cost <= 0) return;
    g_simBudget.Charge(g_simEcon, cost);
    sb::chargedTotal += cost;
    SimBudgetEvents(); // the bankruptcy warning, if this build emptied the treasury
}

// The budget's and the milestones' events into toasts and the news. The bankrupt story waits out the grace days
// (sim_budget.h BudgetNewsAllowed): a first day's spending spree is toasted, but the news only runs it if the city is
// still in the red once a ledger day has gone by.
bool g_simBankruptNewsPending = false;
void SimBudgetEvents() {
    const bool newsOk = tbd::city::BudgetNewsAllowed(g_simEcon, (int)g_simTune.budgetNewsGraceDays);
    if (g_simBankruptNewsPending && newsOk) {
        g_simBankruptNewsPending = false;
        if (g_simBudget.bankrupt) SimNews("bankrupt");
    }
    for (const std::string& e : g_simBudget.events) {
        if (e == "bankrupt") {
            Toast("THE CITY IS BANKRUPT: RAISE TAXES, CUT COSTS OR TAKE A LOAN"), SimNote("Bankrupt: the treasury is below zero");
            if (newsOk) SimNews("bankrupt");
            else g_simBankruptNewsPending = true;
        }
        else if (e == "solvent") Toast("THE TREASURY IS BACK IN THE BLACK"), SimNote("The treasury is back above zero");
        else if (e == "loan_taken") SimNews("loan_taken"), SimNote("Loan taken");
        else if (e == "loan_repaid") SimNote("Loan repaid early");
    }
    g_simBudget.events.clear();
}
void SimMilestonesSync() {
    using namespace tbd::city;
    const bool quiet = g_clock.minutes < g_simMilestoneQuietUntil;
    const int got = g_simMilestones.Update(g_simTotals.population, quiet ? nullptr : &g_simEcon.treasury);
    if (got <= 0 || quiet) return;
    int n = 0;
    const Milestone* m = Milestones(n);
    const Milestone& last = m[g_simMilestones.reached - 1];
    char b[128];
    std::snprintf(b, sizeof(b), "MILESTONE: %s (%u RESIDENTS)  +%lld", last.name, last.population, (long long)(last.reward / 100));
    std::string up = b;
    for (char& c : up) c = (char)std::toupper((unsigned char)c);
    Toast(up);
    g_simBanner = std::string("Milestone reached: ") + last.name;
    g_simBannerSub = std::to_string(last.population) + " residents.  +" + std::to_string((long long)(last.reward / 100)) + " grant.  New tools unlocked.";
    g_simBannerAt = tbd::platform::TickCountMs();
    tbd::Log("Sim: milestone %d %s reached at population %llu", g_simMilestones.reached, last.name, (unsigned long long)g_simTotals.population);
    SimNote(std::string("Milestone: ") + last.name + " (" + std::to_string(last.population) + " residents)");
    SimNews("milestone_reached");
}

// Upkeep of everything the city runs, cents per day: utility sources, and transport / service buildings (services scaled
// by their funding).
int64_t SimUpkeepPerDay() {
    using namespace tbd::city;
    int64_t sum = 0;
    for (const UtilBuilding& b : g_util.items)
        if (b.alive) sum += UpkeepPerDay(UtilDefs()[b.def].upkeep, 1.0, 1.0);
    for (const Placed& b : g_transport.items) {
        if (!b.alive || b.state != BState::Done) continue;
        const BuildingDef& d = Defs()[b.def];
        const int kind = SimServiceKind(d);
        sum += UpkeepPerDay(d.upkeep, kind >= 0 ? g_simFunding[kind] : 1.0, kind >= 0 ? g_simTune.serviceUpkeepScale : 1.0);
    }
    return sum;
}

// SimInit's registration (same place in the order as before the move).
void SimRegisterEconomy() {
    // S4: the ledger, once a game day.
    g_simEcon.Reset(g_simTune);
    g_sim.Register("economy", 1440, 1, [](const tbd::city::SimTick& t) {
        if (t.minute < g_simLedgerFrom + 1440) return; // a fresh city's first (partial) day: no ledger yet
        SimGovBeforeLedger(); // Phase 7: the day's taxes from the households, fees and trade (city_sim_p7.inl)
        g_simBudget.Day(g_simEcon, g_citySim, g_simTune, SimDayUpkeep(), (int64_t)g_simTotals.population); // SimEconomy::Day inside
        SimBudgetEvents();
        SimGovAfterLedger(); // the statistics sample, XP
    });
}

// SimHudFill's money, milestone banner, Budget panel and unlock part (city_sim.inl SimHudFill calls it in the same place).
void SimBudgetHudFill(tbd::gui::CityHud& hud) {
    hud.money = (long long)(g_simEcon.treasury / 100);
    for (int c = 0; c < 4; ++c) hud.incomeDay[c] = (long long)(g_simEcon.lastIncome[c] / 100);
    hud.upkeepDay = (long long)(g_simEcon.lastUpkeep / 100);
    { // the milestone banner: 5 s, fading over the last one
        const double age = g_simBannerAt ? (double)(tbd::platform::TickCountMs() - g_simBannerAt) / 1000.0 : 1e9;
        hud.bannerAlpha = (float)std::clamp(5.0 - age, 0.0, 1.0);
        hud.banner = g_simBanner, hud.bannerSub = g_simBannerSub;
    }
    { // the Budget panel's ledger and loans
        using namespace tbd::city;
        tbd::gui::BudgetView& v = hud.bv;
        for (int e = 0; e < kExpenses; ++e) v.expense[e] = (long long)(g_simBudget.lastExpense[e] / 100);
        v.repayment = (long long)(g_simBudget.lastRepayment / 100);
        v.construction = (long long)(g_simBudget.lastConstruction / 100);
        v.constructionToday = (long long)(g_simBudget.constructionToday / 100);
        int64_t h[SimBudget::kHistory];
        v.historyCount = g_simBudget.History(h, SimBudget::kHistory);
        for (int i = 0; i < v.historyCount; ++i) v.history[i] = (long long)(h[i] / 100);
        v.bankrupt = g_simBudget.bankrupt;
        int nt = 0;
        const LoanTier* t = LoanTiers(nt);
        v.loanCount = std::min(nt, 3);
        for (int i = 0; i < v.loanCount; ++i) {
            auto& r = v.loans[i];
            r = {};
            r.name = t[i].name, r.amount = (long long)(t[i].amount / 100), r.ratePct = t[i].ratePermille / 10.0f, r.termDays = t[i].termDays;
            for (const Loan& l : g_simBudget.loans)
                if (l.tier == i) r.active = true, r.owed = (long long)(l.owed / 100), r.daysLeft = l.daysLeft;
        }
        // milestones: the population each locked toolbar button and transport tab needs
        using tbd::gui::HudTool;
        static const struct { HudTool tool; uint32_t bit; } kTools[] = {
            {HudTool::Terrain, kUnlockTerrain}, {HudTool::Zoning, kUnlockZoning}, {HudTool::Roads, kUnlockRoads},
            {HudTool::Electricity, kUnlockElectricity}, {HudTool::Water, kUnlockWater}, {HudTool::Garbage, kUnlockGarbage},
            {HudTool::Transport, kUnlockTransport}, {HudTool::Industry, kUnlockIndustry}, {HudTool::Fire, kUnlockFire},
            {HudTool::Health, kUnlockHealth}, {HudTool::Police, kUnlockPolice}, {HudTool::Education, kUnlockEducation}, {HudTool::Comms, kUnlockComms}};
        for (const auto& k : kTools) hud.lockPop[(int)k.tool] = g_simMilestones.Unlocked(k.bit) ? 0 : (long long)UnlockPopulation(k.bit);
        hud.milestone = g_simMilestones.Level();
        uint32_t tabs = 0;
        if (!g_simMilestones.Unlocked(kUnlockRail)) tabs |= 1u << (int)TTab::Rail;
        if (!g_simMilestones.Unlocked(kUnlockShip)) tabs |= 1u << (int)TTab::Ship;
        if (!g_simMilestones.Unlocked(kUnlockAirport)) tabs |= 1u << (int)TTab::Air;
        g_transportTool.lockedTabs = tabs;
    }
}
// After the HUD drew: the panel's sliders into the sim.
// Only a slider the player moved in that panel is taken: the Phase 7 panels (city_sim_p7_panels.inl) write the same rates,
// and the HUD gets them back through g_simHudPush.
void SimHudRead(const tbd::gui::CityHud& hud) {
    static float lastTax[4] = {-1, -1, -1, -1}, lastFund[tbd::city::kServices] = {-1, -1, -1, -1, -1, -1};
    for (int c = 0; c < 4; ++c)
        if (hud.tax[c] != lastTax[c]) lastTax[c] = hud.tax[c], g_simEcon.taxRate10[c] = (int)std::lround(std::clamp(hud.tax[c], 0.0f, 30.0f) * 10.0f);
    for (int k = 0; k < tbd::city::kServices; ++k)
        if (hud.funding[k] != lastFund[k]) lastFund[k] = hud.funding[k], g_simFunding[k] = std::round(std::clamp(hud.funding[k], 50.0f, 150.0f)) / 100.0f;
    if (hud.bv.takeLoan >= 0 && g_simBudget.TakeLoan(hud.bv.takeLoan, g_simEcon)) Toast("LOAN TAKEN"), SimBudgetEvents();
    if (hud.bv.repayLoan >= 0) {
        for (size_t i = 0; i < g_simBudget.loans.size(); ++i)
            if (g_simBudget.loans[i].tier == hud.bv.repayLoan) {
                Toast(g_simBudget.RepayLoan((int)i, g_simEcon) ? "LOAN REPAID" : "NOT ENOUGH MONEY TO REPAY THE LOAN");
                break;
            }
        SimBudgetEvents(); // the log line (loan_repaid)
    }
}
