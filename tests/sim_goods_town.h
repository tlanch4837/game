// A synthetic goods town for the Phase 6 tests (sim_goods_test, sim_goods_companies_test): sites on a grid, run helpers.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "city/sim_goods_save.h"

using namespace tbd::city;

static int g_fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fails; } \
    } while (0)

struct GoodsTown {
    std::vector<GoodsSite> sites;
    uint32_t next = 1;
    int32_t node = 1;
    GoodsSite& Add(CompanyKind k, int32_t x, int32_t z, uint16_t jobs, uint16_t workers) {
        GoodsSite s;
        s.building = Handle::Make(next++, 1).v, s.kind = k, s.x = x, s.z = z, s.node = node++, s.cell = (x / 128) & 63;
        s.jobs = jobs, s.workers = workers;
        sites.push_back(s);
        return sites.back();
    }
    GoodsSite& Hub(Good g, int32_t x, int32_t z, int32_t tonnesPerDay) {
        GoodsSite& s = Add(CompanyKind::Raw, x, z, 0, 0);
        s.raw = g, s.rawMilliPerHour = tonnesPerDay * 10 * 1000 / 24;
        return s;
    }
    GoodsSite& Plant(int recipe, int32_t x, int32_t z, uint16_t workers) {
        GoodsSite& s = Add(CompanyKind::Processor, x, z, workers, workers);
        s.recipe = (uint8_t)recipe;
        return s;
    }
    GoodsSite& Shop(int32_t x, int32_t z, uint16_t workers, int32_t locals = 50) {
        GoodsSite& s = Add(CompanyKind::Shop, x, z, workers, workers);
        s.localResidents = locals;
        return s;
    }
};

// A whole chain: the five hubs, one plant per recipe, shops, residents and (optionally) one outside connection.
inline void BuildChain(GoodsTown& t, GoodsWorld& w, bool outside, int shops = 6) {
    const Good raws[5] = {Good::Grain, Good::Livestock, Good::Timber, Good::Oil, Good::Ore};
    for (int i = 0; i < 5; ++i) t.Hub(raws[i], 300 * i, 0, 30);
    for (int r = 0; r < kRecipeCount; ++r) t.Plant(r, 300 * r, 600, 12);
    for (int i = 0; i < shops; ++i) t.Shop(200 * i, 1200, 8);
    w.residents = 3000;
    if (outside) w.outsides.push_back({-2000, 0, 999});
}

// Mass ledger check for every good: stock + in flight == produced - used + imported - consumed - exported - lost.
inline bool MassOk(GoodsWorld& w, bool print = false) {
    const GoodsLedger l = w.Running();
    bool ok = true;
    for (int g = 0; g < kGoodCount; ++g) {
        const size_t i = (size_t)g;
        const int64_t want = l.produced[i] - l.used[i] + l.imported[i] - l.consumed[i] - l.exported[i] - l.lost[i];
        const int64_t have = w.Mass((Good)g);
        if (have != want) {
            ok = false;
            if (print) std::printf("  mass %s: have %lld want %lld\n", GoodName((Good)g), (long long)have, (long long)want);
        }
    }
    return ok;
}

inline void RunMinutes(GoodsWorld& w, int64_t& minute, int64_t n) {
    for (int64_t i = 0; i < n; ++i) w.Tick(minute++);
}

// The three P6 chunks through the real registry: the file's bytes.
inline std::vector<uint8_t> SaveWorld(GoodsWorld& w) {
    SimSaveRegistry reg;
    SimChunkDef a, b, c;
    a.fourcc = simchunk::COMP, a.save = [&w](ByteWriter& o) { WriteGoodsComp(o, w); };
    b.fourcc = simchunk::GOOD, b.save = [&w](ByteWriter& o) { WriteGoodsGood(o, w); };
    c.fourcc = simchunk::TRAD, c.save = [&w](ByteWriter& o) { WriteGoodsTrad(o, w); };
    reg.Register(a), reg.Register(b), reg.Register(c);
    return reg.Save();
}
inline SimLoadReport LoadWorld(GoodsWorld& w, const std::vector<uint8_t>& file) {
    SimSaveRegistry reg;
    SimChunkDef a, b, c;
    a.fourcc = simchunk::COMP, a.load = [&w](ByteReader& r, uint16_t) { return ReadGoodsComp(r, w); };
    b.fourcc = simchunk::GOOD, b.load = [&w](ByteReader& r, uint16_t) { return ReadGoodsGood(r, w); };
    c.fourcc = simchunk::TRAD, c.load = [&w](ByteReader& r, uint16_t) { return ReadGoodsTrad(r, w); };
    a.fresh = b.fresh = c.fresh = [&w] { w.Clear(); };
    reg.Register(a), reg.Register(b), reg.Register(c);
    return reg.Load(file);
}
