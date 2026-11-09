// Phase 3 save round trip (src/city/sim_env_save.h through sim_save.h's registry): POLL WATR WTHR saved mid-run, with
// and without a pass in flight, load into a fresh EnvSim that then continues exactly like the run that never stopped;
// factors and flags only take P3's columns and bits; a flood pushes events; a bad chunk starts P3 fresh.
// Headless. Build and run: tests\sim_env_save_test.bat
#include <cstdio>
#include <vector>

#include "city/sim_env_save.h"

using namespace tbd::city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Town {
    std::vector<CellKey> keys;
    std::vector<EnvSource> src;
    std::vector<EnvPump> pumps;
    Town() {
        for (int z = -8; z < 8; ++z) for (int x = -8; x < 8; ++x) keys.push_back({x, z});
        src.push_back(EnvUtilitySource(-300, 200, 6));
        src.push_back(EnvIndustrySiteSource(500, -400, 5, 1.0f));
        src.push_back(EnvUtilitySource(-900, -900, 13));
        for (int i = 0; i < 40; ++i) src.push_back(EnvRoadSource(-600.0f + 30 * i, 20.0f, 900.0f, 0.8f));
        pumps.push_back({900, -900, 20000, false}); // a shore intake far from the outlet
        pumps.push_back({300, 300, 4000, true});
    }
    static float Height(float x, float z) { return 8.0f + 0.003f * (x * x + z * z) / 100.0f; }
    EnvInput Input(int64_t hour, const WeatherChain& wx, const EnvSim& s, bool allHeights) const {
        EnvInput in;
        in.minute = hour * 60, in.sea = 2.0f, in.sources = src, in.pumps = pumps, in.cityKeys = keys;
        in.cityWaterOk.assign(keys.size(), 1);
        in.weather = WeatherAt(wx, in.minute, 99, SimCalendar());
        if (hour % 24 >= 6 && hour % 24 < 12) in.weather.rainMmH = 30.0f; // a storm every morning: floods
        for (CellKey k : keys)
            if (allHeights || !s.water.table.FindCell(k)) {
                auto& t = in.heights.Cell(k);
                for (int i = 0; i < 16; ++i) t[(size_t)i] = Height((k.x * 4 + i % 4 + 0.5f) * 32.0f, (k.z * 4 + i / 4 + 0.5f) * 32.0f);
            }
        return in;
    }
};
static uint64_t H(const EnvSim& s, const EnvResult& r) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (const auto* f : {&s.poll.air, &s.poll.noise, &s.poll.ground, &s.poll.water, &s.water.table, &s.water.depth})
        for (const auto& t : f->Tiles()) for (uint16_t v : t) mix(v);
    for (const EnvCellOut& c : r.cells) mix(c.air), mix(c.noise), mix(c.ground), mix(c.water), mix(c.floodMm), mix(c.flags);
    return h;
}

