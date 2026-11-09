// Phase 6: the goods world. Companies, trucks, import and export, flags and the goods-availability factor in one
// headless object (tests\sim_goods_test, tests\sim_goods_companies_test); src/city_sim_p6.inl is its host.
//
// Tick(minute) is called every game minute; it only works on 5-minute boundaries, on a twelfth of the companies
// (index % 12) each time, so every company has one turn an hour:
//   its hour       production (or sales), then its match: orders to the nearest local seller with stock (else an import
//                  from the nearest outside connection), exports of a surplus, its flags
//   every hour     shop demand from the residents (slice 0); flag counts and the per-cell
//                  availability (slice 11); shipments unload on every boundary
//   every day      wages and upkeep, profit, bankruptcy and reopening, the ledger rolls over, the truck links
// Everything is integer and visits companies in index order, so the same inputs give the same city on every machine.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "sim_events.h"
#include "sim_goods_produce.h"
#include "sim_goods_trade.h"

namespace tbd {
namespace city {

struct TruckLink { int32_t fromNode = -1, toNode = -1, trucks = 0; }; // trucks a day between two road nodes

class GoodsWorld {
public:
    static constexpr int kSliceMinutes = 5, kSlices = 60 / kSliceMinutes;
    GoodsParams p;
    CompanyStore cos;
    std::vector<Shipment> ships;         // in flight, in departure order
    std::vector<OutsideLink> outsides;   // the host refreshes these
    int64_t residents = 0;               // the customers of the shops
    GoodsLedger today, last, total;      // total: every finished day
    std::vector<int32_t> balance;        // per finished day: export income - import cost (the last 60 days)
    std::vector<int16_t> cellAvail;      // per CitySim cell: shops supplied 0..255, -1 no shop
    std::vector<SimEvent> events;        // pushed since the host last drained them
    int32_t bankruptTotal = 0, shortages = 0, noCustomers = 0, noFreight = 0;
    int64_t tradeCap = -1;               // units a day the outside connections carry in and out (P5; -1 unlimited)
    int64_t TradeRoom() const {          // what today's imports and exports leave of tradeCap
        if (tradeCap < 0) return INT64_MAX;
        int64_t n = 0;
        for (int g = 0; g < kGoodCount; ++g) n += today.imported[(size_t)g] + today.exported[(size_t)g];
        return std::max<int64_t>(0, tradeCap - n);
    }

    void Clear() {
        const GoodsParams keep = p;
        *this = GoodsWorld();
        p = keep;
    }
    // Yesterday's trucks by node pair (node order), for the herds. The day end only swaps the counters; the list is built
    // here, on the first call after it (the host's frame), so the tick that ends the day stays cheap.
    const std::vector<TruckLink>& TrucksLast() {
        if (trucksPending_) {
            trucksLast_.clear();
            for (const auto& [k, n] : trucksDone_) trucksLast_.push_back({(int32_t)(k >> 32), (int32_t)(uint32_t)k, n});
            trucksDone_.clear(), trucksPending_ = false;
        }
        return trucksLast_;
    }
    GoodsLedger Running() const { GoodsLedger r = total; r.Add(today); return r; }
    // Units of g in stock and in flight.
    int64_t Mass(Good g) {
        int64_t m = 0;
        cos.ForEach([&](uint32_t, const CompanyRec& c) { m += c.stock[(size_t)g]; });
        for (const Shipment& s : ships) m += s.good == (int)g ? s.units : 0;
        return m;
    }

    void SetSites(const std::vector<GoodsSite>& sites, int64_t minute) {
        cos.Sync(sites, p, today.lost);
        indexDirty_ = true; // positions and roads may have changed too
        for (CompanyId id : cos.opened) Push(SimEventType::CompanyOpened, minute, (uint32_t)cos.recs[id.Index()].building, 0);
        cos.opened.clear(), cos.removed.clear();
    }

