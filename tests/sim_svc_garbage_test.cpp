// Phase 4 garbage (src/city/sim_svc_garbage.h): piles grow at the lots' rate with no facility, a facility with enough
// capacity keeps them down and sends trucks, an undersized one lets them pile up nearest-last, another road network is
// not served, the input order is irrelevant (deterministic), piles survive a re-list of the lots, GARB round-trips.
// Headless. Build and run: tests\sim_svc_garbage_test.bat
#include "sim_svc_town.h"

using namespace svctest;

static std::vector<SvcGarbLot> Lots(int n, int comp) {
    std::vector<SvcGarbLot> v;
    for (int i = 0; i < n; ++i) {
        SvcGarbLot l;
        l.lot = 1000 + (uint64_t)i, l.x = (float)(i % 20) * 40.0f, l.z = (float)(i / 20) * 40.0f, l.comp = comp, l.node = i % 7, l.perMonth = 20.0f;
        v.push_back(l);
    }
    return v;
}
struct Result { double pile = 0; int piling = 0, unserved = 0, trucks = 0; size_t mirrored = 0; };
static Result Run(SvcGarbage& g, std::vector<SvcGarbFacility> fac, int64_t from, int days) {
    Result r;
    std::vector<SvcTrip> trips;
    std::vector<uint64_t> changed;
    std::vector<std::pair<uint64_t, double>> mirror;
    for (int64_t m = from; m < from + (int64_t)days * 1440; ++m) {
        SvcGarbStats st;
        g.Visit((int)(m % kGarbGroups), m, fac, trips, changed, mirror, st);
        r.unserved += st.unserved;
    }
    for (const auto& grp : g.groups) for (const SvcGarbLot& l : grp) r.pile += l.pile, r.piling += l.Piling();
    r.trucks = (int)trips.size(), r.mirrored = mirror.size();
    return r;
}
static double PileOf(const SvcGarbage& g, uint64_t lot) { const SvcGarbLot* l = g.Find(lot); return l ? l->pile : -1.0; }

int main() {
    const int days = 90; // three game months
    { // no facility: every lot piles up exactly at its rate
        SvcGarbage g;
        g.SetLots(Lots(100, 1));
        const Result r = Run(g, {}, 0, days);
        const double expect = 100 * 20.0 * days * 1440.0 / kGarbMonthMinutes;
        CHECK(std::fabs(r.pile - expect) < expect * 0.02);
        CHECK(r.trucks == 0 && r.unserved > 0 && r.piling == 100 && r.mirrored > 0);
    }
    { // a landfill with room: piles stay small, trucks run (one per 10 t)
        SvcGarbage g;
        g.SetLots(Lots(100, 1));
        SvcGarbFacility f;
        f.id = 1, f.comp = 1, f.node = 3, f.perMonth = 3000, f.x = 200, f.z = 100;
        const Result r = Run(g, {f}, 0, days);
        CHECK(r.piling == 0 && r.pile < 100 * 20.0 * 0.1);
        const double collected = 100 * 20.0 * days / 30.0;
        CHECK(std::fabs(r.trucks - collected / 10.0) < collected / 10.0 * 0.05 + 10);
    }
    { // undersized (half the output): garbage piles up, the far lots first; another road network is never served
        SvcGarbage g;
        g.SetLots(Lots(100, 1));
        SvcGarbFacility f;
        f.id = 1, f.comp = 1, f.node = 3, f.perMonth = 1000, f.x = 0, f.z = 0;
        const Result r = Run(g, {f}, 0, days);
        CHECK(r.piling > 0);
        CHECK(PileOf(g, 1000) < PileOf(g, 1099));
        SvcGarbage o;
        o.SetLots(Lots(50, 2));
        const Result q = Run(o, {f}, 0, 10);
        CHECK(q.trucks == 0 && q.unserved > 0 && q.piling == 0);
    }
    { // input order does not matter; a re-list keeps the piles; GARB round-trips into the same bytes
        SvcGarbFacility f;
        f.id = 7, f.comp = 1, f.node = 2, f.perMonth = 1500;
        SvcGarbage a, b;
        std::vector<SvcGarbLot> rev = Lots(80, 1);
        std::reverse(rev.begin(), rev.end());
        a.SetLots(Lots(80, 1)), b.SetLots(rev);
        Run(a, {f}, 0, 20), Run(b, {f}, 0, 20);
        ByteWriter wa, wb;
        SvcSaveGarb(wa, a), SvcSaveGarb(wb, b);
        CHECK(wa.bytes == wb.bytes);
        const double before = PileOf(a, 1010);
        std::vector<SvcGarbLot> fewer = Lots(80, 1);
        fewer.erase(fewer.begin()); // lot 1000 demolished
        a.SetLots(fewer);
        CHECK(PileOf(a, 1010) == before && PileOf(a, 1000) < 0);
        SvcGarbage d = b; // the deferred re-list (Prepare, Adopt, merged group by group) ends where SetLots does
        d.Adopt(SvcGarbage::Prepare(fewer)), b.SetLots(fewer);
        Run(b, {f}, 20 * 1440, 2), Run(d, {f}, 20 * 1440, 2);
        ByteWriter xb0, xd0;
        SvcSaveGarb(xb0, b), SvcSaveGarb(xd0, d);
        CHECK(xb0.bytes == xd0.bytes);
        b = SvcGarbage(), b.SetLots(rev), Run(b, {f}, 0, 20); // b back to where it was for the load check below
        SvcGarbage c;
        SvcGarbLoaded loaded;
        ByteReader r(wb.bytes);
        CHECK(SvcLoadGarb(r, loaded, c) && r.AtEnd() && loaded.size() == 80);
        c.SetLots(Lots(80, 1), &loaded);
        ByteWriter wc;
        SvcSaveGarb(wc, c);
        CHECK(wc.bytes == wb.bytes && loaded.empty());
        Run(b, {f}, 20 * 1440, 5), Run(c, {f}, 20 * 1440, 5); // and they go on alike
        ByteWriter xb, xc;
        SvcSaveGarb(xb, b), SvcSaveGarb(xc, c);
        CHECK(xb.bytes == xc.bytes);
    }
    std::printf("%s (%d failures)\n", failures ? "sim_svc_garbage_test FAILED" : "sim_svc_garbage_test passed", failures);
    return failures ? 1 : 0;
}
