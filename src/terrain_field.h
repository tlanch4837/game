// Player edits to the terrain: a sparse set of height-delta tiles laid over a base terrain (see docs/terrain_plan.md, sections 3-4).
//   height(x, z) = base(x, z) + HeightField::EditAt(x, z)
// The base is whatever the caller supplies (today the procedural noise). Edits are stored only where the player touched the map:
// 256 m tiles of 128 x 128 int16 deltas at 2 m spacing and 2 cm resolution. Untouched ground costs no memory and no lookup work.
// No platform headers, so this file also builds for iOS. Not thread-safe: mutate on the main thread; workers may read only while
// nothing is mutating (the caller must schedule that).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace tbd {

class HeightField {
public:
    static constexpr float kWorldSize = 20000.0f; // metres; the map is centred on the origin
    static constexpr float kCell = 2.0f;          // metres between samples
    static constexpr float kQuantum = 0.02f;      // metres per stored unit (int16 range: about +-650 m)
    static constexpr int kTileN = 128;            // samples per tile edge
    static constexpr float kTileMetres = kTileN * kCell;
    static constexpr int kGridN = (int)(kWorldSize / kCell); // samples per axis

    enum class BrushMode { Add, Flatten };

    struct TileCoord { int x, z; };

    // Height change at a world position, in metres. 0 where nothing was edited or outside the map.
    float EditAt(float x, float z) const {
        if (tiles_.empty()) return 0.0f;
        const float gx = (x + kWorldSize * 0.5f) / kCell, gz = (z + kWorldSize * 0.5f) / kCell;
        const float fgx = std::floor(gx), fgz = std::floor(gz);
        const int ix = (int)fgx, iz = (int)fgz;
        if (ix < -1 || iz < -1 || ix >= kGridN || iz >= kGridN) return 0.0f;
        const float fx = gx - fgx, fz = gz - fgz;
        const float a = (float)DeltaAt(ix, iz), b = (float)DeltaAt(ix + 1, iz);
        const float c = (float)DeltaAt(ix, iz + 1), d = (float)DeltaAt(ix + 1, iz + 1);
        const float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
        return (top + (bottom - top) * fz) * kQuantum;
    }

