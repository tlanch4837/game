// LotLevels' member functions (included by sim_land_lots.h; see there).
#pragma once

#include "sim_land_lots.h"

namespace tbd {
namespace city {

inline void LotLevels::Schedule(LotState& s, int64_t due) {
    if (s.due == due) return;
    if (s.due >= 0) queue.erase({s.due, s.id});
    s.due = due;
    if (due >= 0) queue.insert({due, s.id});
}

inline void LotLevels::Track(const Lot& l, int64_t minute, size_t li) {
    LotState s;
    s.id = l.id;
    const CellKey k = CellKeyAt(l.c.x, l.c.z);
    s.cellX = k.x, s.cellZ = k.z, s.level = (uint8_t)std::clamp(l.level, 1, 255), s.built = l.Built() ? 1 : 0;
    s.since = minute, s.seen = cycle, ++seenNow;
    s.cat = l.type >= 0 && l.type < (int)ZoneTypes().size() ? (uint8_t)ZoneTypes()[(size_t)l.type].cat : 0;
    st[l.id] = s;
    byCell_[PackKey(k)].push_back(l.id);
    if (li != ~(size_t)0) index_[l.id] = li;
    MarkDirty(l.id);
}

inline void LotLevels::Untrack(uint64_t id) {
    const auto it = st.find(id);
    if (it == st.end()) return;
    if (it->second.due >= 0) queue.erase({it->second.due, id});
    if (it->second.seen == cycle) --seenNow;
    auto c = byCell_.find(PackKey({it->second.cellX, it->second.cellZ}));
    if (c != byCell_.end()) {
        c->second.erase(std::remove(c->second.begin(), c->second.end(), id), c->second.end());
        if (c->second.empty()) byCell_.erase(c);
    }
    index_.erase(id);
    st.erase(it);
}

inline void LotLevels::Rebuild() {
    queue.clear(), byCell_.clear(), index_.clear(), dirty.clear();
    std::vector<uint64_t> ids;
    for (const auto& kv : st) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    for (uint64_t id : ids) {
        LotState& s = st[id];
        byCell_[PackKey({s.cellX, s.cellZ})].push_back(id);
        if (s.due >= 0) queue.insert({s.due, id});
        if (s.dirty) dirty.push_back(id);
    }
}

inline int LotLevels::LotIndex(const ZoneGrid& z, uint64_t id) {
    auto ok = [&](size_t li) { return li < z.lots.size() && z.lots[li].id == id && z.lots[li].alive; };
    auto it = index_.find(id);
    if (it != index_.end() && ok(it->second)) return (int)it->second;
    index_.clear(); // a compaction moved the lots (or the lot is gone): rebuilt once, O(lots)
    for (size_t i = 0; i < z.lots.size(); ++i) if (z.lots[i].alive) index_[z.lots[i].id] = i;
    it = index_.find(id);
    return it != index_.end() && ok(it->second) ? (int)it->second : -1;
}

inline bool LotLevels::HasModel(const ZoneGrid& z, int type, int w, int d, int level) {
    if (modelsFor_ != z.models.size()) {
        modelsFor_ = z.models.size(), models_.clear();
        for (const BuildingModel& m : z.models) models_.insert(((uint64_t)m.type << 24) | ((uint64_t)m.w << 16) | ((uint64_t)m.d << 8) | (uint64_t)m.level);
    }
    return models_.count(((uint64_t)type << 24) | ((uint64_t)w << 16) | ((uint64_t)d << 8) | (uint64_t)level) != 0;
}

// One group of lots: new lots tracked, a finished or rebuilt lot dirty; at the end of a cycle lots that were not seen
// (gone) are dropped.
inline void LotLevels::Sweep(ZoneGrid& z, int64_t minute) {
    const auto& types = ZoneTypes();
    const size_t groups = (size_t)std::max(1, p.sweepGroups);
    for (size_t li = (size_t)sweepGroup; li < z.lots.size(); li += groups) {
        const Lot& l = z.lots[li];
        if (!l.alive || l.type < 0 || l.type >= (int)types.size()) continue;
        const auto it = st.find(l.id);
        if (it == st.end()) { Track(l, minute, li); continue; }
        LotState& s = it->second;
        if (s.seen != cycle) s.seen = cycle, ++seenNow;
        index_[l.id] = li;
        if (s.built != (l.Built() ? 1 : 0) || s.level != l.level) s.built = l.Built() ? 1 : 0, s.level = (uint8_t)std::clamp(l.level, 1, 255), MarkDirty(l.id);
    }
    if (++sweepGroup < (int)groups) return;
    sweepGroup = 0;
    if (seenNow != (int)st.size()) { // some lot was not met this cycle: it is gone
        std::vector<uint64_t> gone;
        for (const auto& kv : st) if (kv.second.seen != cycle) gone.push_back(kv.first);
        std::sort(gone.begin(), gone.end());
        for (uint64_t id : gone) Untrack(id);
    }
    seenNow = 0;
    ++cycle; // every state is now "not seen in this cycle" until the sweep meets its lot again
}

// The lot's condition now and the action time that follows from it (the queue entry is replaced).
template <class Inputs>
void LotLevels::Evaluate(ZoneGrid& z, LotState& s, size_t li, int64_t minute, const double demand[4], Inputs& inputs) {
    const Lot& l = z.lots[li];
    s.level = (uint8_t)std::clamp(l.level, 1, 255), s.built = l.Built() ? 1 : 0;
    if (!s.built) { // under construction: nothing to judge
        if (s.cond != kLotNeutral) s.cond = kLotNeutral, s.since = minute;
        Schedule(s, -1);
        return;
    }
    const int cat = (int)ZoneTypes()[(size_t)l.type].cat;
    uint8_t c = LotCondition(p, l.level, inputs(l, li), demand[std::clamp(cat, 0, 3)]);
    if (s.abandoned && c == kLotGood) c = kLotNeutral; // abandoned: only bad or not bad
    if (c != s.cond) s.cond = c, s.since = minute;
    s.maxed = HasModel(z, l.type, l.w, l.d, l.level + 1) ? 0 : 1;
    const double j = Jitter(s.id);
    int64_t due = -1;
    if (s.abandoned) due = c == kLotBad ? s.abandonedAt + p.abandonMinutes : s.since + p.recoverMinutes;
    else if (c == kLotGood && !s.maxed) due = s.since + (int64_t)((double)p.upMinutes * (1.0 + 0.5 * j));
    else if (c == kLotBad) due = s.since + (int64_t)((double)p.downMinutes * (1.0 + j));
    Schedule(s, due);
}

// A due action (the condition was just re-evaluated and still calls for it).
inline void LotLevels::Act(ZoneGrid& z, LotState& s, size_t li, int64_t minute) {
    Lot& l = z.lots[li];
    const uint64_t id = s.id;
    auto settle = [&] { s.cond = kLotNeutral, s.since = minute; Schedule(s, -1); MarkDirty(id); };
    if (s.abandoned) {
        if (s.cond == kLotBad) {
            z.Demolish((int)li);
            ++demolished, events.push_back({LotEventType::Demolished, id, l.level});
            Untrack(id);
            return;
        }
        s.abandoned = 0, s.abandonedAt = -1, ++recovered;
        events.push_back({LotEventType::Recovered, id, l.level});
        settle();
        return;
    }
    if (s.cond == kLotGood) {
        if (z.Relevel((int)li, l.level + 1)) ++levelUps, events.push_back({LotEventType::LevelUp, id, l.level});
        else s.maxed = 1;
        settle();
        return;
    }
    if (s.cond != kLotBad) return;
    if (l.level > 1 && HasModel(z, l.type, l.w, l.d, l.level - 1) && z.Relevel((int)li, l.level - 1)) {
        ++levelDowns, events.push_back({LotEventType::LevelDown, id, l.level});
        settle();
        return;
    }
    s.abandoned = 1, s.abandonedAt = minute, ++abandonedTotal; // empty and flagged; demolished later unless it recovers
    events.push_back({LotEventType::Abandoned, id, l.level});
    s.since = minute;
    Schedule(s, minute + p.abandonMinutes);
}

template <class Inputs>
void LotLevels::Tick(ZoneGrid& z, int64_t minute, const double demand[4], Inputs&& inputs) {
    if (minute / 60 != hour) hour = minute / 60, actionsThisHour = 0;
    Sweep(z, minute);
    evaluated = 0;
    size_t head = 0;
    for (; head < dirty.size() && evaluated < p.evalPerTick; ++head) {
        const auto it = st.find(dirty[head]);
        if (it == st.end() || !it->second.dirty) continue;
        it->second.dirty = 0;
        const int li = LotIndex(z, it->first);
        if (li < 0) { Untrack(it->first); continue; }
        Evaluate(z, it->second, (size_t)li, minute, demand, inputs), ++evaluated;
    }
    dirty.erase(dirty.begin(), dirty.begin() + (std::ptrdiff_t)head);
    const int cap = std::max(p.maxActionsPerHour, Tracked() / std::max(1, p.actionsPerLots));
    int actedNow = 0; // at most actionsPerTick a tick: the hour's actions spread instead of landing at its first minute
    for (int pops = 0; pops < p.popsPerTick && !queue.empty() && queue.begin()->first <= minute; ++pops) {
        const uint64_t id = queue.begin()->second;
        const auto it = st.find(id);
        if (it == st.end()) { queue.erase(queue.begin()); continue; }
        LotState& s = it->second;
        const int li = LotIndex(z, id);
        if (li < 0) { Untrack(id); continue; }
        Evaluate(z, s, (size_t)li, minute, demand, inputs), ++evaluated; // fresh inputs: never act on stale ones
        if (s.due < 0 || s.due > minute) continue;
        if (actionsThisHour >= cap) { Schedule(s, (hour + 1) * 60 + (int64_t)(id % 60)); continue; } // no waves
        if (actedNow >= p.actionsPerTick) { Schedule(s, minute + 1); continue; }
        ++actionsThisHour, ++actedNow;
        Act(z, s, (size_t)li, minute);
    }
}

// Every tracked lot re-judged from scratch against what it stores (condition and due time). 0: nothing is stale.
template <class Inputs>
int LotLevels::FullCheck(const ZoneGrid& z, const double demand[4], Inputs&& inputs, size_t group, size_t groups) const {
    int bad = 0;
    for (size_t li = group; li < z.lots.size(); li += std::max<size_t>(1, groups)) {
        const Lot& l = z.lots[li];
        const auto it = st.find(l.id);
        if (!l.alive || it == st.end()) continue;
        const LotState& s = it->second;
        if (s.dirty || s.built != (l.Built() ? 1 : 0) || s.level != l.level) continue; // not judged yet / the sweep has not seen the change
        if (!l.Built()) { bad += s.cond != kLotNeutral; continue; }
        uint8_t c = LotCondition(p, l.level, inputs(l, li), demand[std::clamp((int)ZoneTypes()[(size_t)l.type].cat, 0, 3)]);
        if (s.abandoned && c == kLotGood) c = kLotNeutral;
        bad += c != s.cond;
    }
    return bad;
}

} // namespace city
} // namespace tbd