    void Tick(int64_t minute) {
        if (minute % 1440 == 1439) { DayEnd(minute); return; } // a tick of its own: the ledger rolls over
        if (minute % kSliceMinutes != 0) return;
        Arrivals(minute);
        const uint32_t slice = (uint32_t)((minute / kSliceMinutes) % kSlices);
        if (indexDirty_) sellers_.Build(cos), indexDirty_ = false;
        if (slice == 0) Demand(); // once an hour
        cos.ForEach([&](uint32_t i, CompanyRec& c) {
            if (i % kSlices != slice) return;
            const HourResult r = CompanyHour(c, p);
            if (r.made != Good::None) today.produced[(size_t)r.made] += r.madeUnits;
            for (int k = 0; k < kMaxInputs; ++k)
                if (r.used[k] != Good::None) today.used[(size_t)r.used[k]] += r.usedUnits; // processing moves mass between goods
            today.consumed[(size_t)kRetailGoods[0]] += r.sold[0], today.consumed[(size_t)kRetailGoods[1]] += r.sold[1];
            if (c.closedDays == 0) { // its hourly match: orders (local first, then imports), then exports
                Good in[kMaxInputs];
                const int n = CompanyInputs(c, in);
                for (int k = 0; k < n; ++k) Order(minute, i, c, k, in[k]);
                Export(minute, i, c);
            }
            Flag(minute, c);
            if (minute % 1440 >= 1380) CompanyEnd(minute, c); // each company closes its day in its last turn of the day
        });
        if (slice == kSlices - 1) Summarize();
    }

    void Push(SimEventType t, int64_t minute, uint32_t building, int64_t value) {
        SimEvent e;
        e.type = t, e.minute = minute, e.building = building, e.value = value;
        if (events.size() < 4096) events.push_back(e);
    }

private:
    SellerIndex sellers_;
    bool indexDirty_ = true; // a load or SetSites: rebuild before the next pick
    std::map<uint64_t, int32_t> trucksToday_, trucksDone_; // ordered: read in key order, no sort
    std::vector<TruckLink> trucksLast_;
    bool trucksPending_ = false;
    std::vector<int32_t> sum_, cnt_;

    void Arrivals(int64_t minute) {
        size_t w = 0;
        for (size_t i = 0; i < ships.size(); ++i) {
            const Shipment& s = ships[i];
            if (s.arrive > minute) { ships[w++] = s; continue; }
            const size_t g = (size_t)s.good;
            if (s.to == 0) { today.exported[g] += s.units; continue; }
            CompanyRec* c = cos.Get(CompanyId((uint32_t)s.to));
            if (!c) { today.lost[g] += s.units; continue; } // the buyer is gone
            c->stock[g] += s.units;
            c->incoming[g] = std::max(0, c->incoming[g] - s.units);
        }
        ships.resize(w);
    }
    void Ship(int64_t minute, Good g, int32_t units, uint32_t from, uint32_t to, int32_t fromNode, int32_t toNode, int64_t d2) {
        Shipment s;
        s.good = (int32_t)g, s.units = units, s.from = (int32_t)from, s.to = (int32_t)to, s.fromNode = fromNode, s.toNode = toNode;
        s.depart = (int32_t)minute;
        s.arrive = (int32_t)(minute + p.loadMinutes + ISqrt(d2) / std::max(1, p.truckMetresPerMinute));
        ships.push_back(s);
        ++today.trucks;
        if (fromNode >= 0 && toNode >= 0 && fromNode != toNode) ++trucksToday_[(uint64_t)(uint32_t)fromNode << 32 | (uint32_t)toNode];
    }