    // A round brush. `Add`: strength is metres added at the centre (negative digs). `Flatten`: strength is 0..1, how far each
    // sample moves toward `target` (an absolute height) per call. `baseAt(x, z)` gives the untouched terrain height; Flatten needs it.
    // `hardRadius` > 0: full weight out to it, falling off between it and `radius` (the road tool levels its beds this way).
    template <class BaseFn>
    void ApplyBrush(float cx, float cz, float radius, float strength, BrushMode mode, float target, BaseFn&& baseAt, float hardRadius = 0.0f) {
        if (radius <= 0.0f) return;
        const float half = kWorldSize * 0.5f;
        const int i0 = std::max(0, (int)std::floor((cx - radius + half) / kCell)), i1 = std::min(kGridN - 1, (int)std::floor((cx + radius + half) / kCell));
        const int j0 = std::max(0, (int)std::floor((cz - radius + half) / kCell)), j1 = std::min(kGridN - 1, (int)std::floor((cz + radius + half) / kCell));
        if (i0 > i1 || j0 > j1) return;
        const float r2 = radius * radius;
        std::vector<uint64_t> touched, blocks;
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const float x = (float)i * kCell - half, z = (float)j * kCell - half;
                const float dx = x - cx, dz = z - cz;
                const float t = (dx * dx + dz * dz) / r2;
                if (t >= 1.0f) continue;
                float w = (1.0f - t) * (1.0f - t);
                if (hardRadius > 0.0f) { // a plateau (road beds): full strength inside hardRadius, the falloff only beyond it
                    const float d = std::sqrt(dx * dx + dz * dz), u = std::clamp((d - hardRadius) / std::max(0.01f, radius - hardRadius), 0.0f, 1.0f);
                    w = (1.0f - u * u) * (1.0f - u * u);
                }
                Tile& tile = TileFor(i / kTileN, j / kTileN);
                int16_t& v = tile.d[(size_t)(j % kTileN) * kTileN + (i % kTileN)];
                int next;
                if (mode == BrushMode::Add) {
                    next = (int)v + (int)std::lround(w * strength / kQuantum); // odd in `strength`, so +s then -s restores exactly
                } else {
                    const float base = baseAt(x, z);
                    const float current = base + (float)v * kQuantum;
                    const float moved = current + (target - current) * std::clamp(w * strength, 0.0f, 1.0f);
                    next = (int)std::lround((moved - base) / kQuantum);
                }
                next = std::clamp(next, -32000, 32000);
                if (next == v) continue;
                v = (int16_t)next;
                const uint64_t key = Key(i / kTileN, j / kTileN);
                if (touched.empty() || touched.back() != key) {
                    if (std::find(touched.begin(), touched.end(), key) == touched.end()) touched.push_back(key);
                }
                // The 64 m blocks this sample can affect: its own, plus a neighbour when it lies within 2 samples of the border
                // (the neighbour's edge normals read across the seam).
                for (int bz = BlockOf(j - 2); bz <= BlockOf(j + 2); ++bz)
                    for (int bx = BlockOf(i - 2); bx <= BlockOf(i + 2); ++bx) {
                        const uint64_t bk = BlockKey(bx, bz);
                        if (blocks.empty() || blocks.back() != bk)
                            if (std::find(blocks.begin(), blocks.end(), bk) == blocks.end()) blocks.push_back(bk);
                    }
            }
        const uint32_t strokeVersion = ++counter_;
        for (uint64_t bk : blocks) blockVersion_[bk] = strokeVersion;
        for (uint64_t key : touched) {
            Tile& tile = tiles_[key];
            tile.version = ++counter_;
            int16_t lo = 0, hi = 0;
            for (int16_t d : tile.d) { lo = std::min(lo, d); hi = std::max(hi, d); }
            tile.minDelta = lo;
            tile.maxDelta = hi;
            dirty_.push_back({(int)(key >> 32), (int)(key & 0xFFFFFFFFu)});
        }
    }

    // Tiles changed since the last call (a tile appears once per change; duplicates are removed). Renderer, collision, water and
    // vegetation use this to rebuild only what moved.
    std::vector<TileCoord> TakeDirty() {
        std::vector<TileCoord> out;
        for (const TileCoord& c : dirty_) {
            bool seen = false;
            for (const TileCoord& o : out) if (o.x == c.x && o.z == c.z) { seen = true; break; }
            if (!seen) out.push_back(c);
        }
        dirty_.clear();
        return out;
    }

    // Changes whenever an edit touches a 64 m block (aligned to the world's 64 m multiples, like the terrain chunks) overlapping the
    // rectangle, or when edits were loaded or cleared. Compare with a value stored at build time to decide whether a chunk needs
    // rebuilding. Keep the rectangle small (a chunk): it visits every block it covers.
    uint32_t VersionInRect(float x0, float z0, float x1, float z1) const {
        if (tiles_.empty()) return 0;
        uint32_t v = epoch_;
        const int bx0 = (int)std::floor(x0 / kBlockMetres), bx1 = (int)std::floor(x1 / kBlockMetres);
        const int bz0 = (int)std::floor(z0 / kBlockMetres), bz1 = (int)std::floor(z1 / kBlockMetres);
        for (int bz = bz0; bz <= bz1; ++bz)
            for (int bx = bx0; bx <= bx1; ++bx) {
                const auto it = blockVersion_.find(BlockKey(bx, bz));
                if (it != blockVersion_.end()) v = std::max(v, it->second);
            }
        return v;
    }

    // Like VersionInRect but per 256 m edit tile: coarser and cheap over big areas (the far terrain rings).
    uint32_t TileVersionInRect(float x0, float z0, float x1, float z1) const {
        if (tiles_.empty()) return 0;
        const float half = kWorldSize * 0.5f;
        const int tx0 = std::max(0, (int)std::floor((x0 + half) / kTileMetres)), tx1 = (int)std::floor((x1 + half) / kTileMetres);
        const int tz0 = std::max(0, (int)std::floor((z0 + half) / kTileMetres)), tz1 = (int)std::floor((z1 + half) / kTileMetres);
        uint32_t v = epoch_;
        for (int tz = tz0; tz <= tz1; ++tz)
            for (int tx = tx0; tx <= tx1; ++tx) {
                const auto it = tiles_.find(Key(tx, tz));
                if (it != tiles_.end()) v = std::max(v, it->second.version);
            }
        return v;
    }
    uint32_t Version() const { return counter_; }
    size_t TileCount() const { return tiles_.size(); }
    size_t ResidentBytes() const { return tiles_.size() * (sizeof(int16_t) * kTileN * kTileN); }
    bool Empty() const { return tiles_.empty(); }

    // Frees tiles whose deltas are all zero (for example after the player undid an edit). Does not bump versions: such a tile
    // contributes nothing, so nothing visible changes. Call between frames.
    size_t Compact() {
        size_t removed = 0;
        for (auto it = tiles_.begin(); it != tiles_.end();) {
            if (it->second.minDelta == 0 && it->second.maxDelta == 0) { it = tiles_.erase(it); ++removed; }
            else ++it;
        }
        return removed;
    }

    // Save format: "HFD1", counts, then per non-empty tile its coordinates and zero-run-length coded deltas.
    void Save(std::vector<uint8_t>& out) const {
        Put(out, "HFD1", 4);
        size_t count = 0;
        for (const auto& kv : tiles_) if (kv.second.minDelta != 0 || kv.second.maxDelta != 0) ++count;
        PutU32(out, (uint32_t)kTileN);
        PutU32(out, (uint32_t)count);
        for (const auto& kv : tiles_) {
            const Tile& t = kv.second;
            if (t.minDelta == 0 && t.maxDelta == 0) continue;
            PutU32(out, (uint32_t)(kv.first >> 32));
            PutU32(out, (uint32_t)(kv.first & 0xFFFFFFFFu));
            const size_t n = t.d.size();
            size_t i = 0;
            const size_t sizePos = out.size();
            PutU32(out, 0); // payload size, patched below
            while (i < n) {
                size_t zeros = 0;
                while (i + zeros < n && t.d[i + zeros] == 0) ++zeros;
                i += zeros;
                size_t lit = 0;
                while (i + lit < n && t.d[i + lit] != 0 && lit < 65535) ++lit;
                PutU16(out, (uint16_t)std::min<size_t>(zeros, 65535));
                PutU16(out, (uint16_t)lit);
                for (size_t k = 0; k < lit; ++k) PutU16(out, (uint16_t)t.d[i + k]);
                i += lit;
            }
            const uint32_t payload = (uint32_t)(out.size() - sizePos - 4);
            std::memcpy(&out[sizePos], &payload, 4);
        }
    }

    // Replaces all edits. Returns false (leaving the field unchanged) if the data is malformed or from another tile size.
    bool Load(const uint8_t* data, size_t size) {
        size_t p = 0;
        if (size < 12 || std::memcmp(data, "HFD1", 4) != 0) return false;
        p = 4;
        uint32_t tileN = 0, count = 0;
        if (!GetU32(data, size, p, tileN) || !GetU32(data, size, p, count) || tileN != (uint32_t)kTileN) return false;
        std::unordered_map<uint64_t, Tile> loaded;
        for (uint32_t t = 0; t < count; ++t) {
            uint32_t tx = 0, tz = 0, payload = 0;
            if (!GetU32(data, size, p, tx) || !GetU32(data, size, p, tz) || !GetU32(data, size, p, payload)) return false;
            if (payload > size - p) return false;
            Tile tile;
            const size_t end = p + payload;
            size_t i = 0;
            while (p < end) {
                uint16_t zeros = 0, lit = 0;
                if (!GetU16(data, end, p, zeros) || !GetU16(data, end, p, lit)) return false;
                if (i + zeros + lit > tile.d.size()) return false;
                i += zeros;
                for (uint16_t k = 0; k < lit; ++k) {
                    uint16_t v = 0;
                    if (!GetU16(data, end, p, v)) return false;
                    tile.d[i++] = (int16_t)v;
                }
            }
            int16_t lo = 0, hi = 0;
            for (int16_t d : tile.d) { lo = std::min(lo, d); hi = std::max(hi, d); }
            tile.minDelta = lo;
            tile.maxDelta = hi;
            loaded[((uint64_t)tx << 32) | tz] = std::move(tile);
        }
        tiles_ = std::move(loaded);
        blockVersion_.clear();
        epoch_ = ++counter_; // every chunk and ring that overlaps an edit rebuilds once
        dirty_.clear();
        for (auto& kv : tiles_) { kv.second.version = ++counter_; dirty_.push_back({(int)(kv.first >> 32), (int)(kv.first & 0xFFFFFFFFu)}); }
        return true;
    }

    void Clear() {
        for (const auto& kv : tiles_) dirty_.push_back({(int)(kv.first >> 32), (int)(kv.first & 0xFFFFFFFFu)});
        tiles_.clear();
        blockVersion_.clear();
        epoch_ = ++counter_;
    }

