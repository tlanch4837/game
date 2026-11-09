// Handles for the city sim's records (CS2 sim roadmap, Phase 0; frozen interface). Header only, headless.
//
// A Handle is a 32-bit value: the low 24 bits are a slot index, the high 8 bits the slot's generation. kNone (0) is never
// a live handle: slot 0 is reserved, so a zero-initialised field means "nobody". A slot is reused only after its
// generation has been bumped, so a stale handle to a freed slot never matches the new occupant.
#pragma once

#include <cstdint>
#include <vector>

namespace tbd {
namespace city {

struct Handle {
    uint32_t v = 0;
    static constexpr uint32_t kIndexBits = 24, kIndexMask = (1u << kIndexBits) - 1u, kMaxIndex = kIndexMask;
    constexpr Handle() = default;
    constexpr explicit Handle(uint32_t raw) : v(raw) {}
    static constexpr Handle Make(uint32_t index, uint8_t gen) { return Handle((uint32_t)gen << kIndexBits | (index & kIndexMask)); }
    constexpr uint32_t Index() const { return v & kIndexMask; }
    constexpr uint8_t Gen() const { return (uint8_t)(v >> kIndexBits); }
    constexpr bool Valid() const { return v != 0; }
    constexpr bool operator==(Handle o) const { return v == o.v; }
    constexpr bool operator!=(Handle o) const { return v != o.v; }
};
constexpr uint32_t kNone = 0;

// Typed aliases: distinct types so a CitizenId is never passed where a BuildingId is wanted.
template <int Tag>
struct TypedId : Handle {
    constexpr TypedId() = default;
    constexpr explicit TypedId(uint32_t raw) : Handle(raw) {}
    constexpr TypedId(Handle h) : Handle(h.v) {}
};
using CitizenId = TypedId<1>;
using HouseholdId = TypedId<2>;
using BuildingId = TypedId<3>;
using CompanyId = TypedId<4>;
using IncidentId = TypedId<5>;

// Slot allocator: generations per slot plus a free list (LIFO). Slot 0 is reserved. A store keeps its own SoA columns
// sized to Capacity() and indexes them by Handle::Index().
class HandlePool {
public:
    HandlePool() { gen_.push_back(0), alive_.push_back(0); }

    Handle Alloc() {
        uint32_t i;
        if (!free_.empty()) {
            i = free_.back();
            free_.pop_back();
        } else {
            if (gen_.size() > Handle::kMaxIndex) return Handle();
            i = (uint32_t)gen_.size();
            gen_.push_back(1), alive_.push_back(0);
        }
        alive_[i] = 1;
        ++live_;
        return Handle::Make(i, gen_[i]);
    }
    // Frees h (no-op for a stale or empty handle). The slot's generation is bumped now, so h never matches again.
    bool Free(Handle h) {
        if (!Alive(h)) return false;
        const uint32_t i = h.Index();
        alive_[i] = 0;
        gen_[i] = (uint8_t)(gen_[i] + 1 == 0 ? 1 : gen_[i] + 1); // generation 0 is skipped: index 0 gen 0 would be kNone
        free_.push_back(i);
        --live_;
        return true;
    }
    bool Alive(Handle h) const { const uint32_t i = h.Index(); return h.Valid() && i < gen_.size() && alive_[i] && gen_[i] == h.Gen(); }
    // The live handle of slot i, or an empty one.
    Handle At(uint32_t i) const { return i < gen_.size() && alive_[i] ? Handle::Make(i, gen_[i]) : Handle(); }
    uint32_t Capacity() const { return (uint32_t)gen_.size(); } // slot count including the reserved slot 0
    uint32_t Live() const { return live_; }
    void Clear() { *this = HandlePool(); }

    // Load: a handle exactly as saved (slot and generation); the slots skipped on the way become free. Only before any
    // Alloc of the new session, in increasing index order. False if the slot is taken.
    bool Restore(Handle h) {
        const uint32_t i = h.Index();
        if (!h.Valid() || i == 0 || i > Handle::kMaxIndex) return false;
        while (gen_.size() <= i) { free_.push_back((uint32_t)gen_.size()); gen_.push_back(1), alive_.push_back(0); }
        if (alive_[i]) return false;
        for (size_t k = 0; k < free_.size(); ++k)
            if (free_[k] == i) { free_.erase(free_.begin() + (long)k); break; }
        gen_[i] = h.Gen(), alive_[i] = 1, ++live_;
        return true;
    }
    // Raw state for the save (gens, alive bits, free list order).
    const std::vector<uint8_t>& Gens() const { return gen_; }
    const std::vector<uint8_t>& AliveBits() const { return alive_; }
    const std::vector<uint32_t>& FreeList() const { return free_; }
    bool SetRaw(std::vector<uint8_t> gens, std::vector<uint8_t> alive, std::vector<uint32_t> freeList) {
        if (gens.empty() || gens.size() != alive.size()) return false;
        for (uint32_t f : freeList) if (f == 0 || f >= gens.size() || alive[f]) return false;
        gen_ = std::move(gens), alive_ = std::move(alive), free_ = std::move(freeList);
        alive_[0] = 0, live_ = 0;
        for (uint8_t a : alive_) live_ += a ? 1u : 0u;
        return true;
    }

private:
    std::vector<uint8_t> gen_, alive_;
    std::vector<uint32_t> free_;
    uint32_t live_ = 0;
};

} // namespace city
} // namespace tbd
