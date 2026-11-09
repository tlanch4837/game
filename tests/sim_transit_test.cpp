// Phase 5 transit tests (headless): mode choice (a line shifts the shares, no stop means no transit, wealth shifts to
// the car, a free fare shifts to transit), ridership conservation and capacity, the incremental state against a full
// recompute, determinism, the LINE / MODE / OUTC save round trip, outside connection splits, and the cost per tick.
// Build and run: tests\sim_transit_test.bat
#include "sim_transit_town.h"

static void TestChooseModes() {
    ModeParams p;
    ModeTrip t;
    t.metres = 2500;
    const float mid[4] = {0, 1, 0, 0}, poor[4] = {1, 0, 0, 0}, rich[4] = {0, 0, 0, 1};
    ModeShares a = ChooseModes(p, t, mid);
    CHECK(a.s[kModeTransit] == 0.0f && a.s[kModeTaxi] == 0.0f); // no line, no taxis
    CHECK(std::fabs(a.s[0] + a.s[1] + a.s[2] + a.s[3] - 1.0f) < 1e-4f);
    t.transit = true, t.transitMin = 14, t.fare = 2.0f;
    ModeShares b = ChooseModes(p, t, mid);
    CHECK(b.s[kModeTransit] > 0.05f && b.s[kModeCar] < a.s[kModeCar]);
    t.fare = 0;
    ModeShares c = ChooseModes(p, t, mid);
    CHECK(c.s[kModeTransit] > b.s[kModeTransit]); // free transit shifts the shares
    t.fare = 2.0f;
    ModeShares pr = ChooseModes(p, t, poor), ri = ChooseModes(p, t, rich);
    CHECK(ri.s[kModeCar] > pr.s[kModeCar]);        // wealth shifts to the car
    CHECK(pr.s[kModeTransit] > ri.s[kModeTransit]);
    uint16_t q[kModeCount];
    QuantiseShares(b, q);
    CHECK(q[0] + q[1] + q[2] + q[3] == 1000);
    std::printf("modes 2.5 km: no line car %.2f walk %.2f | line car %.2f transit %.2f | free %.2f | poor car %.2f rich car %.2f\n",
                a.s[1], a.s[0], b.s[1], b.s[2], c.s[2], pr.s[1], ri.s[1]);
}

static void TestLineShiftsShares() {
    TransitTown t;
    t.Build(8, 400, 6);
    TransitSim s;
    t.Feed(s);
    s.Drain();
    const float car0 = s.Share(kModeCar), tr0 = s.Share(kModeTransit);
    CHECK(tr0 == 0.0f);
    CHECK(s.Verify() == 0);
    t.lines.push_back(t.Row(101, 3, 6));
    t.lines.push_back(t.Column(102, 4, 6, 1)); // a tram across it: transfers
    t.Feed(s);
    const int64_t before = s.evaluations;
    s.Drain();
    CHECK(s.Share(kModeTransit) > 0.02f && s.Share(kModeCar) < car0);
    CHECK(s.evaluations - before < (int64_t)s.links.size()); // only links near the lines were looked at again
    CHECK(s.Verify() == 0);
    // a zone with no stop in reach: none of its trips ride
    const int far = s.Zone(TransitTown::Key(0, 7));
    bool anyFar = false;
    for (const TransitLink& l : s.links) if ((l.from == far || l.to == far) && l.share[kModeTransit] > 0) anyFar = true;
    CHECK(far >= 0 && !anyFar && s.zones[(size_t)far].access.empty());
    // a free fare shifts more onto the lines; only links that can ride are re-evaluated
    const float tr1 = s.Share(kModeTransit);
    s.SetFare(0.0f, 1.0f);
    s.Drain();
    CHECK(s.Share(kModeTransit) > tr1);
    CHECK(s.Verify() == 0);
    std::printf("line added: car %.3f -> %.3f, transit %.3f, free fare %.3f; %lld evaluations for %zu links\n", car0, s.Share(kModeCar), tr1,
                s.Share(kModeTransit), (long long)(s.evaluations - before), s.links.size());
}

