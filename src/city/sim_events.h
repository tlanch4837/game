// SimEvent ring (CS2 sim roadmap 3.2, Phase 0; frozen interface). Header only, headless.
//
// Producers Push; consumers (news, radio, chronicle, deathcare, dispatch) each keep a cursor and Read what came after it.
// A fixed ring of 4096 events: it never allocates. A consumer that falls more than 4096 events behind loses the oldest
// (Read reports how many were missed). Events are not saved: they describe the moment.
#pragma once

#include <array>
#include <cstdint>

namespace tbd {
namespace city {

enum class SimEventType : uint16_t {
    None = 0,
    // P1 people
    Birth, Death, MoveIn, MoveOut, Immigrated, Emigrated, Graduated, Hired, Fired, Retired, Partnered,
    // P2 land
    LevelUp, LevelDown, Abandoned, Recovered, HighRent,
    // P3 environment
    Flood, Storm, SeasonChanged, PollutionAlert,
    // P4 services and incidents
    Crime, Arrest, Fire, FireOut, Sick, Healed, BodyCollected, Disaster, ServiceFull,
    // P5 transport
    LineOpened, LineClosed, LineCrowded,
    // P6 goods
    CompanyBankrupt, CompanyOpened, GoodsShortage, Export, Import,
    // P7 budget and progress
    Milestone, PolicyChanged, DistrictMade, Bankrupt, Autosaved,
    Count
};

struct SimEvent {
    SimEventType type = SimEventType::None;
    uint16_t flags = 0;
    int64_t minute = 0;   // sim minute it happened
    uint32_t building = 0;// BuildingId (sim_ids.h), kNone if none
    uint32_t citizen = 0; // CitizenId, kNone if none
    int64_t value = 0;    // type-specific (cents, count, level...)
};

class SimEventRing {
public:
    static constexpr uint32_t kSize = 4096;
    using Cursor = uint64_t; // the sequence number of the next event to read

    void Push(const SimEvent& e) { ring_[(size_t)(next_ % kSize)] = e, ++next_; }
    void Push(SimEventType t, int64_t minute, uint32_t building = 0, uint32_t citizen = 0, int64_t value = 0) {
        SimEvent e;
        e.type = t, e.minute = minute, e.building = building, e.citizen = citizen, e.value = value;
        Push(e);
    }
    uint64_t Pushed() const { return next_; }   // events ever pushed
    Cursor Now() const { return next_; }        // a cursor that skips everything so far (a new consumer)
    // The next event after `c` (and advances c), false when caught up. `missed` adds events lost to the ring wrapping.
    bool Read(Cursor& c, SimEvent& out, uint64_t* missed = nullptr) {
        if (c + kSize < next_) {
            if (missed) *missed += next_ - kSize - c;
            c = next_ - kSize;
        }
        if (c >= next_) return false;
        out = ring_[(size_t)(c % kSize)];
        ++c;
        return true;
    }
    // No Clear: the sequence only grows, so outstanding cursors stay valid across a city reset (consumers take Now()).

private:
    std::array<SimEvent, kSize> ring_{};
    uint64_t next_ = 0;
};

} // namespace city
} // namespace tbd
