// Phase 4 incidents (sim_svc_rates.h, sim_svc_step.h): rates match the design doc's baselines within 20 % over ten
// years in a neutral city, police coverage lowers crime, dispatch drives a vehicle there and back, an unfought fire
// burns the building down, patients and bodies take units, the incremental counts never go stale, placement is
// deterministic. Headless. Build and run: tests\sim_svc_incidents_test.bat
#include "sim_svc_town.h"

using namespace svctest;

struct Counts { double n[kIncTypes] = {}; };
static Counts RunDays(SvcWorld& w, CellFactors* f, int days, int64_t& minute, int* staleOut = nullptr) {
    Counts c;
    for (int d = 0; d < days; ++d) {
        for (int m = 0; m < 1440; ++m, ++minute) {
            SvcTick(w, minute, f);
            if (minute % 1440 == 0 && minute > 0)
                for (int t = 0; t < kIncTypes; ++t) c.n[t] += w.yesterday.n[t];
        }
        if (staleOut) *staleOut += SvcVerifyCounts(w);
    }
    for (int t = 0; t < kIncTypes; ++t) c.n[t] += w.today.n[t];
    return c;
}
static void SetCoverage(SvcWorld& w, int slot, uint8_t v) { for (auto& c : w.out.cov) c[(size_t)slot] = v; }