static void TestRidership() {
    TransitTown t;
    t.Build(8, 512, 8);
    t.lines.push_back(t.Row(1, 2, 1));        // one bus on a long line: crowded
    t.lines.push_back(t.Column(2, 5, 8, 5));  // a metro
    t.lines.back().closed = true;
    TransitSim s;
    t.Feed(s);
    s.Drain();
    int64_t expect = 0; // every rider boards once per leg, there and back
    for (const TransitLink& l : s.links)
        if (l.out.legs) expect += s.TransitRiders(l) * (l.out.legs + (l.back.legs ? l.back.legs : l.out.legs));
    CHECK(s.ledger.Boardings() == s.ledger.Alightings());
    CHECK(s.ledger.Boardings() == expect && expect > 0);
    for (size_t l = 0; l < s.net.lines.size(); ++l) CHECK(RidershipLedger::Conserved(s.net.lines[l], s.ledger.lines[l]));
    LineBook book;
    book.Day(s.net, s.ledger, 2.0f);
    const LineDay& bus = book.last[0];
    CHECK(bus.riders > 0 && bus.served <= bus.riders);
    const double carried = (double)s.ledger.lines[0].MaxLoad() * RidershipLedger::Served(s.net.lines[0], s.ledger.lines[0]);
    CHECK(carried <= (double)s.net.lines[0].capPerDir + 1.0); // capacity caps boarding
    CHECK(book.FaresYesterday() > 0 && book.UpkeepYesterday() > 0);
    CHECK(s.Verify() == 0);
    std::printf("ridership: %lld boardings; bus %lld wanting, %lld carried (cap %lld a direction); metro %lld\n", (long long)s.ledger.Boardings(),
                (long long)bus.riders, (long long)bus.served, (long long)s.net.lines[0].capPerDir, (long long)book.last[1].riders);
}

// A day of hourly input changes run with the given per-tick limits; parking off (its levels depend on how far the
// evaluation got at the hour, which is fixed in the game but differs between these two limits on purpose).
static TransitSim RunHours(int linksPerTick, int zonesPerTick, bool parking, int hours) {
    TransitTown t;
    t.Build(8, 512, 6);
    TransitSim s;
    s.p.linksPerTick = linksPerTick, s.p.zonesPerTick = zonesPerTick;
    for (int h = 0; h < hours; ++h) {
        if (h == 3) t.lines.push_back(t.Row(11, 3, 5));
        if (h == 6) t.lines.push_back(t.Column(12, 2, 4, 1));
        if (h == 9) t.lines.erase(t.lines.begin());
        if (h == 11) s.SetFare(1.0f, 0.5f);
        if (h == 13) s.SetTaxis(40);
        t.Churn(0.06);
        t.Feed(s);
        for (int m = 0; m < 60; ++m) s.Tick(h * 60 + m);
        if (parking) s.SummarizeParking();
    }
    s.Drain();
    return s;
}

static void TestIncrementalAndDeterminism() {
    const TransitSim slow = RunHours(7, 3, false, 16), fast = RunHours(5000, 500, false, 16);
    CHECK(slow.Verify() == 0 && fast.Verify() == 0); // nothing stale after the incremental run
    CHECK(SaveMode(slow) == SaveMode(fast));           // the result does not depend on how the work was spread
    const TransitSim a = RunHours(64, 16, true, 16), b = RunHours(64, 16, true, 16);
    CHECK(a.Verify() == 0);
    CHECK(SaveMode(a) == SaveMode(b));                 // same inputs, same bytes
    CHECK(a.Share(kModeTaxi) > 0.0f && a.Share(kModeTransit) > 0.0f);
    std::printf("incremental: %lld evaluations (slow) vs %lld (fast), %zu links, verify 0; car %.3f transit %.3f taxi %.3f walk %.3f\n",
                (long long)slow.evaluations, (long long)fast.evaluations, slow.links.size(), a.Share(kModeCar), a.Share(kModeTransit),
                a.Share(kModeTaxi), a.Share(kModeWalk));
}

static void TestSaveRoundTrip() {
    TransitSim a = RunHours(64, 16, true, 10);
    const std::vector<uint8_t> bytes = SaveMode(a);
    TransitSim c;
    ByteReader r(bytes);
    CHECK(ReadTransitMode(r, c) && r.AtEnd());
    c.Drain(); // the access sweep after a load: equal signatures, no evaluation
    CHECK(c.Verify() == 0 && c.evaluations == 0);
    CHECK(SaveMode(c) == bytes);
    CHECK(c.ledger.Boardings() == a.ledger.Boardings() && c.Share(kModeTransit) == a.Share(kModeTransit));
    LineBook book, book2;
    book.Set(11, 7), book.Set(5, 2);
    book.Day(a.net, a.ledger, 2.0f), book.Day(a.net, a.ledger, 2.0f);
    ByteWriter w;
    WriteTransitLine(w, book);
    ByteReader rl(w.bytes);
    CHECK(ReadTransitLine(rl, book2) && rl.AtEnd());
    ByteWriter w2;
    WriteTransitLine(w2, book2);
    CHECK(w.bytes == w2.bytes && book2.Setting(11) == 7 && book2.Setting(5) == 2 && book2.Setting(9) == 0);
    std::vector<uint8_t> bad = bytes; // a damaged payload is refused, never half-read
    bad.resize(bad.size() / 2);
    TransitSim d;
    ByteReader rb(bad);
    CHECK(!ReadTransitMode(rb, d));
    std::printf("save: MODE %zu bytes, LINE %zu bytes, round trips equal\n", bytes.size(), w.bytes.size());
}

