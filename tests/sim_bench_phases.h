// sim_bench's Phases 2, 4, 5 and 7 (included by sim_bench.cpp after its Bench): each phase's main-thread systems at the
// game's periods on the bench's city (its lots, CitySim cells and citizen records), the workers' passes timed apart.
//   P2  land.lots every minute (LotLevels), demand every 10 minutes, a household rent group every 15, the hourly land
//       snapshot (main) and LandStep (worker)
//   P4  the svc minute (due incidents, the cell group, the flags flush, the seat slice every 10 minutes, a garbage group)
//       on the bench's lots as buildings, sites scaled to the city; the coverage pass (worker) every game hour
//   P5  the transit minute: zones at :00, links at :20, parking at :40, the dirty links every minute, the day's book
//   P7  the hourly tax and happiness group, the daily finish and taxBurden, the mods compose (on change: once)
#pragma once

#include "city/sim_demand.h"
#include "city/sim_gov.h"
#include "city/sim_land.h"
#include "city/sim_land_lots.h"
#include "city/sim_land_rent.h"
#include "city/sim_svc.h"
#include "city/sim_transit_save.h"

namespace bench_phases {

// ---- P2
struct P2 {
    LotLevels lots;
    LandState land;
    LandInput in;
    LandResult out;
    RentDay rent;
    RentParams rp;
    DemandParams dp;
    double bars[kDemandCats] = {};
};

inline void P2Lots(P2& p, Bench& b, int64_t m) {
    p.lots.Tick(b.zones, m, p.bars, [&](const Lot& l, size_t li) {
        LotInputs i;
        i.land = p.land.field.CellCount() ? p.land.field.At(l.c.x, l.c.z) : (uint8_t)60;
        const uint8_t u = li < b.lotUtil.size() ? b.lotUtil[li] : kLotAll;
        i.bits = (uint8_t)((u & kLotPower ? kLotInPower : 0) | (u & kLotWater ? kLotInWater : 0) | (u & kLotSewage ? kLotInSewage : 0));
        return i;
    });
    p.lots.events.clear();
}

inline void P2Demand(P2& p, Bench& b) {
    DemandInput d;
    for (const CellSim& c : b.cells.sim) {
        d.population += c.Population(), d.homes += c.homeCap;
        for (int k = 0; k < 3; ++k) d.jobs[k] += c.jobCap[k], d.vacancies[k] += std::max(0, (int)c.jobCap[k] - (int)c.employed[k]);
    }
    d.adults = (int64_t)b.cells.Adults(), d.rentPressure = p.rent.last.Pressure();
    SmoothDemand(p.bars, ComputeDemand(d, p.dp), p.dp, 10.0);
}

// One of 96 household groups (the records), as city_sim_p2.inl SimP2RentGroup.
inline void P2Rent(P2& p, Bench& b) {
    const LifeWorld& w = b.life.w;
    for (uint32_t hi = (uint32_t)(p.rent.group ? p.rent.group : 96); hi < w.hh.Capacity(); hi += 96) {
        if (!w.hh.At(hi).Valid() || hi >= w.hh.rentMonth.size() || !w.hh.home[hi]) continue;
        const bool hit = RentBurdened(p.rp, w.hh.rentMonth[hi], w.hh.incomeMonth[hi]);
        const int band = std::clamp<int>(w.hh.wealth[hi], 0, 3);
        ++p.rent.cur.households[band], p.rent.cur.burdened[band] += hit ? 1 : 0;
        p.rent.cur.spend += w.hh.spendMonth[hi], p.rent.cur.income += w.hh.incomeMonth[hi], p.rent.cur.persons += w.hh.size[hi];
        p.rent.Count(BuildingId(w.hh.home[hi]).Index(), hit);
    }
    if (++p.rent.group >= 96) { // the day's flags: every building's high-rent check (the game writes the changed ones)
        p.rent.group = 0, p.rent.last = p.rent.cur, p.rent.cur = Affordability();
        uint32_t high = 0;
        for (uint32_t i = 1; i < b.reg.Capacity(); ++i) high += p.rent.High(p.rp, i) ? 1 : 0;
        std::fill(p.rent.seen.begin(), p.rent.seen.end(), (uint16_t)0), std::fill(p.rent.hit.begin(), p.rent.hit.end(), (uint16_t)0);
        (void)high;
    }
}

// The hourly snapshot of every cell (main thread).
inline void P2Snapshot(P2& p, Bench& b, int64_t m) {
    p.in = LandInput();
    p.in.minute = m;
    p.in.cells.resize(b.cells.sim.size());
    for (size_t c = 0; c < b.cells.sim.size(); ++c) {
        const CellSim& cs = b.cells.sim[c];
        const CellFactor& f = b.factors.At((int)c);
        LandCellIn& o = p.in.cells[c];
        o.key = b.cells.cells.cells[c].key, o.road = 1;
        o.jobs = (uint16_t)std::min(65535, cs.jobCap[0] + cs.jobCap[1] + cs.jobCap[2]), o.industry = (uint16_t)cs.jobCap[kJobIndustrial];
        o.people = (uint16_t)std::min<uint32_t>(65535, cs.Population()), o.commuteMin = f.commuteMin;
        o.cov = (uint8_t)((f.coverage[kSvcFire] + f.coverage[kSvcPolice] + f.coverage[kSvcHealth] + f.coverage[kSvcSchool]) / 4);
        o.air = f.air, o.noise = f.noise, o.ground = f.ground, o.crime = f.crime;
    }
}

// ---- P4: the bench's CitySim cells as cov cells (in its order), a road node per cell (the svc graph is a cell grid at
// city speed), every lot a building, the sites of sim_svc_bench scaled to the lot count.
struct P4 {
    SvcWorld w;
    SvcSeatPass seats;
    SvcGarbage garb;
    std::vector<SvcGarbFacility> fac;
    std::shared_ptr<const SvcGraph> graph;
    std::vector<SvcSite> sites;
    std::unordered_map<uint64_t, double> pile;
    std::vector<std::pair<uint64_t, double>> mirror;
    std::vector<SvcTrip> trips;
    std::vector<uint64_t> changed;
    CellFactors f;
};

inline SvcCovIn P4Input(const P4& p, const Bench& b) {
    SvcCovIn in;
    for (size_t c = 0; c < b.cells.cells.cells.size(); ++c) {
        SvcCellIn ci;
        const CellKey k = b.cells.cells.cells[c].key;
        ci.key = k, ci.x = (k.x + 0.5f) * kCityCellSize, ci.z = (k.z + 0.5f) * kCityCellSize, ci.node = (int)c, ci.people = b.cells.sim[c].Population();
        in.cells.push_back(ci);
    }
    in.sites = p.sites, in.graph = p.graph;
    return in;
}

inline void P4Build(P4& p, Bench& b) {
    const auto& cc = b.cells.cells;
    LinkTable t; // neighbouring cells joined both ways at 30 km/h
    for (size_t c = 0; c < cc.cells.size(); ++c) {
        const CellKey k = cc.cells[c].key;
        const V2 a{(k.x + 0.5f) * kCityCellSize, (k.z + 0.5f) * kCityCellSize};
        for (const CellKey n : {CellKey{k.x + 1, k.z}, CellKey{k.x, k.z + 1}}) {
            const int j = cc.Find(n);
            if (j < 0) continue;
            const V2 q{(n.x + 0.5f) * kCityCellSize, (n.z + 0.5f) * kCityCellSize};
            t.AddStraight((int)c, j, a, q, 30.0f, 800), t.AddStraight(j, (int)c, q, a, 30.0f, 800);
        }
    }
    t.nodes = (int)cc.cells.size();
    t.BuildAdjacency();
    p.graph = std::make_shared<const SvcGraph>(SvcGraph::From(t));
    const struct { int tab; const char* name; int wp, veh, n; } kSites[] = {
        {kTabFire, "Fire Station", 20, 4, 10}, {kTabPolice, "Police Station", 36, 3, 10}, {kTabHealth, "Community Hospital", 36, 4, 8},
        {kTabHealth, "Small Park", 4, 0, 8}, {kTabDeath, "Crematorium", 16, 2, 4}, {kTabEducation, "Elementary School", 27, 0, 12},
        {kTabEducation, "High School", 162, 0, 5}, {kTabEducation, "University", 75, 0, 2}, {kTabComms, "Cell Tower", 12, 0, 8},
        {kTabComms, "Post Office", 20, 4, 3}};
    const double scale = std::max(0.2, (double)b.zones.lots.size() / 15000.0); // the 50k-resident set at about 15k lots
    uint32_t k = 0;
    for (const auto& s : kSites)
        for (int i = 0; i < std::max(1, (int)std::lround(s.n * scale)); ++i, ++k) {
            const int c = (int)((k * 7919u) % (uint32_t)cc.cells.size());
            SvcSite site;
            site.id = 0xE00000u + k, site.spec = SvcFunded(SvcSpecFor(s.tab, s.name, s.wp, s.veh, 3, 3), 1.0f);
            site.x = (cc.cells[(size_t)c].key.x + 0.5f) * kCityCellSize, site.z = (cc.cells[(size_t)c].key.z + 0.5f) * kCityCellSize, site.node = c;
            p.sites.push_back(site);
        }
    p.w.seed = 5;
    p.f = b.factors;
    p.f.Fit(cc.cells.size());
}

// The building list (the game: on a registry change) and the garbage lots, then the first coverage.
inline void P4Buildings(P4& p, Bench& b) {
    const auto& types = ZoneTypes();
    std::vector<SvcBldg> list;
    std::vector<SvcGarbLot> glots;
    for (const Lot& l : b.zones.lots) {
        SvcBldg h;
        h.id = b.reg.Find(BOrigin::Lot, l.id).v, h.lot = l.id, h.key = CellKeyAt(l.c.x, l.c.z), h.cell = b.cells.cells.Find(h.key), h.node = h.cell;
        const ZoneCat cat = types[(size_t)l.type].cat;
        const LotCap cap = LotCapacity(l);
        h.kind = cat == ZoneCat::Residential ? kBkHome : cat == ZoneCat::Commercial ? kBkShop : cat == ZoneCat::Office ? kBkOffice : kBkIndustry;
        h.homes = (uint16_t)cap.homes, h.jobs = (uint16_t)cap.Jobs(), h.level = (uint8_t)l.level;
        list.push_back(h);
        SvcGarbLot g;
        g.lot = l.id, g.x = l.c.x, g.z = l.c.z, g.comp = 1, g.node = h.node, g.perMonth = 12;
        glots.push_back(g);
    }
    for (const SvcSite& s : p.sites) { SvcBldg h; h.id = s.id, h.key = CellKeyAt(s.x, s.z), h.node = s.node, h.kind = kBkService; list.push_back(h); }
    std::sort(list.begin(), list.end(), [](const SvcBldg& x, const SvcBldg& y) { return x.id < y.id; });
    SvcSetBuildings(p.w, std::move(list));
    p.garb.SetLots(glots);
    p.fac.assign(4, SvcGarbFacility());
    for (int i = 0; i < 4; ++i) {
        SvcGarbFacility& f = p.fac[(size_t)i];
        f.id = (uint32_t)i + 1, f.comp = 1, f.perMonth = 60000.0f * (float)std::max(1.0, b.zones.lots.size() / 15000.0), f.range = 4000;
        f.x = p.sites.empty() ? 0.0f : p.sites[(size_t)i % p.sites.size()].x, f.z = p.sites.empty() ? 0.0f : p.sites[(size_t)i % p.sites.size()].z;
    }
}

// The coverage pass: the snapshot (main), SvcCover + SvcPrepare (worker), the install (main). Returns the worker's ms.
inline double P4Coverage(P4& p, Bench& b, double& mainMs) {
    const double a = NowMs();
    SvcPrepared prep;
    prep.cov = P4Input(p, b), prep.bldSnap = p.w.bld;
    prep.stats.resize(prep.cov.cells.size());
    for (size_t c = 0; c < prep.stats.size(); ++c) {
        const CellSim& cs = b.cells.sim[c];
        prep.stats[c].residents = cs.Population(), prep.stats[c].homes = cs.homeCap, prep.stats[c].jobs = (uint32_t)cs.jobCap[0] + cs.jobCap[1] + cs.jobCap[2];
        prep.stats[c].industryJobs = cs.jobCap[kJobIndustrial];
    }
    const double w0 = NowMs();
    SvcCover(prep.cov, prep.out);
    SvcPrepare(prep);
    const double worker = NowMs() - w0;
    SvcInstall(p.w, std::move(prep));
    mainMs = NowMs() - a - worker;
    return worker;
}

inline void P4Minute(P4& p, Bench& b, int64_t m) {
    SvcTick(p.w, m, &p.f);
    SvcLotLevelFn level = [&p](uint64_t lot) { const auto it = p.pile.find(lot); return it == p.pile.end() ? 0.0f : (float)(it->second / 36.0); };
    SvcFlushFlags(p.w, b.flagTable, m, level);
    if (m % 10 == 0) {
        LifeWorld& w = b.life.w;
        SvcSeatSlice(p.w, w.cit, p.seats, w.cit.Capacity() / 144 + 1, [&w](uint32_t ci) { return w.AgeOf(ci); }, [&w](uint32_t ci) {
            const uint32_t h = w.cit.household[ci];
            const LifeBuilding* L = h ? w.B(w.hh.home[LifeWorld::Ix(h)]) : nullptr;
            return L ? L->cell : -1;
        });
    }
    SvcGarbStats st;
    p.mirror.clear();
    p.garb.Visit((int)(m % kGarbGroups), m, p.fac, p.trips, p.changed, p.mirror, st);
    for (const auto& [lot, v] : p.mirror) p.pile[lot] = v;
}

// ---- P5: the supercells as zones, the records' commute links, a bus line along every 4th street row and column (the
// city's lines), two road connections; the game's minute schedule (city_sim_p5.inl Tick).
struct P5 {
    TransitSim world;
    LineBook book;
    OutsideWorld outside;
    std::vector<OutsideIn> conns;
    std::vector<TransitZoneIn> zin;
    std::vector<TransitLinkIn> lin;
};
inline uint64_t P5Key(const Bench& b, int s) { const CellKey k = b.cells.SuperKey(s); return (uint64_t)(uint32_t)k.x << 32 | (uint32_t)k.z; }

inline void P5Lines(P5& p, const Bench& b) {
    const int B = (int)std::lround(std::sqrt((double)b.net.nodes.size())) - 1; // blocks per side
    std::vector<LineInput> lines;
    for (int r = 0, id = 1; r <= B; r += 4) {
        for (int dir = 0; dir < 2; ++dir, ++id) {
            LineInput l;
            l.id = (uint64_t)id, l.mode = 0, l.vehicles = 4;
            for (int i = 0; i <= B; i += 2) l.x.push_back((float)(dir ? r : i) * 120.0f), l.z.push_back((float)(dir ? i : r) * 120.0f);
            for (size_t s = 1; s < l.x.size(); ++s) l.legLen.push_back(240.0f);
            lines.push_back(l);
        }
    }
    p.world.SetLines(lines);
    p.book.Keep(p.world.net);
    p.conns = {{1, 0, true, true, -200.0f, 0.0f}, {2, 0, true, true, (float)B * 120.0f + 200.0f, (float)B * 120.0f}};
}

inline void P5Zones(P5& p, const Bench& b) {
    p.zin.clear();
    for (int s = 0; s < b.cells.Supercells(); ++s) {
        double w = 0, x = 0, z = 0;
        TransitZoneIn q;
        q.key = P5Key(b, s);
        uint32_t pop = 0, jobs = 0;
        for (int c : b.cells.SuperCellList(s)) {
            const CellSim& cs = b.cells.sim[(size_t)c];
            const uint32_t cj = (uint32_t)cs.jobCap[0] + cs.jobCap[1] + cs.jobCap[2];
            const double cw = (double)cs.Population() + cj;
            const CellKey k = b.cells.cells.cells[(size_t)c].key;
            x += cw * (k.x + 0.5) * kCityCellSize, z += cw * (k.z + 0.5) * kCityCellSize, w += cw, pop += cs.Population(), jobs += cj;
            for (int i = 0; i < 4; ++i) q.wealth[i] += cs.wealth[i];
        }
        if (w <= 0) continue;
        q.x = std::round((float)(x / w)), q.z = std::round((float)(z / w)), q.pop = pop, q.jobs = jobs;
        p.zin.push_back(q);
    }
    p.world.SetZones(p.zin);
}

inline void P5Minute(P5& p, Bench& b, int64_t m) {
    const int mh = (int)(m % 60);
    p.world.SetFare(1.0f, 1.0f);
    if (mh == 0) P5Zones(p, b), p.world.SetTaxis(20);
    else if (mh == 20) {
        p.lin.clear();
        b.cells.ForEachLink([&](int h, int j, int32_t n) { if (h != j && n > 0) p.lin.push_back({P5Key(b, h), P5Key(b, j), (float)n}); });
        p.world.SetLinks(p.lin);
    } else if (mh == 40) p.world.SummarizeParking();
    p.world.Tick(m);
    if (m % 1440 == 5) {
        OutsideDayIn d;
        for (const TransitZoneIn& q : p.zin) d.residents += q.pop;
        p.outside.Day(p.conns, d);
        p.book.Day(p.world.net, p.world.ledger, p.world.p.fare * p.world.p.fareMul);
    }
}

// ---- P7: hoods = supercells (as the bench's P1), 8 districts; the hourly group, the ledger finish
struct P7 {
    GovState g;
    std::vector<int> cellHood;
    GovIncome inc;
};
inline GovTaxInput P7Input(P7& p, Bench& b) {
    GovTaxInput in;
    in.w = &b.life.w, in.s = &b.cells, in.cellHood = &p.cellHood, in.hoodMods = &p.g.mods, in.rates = &p.g.rates, in.mainRate10 = 100;
    return in;
}
inline void P7Compose(P7& p, Bench& b) {
    p.cellHood.resize(b.cells.sim.size());
    for (size_t c = 0; c < p.cellHood.size(); ++c) p.cellHood[c] = b.cells.SuperOf((int)c);
    if (p.g.districts.Count() == 0)
        for (int d = 1; d <= 8; ++d) {
            p.g.districts.Add("D" + std::to_string(d));
            std::vector<uint64_t> keys;
            for (size_t c = (size_t)d; c < p.cellHood.size(); c += 9) keys.push_back(PackKey(b.cells.cells.cells[c].key));
            p.g.districts.Paint(keys, d);
        }
    p.g.Compose(p.cellHood, b.cells.Supercells(), 1, [&b](int c) { return PackKey(b.cells.cells.cells[(size_t)c].key); });
}
inline void P7Hour(P7& p, Bench& b, int64_t m) {
    const int hour = (int)((m / 60) % 24);
    if (hour == 0) p.g.acc.Clear(), p.g.happy.Clear();
    GovTaxSlice(P7Input(p, b), hour, p.g.acc);
    const auto& c = b.life.w.cit;
    for (uint32_t ci = (uint32_t)hour; ci < c.Capacity(); ci += kGovSlices)
        if (ci && c.At(ci).Valid()) p.g.happy.sum += c.happy[ci], ++p.g.happy.count;
}
inline void P7Day(P7& p, Bench& b) {
    const int rates[4] = {100, 100, 100, 100};
    const double perJob[3] = {60, 80, 50};
    GovTaxFinish(p.g.acc, P7Input(p, b), rates, perJob, 40, 1, p.inc);
    p.g.WriteTaxBurden(b.factors, p.cellHood, 100, b.cells.sim.size());
}

} // namespace bench_phases