    static int64_t ShopWeight(const CompanyRec& c, const GoodsParams& p) {
        return (int64_t)CompanyRateMilli(c, p) * (100 + std::min(c.localRes, 400)) / 100; // staff, and the residents next door
    }
    // The residents' spending per hour over the open shops (by staff, and more for the residents next door).
    void Demand() {
        int64_t wsum = 0;
        cos.ForEach([&](uint32_t, CompanyRec& c) { if (c.kind == (int)CompanyKind::Shop && c.closedDays == 0) wsum += ShopWeight(c, p); });
        const int64_t spend = residents * p.residentMilliPerHour;
        cos.ForEach([&](uint32_t, CompanyRec& c) {
            if (c.kind != (int)CompanyKind::Shop) return;
            c.demandMilli = c.closedDays == 0 && wsum > 0 ? (int32_t)std::min<int64_t>(spend * ShopWeight(c, p) / wsum, 1 << 30) : 0;
        });
    }
    void Order(int64_t minute, uint32_t i, CompanyRec& c, int k, Good g) {
        const size_t gi = (size_t)g;
        const int32_t target = InputTarget(c, p, k);
        const int32_t minOrder = std::max(1, std::min(p.truckUnits, target) / 2);
        int32_t need = target - c.stock[gi] - c.incoming[gi];
        if (target <= 0 || need < minOrder || c.node < 0) return;
        const uint32_t self = cos.At(i).v;
        bool local = true;
        for (int t = 0; t < 3 && need >= minOrder; ++t) {
            int32_t units;
            const uint32_t s = local ? sellers_.Nearest(cos, g, c.x, c.z, p.matchRadiusM, i) : 0;
            local = s != 0; // no local seller now: the rest of this hour's order is imported without searching again
            if (s) { // local first: the nearest seller with stock
                CompanyRec& sc = cos.recs[s];
                units = std::min({need, p.truckUnits, sc.stock[gi]});
                const int32_t pay = units * GoodPrice(g);
                sc.stock[gi] -= units, sc.profitToday += pay, c.profitToday -= pay;
                Ship(minute, g, units, cos.At(s).v, self, sc.node, c.node, Dist2(sc.x, sc.z, c.x, c.z));
            } else { // then the outside world, at a quarter more
                const int o = NearestOutside(outsides, c.x, c.z);
                if (o < 0) return;
                units = (int32_t)std::min<int64_t>(std::min(need, p.truckUnits), TradeRoom());
                if (units <= 0) return; // the connections are full today
                const int32_t cost = units * GoodPrice(g) * 125 / 100;
                today.imported[gi] += units, today.importCost += cost, c.profitToday -= cost;
                const OutsideLink& ol = outsides[(size_t)o];
                Ship(minute, g, units, 0, self, ol.node, c.node, Dist2(ol.x, ol.z, c.x, c.z));
            }
            c.incoming[gi] += units, need -= units;
        }
    }
    // A surplus past exportAtPercent of the output cap is sold to the outside world (at 90 % of the price).
    void Export(int64_t minute, uint32_t i, CompanyRec& c) {
        const Good g = CompanyOutput(c);
        if (g == Good::None || c.closedDays > 0 || c.node < 0) return;
        const size_t gi = (size_t)g;
        const int32_t cap = OutputCap(c, p);
        if ((int64_t)c.stock[gi] * 100 < (int64_t)cap * p.exportAtPercent) return;
        const int o = NearestOutside(outsides, c.x, c.z);
        if (o < 0) return;
        const OutsideLink& ol = outsides[(size_t)o];
        int32_t surplus = c.stock[gi] - cap / 2;
        for (int t = 0; t < 3 && surplus > 0; ++t) {
            const int32_t units = (int32_t)std::min<int64_t>(std::min(surplus, p.truckUnits), TradeRoom()), income = units * GoodPrice(g) * 90 / 100;
            if (units <= 0) break; // the connections are full today
            c.stock[gi] -= units, surplus -= units, c.profitToday += income, today.exportIncome += income;
            Ship(minute, g, units, cos.At(i).v, 0, c.node, ol.node, Dist2(ol.x, ol.z, c.x, c.z));
        }
    }