int main() {
    const Town town;
    const EnvParams p;
    const SimCalendar cal;
    // the reference: 72 hours straight
    EnvSim ref;
    EnvResult refR;
    WeatherChain wx;
    std::vector<uint64_t> refHash;
    int floods = 0;
    SimEventRing ring;
    for (int64_t h = 0; h < 72; ++h) {
        wx = WeatherChain::Replay(h * 60 / kWeatherStepMinutes, 99, cal, &wx);
        ref.Step(town.Input(h, wx, ref, false), p, refR);
        PushEnvEvents(refR, ring);
        floods += (int)refR.newlyFlooded.size();
        refHash.push_back(H(ref, refR));
    }
    std::printf("reference: %d cells flooded over 72 h, %llu events, mean air %.1f\n", floods, (unsigned long long)ring.Pushed(), refR.meanAir);
    CHECK(floods > 0 && ring.Pushed() == (uint64_t)floods);
    // save at hour 30 with the hour-30 pass in flight (cur = after hour 29, pending = after hour 30), load, continue
    for (int pendingCase = 0; pendingCase < 2; ++pendingCase) {
        EnvSim cur, pend;
        EnvResult curR, pendR;
        WeatherChain w;
        for (int64_t h = 0; h <= 30; ++h) {
            w = WeatherChain::Replay(h * 60 / kWeatherStepMinutes, 99, cal, &w);
            if (h < 30 || !pendingCase) cur.Step(town.Input(h, w, cur, false), p, curR);
        }
        if (pendingCase) pend = cur, pend.Step(town.Input(30, w, cur, false), p, pendR);
        EnvSaveView v{&cur, &curR, pendingCase ? &pend : nullptr, pendingCase ? &pendR : nullptr};
        EnvWeatherState ws;
        ws.chain = w, ws.lastSeason = cal.SeasonOf(30 * 60);
        EnvLoadInto into;
        EnvWeatherState wsIn;
        SimSaveRegistry reg;
        reg.Register({simchunk::POLL, kEnvPollVersion, 0, [&](ByteWriter& bw) { EnvSavePoll(bw, v); }, [&](ByteReader& r, uint16_t) { return EnvLoadPoll(r, into); }, [] {}});
        reg.Register({simchunk::WATR, kEnvWaterVersion, 0, [&](ByteWriter& bw) { EnvSaveWater(bw, v); }, [&](ByteReader& r, uint16_t) { return EnvLoadWater(r, into); }, [] {}});
        reg.Register({simchunk::WTHR, kEnvWeatherVersion, 0, [&](ByteWriter& bw) { EnvSaveWeather(bw, ws); }, [&](ByteReader& r, uint16_t) { return EnvLoadWeather(r, wsIn); }, [] {}});
        const std::vector<uint8_t> file = reg.Save();
        const SimLoadReport rep = reg.Load(file);
        CHECK(rep.Loaded() == 3);
        CHECK(into.hasPollPending == (pendingCase == 1) && into.hasWaterPending == (pendingCase == 1));
        CHECK(wsIn.chain.step == w.step && wsIn.chain.kind == w.kind && wsIn.chain.windDir == w.windDir);
        // stitch the two halves the host's way: the loaded applied state, then (if any) the pending one becomes current
        EnvSim s = pendingCase ? into.pending : into.cur;
        EnvResult r = pendingCase ? into.pendingResult : into.curResult;
        CHECK(H(s, r) == refHash[30]); // heights are zero until re-sampled (the hash leaves them out)
        bool first = true;
        for (int64_t h = 31; h < 72; ++h) {
            w = WeatherChain::Replay(h * 60 / kWeatherStepMinutes, 99, cal, &w);
            s.Step(town.Input(h, w, s, first), p, r); // the first pass after a load re-samples every height
            first = false;
            if (H(s, r) != refHash[(size_t)h]) { std::printf("diverged at hour %lld (pending %d)\n", (long long)h, pendingCase); CHECK(false); break; }
        }
        std::printf("round trip (%s): %zu bytes, continues identically to hour 71\n", pendingCase ? "pass in flight" : "idle", file.size());
    }
    // P3 writes only its factor columns and flag bits
    {
        CellFactors f;
        f.Fit(refR.cells.size());
        f.cells[3].crime = 77, f.cells[3].rentBurden = 11;
        WriteEnvFactors(refR, f);
        CHECK(f.cells[3].crime == 77 && f.cells[3].rentBurden == 11);
        CHECK(f.cells[0].air == refR.cells[0].air && f.cells[0].water == refR.cells[0].water);
        BuildingRegistry reg;
        reg.BeginSync();
        std::vector<BuildingId> ids;
        for (size_t c = 0; c < town.keys.size(); ++c) {
            BuildingRow row; row.cellX = town.keys[c].x, row.cellZ = town.keys[c].z;
            ids.push_back(reg.Upsert(BOrigin::Lot, c + 1, row));
        }
        reg.EndSync();
        SimFlagTable t;
        for (BuildingId b : ids) t.Write(b, kFlagNoPower | kFlagCrime, kFlagNoPower | kFlagCrime);
        WriteEnvFlags(refR, reg, t);
        int env = 0, kept = 0;
        for (size_t c = 0; c < ids.size(); ++c) {
            env += (t.Get(ids[c]) & SimFlagPhaseMask(3)) == refR.cells[c].flags;
            kept += t.Has(ids[c], kFlagNoPower) && t.Has(ids[c], kFlagCrime);
        }
        CHECK(env == (int)ids.size() && kept == (int)ids.size());
        std::printf("flags: polluted %d noisy %d flooded %d cells; other phases' bits untouched\n", refR.polluted, refR.noisy, refR.flooded);
        CHECK(refR.polluted > 0 && refR.polluted < (int)town.keys.size() && refR.noisy > 0);
    }
    // a corrupt chunk: P3's load fails, so the registry calls fresh
    {
        EnvLoadInto into;
        bool fresh = false;
        SimSaveRegistry reg;
        reg.Register({simchunk::POLL, kEnvPollVersion, 0, [](ByteWriter& bw) { bw.U32(7); }, [&](ByteReader& r, uint16_t) { return EnvLoadPoll(r, into); }, [&] { fresh = true; }});
        const SimLoadReport rep = reg.Load(reg.Save());
        CHECK(fresh && rep.Loaded() == 0);
    }
    std::printf(g_fail ? "sim_env_save_test: %d FAILED\n" : "sim_env_save_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
