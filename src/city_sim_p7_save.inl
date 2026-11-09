// ------------------------------------------------------------------- city sim Phase 7: .tbdsim chunks (included by city_sim_p7.inl)
// BUDG PLCY DIST MILE (sim_gov_save.h). BUDG and MILE read into copies and commit only when the whole payload checked
// out, so a bad chunk leaves what the M record restored. A save from before this phase (chunks missing) keeps the M
// record's treasury, rates, funding and B1 budget; the milestones are re-reached quietly as before.

void SimP7Save(tbd::city::SimSaveRegistry& reg) {
    using namespace tbd::city;
    using sim_p7::gov;
    SimChunkDef budg, plcy, dist, mile;
    budg.fourcc = simchunk::BUDG, budg.version = kGovBudgVersion;
    budg.save = [](ByteWriter& w) { WriteGovBudg(w, gov, g_simEcon, g_simBudget); };
    budg.load = [](ByteReader& r, uint16_t) {
        GovState g = gov;
        SimEconomy e = g_simEcon;
        SimBudget b = g_simBudget;
        if (!ReadGovBudg(r, g, e, b) || !r.ok) return false;
        gov = std::move(g), g_simEcon = e, g_simBudget = std::move(b);
        g_simBudget.events.clear();
        return true;
    };
    budg.fresh = [] {
        const GovRates keepVersion = gov.rates;
        gov.rates = GovRates(), gov.rates.version = keepVersion.version + 1;
        gov.acc.Clear(), gov.happy.Clear(), gov.last = GovIncome(), gov.stats.Clear();
    };
    plcy.fourcc = simchunk::PLCY, plcy.version = kGovPlcyVersion;
    plcy.save = [](ByteWriter& w) { WriteGovPlcy(w, gov); };
    plcy.load = [](ByteReader& r, uint16_t) { return ReadGovPlcy(r, gov); };
    plcy.fresh = [] { gov.cityPolicies = 0, gov.Touch(); };
    dist.fourcc = simchunk::DIST, dist.version = kGovDistVersion;
    dist.save = [](ByteWriter& w) { WriteGovDist(w, gov); };
    dist.load = [](ByteReader& r, uint16_t) { return ReadGovDist(r, gov); };
    dist.fresh = [] { gov.districts.Clear(); };
    mile.fourcc = simchunk::MILE, mile.version = kGovMileVersion;
    mile.save = [](ByteWriter& w) { WriteGovMile(w, gov, g_simMilestones); };
    mile.load = [](ByteReader& r, uint16_t) {
        GovState g;
        g.progress = gov.progress;
        SimMilestones m = g_simMilestones;
        if (!ReadGovMile(r, g, m) || !r.ok) return false;
        gov.progress = g.progress, g_simMilestones.reached = m.reached, g_simMilestones.unlocked = m.unlocked, gov.Touch();
        return true;
    };
    mile.fresh = [] { gov.progress.Reset(), gov.Touch(); };
    reg.Register(budg), reg.Register(plcy), reg.Register(dist), reg.Register(mile);
}

void SimP7Load(const tbd::city::SimLoadReport& rep) {
    using namespace tbd::city;
    using namespace sim_p7;
    const SimChunkResult* b = rep.Find(simchunk::BUDG);
    if (b && b->status == SimChunkStatus::Loaded) { // the chunk's sliders lead; the CS2 HUD gets them
        for (int k = 0; k < kServices; ++k) g_simFunding[k] = (float)gov.rates.budgetPct[k] / 100.0f;
        g_simHudPush = true;
    } else { // the M record's funding (or the defaults of a new map)
        for (int k = 0; k < kServices; ++k) gov.rates.budgetPct[k] = std::clamp((int)std::lround(g_simFunding[k] * 100.0f), 50, 150);
    }
    for (int k = 0; k < kServices; ++k) fundMirror[k] = g_simFunding[k];
    gov.Touch(), gov.composedKey = 0, burdenKey = ~0ull, milestonesSeen = ~0ull;
    if (rep.Loaded() > 0 && b && b->status == SimChunkStatus::Loaded)
        tbd::Log("Sim: P7 loaded: %d districts, %d days of statistics, city level %d", gov.districts.Count(), gov.stats.Days(), gov.progress.level);
}