int main() {
    { // neutral city, 10 life years (120 game days): 16 x 16 cells of 400 residents and 160 homes, coverage 128
        Town t;
        t.Build(16, 400);
        SvcWorld w;
        w.P.spreadChance = 0, w.P.destroyedMinutes = 1;
        t.Into(w, 160);
        for (int s = 0; s < kServicesV2; ++s) SetCoverage(w, s, 128);
        CellFactors f = Neutral(256);
        int64_t minute = 1;
        const Counts c = RunDays(w, &f, 120, minute);
        const double res = 256.0 * 400.0, homes = 256.0 * 160.0 + 256.0 * 100.0 * 0.1;
        const double crime = 24.0 / 1000.0 * res * 10, fire = 2.5 / 1000.0 * homes * 10, sick = 60.0 / 1000.0 * res * 10;
        std::printf("10 years: crime %.0f (%.0f), fire %.0f (%.0f), sick %.0f (%.0f)\n", c.n[kIncCrime], crime, c.n[kIncFire], fire, c.n[kIncSick], sick);
        CHECK(std::fabs(c.n[kIncCrime] / crime - 1.0) < 0.2);
        CHECK(std::fabs(c.n[kIncFire] / fire - 1.0) < 0.2);
        CHECK(std::fabs(c.n[kIncSick] / sick - 1.0) < 0.2);
        CHECK(f.cells[5].crime > 0 && f.cells[5].fireRisk > 0); // the factors are written
    }
    { // police coverage lowers crime
        Town t;
        t.Build(8, 400);
        SvcWorld lo, hi;
        t.Into(lo, 160), t.Into(hi, 160);
        SetCoverage(lo, kSvcPolice, 0), SetCoverage(hi, kSvcPolice, 255);
        CellFactors f = Neutral(64);
        int64_t a = 1, b = 1;
        const Counts cl = RunDays(lo, &f, 24, a), ch = RunDays(hi, &f, 24, b);
        CHECK(ch.n[kIncCrime] < cl.n[kIncCrime] * 0.6);
        CHECK(SvcRatesOf(hi, 3, f.cells[3]).crimeMult < SvcRatesOf(lo, 3, f.cells[3]).crimeMult);
    }
    { // dispatch: a fire engine drives out, fights the fire and comes home; the counts stay true all along
        Town t;
        t.Build(5, 100);
        const uint32_t station = t.AddSite(2, 2, kTabFire, "Neighbourhood Firehouse", 8, 1);
        SvcWorld w;
        SimEventRing ev;
        w.events = &ev;
        t.Into(w, 40);
        SimEventRing::Cursor cur = ev.Now();
        const uint32_t b = (uint32_t)(t.Node(4, 4) + 1);
        CHECK(SvcReport(w, kIncFire, b, 10, 30) != 0);
        CHECK(SvcReport(w, kIncFire, b, 10, 30) == 0); // one fire per building
        int stale = 0, maxBusy = 0;
        bool out = false;
        for (int64_t m = 10; m < 400; ++m) {
            SvcTickDue(w, m);
            stale += SvcVerifyCounts(w);
            maxBusy = std::max(maxBusy, (int)w.site[station].busy);
            SimEvent e;
            while (ev.Read(cur, e)) out |= e.type == SimEventType::FireOut;
        }
        CHECK(stale == 0 && maxBusy == 1 && out);
        CHECK(w.site[station].busy == 0 && w.inc.Live() == 0 && w.site[station].handled == 1);
        CHECK(w.trips.size() == 2 && w.trips[0].fromNode == t.Node(2, 2) && w.trips[0].toNode == t.Node(4, 4));
        CHECK(!w.destroyedUntil.count(b));
    }
    { // no fire station: the building burns down after burnMinutes
        Town t;
        t.Build(5, 100);
        SvcWorld w;
        w.P.spreadChance = 0;
        t.Into(w, 40);
        const uint32_t b = (uint32_t)(t.Node(1, 1) + 1);
        SvcReport(w, kIncFire, b, 0, 30);
        for (int64_t m = 0; m <= w.P.burnMinutes + 15; ++m) SvcTickDue(w, m);
        CHECK(w.destroyedUntil.count(b) == 1 && w.inc.Live() == 0 && w.today.destroyed == 1);
        SimFlagTable table;
        SvcFlushFlags(w, table, w.P.burnMinutes + 15, nullptr);
        CHECK(table.Has(BuildingId(b), kFlagDestroyed));
        CHECK(SvcVerifyFlags(w, table, w.P.burnMinutes + 15, nullptr) == 0);
    }
    { // a patient takes a bed until released; a body takes a grave for good; a crematorium frees its place
        Town t;
        t.Build(5, 100);
        const uint32_t hosp = t.AddSite(2, 2, kTabHealth, "Medical Centre", 18, 2);
        const uint32_t cem = t.AddSite(0, 0, kTabDeath, "Small Cemetery", 6, 2, 1.0f, 4, 4);
        SvcWorld w;
        t.Into(w, 40);
        SvcReport(w, kIncSick, (uint32_t)(t.Node(3, 3) + 1), 0);
        SvcReport(w, kIncBody, (uint32_t)(t.Node(1, 0) + 1), 0);
        uint32_t bedPeak = 0;
        int stale = 0;
        for (int64_t m = 0; m < 600; ++m) SvcTickDue(w, m), bedPeak = std::max(bedPeak, w.site[hosp].used), stale += SvcVerifyCounts(w);
        CHECK(bedPeak == 1 && w.site[hosp].used == 0 && stale == 0);
        CHECK(w.site[cem].used == 1 && w.site[cem].graves == 1);
    }
    { // deterministic placement: two runs, identical incident records
        Town t;
        t.Build(10, 300);
        t.AddSite(5, 5, kTabPolice, "Police Station", 36, 3);
        t.AddSite(2, 7, kTabFire, "Fire Station", 20, 4);
        t.AddSite(7, 2, kTabHealth, "Community Hospital", 36, 4);
        SvcWorld a, b;
        a.seed = b.seed = 77;
        t.Into(a, 120), t.Into(b, 120);
        CellFactors f = Neutral(100);
        int64_t ma = 1, mb = 1;
        int stale = 0;
        RunDays(a, &f, 6, ma, &stale), RunDays(b, &f, 6, mb);
        ByteWriter wa, wb;
        SvcSaveIncd(wa, a), SvcSaveIncd(wb, b), SvcSaveServ(wa, a), SvcSaveServ(wb, b);
        CHECK(wa.bytes == wb.bytes && stale == 0);
        CHECK(a.yesterday.n[kIncCrime] > 0 && a.yesterday.responses > 0);
    }
    std::printf("%s (%d failures)\n", failures ? "sim_svc_incidents_test FAILED" : "sim_svc_incidents_test passed", failures);
    return failures ? 1 : 0;
}
