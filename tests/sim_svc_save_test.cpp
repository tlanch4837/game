// Phase 4 save (src/city/sim_svc_save.h through sim_save.h's registry): SERV INCD saved mid-run (vehicles out, patients
// in beds, fires waiting) load into a fresh world that then continues exactly like the run that never stopped (the
// saves of both are byte-identical days later); the flags rebuilt after the load match a full recompute; a bad chunk
// starts the phase fresh. Headless. Build and run: tests\sim_svc_save_test.bat
#include "sim_svc_town.h"

using namespace svctest;

static void Days(SvcWorld& w, CellFactors& f, SimFlagTable& t, int64_t& minute, int days) {
    for (int m = 0; m < days * 1440; ++m, ++minute) SvcTick(w, minute, &f), SvcFlushFlags(w, t, minute, nullptr);
}
static std::vector<uint8_t> Bytes(const SvcWorld& w) {
    ByteWriter b;
    SvcSaveServ(b, w), SvcSaveIncd(b, w);
    return b.bytes;
}
static void Setup(Town& t) {
    t.Build(10, 400);
    t.AddSite(5, 5, kTabPolice, "Police Post", 12, 1);
    t.AddSite(2, 7, kTabFire, "Neighbourhood Firehouse", 8, 1);
    t.AddSite(7, 2, kTabHealth, "Medical Centre", 18, 2);
    t.AddSite(8, 8, kTabDeath, "Crematorium", 16, 2, 1.0f, 4, 4);
}

int main() {
    Town t;
    Setup(t);
    SvcWorld a;
    a.seed = 9;
    t.Into(a, 160);
    CellFactors fa = Neutral(100), fb = Neutral(100);
    SimFlagTable ta, tb;
    int64_t ma = 1;
    Days(a, fa, ta, ma, 3);
    for (int k = 0; k < 6; ++k) SvcReport(a, kIncBody, (uint32_t)(k * 7 + 1), ma); // hearses out at the save
    for (int m = 0; m < 37; ++m, ++ma) SvcTick(a, ma, &fa);
    CHECK(a.inc.Live() > 0);
    // save through the registry
    SimSaveRegistry reg;
    SvcWorld b;
    b.seed = 9;
    t.Into(b, 160);
    auto add = [&reg](uint32_t fourcc, SvcWorld* src, SvcWorld* dst, bool serv) {
        SimChunkDef d;
        d.fourcc = fourcc, d.version = serv ? kSvcServVersion : kSvcIncdVersion;
        d.save = [src, serv](ByteWriter& w) { serv ? SvcSaveServ(w, *src) : SvcSaveIncd(w, *src); };
        d.load = [dst, serv](ByteReader& r, uint16_t) { return serv ? SvcLoadServ(r, *dst) : SvcLoadIncd(r, *dst); };
        d.fresh = [dst] { dst->inc.Clear(), dst->site.clear(); };
        reg.Register(d);
    };
    add(simchunk::SERV, &a, &b, true), add(simchunk::INCD, &a, &b, false);
    const std::vector<uint8_t> file = reg.Save();
    const SimLoadReport rep = reg.Load(file);
    CHECK(rep.Loaded() == 2);
    SvcApplyCoverage(b, SvcCovIn(a.cov), SvcCovOut(a.out), std::vector<SvcCellStat>(a.cellStat)); // the host recomputes it
    CHECK(b.crimeF == a.crimeF && b.fireF == a.fireF); // carried over by key from SERV
    SvcRecount(b);
    b.trips = a.trips;
    CHECK(Bytes(a) == Bytes(b));
    CHECK(SvcVerifyCounts(b) == 0);
    int64_t mb = ma;
    Days(a, fa, ta, ma, 4), Days(b, fb, tb, mb, 4);
    CHECK(Bytes(a) == Bytes(b));
    CHECK(SvcVerifyCounts(a) == 0 && SvcVerifyCounts(b) == 0);
    while (!SvcFlagsSettled(b)) SvcFlushFlags(b, tb, mb, nullptr);
    while (!SvcFlagsSettled(a)) SvcFlushFlags(a, ta, ma, nullptr);
    CHECK(SvcVerifyFlags(a, ta, ma, nullptr) == 0 && SvcVerifyFlags(b, tb, mb, nullptr) == 0);
    { // a corrupted INCD: that chunk starts fresh, SERV still loads
        std::vector<uint8_t> bad = file;
        bad[bad.size() - 3] ^= 0x5A;
        SvcWorld c;
        t.Into(c, 160);
        SimSaveRegistry r2;
        SimChunkDef s, i;
        s.fourcc = simchunk::SERV, s.load = [&c](ByteReader& r, uint16_t) { return SvcLoadServ(r, c); };
        i.fourcc = simchunk::INCD, i.load = [&c](ByteReader& r, uint16_t) { return SvcLoadIncd(r, c); }, i.fresh = [&c] { c.inc.Clear(); };
        r2.Register(s), r2.Register(i);
        const SimLoadReport rr = r2.Load(bad);
        CHECK(rr.Find(simchunk::SERV)->status == SimChunkStatus::Loaded);
        CHECK(rr.Find(simchunk::INCD)->status != SimChunkStatus::Loaded && c.inc.Live() == 0);
    }
    std::printf("%s (%d failures)\n", failures ? "sim_svc_save_test FAILED" : "sim_svc_save_test passed", failures);
    return failures ? 1 : 0;
}
