// Headless checks for Phase 7's policies, districts, progression and save (src/city/sim_policy.h, sim_gov_districts.h,
// sim_gov_progress.h, sim_gov.h, sim_gov_save.h):
//   a district policy changes only its district's hoods, mods multiply, removal restores, a city policy reaches every
//   hood; locked policies stay off until their tree node is bought; milestones unlock tools and pay points; XP levels;
//   the incremental mods equal a fresh compose; the same inputs give the same bytes; the four chunks round-trip.
// Build and run: tests\sim_gov_policy_test.bat
#include <cmath>
#include <cstdio>
#include <cstring>

#include "city/sim_gov_save.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                            \
    do {                                                                                    \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

// Three hoods of four cells each (cell c is in hood c / 4; its packed key is 1000 + c).
static const std::vector<int> kCellHood = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2};
static uint64_t Key(int c) { return 1000u + (uint64_t)c; }
static void Compose(GovState& g) { g.Compose(kCellHood, 3, 7, Key); }

static void TestDistrictScope() {
    GovState g;
    const int watch = PolicyIndex("watch"), clinics = PolicyIndex("clinics"), curfew = PolicyIndex("curfew");
    CHECK(watch >= 0 && clinics >= 0 && curfew >= 0);
    const int d = g.districts.Add("Old Town");
    CHECK(d == 1 && g.districts.Paint({Key(0), Key(1), Key(2), Key(5)}, d) == 4); // hood 0 (and one cell of hood 1)
    CHECK(g.SetDistrictPolicy(d, watch, true));
    Compose(g);
    CHECK(g.hoodDistrict[0] == 1 && g.hoodDistrict[1] == 0 && g.hoodDistrict[2] == 0); // majority: hood 1 stays out
    CHECK(std::fabs(g.mods[0].crimeRate - 0.92f) < 1e-6f && g.mods[1].crimeRate == 1.0f && g.mods[2].crimeRate == 1.0f);
    // locked until the tree node: Curfew needs Public Safety, which needs Efficient Services
    CHECK(!g.SetDistrictPolicy(d, curfew, true));
    g.progress.points = 3;
    CHECK(!g.progress.Buy(2) && g.progress.Buy(0) && g.progress.Buy(2) && g.progress.points == 0);
    CHECK(g.SetDistrictPolicy(d, curfew, true));
    Compose(g);
    const float perk = 0.95f; // Public Safety's city-wide crime perk
    CHECK(std::fabs(g.mods[0].crimeRate - 0.92f * 0.85f * perk) < 1e-5f && std::fabs(g.mods[1].crimeRate - perk) < 1e-6f);
    // removal restores exactly
    CHECK(g.SetDistrictPolicy(d, curfew, false) && g.SetDistrictPolicy(d, watch, false));
    Compose(g);
    CHECK(g.mods[0].crimeRate == g.mods[1].crimeRate && g.mods[0].leisure == 1.0f);
    // a city-wide policy reaches every hood; a district-only one cannot be city wide
    CHECK(g.SetCityPolicy(PolicyIndex("austerity"), true) && !g.SetCityPolicy(watch, true));
    Compose(g);
    for (int h = 0; h < 3; ++h) CHECK(std::fabs(g.mods[h].serviceCapacity - 0.9f) < 1e-6f);
    // the incremental result equals a fresh compose (the no-stale-state check)
    std::vector<PolicyMods> fresh;
    std::vector<uint8_t> hd;
    g.ComposeFresh(kCellHood, 3, Key, fresh, hd);
    for (int h = 0; h < 3; ++h) CHECK(std::memcmp(&fresh[h], &g.mods[h], sizeof(PolicyMods)) == 0 && hd[h] == g.hoodDistrict[h]);
    // policy costs: Neighbourhood Watch costs 1 cent per covered resident
    g.SetDistrictPolicy(d, watch, true);
    Compose(g);
    CHECK(g.PolicyCostPerDay({100, 200, 300}, 600) == 100 * 1);
    // removing the district frees its cells
    g.districts.RemoveLast();
    Compose(g);
    CHECK(g.districts.cells.empty() && g.hoodDistrict[0] == 0 && g.mods[0].crimeRate == g.mods[2].crimeRate);
}

static void TestProgress() {
    SimMilestones m;
    m.Reset();
    CHECK(!m.Unlocked(kUnlockPolice));
    int64_t money = 0;
    CHECK(m.Update(1000, &money) == 2 && m.Unlocked(kUnlockPolice | kUnlockFire) && money == 60000 * 100LL);
    GovProgress p;
    CHECK(p.OnMilestones(m.reached) == 2 && p.OnMilestones(m.reached) == 0 && p.points == 2);
    CHECK(p.OnPopulation(500, 1.0) == 1 && p.level == 1 && p.points == 3); // 500 XP: past the first 400
    CHECK(p.OnPopulation(400, 1.0) == 0 && p.xp == 500);                    // a dip and a regain pays once
    CHECK(p.XpForLevel(2) == 400 + 540);
}

static std::vector<uint8_t> SaveAll(const GovState& g, const SimEconomy& e, const SimBudget& b, const SimMilestones& m) {
    ByteWriter w;
    WriteGovBudg(w, g, e, b), WriteGovPlcy(w, g), WriteGovDist(w, g), WriteGovMile(w, g, m);
    return w.bytes;
}
static void TestSave() {
    GovState g;
    SimEconomy e;
    e.Reset(SimTuning());
    SimBudget b;
    SimMilestones m;
    m.Reset(), m.Update(3000, nullptr);
    b.TakeLoan(1, e);
    g.rates.eduOffset10[4] = 20, g.rates.budgetPct[kSvcHealth] = 130, g.rates.powerFeePct = 80;
    g.districts.Add("Docks"), g.districts.Add("Hill");
    g.districts.Paint({Key(4), Key(9)}, 2), g.SetDistrictPolicy(2, PolicyIndex("watch"), true);
    g.progress.AddXp(1234), g.progress.Buy(0);
    const int64_t day[kStatSeries] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    g.stats.Push(day), g.stats.Push(day);
    const std::vector<uint8_t> a = SaveAll(g, e, b, m), a2 = SaveAll(g, e, b, m);
    CHECK(a == a2); // determinism: the same state writes the same bytes
    GovState g2;
    SimEconomy e2;
    SimBudget b2;
    SimMilestones m2;
    ByteReader r(a);
    CHECK(ReadGovBudg(r, g2, e2, b2) && ReadGovPlcy(r, g2) && ReadGovDist(r, g2) && ReadGovMile(r, g2, m2) && r.AtEnd());
    CHECK(SaveAll(g2, e2, b2, m2) == a);
    CHECK(g2.districts.Of(Key(9)) == 2 && b2.loans.size() == 1 && m2.reached == m.reached && g2.progress.xp == 1234);
    std::vector<uint8_t> bad = a;
    bad.resize(bad.size() / 2);
    ByteReader rb(bad);
    GovState g3;
    CHECK(!(ReadGovBudg(rb, g3, e2, b2) && ReadGovPlcy(rb, g3) && ReadGovDist(rb, g3) && ReadGovMile(rb, g3, m2) && rb.ok));
}

int main() {
    TestDistrictScope();
    TestProgress();
    TestSave();
    std::printf(failures ? "sim_gov_policy_test: %d FAILED\n" : "sim_gov_policy_test: all passed\n", failures);
    return failures ? 1 : 0;
}
