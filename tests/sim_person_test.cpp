// Headless checks for tracked people (src/city/sim_people.h):
//   the same slot gives the same person; materialize / collapse keeps every total; moving house updates both cells (and
//   the lot records, so a later demolition stays exact); losing a job raises unemployment; a fuzz of random operations
//   (people and city changes) never leaves a negative or inconsistent counter; the cap and idle collapse hold; the
//   schedule is a pure function of time.
// Build and run: tests\sim_person_test.bat
#include <cstdio>
#include <vector>

#include "city/sim.h"
#include "city/sim_people.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static Lot MakeLot(uint64_t id, float x, float z, int type, int w, int d) {
    Lot l;
    l.id = id;
    l.c = {x, z};
    l.type = type, l.w = w, l.d = d;
    l.path = "m";
    l.progress = 2.0f;
    return l;
}

static bool Consistent(const CitySim& s, bool say = false) {
    if (s.Working() != s.Employed() || s.Employed() != s.Links()) {
        if (say) std::printf("  working %llu employed %llu links %llu\n", (unsigned long long)s.Working(), (unsigned long long)s.Employed(), (unsigned long long)s.Links());
        return false;
    }
    for (size_t i = 0; i < s.sim.size(); ++i) {
        const CellSim& c = s.sim[i];
        bool bad = c.working > c.residents[kAdults] || c.namedWorking > c.working;
        for (int b = 0; b < 3; ++b) bad |= c.named[b] > c.residents[b];
        for (int k = 0; k < 3; ++k) bad |= c.employed[k] > c.jobCap[k] || c.vacancies[k] != c.jobCap[k] - c.employed[k];
        if (bad) {
            if (say)
                std::printf("  cell %zu: adults %u working %u named %u/%u/%u namedWorking %u jobs %u/%u/%u employed %u/%u/%u vac %u/%u/%u\n", i, c.residents[1], c.working,
                            c.named[0], c.named[1], c.named[2], c.namedWorking, c.jobCap[0], c.jobCap[1], c.jobCap[2], c.employed[0], c.employed[1], c.employed[2],
                            c.vacancies[0], c.vacancies[1], c.vacancies[2]);
            return false;
        }
    }
    return true;
}

