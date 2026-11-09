// Districts (CS2 sim roadmap Phase 7): player-painted groups of cells over the neighbourhoods. Header only, headless
// (tests\sim_gov_test).
//
//   Cells    a CityCell (by its packed key, so a district survives hood rebuilds, lots compaction and loads) belongs to
//            at most one district. Painting takes whole hoods (the hood under the finger / the camera): all its cells.
//   Hoods    PolicyMods are per hood (sim_factors.h): a hood follows the district most of its cells belong to, if it has
//            more of them than cells in no district (ties: the lower id, and "none" wins a tie). Recomputed only when the districts or the hoods changed (version).
//   Ids      1..count; the panel's count slider adds a district at the end or removes the last one (its cells go free).
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tbd {
namespace city {

struct GovDistrict {
    std::string name;
    uint32_t policies = 0; // sim_policy.h bits (district-scoped ones)
};

class GovDistricts {
public:
    static constexpr int kMax = 16;
    std::vector<GovDistrict> list;                // id = index + 1
    std::unordered_map<uint64_t, uint8_t> cells;  // packed CellKey -> district id
    uint64_t version = 1;                         // bumped on any change (the mods and the overlay recompose)

    int Count() const { return (int)list.size(); }
    void Clear() { list.clear(), cells.clear(), ++version; }
    GovDistrict* Get(int id) { return id >= 1 && id <= Count() ? &list[(size_t)id - 1] : nullptr; }
    const GovDistrict* Get(int id) const { return id >= 1 && id <= Count() ? &list[(size_t)id - 1] : nullptr; }

    int Add(const std::string& name) {
        if (Count() >= kMax) return 0;
        list.push_back({name, 0});
        ++version;
        return Count();
    }
    void RemoveLast() {
        if (list.empty()) return;
        const uint8_t id = (uint8_t)Count();
        list.pop_back();
        for (auto it = cells.begin(); it != cells.end();) it = it->second == id ? cells.erase(it) : std::next(it);
        ++version;
    }
    int Of(uint64_t key) const {
        const auto it = cells.find(key);
        return it == cells.end() ? 0 : it->second;
    }
    // Puts the cells into district id (0: out of any district). Returns how many changed.
    int Paint(const std::vector<uint64_t>& keys, int id) {
        if (id < 0 || id > Count()) return 0;
        int changed = 0;
        for (uint64_t k : keys) {
            const int was = Of(k);
            if (was == id) continue;
            if (id == 0) cells.erase(k);
            else cells[k] = (uint8_t)id;
            ++changed;
        }
        if (changed) ++version;
        return changed;
    }
    bool SetPolicy(int id, int policy, bool on) {
        GovDistrict* d = Get(id);
        if (!d || policy < 0 || policy >= 32) return false;
        const uint32_t was = d->policies;
        d->policies = on ? was | (1u << policy) : was & ~(1u << policy);
        if (d->policies != was) ++version;
        return d->policies != was;
    }

    // The district of each hood: cellHood[c] = hood of CitySim cell c (-1 none), keyOf(c) its packed key.
    template <class KeyOf>
    void HoodDistricts(const std::vector<int>& cellHood, int hoodCount, KeyOf&& keyOf, std::vector<uint8_t>& out) const {
        out.assign((size_t)std::max(0, hoodCount), 0);
        if (cells.empty() || hoodCount <= 0) return;
        std::vector<uint16_t> votes((size_t)hoodCount * (kMax + 1), 0); // slot 0: the cells in no district
        for (size_t c = 0; c < cellHood.size(); ++c) {
            const int h = cellHood[c];
            if (h < 0 || h >= hoodCount) continue;
            ++votes[(size_t)h * (kMax + 1) + (size_t)Of(keyOf((int)c))];
        }
        for (int h = 0; h < hoodCount; ++h) {
            int best = 0;
            uint16_t bn = votes[(size_t)h * (kMax + 1)]; // a district needs more cells of the hood than "none" has
            for (int d = 1; d <= kMax; ++d)
                if (votes[(size_t)h * (kMax + 1) + (size_t)d] > bn) bn = votes[(size_t)h * (kMax + 1) + (size_t)d], best = d;
            out[(size_t)h] = (uint8_t)best;
        }
    }
    // The cells in key order (the save writes these: the same districts always give the same bytes).
    std::vector<std::pair<uint64_t, uint8_t>> Sorted() const {
        std::vector<std::pair<uint64_t, uint8_t>> v(cells.begin(), cells.end());
        std::sort(v.begin(), v.end());
        return v;
    }
};

} // namespace city
} // namespace tbd