    // A company's flags (its slice, after its match).
    void Flag(int64_t minute, CompanyRec& c) {
        const int32_t was = c.flags;
        int32_t f = 0, targetSum = 0, stockSum = 0;
        if (c.closedDays == 0) {
            Good in[kMaxInputs];
            const int n = CompanyInputs(c, in);
            for (int k = 0; k < n; ++k) {
                const int32_t t = InputTarget(c, p, k), s = c.stock[(size_t)in[k]];
                targetSum += t, stockSum += s;
                if (c.kind == (int)CompanyKind::Processor && t > 0 && (int64_t)s * 100 < (int64_t)t * p.noGoodsPercent) f |= kCoNoGoods;
            }
            if (c.kind == (int)CompanyKind::Shop) {
                if (targetSum > 0 && (int64_t)stockSum * 100 < (int64_t)targetSum * p.noGoodsPercent) f |= kCoNoGoods;
                const int32_t rate = CompanyRateMilli(c, p);
                if (rate > 0 && (int64_t)c.demandMilli * 100 < (int64_t)rate * p.noCustomersPercent) f |= kCoNoCustomers;
            }
            const Good o = CompanyOutput(c);
            if (o != Good::None && c.stock[(size_t)o] >= OutputCap(c, p) && (c.node < 0 || outsides.empty())) f |= kCoNoFreight;
        }
        c.flags = f;
        if ((f & kCoNoGoods) && !(was & kCoNoGoods)) Push(SimEventType::GoodsShortage, minute, (uint32_t)c.building, 0);
    }
    // Once an hour: the flag counts and the shops' supply per cell (0..255, -1 for a cell without shops).
    void Summarize() {
        shortages = noCustomers = noFreight = 0;
        sum_.assign(sum_.size(), 0), cnt_.assign(cnt_.size(), 0);
        cos.ForEach([&](uint32_t, CompanyRec& c) {
            const int32_t f = c.flags;
            shortages += (f & kCoNoGoods) != 0, noCustomers += (f & kCoNoCustomers) != 0, noFreight += (f & kCoNoFreight) != 0;
            if (c.kind != (int)CompanyKind::Shop || c.cell < 0 || c.cell >= (1 << 22)) return;
            if (sum_.size() <= (size_t)c.cell) sum_.resize((size_t)c.cell + 1, 0), cnt_.resize((size_t)c.cell + 1, 0);
            const int32_t t = InputTarget(c, p, 0) + InputTarget(c, p, 1);
            const int32_t s = c.stock[(size_t)kRetailGoods[0]] + c.stock[(size_t)kRetailGoods[1]];
            sum_[(size_t)c.cell] += c.closedDays > 0 ? 0 : t > 0 ? std::min(255, (int32_t)((int64_t)s * 255 / t)) : 255;
            ++cnt_[(size_t)c.cell];
        });
        cellAvail.assign(sum_.size(), -1);
        for (size_t i = 0; i < sum_.size(); ++i)
            if (cnt_[i]) cellAvail[i] = (int16_t)(sum_[i] / cnt_[i]);
    }

    void CompanyEnd(int64_t minute, CompanyRec& c) {
        const int r = CompanyDayEnd(c, p, today.lost);
        if (r == 1) Push(SimEventType::CompanyBankrupt, minute, (uint32_t)c.building, c.cash), ++bankruptTotal;
        if (r == 2) Push(SimEventType::CompanyOpened, minute, (uint32_t)c.building, 0);
    }
    void DayEnd(int64_t minute) {
        int64_t imp = 0, exp = 0;
        for (int g = 0; g < kGoodCount; ++g) imp += today.imported[(size_t)g], exp += today.exported[(size_t)g];
        if (imp) Push(SimEventType::Import, minute, 0, imp);
        if (exp) Push(SimEventType::Export, minute, 0, exp);
        balance.push_back((int32_t)std::clamp<int64_t>(today.exportIncome - today.importCost, -2000000000, 2000000000));
        if (balance.size() > 60) balance.erase(balance.begin());
        if (trucksPending_) TrucksLast(); // nobody asked yesterday: convert now (rare)
        std::swap(trucksToday_, trucksDone_), trucksPending_ = true; // trucksDone_ was emptied by the conversion
        total.Add(today), last = today, today.Clear();
    }
    friend struct GoodsSaveAccess;
};

} // namespace city
} // namespace tbd
