// Tracked people (Track S, phase S6): everyone is a counter (sim_cells.h) until the player clicks someone; then that one
// is materialized as a Person consistent with the counters, and stays named until untouched for a while or past the cap.
// Header only, headless (tests\sim_person_test.cpp).
//
//   Slots      a cell's residents are slots 0 .. population-1, in lot order (the cell's lots as they arrived), kids then
//              adults then seniors within a lot. Peek(cell, slot) is a pure function of the counters and a seed from
//              (cell key, slot, generatorVersion): the same slot gives the same person until something is stored.
//   Named      Materialize stores the person and marks one resident (and worker) of the cell as named: the totals do
//              not change, the anonymous matching just never touches them. Collapse gives them back to the counters.
//   Changes    LoseJob drops their link (unemployment rises); MoveHouse moves them and their lot records into a free
//              home elsewhere (both cells' counters change). Sync settles what the counters did to named people: their
//              building went (collapsed), their job went (unemployed), idle or over the cap (collapsed, oldest first).
//   Schedule   Location(person, minute) is a pure function of the person's seed, job and the time.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "sim.h"
#include "sim_cells.h"

namespace tbd {
namespace city {

struct Person {
    uint32_t id = 0;
    uint64_t seed = 0;     // cosmetic details (name, traits) come from this
    uint64_t homeLot = 0;
    int homeCell = -1;
    int jobCell = -1;      // -1: no job
    uint8_t jobKind = 0;   // JobKind
    uint8_t band = kAdults;
    uint8_t age = 30;      // years
    uint8_t edu = 0;       // 0 basic, 1 skilled, 2 higher
    uint8_t wealth = 1;    // 0..3
    uint8_t health = 200, happiness = 128;
    int64_t touched = 0;   // game minute of the last time the player (or the sim) did something with them
    bool Working() const { return jobCell >= 0; }
};

enum class Place : uint8_t { Home, Commuting, Work, Shopping };

class SimPeople {
public:
    uint32_t generatorVersion = 1;
    size_t cap = 2000;              // named people at most; past it the least recently touched collapse
    int64_t idleMinutes = 7 * 1440; // untouched this long: collapsed
    std::vector<Person> people;

    // The person in `slot` of `cell` as the counters say now; false if there is no such slot.
    bool Peek(const CitySim& s, int cell, uint32_t slot, Person& out) const {
        if (cell < 0 || cell >= (int)s.sim.size()) return false;
        const auto it = bySlot_.find(SlotKey(s, cell, slot));
        if (it != bySlot_.end()) {
            out = people[it->second];
            return true;
        }
        const CellSim& c = s.sim[(size_t)cell];
        if (slot >= c.Population()) return false;
        // which lot and band the slot falls in
        uint32_t at = 0;
        uint64_t lot = 0;
        int band = -1;
        for (uint64_t id : s.cells.cells[(size_t)cell].lots) {
            CitySim::LotView v;
            if (!s.FindLot(id, v) || !v.built) continue;
            for (int b = 0; b < kAgeBands && band < 0; ++b) {
                if (slot < at + v.res[b]) band = b, lot = id;
                at += v.res[b];
            }
            if (band >= 0) break;
        }
        if (band < 0) return false; // the counters and the lots disagree (a move in flight): no one here
        SplitMix64 r(SimSeed(PackKey(s.cells.cells[(size_t)cell].key), slot, generatorVersion));
        Person p;
        p.seed = r.Next();
        p.homeLot = lot;
        p.homeCell = cell;
        p.band = (uint8_t)band;
        p.age = (uint8_t)(band == kKids ? r.Below(18) : band == kAdults ? 18 + r.Below(47) : 65 + r.Below(26));
        p.edu = band == kKids ? 0 : (uint8_t)Pick(r, c.edu, 3);
        p.wealth = (uint8_t)Pick(r, c.wealth, 4);
        p.health = (uint8_t)(140 + r.Below(110));
        p.happiness = c.happiness;
        if (band == kAdults && c.AnonAdults() > 0 && r.Below(c.AnonAdults()) < c.AnonWorking()) {
            // a job: a supercell by the links, a cell there by its filled jobs, a kind by their mix
            const int sh = s.SuperOf(cell);
            uint64_t total = 0;
            s.ForEachJobOf(sh, [&](int, int32_t n) { total += (uint64_t)n; });
            if (total) {
                uint64_t pick = r.Next() % total;
                int sj = -1;
                s.ForEachJobOf(sh, [&](int j, int32_t n) {
                    if (sj < 0 && pick < (uint64_t)n) sj = j;
                    else if (sj < 0) pick -= (uint64_t)n;
                });
                uint64_t emp = 0;
                for (int jc : s.SuperCellList(sj)) emp += (uint64_t)s.sim[(size_t)jc].employed[0] + s.sim[(size_t)jc].employed[1] + s.sim[(size_t)jc].employed[2];
                if (emp) {
                    uint64_t e = r.Next() % emp;
                    for (int jc : s.SuperCellList(sj)) {
                        const CellSim& J = s.sim[(size_t)jc];
                        for (int k = 0; k < kJobKinds && p.jobCell < 0; ++k) {
                            if (e < J.employed[k]) p.jobCell = jc, p.jobKind = (uint8_t)k;
                            else e -= J.employed[k];
                        }
                        if (p.jobCell >= 0) break;
                    }
                }
            }
        }
        out = p;
        return true;
    }