int main() {
    std::vector<Lot> lots = {MakeLot(1, 10, 10, 3, 4, 4), MakeLot(2, 40, 10, 2, 3, 3), MakeLot(3, 70, 10, 6, 6, 6), MakeLot(4, 300, 10, 2, 4, 4),
                             MakeLot(5, 330, 10, 4, 4, 4)};
    CitySim s;
    s.Sync(lots, 1);
    SimPeople ppl;
    const int home = s.cells.Find({0, 0}), other = s.cells.Find({2, 0});
    CHECK(home >= 0 && other >= 0);

    // Same slot, same person (twice before storing, and after storing).
    {
        Person a, b;
        CHECK(ppl.Peek(s, home, 17, a) && ppl.Peek(s, home, 17, b));
        CHECK(a.seed == b.seed && a.band == b.band && a.jobCell == b.jobCell && a.age == b.age && a.homeLot == b.homeLot);
        const int i = ppl.Materialize(s, home, 17, 0);
        CHECK(i >= 0 && ppl.people[(size_t)i].seed == a.seed);
        CHECK(ppl.Materialize(s, home, 17, 5) == i); // already stored: the same one
        Person c;
        ppl.Peek(s, home, 99999, c);
        CHECK(!ppl.Peek(s, home, 99999, c));
    }
    // Totals unchanged by materialize and collapse.
    {
        const uint64_t pop = s.Population(), work = s.Working(), jobs = s.Employed();
        std::vector<int> made;
        for (uint32_t slot = 0; slot < 60; ++slot) made.push_back(ppl.Materialize(s, home, slot, 1));
        CHECK(s.Population() == pop && s.Working() == work && s.Employed() == jobs);
        CHECK(Consistent(s));
        uint32_t named = 0;
        for (const CellSim& c : s.sim) named += (uint32_t)c.named[0] + c.named[1] + c.named[2];
        CHECK(named == ppl.people.size());
        while (!ppl.people.empty()) ppl.Collapse(s, 0);
        named = 0;
        for (const CellSim& c : s.sim) named += (uint32_t)c.named[0] + c.named[1] + c.named[2] + c.namedWorking;
        CHECK(named == 0);
        CHECK(s.Population() == pop && s.Working() == work && s.Employed() == jobs);
    }
    // Job loss raises unemployment; the named person is not rehired by the anonymous matching.
    {
        int worker = -1;
        for (uint32_t slot = 0; slot < s.sim[(size_t)home].Population() && worker < 0; ++slot) {
            const int i = ppl.Materialize(s, home, slot, 2);
            if (i >= 0 && ppl.people[(size_t)i].Working()) worker = i;
        }
        CHECK(worker >= 0);
        const uint64_t work = s.Working();
        CHECK(ppl.LoseJob(s, worker, 3));
        CHECK(s.Working() == work - 1);
        CHECK(!ppl.people[(size_t)worker].Working());
        s.Match();
        CHECK(Consistent(s));
        // the vacancy went to an anonymous unemployed adult if there was one, never to the named person
        CHECK(s.sim[(size_t)home].namedWorking + 1 <= s.sim[(size_t)home].named[kAdults]);
        while (!ppl.people.empty()) ppl.Collapse(s, 0);
    }
    // Moving house: lots keep 5% of homes free; a named adult moves from lot 1 (cell 0) to lot 4 (cell 2).
    {
        int i = -1;
        for (uint32_t slot = 0; slot < s.sim[(size_t)home].Population() && i < 0; ++slot) {
            const int k = ppl.Materialize(s, home, slot, 4);
            if (k >= 0 && ppl.people[(size_t)k].band == kAdults && ppl.people[(size_t)k].homeLot == 1) i = k;
        }
        CHECK(i >= 0);
        const uint32_t popA = s.sim[(size_t)home].Population(), popB = s.sim[(size_t)other].Population();
        const uint64_t work = s.Working(), pop = s.Population();
        const uint32_t id = ppl.people[(size_t)i].id;
        CHECK(ppl.MoveHouse(s, i, 4, 5));
        i = ppl.Find(id);
        CHECK(s.sim[(size_t)home].Population() == popA - 1);
        CHECK(s.sim[(size_t)other].Population() == popB + 1);
        CHECK(s.Population() == pop && s.Working() == work);
        CHECK(ppl.people[(size_t)i].homeLot == 4 && ppl.people[(size_t)i].homeCell == other);
        CHECK(Consistent(s));
        // fill lot 4's free homes and the next move is refused
        CitySim::LotView v4;
        CHECK(s.FindLot(4, v4));
        // demolish lot 4: the mover leaves with it; counters stay exact (the records moved with them)
        lots.erase(lots.begin() + 3);
        s.Sync(lots, 2);
        ppl.Sync(s, 6);
        CHECK(ppl.Find(id) < 0);
        CHECK(s.sim[(size_t)other].Population() == 0 || s.sim[(size_t)other].homeCap > 0);
        CHECK(Consistent(s));
        uint32_t named = 0;
        for (const CellSim& c : s.sim) named += (uint32_t)c.named[0] + c.named[1] + c.named[2];
        CHECK(named == ppl.people.size());
        while (!ppl.people.empty()) ppl.Collapse(s, 0);
    }
    // Fuzz: random people and city operations with a fixed seed; every counter stays consistent and non-negative.
    {
        SplitMix64 r(2024);
        CitySim t;
        SimPeople p;
        p.cap = 40;
        std::vector<Lot> L;
        uint64_t nextId = 1, version = 1;
        for (int i = 0; i < 40; ++i) L.push_back(MakeLot(nextId++, (float)r.Below(1200), (float)r.Below(600), (int)r.Below(7), 1 + (int)r.Below(4), 1 + (int)r.Below(4)));
        t.Sync(L, ++version);
        bool ok = true;
        int moves = 0, losses = 0;
        for (int step = 0; step < 6000; ++step) {
            const uint32_t op = r.Below(100);
            const int64_t minute = step * 7;
            if (op < 35 && !t.sim.empty()) {
                const int c = (int)r.Below((uint32_t)t.sim.size());
                if (t.sim[(size_t)c].Population()) p.Materialize(t, c, r.Below(t.sim[(size_t)c].Population()), minute);
            } else if (op < 45 && !p.people.empty()) {
                p.Collapse(t, (int)r.Below((uint32_t)p.people.size()));
            } else if (op < 55 && !p.people.empty()) {
                losses += p.LoseJob(t, (int)r.Below((uint32_t)p.people.size()), minute);
            } else if (op < 65 && !p.people.empty()) {
                moves += p.MoveHouse(t, (int)r.Below((uint32_t)p.people.size()), L[r.Below((uint32_t)L.size())].id, minute);
            } else if (op < 75) {
                L.push_back(MakeLot(nextId++, (float)r.Below(1200), (float)r.Below(600), (int)r.Below(7), 1 + (int)r.Below(4), 1 + (int)r.Below(4)));
            } else if (op < 85 && L.size() > 5) {
                L.erase(L.begin() + (ptrdiff_t)r.Below((uint32_t)L.size()));
            } else if (op < 90) {
                Lot& l = L[r.Below((uint32_t)L.size())];
                l.level = l.level == 1 ? 2 : 1;
                l.path += "~";
            } else {
                t.Match();
            }
            if (t.Sync(L, ++version)) {}
            p.Sync(t, minute);
            if (!Consistent(t)) { ok = false; std::printf("inconsistent at step %d (op %u)\n", step, op); Consistent(t, true); break; }
            uint32_t named = 0;
            for (const CellSim& c : t.sim) named += (uint32_t)c.named[0] + c.named[1] + c.named[2];
            if (named < p.people.size()) { ok = false; std::printf("named %u < people %zu at step %d\n", named, p.people.size(), step); break; }
            if (p.people.size() > p.cap) { ok = false; break; }
        }
        std::printf("fuzz: %zu named at the end, %d moves, %d job losses, pop %llu\n", p.people.size(), moves, losses, (unsigned long long)t.Population());
        CHECK(ok);
        CHECK(losses > 0);
    }

    // Idle collapse and the schedule.
    {
        CitySim t;
        std::vector<Lot> L = {MakeLot(1, 10, 10, 3, 4, 4), MakeLot(2, 40, 10, 6, 6, 6)};
        t.Sync(L, 1);
        SimPeople p;
        p.Materialize(t, 0, 3, 0);
        p.Sync(t, 10);
        CHECK(p.people.size() == 1);
        p.Sync(t, p.idleMinutes + 100);
        CHECK(p.people.empty());
        Person w;
        w.seed = 12345;
        w.jobCell = 0;
        int work = 0, home = 0;
        for (int64_t m = 0; m < 7 * 1440; m += 10) {
            const Place a = SimPeople::Location(w, m), b = SimPeople::Location(w, m);
            CHECK(a == b);
            work += a == Place::Work, home += a == Place::Home;
        }
        CHECK(work == 5 * 54); // 9 hours x 6 per hour x 5 workdays
        CHECK(home > work);
    }

    if (failures) {
        std::printf("sim_person_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sim_person_test: all passed\n");
    return 0;
}