static void TestOutside() {
    std::vector<OutsideIn> c(5);
    for (int i = 0; i < 4; ++i) c[(size_t)i].id = (uint64_t)i + 1, c[(size_t)i].kind = (uint8_t)i, c[(size_t)i].linked = true, c[(size_t)i].terminal = true;
    c[4].id = 9, c[4].kind = 1, c[4].linked = false; // not reaching the city: nothing comes through it
    OutsideDayIn d;
    d.residents = 30000, d.shopJobs = 2400, d.immigrants = 101, d.emigrants = 17, d.cargoImport = 12345, d.cargoExport = 999;
    OutsideWorld o, o2;
    o.Day(c, d), o2.Day(c, d);
    int64_t im = 0, em = 0, ci = 0, co = 0;
    for (const OutsideFlow& f : o.flows) im += f.immigrants, em += f.emigrants, ci += f.cargoIn, co += f.cargoOut;
    CHECK(im == 101 && em == 17 && ci == 12345 && co == 999);
    CHECK(o.flows[4].immigrants == 0 && o.flows[4].cargoIn == 0 && o.flows[4].tourists == 0);
    CHECK(o.flows[3].tourists > o.flows[0].tourists && o.touristDays.back() > 0);
    ByteWriter w, w2;
    WriteTransitOutc(w, o), WriteTransitOutc(w2, o2);
    CHECK(w.bytes == w2.bytes);
    OutsideWorld o3;
    ByteReader r(w.bytes);
    CHECK(ReadTransitOutc(r, o3) && r.AtEnd() && o3.flows.size() == 5 && o3.touristsTotal == o.touristsTotal);
    std::printf("outside: tourists %d a day (air %d, road %d), immigrants split %d/%d/%d/%d\n", o.touristDays.back(), o.flows[3].tourists,
                o.flows[0].tourists, o.flows[0].immigrants, o.flows[1].immigrants, o.flows[2].immigrants, o.flows[3].immigrants);
}

// The cost per game minute at a city of 400 zones and about 4000 links (a 50k-resident map's supercells are fewer):
// hourly input refreshes (churn past the thresholds), 12 lines added on day one, the parking pass. avg and worst tick.
static void Bench() {
    TransitTown t;
    t.Build(20, 400, 10);
    TransitSim s;
    t.Feed(s);
    s.Drain();
    double sum = 0, worst = 0, worstPart[4] = {0, 0, 0, 0}; // zones, links, lines, parking
    int ticks = 0;
    const int64_t evalBefore = s.evaluations;
    for (int h = 0; h < 72; ++h) {
        if (h == 2) for (int k = 0; k < 6; ++k) t.lines.push_back(t.Row(200 + k, 2 + 3 * k, 6)), t.lines.push_back(t.Column(300 + k, 1 + 3 * k, 6, k % 2 ? 5 : 1));
        t.Churn(0.03);
        for (int m = 0; m < 60; ++m) { // the host spreads the hourly inputs over the hour: zones :00, links :20, parking :40
            const double a = NowMs();
            int part = -1;
            if (m == 0) s.SetZones(t.zones), part = 0;
            if (m == 20) s.SetLinks(t.links), part = 1;
            if (m == 30 && h == 2) s.SetLines(t.lines), part = 2;
            if (m == 40) s.SummarizeParking(), part = 3;
            const double b = NowMs();
            s.Tick(h * 60 + m);
            const double ms = NowMs() - a;
            if (part >= 0) worstPart[part] = std::max(worstPart[part], b - a);
            sum += ms, worst = std::max(worst, ms), ++ticks;
        }
    }
    const double worstRefresh = std::max(std::max(worstPart[0], worstPart[1]), std::max(worstPart[2], worstPart[3]));
    std::printf("BENCH parts worst: zones %.3f links %.3f lines %.3f parking %.3f ms\n", worstPart[0], worstPart[1], worstPart[2], worstPart[3]);
    const double vt = NowMs();
    s.Drain();
    const int bad = s.Verify();
    const double verifyMs = NowMs() - vt;
    CHECK(bad == 0);
    std::printf("BENCH transit: %zu zones, %zu links, %zu lines: %.4f ms/tick avg, %.3f ms worst (refresh %.3f), %lld evaluations in 3 days, "
                "verify %.2f ms; riders %lld a day, transit %.3f\n",
                s.zones.size(), s.links.size(), s.net.lines.size(), sum / ticks, worst, worstRefresh, (long long)(s.evaluations - evalBefore), verifyMs,
                (long long)s.ledger.Boardings(), s.Share(kModeTransit));
    CHECK(sum / ticks < 0.2);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash still shows how far it got
    const bool benchOnly = argc > 1 && std::string(argv[1]) == "bench";
    if (!benchOnly) {
        TestChooseModes();
        TestLineShiftsShares();
        TestRidership();
        TestIncrementalAndDeterminism();
        TestSaveRoundTrip();
        TestOutside();
    }
    Bench();
    std::printf(g_fails ? "sim_transit_test: %d FAILED\n" : "sim_transit_test: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