    // Peek and store. Returns the person's index in `people`, -1 for no such slot.
    int Materialize(CitySim& s, int cell, uint32_t slot, int64_t minute) {
        const uint64_t key = SlotKey(s, cell, slot);
        if (const auto it = bySlot_.find(key); it != bySlot_.end()) {
            people[it->second].touched = minute;
            return (int)it->second;
        }
        Person p;
        if (!Peek(s, cell, slot, p)) return -1;
        p.id = nextId_++;
        p.touched = minute;
        s.Name(p.homeCell, p.band, p.Working(), +1);
        people.push_back(p);
        slotOf_.push_back(key);
        bySlot_[key] = people.size() - 1;
        EnforceCap(s);
        return Find(p.id);
    }

    // A saved person back on top of the counters (game load), without changing a total: they become one of their home
    // cell's residents (named), and keep their job only if an anonymous worker of the cell commutes the same way.
    bool Restore(CitySim& s, Person p) {
        CitySim::LotView v;
        if (!s.FindLot(p.homeLot, v) || !v.built || !v.res[p.band]) return false;
        p.homeCell = v.cell;
        CellSim& c = s.sim[(size_t)v.cell];
        if (c.named[p.band] >= c.residents[p.band]) return false;
        bool working = false;
        if (p.band == kAdults && p.jobCell >= 0 && p.jobCell < (int)s.sim.size() && c.AnonWorking() > 0 &&
            s.LinkCount(s.SuperOf(v.cell), s.SuperOf(p.jobCell)) > 0 && s.sim[(size_t)p.jobCell].employed[p.jobKind] > 0)
            working = true;
        if (!working) p.jobCell = -1;
        s.Name(v.cell, p.band, working, +1);
        nextId_ = std::max(nextId_, p.id + 1);
        people.push_back(p);
        slotOf_.push_back(MovedKey(p.id));
        bySlot_[slotOf_.back()] = people.size() - 1;
        return true;
    }

    void Collapse(CitySim& s, int i) {
        if (i < 0 || i >= (int)people.size()) return;
        const Person& p = people[(size_t)i];
        if (p.homeCell >= 0 && p.homeCell < (int)s.sim.size()) s.Name(p.homeCell, p.band, p.Working(), -1);
        Erase(i);
    }

    bool LoseJob(CitySim& s, int i, int64_t minute) {
        if (i < 0 || i >= (int)people.size()) return false;
        Person& p = people[(size_t)i];
        if (!p.Working() || !s.DropJob(p.homeCell, p.jobCell, p.jobKind, true)) return false;
        p.jobCell = -1;
        p.touched = minute;
        return true;
    }

    bool MoveHouse(CitySim& s, int i, uint64_t toLot, int64_t minute) {
        if (i < 0 || i >= (int)people.size()) return false;
        Person& p = people[(size_t)i];
        CitySim::LotView to;
        if (!s.FindLot(toLot, to) || !s.MoveResident(p.homeLot, toLot, p.band, p.Working(), p.jobCell)) return false;
        p.homeLot = toLot;
        p.homeCell = to.cell;
        p.touched = minute;
        // the old slot is anonymous again; the person is found by id from now on
        bySlot_.erase(slotOf_[(size_t)i]);
        slotOf_[(size_t)i] = MovedKey(p.id);
        bySlot_[slotOf_[(size_t)i]] = (size_t)i;
        return true;
    }

    // After the counters changed (CitySim::Sync): settle named people.
    void Sync(CitySim& s, int64_t minute) {
        for (int i = (int)people.size() - 1; i >= 0; --i) {
            Person& p = people[(size_t)i];
            CitySim::LotView v;
            if (!s.FindLot(p.homeLot, v) || !v.built) { // the building went (or is being rebuilt): they leave with it
                const int cell = p.homeCell, band = p.band;
                const bool working = p.Working();
                Erase(i); // p is another person from here on
                if (cell >= 0 && cell < (int)s.sim.size()) s.Name(cell, band, working, -1);
                continue;
            }
            CellSim& c = s.sim[(size_t)p.homeCell];
            if (p.Working() && c.namedLost) { // their job went with a workplace's capacity
                --c.namedLost;
                p.jobCell = -1;
            }
            if (minute - p.touched > idleMinutes) Collapse(s, i);
        }
        for (CellSim& c : s.sim) c.namedLost = 0;
        // named counts can only be as high as the residents they are part of
        for (CellSim& c : s.sim) {
            for (int b = 0; b < kAgeBands; ++b) c.named[b] = std::min(c.named[b], c.residents[b]);
            c.namedWorking = std::min({c.namedWorking, c.working, c.named[kAdults]});
        }
        // a building that shrank (a smaller model) may house fewer than its named people: the extra ones go back to
        // the counters (the clamp above already took them off)
        std::unordered_map<uint64_t, uint32_t> seen; // (cell, band) -> named people met so far
        for (int i = (int)people.size() - 1; i >= 0; --i) {
            const Person& p = people[(size_t)i];
            const uint64_t k = ((uint64_t)(uint32_t)p.homeCell << 2) | p.band;
            if (++seen[k] > s.sim[(size_t)p.homeCell].named[p.band]) Erase(i);
        }
        EnforceCap(s);
    }