private:
    struct Tile {
        std::vector<int16_t> d = std::vector<int16_t>((size_t)kTileN * kTileN, 0);
        int16_t minDelta = 0, maxDelta = 0;
        uint32_t version = 0;
    };
    static uint64_t Key(int tx, int tz) { return ((uint64_t)(uint32_t)tx << 32) | (uint32_t)tz; }

    Tile& TileFor(int tx, int tz) { return tiles_[Key(tx, tz)]; }

    // Raw delta at a grid sample; 0 outside the map or in untouched tiles.
    int DeltaAt(int i, int j) const {
        if (i < 0 || j < 0 || i >= kGridN || j >= kGridN) return 0;
        const auto it = tiles_.find(Key(i / kTileN, j / kTileN));
        return it == tiles_.end() ? 0 : it->second.d[(size_t)(j % kTileN) * kTileN + (i % kTileN)];
    }

    static void Put(std::vector<uint8_t>& o, const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; o.insert(o.end(), b, b + n); }
    static void PutU32(std::vector<uint8_t>& o, uint32_t v) { Put(o, &v, 4); }
    static void PutU16(std::vector<uint8_t>& o, uint16_t v) { Put(o, &v, 2); }
    static bool GetU32(const uint8_t* d, size_t size, size_t& p, uint32_t& v) { if (p > size || size - p < 4) return false; std::memcpy(&v, d + p, 4); p += 4; return true; }
    static bool GetU16(const uint8_t* d, size_t size, size_t& p, uint16_t& v) { if (p > size || size - p < 2) return false; std::memcpy(&v, d + p, 2); p += 2; return true; }

    static constexpr float kBlockMetres = 64.0f;
    // 64 m block of world space (multiples of 64 from the origin, matching Terrain::kChunkSize) that a sample belongs to.
    static int BlockOf(int sample) { return (int)std::floor(((float)sample * kCell - kWorldSize * 0.5f) / kBlockMetres); }
    static uint64_t BlockKey(int bx, int bz) { return ((uint64_t)(uint32_t)bx << 32) | (uint32_t)bz; }

    std::unordered_map<uint64_t, Tile> tiles_;
    std::unordered_map<uint64_t, uint32_t> blockVersion_; // version of the last stroke that touched each 64 m block
    std::vector<TileCoord> dirty_;
    uint32_t counter_ = 0;
    uint32_t epoch_ = 0; // bumped by Load and Clear: older stored versions no longer match
};

} // namespace tbd