    // The slots of a lot's residents in its cell: [first, first + count).
    static bool LotSlots(const CitySim& s, uint64_t lot, int& cell, uint32_t& first, uint32_t& count) {
        CitySim::LotView v;
        if (!s.FindLot(lot, v) || !v.built) return false;
        cell = v.cell;
        first = 0;
        for (uint64_t id : s.cells.cells[(size_t)cell].lots) {
            CitySim::LotView o;
            if (!s.FindLot(id, o) || !o.built) continue;
            const uint32_t n = (uint32_t)o.res[0] + o.res[1] + o.res[2];
            if (id == lot) {
                count = n;
                return true;
            }
            first += n;
        }
        return false;
    }
    // A name for the person, from the seed (cosmetic).
    static std::string Name(const Person& p) {
        static const char* const kFirst[] = {"Ada", "Ben", "Cleo", "Dev", "Elif", "Farid", "Greta", "Hugo", "Ines", "Jonas", "Kira", "Luca", "Maya", "Nils",
                                             "Omar", "Priya", "Quinn", "Rosa", "Sami", "Tess", "Umar", "Vera", "Wim", "Yara", "Zoe"};
        static const char* const kLast[] = {"Abbott", "Brandt", "Costa", "Dube", "Eklund", "Fischer", "Garcia", "Haddad", "Ito", "Jansen", "Kowal", "Lindqvist",
                                            "Moreau", "Novak", "Okafor", "Petrov", "Quist", "Rossi", "Silva", "Tanaka", "Varga", "Weber", "Yilmaz", "Zielinski"};
        SplitMix64 r(p.seed);
        return std::string(kFirst[r.Below(25)]) + " " + kLast[r.Below(24)];
    }

    int Find(uint32_t id) const {
        for (size_t i = 0; i < people.size(); ++i)
            if (people[i].id == id) return (int)i;
        return -1;
    }

    // Where they are at a game minute: workdays (5 of 7) 8:00-17:00 at work with half an hour each way on the road;
    // some evenings (from the seed) shopping 18:00-19:30; home otherwise. Pure.
    static Place Location(const Person& p, int64_t minute) {
        const int64_t day = minute / 1440, m = minute % 1440;
        const bool workday = (day + (int64_t)(p.seed % 7)) % 7 < 5;
        if (p.Working() && workday) {
            if (m >= 7 * 60 + 30 && m < 8 * 60) return Place::Commuting;
            if (m >= 8 * 60 && m < 17 * 60) return Place::Work;
            if (m >= 17 * 60 && m < 17 * 60 + 30) return Place::Commuting;
        }
        if (m >= 18 * 60 && m < 19 * 60 + 30 && SplitMix64(p.seed ^ (uint64_t)day).Below(4) == 0) return Place::Shopping;
        return Place::Home;
    }

private:
    std::unordered_map<uint64_t, size_t> bySlot_; // slot key -> index in people
    std::vector<uint64_t> slotOf_;                // per person: its key in bySlot_
    uint32_t nextId_ = 1;

    static uint64_t SlotKey(const CitySim& s, int cell, uint32_t slot) {
        return PackKey(s.cells.cells[(size_t)cell].key) * 0x9E3779B97F4A7C15ull ^ (uint64_t)slot;
    }
    static uint64_t MovedKey(uint32_t id) { return 0xFFFF000000000000ull | id; }

    static int Pick(SplitMix64& r, const uint16_t* counts, int n) {
        uint32_t total = 0;
        for (int i = 0; i < n; ++i) total += counts[i];
        if (!total) return n > 1 ? 1 : 0;
        uint32_t x = r.Below(total);
        for (int i = 0; i < n; ++i) {
            if (x < counts[i]) return i;
            x -= counts[i];
        }
        return n - 1;
    }

    void Erase(int i) {
        bySlot_.erase(slotOf_[(size_t)i]);
        const size_t last = people.size() - 1;
        if ((size_t)i != last) {
            people[(size_t)i] = people[last];
            slotOf_[(size_t)i] = slotOf_[last];
            bySlot_[slotOf_[(size_t)i]] = (size_t)i;
        }
        people.pop_back();
        slotOf_.pop_back();
    }

    void EnforceCap(CitySim& s) {
        while (people.size() > cap) {
            size_t oldest = 0;
            for (size_t i = 1; i < people.size(); ++i)
                if (people[i].touched < people[oldest].touched) oldest = i;
            Collapse(s, (int)oldest);
        }
    }
};

} // namespace city
} // namespace tbd
