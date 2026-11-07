// Distance-based streaming for big scenes (full_world). Part of main.cpp's translation unit: it works on g_engine.
//
// At scene open only the .glb JSON chunks are read, for bounds. Each frame the camera decides which entities are wanted.
// Import, grouping, welding, image decode and mips run as background jobs; the main thread creates the GPU objects with no
// GPU wait, a few milliseconds per frame, and publishes an entity when all of it is ready. Entities past the unload distance
// are retired (the renderer frees them once no frame in flight can use them). Pieces too big to stream (whole-map terrain,
// border, water) and instanced assets are always wanted: they are what stays on screen far away.
//
// Env: TBD_STREAMING=0|1 forces it off or on; TBD_STREAM_RADIUS (m, default 1000); TBD_STREAM_BUDGET_MB (default 60% of the
// GPU's memory). TBD_SHOT2="tx ty tz speed" moves a TBD_SHOT camera there once loaded (speed m/s, 0 = jump), to test motion.

namespace world_streaming {

constexpr float kAlwaysRadius = 5000.0f;       // a piece this big is far representation, not a thing to stream
constexpr float kWholeMapCell = 2048.0f;
constexpr float kUnloadFactor = 1.4f;          // hysteresis: unloads at this many times the load distance
constexpr int kMaxAssetsInFlight = 6;          // imported or being decoded at once
constexpr uint64_t kMaxPendingBytes = 2500ull << 20; // decoded data waiting for the GPU before no new asset starts
constexpr double kPumpMs = 1.5;                // GPU creation per frame (TBD_STREAM_PUMP_MS); at least one object always goes
constexpr double kPumpMaxMs = 4.0;             // when the frame has room under kFrameTargetMs
constexpr double kFrameTargetMs = 8.3;         // 120 Hz
constexpr double kCaptureTimeoutSeconds = 600.0;

struct TextureEntry {
    enum State { Decoding, Ready, Uploaded, Failed } state = Decoding;
    tbd::TextureId id = tbd::kNoTexture;
    int refs = 0;
    uint64_t bytes = 0;
    std::shared_ptr<tbd::MipChain> mips;
    bool opaque = false; // every texel's alpha at least 103: mesh.hlsl's cut at 0.4 can never fire, filtered or not
};

struct LoadedPart {
    std::vector<tbd::Vertex> vertices;
    std::vector<uint32_t> indices;
    tbd::Sphere bounds;
    uint64_t albedo = 0, normal = 0; // content hashes; 0 = none
    bool skin = false, pores = false, glass = false;
    size_t flatCount = 0;
    std::vector<tbd::WindowPortal> portals; // glass panes, for the probes
    std::vector<tbd::InteriorRoom> rooms;   // the building's storeys, on the first part with panes
    bool lodChain = false, interior = false; // see MatPart
    float lodError = 0.0f, lodCoarser = -1.0f;
};

struct SourceRange { size_t first, count; uint64_t albedo, normal; bool skin, pores; std::shared_ptr<std::vector<uint8_t>> albedoBytes, normalBytes, roughBytes; bool art = false; tbd::surface_maps::ExtraInputs extra; };

struct LoadJob {
    size_t item = 0;
    std::string path;
    tbd::Vec3 color = {1, 1, 1};
    std::wstring instanceFile; // non-empty: an instanced asset, its matrices in this file
    float instanceCellSize = 0;
    float splitCell = 128.0f; // meters; a whole-map piece splits far coarser, or it is thousands of objects to create

    bool ok = false;
    tbd::ImportedModel model;
    std::vector<LoadedPart> parts;
    std::vector<SourceRange> source;
    std::vector<MatPart> sourceParts; // one per `source` range, made by the worker; Publish only fills in the textures
    std::vector<uint64_t> textures; // one reference each, held from import to release
    tbd::Vertex handle[3] = {};
    tbd::Sphere whole;
    tbd::Vec3 half = {0.5f, 0.5f, 0.5f};
    std::vector<float> rows;
    std::vector<tbd::InstanceCell> cells;
    uint64_t meshBytes = 0;
    std::atomic<int> remaining{1};

    size_t nextTexture = 0, nextPart = 0; // commit progress, main thread
    bool budgetChecked = false;
    std::vector<MatPart> made;
};

// Everything background jobs touch. Jobs hold a reference, so a reset just abandons it to them.
struct Shared {
    std::mutex mutex;
    std::map<uint64_t, TextureEntry> textures;
    std::vector<std::shared_ptr<LoadJob>> done;
    uint64_t pendingBytes = 0;
    // The project's two skin maps, decoded ahead so the first character near the camera doesn't stall a frame.
    std::shared_ptr<tbd::MipChain> skinSurface, poreTile;
    int skinMapsDecoded = 0;
};

enum class ItemState { Unloaded, Loading, Loaded, Denied, Failed, Pinned };

struct Item {
    int entity = -1, instanceSet = -1;
    tbd::Sphere world;
    bool always = false;
    ItemState state = ItemState::Unloaded;
    float distance = 0.0f;
    tbd::MeshId handle = tbd::kInvalidMesh;
    std::vector<tbd::MeshId> partMeshes;
    std::vector<uint64_t> textures;
    uint64_t meshBytes = 0, lastCost = 0;
};

inline bool Streamable(const tbd::Entity& e) {
    return e.mesh == tbd::MeshKind::Imported && e.animClip.empty() && e.faceClip.empty();
}

inline bool EnvFloat(const char* name, float& out) {
    char v[32] = {};
    if (tbd::platform::GetEnvA(name, v, sizeof(v)) == 0) return false;
    out = (float)atof(v);
    return true;
}

inline uint64_t MipBytes(const tbd::MipChain& c) {
    uint64_t total = 0;
    for (const auto& level : c.levels) total += level.size();
    return total;
}

class Streamer {
public:
    bool Active() const { return active_; }

    // A scene whose placements spread wider than a town streams; TBD_STREAMING overrides.
    static bool Wanted(const tbd::Scene& scene) {
        float forced = 0.0f;
        if (EnvFloat("TBD_STREAMING", forced)) return forced != 0.0f;
        tbd::Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
        for (const tbd::Entity& e : scene.entities) {
            lo = {std::min(lo.x, e.position.x), 0, std::min(lo.z, e.position.z)};
            hi = {std::max(hi.x, e.position.x), 0, std::max(hi.z, e.position.z)};
        }
        return !scene.entities.empty() && std::max(hi.x - lo.x, hi.z - lo.z) > 1500.0f;
    }

    // Reads the bounds of everything and makes the item list. Blocks only for the JSON chunks.
    void Start(tbd::Renderer& r) {
        Stop(r);
        LogSinceProcessStart("scene meshes that do not stream are made");
        int64_t t0, t1;
        t0 = tbd::platform::Ticks();
        active_ = true;
        shared_ = std::make_shared<Shared>();
        loadRadius_ = 1000.0f;
        EnvFloat("TBD_STREAM_RADIUS", loadRadius_);
        float mb = 0.0f;
        const uint64_t local = r.LocalMemoryBytes();
        budget_ = EnvFloat("TBD_STREAM_BUDGET_MB", mb) ? (uint64_t)mb << 20 : local ? local / 10 * 6 : 4096ull << 20;

        StartSkinMaps();
        const tbd::Scene& scene = g_engine.scene;
        for (size_t i = 0; i < scene.entities.size(); ++i)
            if (Streamable(scene.entities[i])) { Item item; item.entity = (int)i; items_.push_back(std::move(item)); }
        for (size_t i = 0; i < scene.instances.size(); ++i) { Item item; item.instanceSet = (int)i; item.always = true; items_.push_back(std::move(item)); }

        std::vector<uint8_t> known(items_.size(), 0);
        std::vector<tbd::Vec3> half(items_.size(), tbd::Vec3{0.5f, 0.5f, 0.5f});
        g_engine.jobs->ParallelFor(items_.size(), 4, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                Item& item = items_[i];
                if (item.entity < 0) continue;
                const tbd::Entity& e = scene.entities[(size_t)item.entity];
                const size_t tag = e.meshPath.rfind("#native");
                tbd::Vec3 lo, hi;
                if (tag == std::string::npos || !tbd::ReadGlbBounds(e.meshPath.substr(0, tag), lo, hi)) { item.always = true; continue; }
                tbd::Sphere local;
                local.center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
                half[i] = {std::max((hi.x - lo.x) * 0.5f, 0.01f), std::max((hi.y - lo.y) * 0.5f, 0.01f), std::max((hi.z - lo.z) * 0.5f, 0.01f)};
                local.radius = std::sqrt(tbd::Dot(half[i], half[i]));
                item.world = tbd::TransformSphere(local, e.Model());
                item.always = item.world.radius > kAlwaysRadius || e.meshPath.find("\map_") != std::string::npos; // the whole-map terrain, border and water
                known[i] = 1;
            }
        });
        size_t always = 0;
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].entity >= 0 && known[i]) tbd::ImportedHalfExtents()[scene.entities[(size_t)items_[i].entity].meshPath] = half[i];
            if (items_[i].entity >= 0 && items_[i].always) {
                ++always;
                tbd::Log("Streaming: always loaded: %s, radius %.0f m%s", scene.entities[(size_t)items_[i].entity].name.c_str(), items_[i].world.radius, known[i] ? "" : " (bounds unreadable)");
            }
        }
        t1 = tbd::platform::Ticks();
        loadStart_ = tbd::platform::Ticks();
        lastFrame_ = {};
        ResetFrameStats();
        idleLogged_ = false;
        tbd::Log("Streaming: %zu items, %zu whole-map or unknown; bounds in %.0f ms; load %.0f m, unload %.0f m, budget %llu MB of %llu MB",
                 items_.size(), always, Seconds(t0, t1) * 1000.0, loadRadius_, loadRadius_ * kUnloadFactor,
                 (unsigned long long)(budget_ >> 20), (unsigned long long)(local >> 20));
    }

    // Before the scene's meshes go. A new scene decodes the skin maps again unless they are up. Frees all that streaming owns; abandons jobs still running.
    void Stop(tbd::Renderer& r) {
        if (!active_) return;
        for (Item& item : items_) {
            if (item.state != ItemState::Loaded) continue;
            if (item.instanceSet >= 0) ReleaseTextures(r, item.textures); // DestroyInstancedAssets frees its meshes
            else Unload(r, item, false);
        }
        if (committing_) Drop(r, committing_);
        DrainDone(r);
        { // jobs still running hold references to these; they will find a dead Shared and nothing on the GPU
            std::lock_guard<std::mutex> lock(shared_->mutex);
            for (auto& [hash, entry] : shared_->textures)
                if (entry.state == TextureEntry::Uploaded) { r.RetireTexture(entry.id); entry.state = TextureEntry::Failed; }
        }
        Clear();
    }

    // The renderer is about to die with everything in it.
    void Forget() { Clear(); pores_ = tbd::kNoTexture; poresTried_ = false; skinMapsUploaded_ = false; }

    void Clear() {
        active_ = false;
        items_.clear();
        shared_.reset();
        committing_.reset();
        used_ = 0;
        inFlight_ = 0;
        lastIdleLoaded_ = 0;
        failedLogged_.clear();
    }

    void Update(tbd::Renderer& r, const tbd::Vec3& eye) {
        if (!active_) return;
        int64_t now;
        now = tbd::platform::Ticks();
        if (!lastFrame_ && !firstFrameLogged_) {
            firstFrameLogged_ = true;
            LogSinceProcessStart("first frame");
        }
        if (lastFrame_) {
            const double ms = Seconds(lastFrame_, now) * 1000.0;
            lastInterval_ = ms;
            if (ms > 100.0) tbd::Log("Streaming: %.0f ms frame; the pump before it took %.1f ms", ms, lastPump_);
            worstFrame_ = std::max(worstFrame_, ms);
            hitches33_ += ms > 33.3 ? 1 : 0;
            hitches100_ += ms > 100.0 ? 1 : 0;
            ++frames_;
        }
        lastFrame_ = now;

        const float unload = loadRadius_ * kUnloadFactor;
        for (Item& item : items_) {
            if (item.entity >= 0 && item.state != ItemState::Pinned && item.entity < (int)g_engine.meshes.size()) {
                // The editor replaced this entity's mesh (a tool, an undo): it is no longer ours to stream.
                if (g_engine.meshes[(size_t)item.entity] != item.handle) {
                    ReleaseTextures(r, item.textures);
                    used_ -= std::min(used_, item.meshBytes);
                    item.meshBytes = 0;
                    item.state = ItemState::Pinned;
                    item.handle = tbd::kInvalidMesh;
                    continue;
                }
            }
            // Nearest first: the ground and town around the camera, then the whole map, then scattered instances.
            if (item.always) { item.distance = item.instanceSet >= 0 ? loadRadius_ : loadRadius_ * 0.25f; continue; }
            const tbd::Vec3 away = item.world.center - eye;
            item.distance = std::max(0.0f, std::sqrt(tbd::Dot(away, away)) - item.world.radius);
            if (item.state == ItemState::Loaded && item.distance > unload) Unload(r, item, true);
            if (item.state == ItemState::Denied && item.distance > loadRadius_) item.state = ItemState::Unloaded;
        }

        // Whatever a 120 Hz frame has to spare, never under the minimum, so loading never costs a visible hitch.
        static const double minPump = [] { float v = (float)kPumpMs; EnvFloat("TBD_STREAM_PUMP_MS", v); return std::max(0.25, (double)v); }();
        pumpBudgetMs_ = std::clamp(kFrameTargetMs - (lastInterval_ - lastPump_), minPump, std::max(minPump, kPumpMaxMs));
        int64_t pump0, pump1;
        pump0 = tbd::platform::Ticks();
        pumpTextureMs_ = pumpMeshMs_ = pumpPublishMs_ = 0.0;
        pumpTextures_ = pumpMeshes_ = 0;
        Pump(r, pump0);
        if ((!deadVertices_.empty() || !deadIndices_.empty()) && g_engine.jobs) {
            auto vertices = std::make_shared<std::vector<std::vector<tbd::Vertex>>>(std::move(deadVertices_));
            auto indices = std::make_shared<std::vector<std::vector<uint32_t>>>(std::move(deadIndices_));
            deadVertices_.clear();
            deadIndices_.clear();
            g_engine.jobs->RunBackground([vertices, indices]() mutable { vertices.reset(); indices.reset(); });
        }
        pump1 = tbd::platform::Ticks();
        lastPump_ = Seconds(pump0, pump1) * 1000.0;
        if (lastPump_ > 20.0)
            tbd::Log("Streaming: pump %.1f ms: %u textures %.1f ms, %u meshes %.1f ms, publish %.1f ms, other %.1f ms", lastPump_, pumpTextures_, pumpTextureMs_, pumpMeshes_, pumpMeshMs_,
                     pumpPublishMs_, lastPump_ - pumpTextureMs_ - pumpMeshMs_ - pumpPublishMs_);
        worstPump_ = std::max(worstPump_, lastPump_);
        Launch();

        const bool idle = IsIdle();
        if (!idle && Seconds(lastProgress_, now) > 2.0) {
            lastProgress_ = now;
            size_t queued;
            uint64_t pending;
            {
                std::lock_guard<std::mutex> lock(shared_->mutex);
                queued = shared_->done.size();
                pending = shared_->pendingBytes;
            }
            tbd::Log("Streaming: %.1f s, %u frames: %zu loaded, %d in flight, %zu waiting for GPU, %llu MB pending; committing %s part %zu of %zu",
                     Seconds(loadStart_, now), frames_, Count(ItemState::Loaded), inFlight_, queued, (unsigned long long)(pending >> 20),
                     committing_ ? committing_->path.c_str() : "-", committing_ ? committing_->nextPart : 0, committing_ ? committing_->parts.size() : 0);
        }
        if (idle && !idleLogged_) {
            size_t loaded = 0;
            for (const Item& item : items_) loaded += item.state == ItemState::Loaded ? 1 : 0;
            // A burst of a few items while the camera moves goes idle again in a frame or two: only bursts that took a while,
            // hitched or changed what is missing are worth a line.
            const bool worth = Seconds(loadStart_, now) > 0.5 || hitches33_ > 0 || loaded < lastIdleLoaded_ || loaded >= lastIdleLoaded_ + 50;
            if (worth) {
                tbd::Log("Streaming: idle %.2f s after start; %zu of %zu loaded, %llu MB estimated; %u frames, worst %.1f ms, longest pump %.1f ms, over 33 ms: %u, over 100 ms: %u; %zu denied by the budget",
                         Seconds(loadStart_, now), loaded, items_.size(), (unsigned long long)(used_ >> 20), frames_, worstFrame_, worstPump_, hitches33_, hitches100_, Count(ItemState::Denied));
                LogNotLoaded();
                lastIdleLoaded_ = loaded;
            }
            idleLogged_ = true;
        } else if (!idle && idleLogged_) {
            loadStart_ = tbd::platform::Ticks();
            ResetFrameStats();
            idleLogged_ = false;
        }
    }

    // Why each item that is not on the GPU is not: farther than the load radius (normal), refused by the budget, failed to
    // import (named, each once), or taken over by the editor. Every item that should load is then accounted for.
    void LogNotLoaded() {
        size_t beyond = 0, denied = 0, failed = 0, pinned = 0, waiting = 0;
        float nearestFar = 1e30f;
        for (const Item& item : items_) {
            switch (item.state) {
            case ItemState::Loaded: break;
            case ItemState::Unloaded:
                if (item.always || item.distance <= loadRadius_) ++waiting;
                else { ++beyond; nearestFar = std::min(nearestFar, item.distance); }
                break;
            case ItemState::Loading: ++waiting; break;
            case ItemState::Denied: ++denied; break;
            case ItemState::Failed: ++failed; break;
            case ItemState::Pinned: ++pinned; break;
            }
        }
        tbd::Log("Streaming: not loaded: %zu beyond the %.0f m load radius (nearest %.0f m away), %zu denied by the budget, %zu failed, %zu owned by the editor, %zu still due",
                 beyond, loadRadius_, beyond ? nearestFar : 0.0f, denied, failed, pinned, waiting);
        for (size_t i = 0; i < items_.size(); ++i) {
            const Item& item = items_[i];
            if ((item.state != ItemState::Failed && item.state != ItemState::Denied) || i < failedLogged_.size() && failedLogged_[i]) continue;
            if (failedLogged_.size() < items_.size()) failedLogged_.resize(items_.size(), 0);
            failedLogged_[i] = 1;
            const std::string& path = item.entity >= 0 ? g_engine.scene.entities[(size_t)item.entity].meshPath : g_engine.scene.instances[(size_t)item.instanceSet].meshPath;
            tbd::Log("Streaming: %s %s (%.0f m away)", item.state == ItemState::Failed ? "failed:" : "denied by the budget:", path.c_str(), item.distance);
        }
    }

    bool IsIdle() const {
        if (committing_ || inFlight_ > 0) return false;
        for (const Item& item : items_)
            if (item.state == ItemState::Unloaded && (item.always || item.distance <= loadRadius_)) return false;
        return true;
    }

    bool CaptureMayStart() const {
        if (!active_ || IsIdle()) return true;
        int64_t now;
        now = tbd::platform::Ticks();
        if (Seconds(loadStart_, now) < kCaptureTimeoutSeconds) return false;
        tbd::Log("Streaming: capture gave up waiting after %.0f s; this capture is NOT of a settled scene", kCaptureTimeoutSeconds);
        return true;
    }

    std::string StatusLine() const {
        char line[96];
        size_t nearby = 0, nearLoaded = 0;
        for (const Item& item : items_)
            if (item.always || item.distance <= loadRadius_) {
                ++nearby;
                nearLoaded += item.state == ItemState::Loaded ? 1 : 0;
            }
        if (IsIdle()) snprintf(line, sizeof(line), "STREAM %zu OF %zu, %.1f GB", Count(ItemState::Loaded), items_.size(), (double)used_ / (1024.0 * 1024.0 * 1024.0));
        else snprintf(line, sizeof(line), "LOADING WORLD %zu OF %zu NEARBY, %.1f GB", nearLoaded, nearby, (double)used_ / (1024.0 * 1024.0 * 1024.0));
        return line;
    }

    // For the profiler overlay (F3): one pass over the items, nothing changed. `active` is false when not streaming.
    tbd::profiler::StreamStats Snapshot() const {
        tbd::profiler::StreamStats s;
        if (!active_) return s;
        s.active = true;
        s.total = (uint32_t)items_.size();
        for (const Item& item : items_) {
            switch (item.state) {
            case ItemState::Loaded: ++s.loaded; break;
            case ItemState::Loading: ++s.loading; break;
            case ItemState::Denied: ++s.denied; break;
            case ItemState::Failed: ++s.failed; break;
            default: break;
            }
        }
        s.committing = committing_ != nullptr;
        s.usedBytes = used_;
        s.budgetBytes = budget_;
        s.lastPumpMs = lastPump_;
        s.worstPumpMs = worstPump_;
        s.worstFrameMs = worstFrame_;
        s.over33 = hitches33_;
        s.over100 = hitches100_;
        return s;
    }

    // Keeps the item list in step when the editor deletes an entity.
    // Loaded entities in `entities` go back to Unloaded, so they stream in again from their files (World > Generate LODs).
    void Reload(tbd::Renderer& r, const std::set<int>& entities) {
        for (Item& item : items_)
            if (item.entity >= 0 && entities.count(item.entity) && item.state == ItemState::Loaded) Unload(r, item, false);
    }

    // Undo put an entity back at `index`: later items move up one (the inverse of OnEntityErased's renumbering).
    void OnEntityInserted(int index) {
        if (!active_) return;
        for (Item& item : items_) if (item.entity >= index) ++item.entity;
    }

    void OnEntityErased(tbd::Renderer* r, int index) {
        if (!active_) return;
        for (size_t i = 0; i < items_.size();) {
            Item& item = items_[i];
            if (item.entity == index) {
                if (r) ReleaseTextures(*r, item.textures);
                used_ -= std::min(used_, item.meshBytes);
                item.entity = -1;
                item.state = ItemState::Pinned;
                item.always = false;
                item.world.radius = 0;
            } else if (item.entity > index) --item.entity;
            ++i;
        }
    }

private:
    double Seconds(const int64_t& a, const int64_t& b) const { return (double)(b - a) / (double)g_engine.tickFreq; }

    static void LogSinceProcessStart(const char* what) {
        tbd::Log("Streaming: %s %.2f s after process start", what, tbd::platform::SecondsSinceProcessStart());
    }

    void ResetFrameStats() { frames_ = 0; worstFrame_ = 0; worstPump_ = 0; hitches33_ = 0; hitches100_ = 0; }

    size_t Count(ItemState s) const {
        size_t n = 0;
        for (const Item& item : items_) n += item.state == s ? 1 : 0;
        return n;
    }

    // Nearest first: whole-map pieces (-1), then what is around the camera, then instanced assets.
    void Launch() {
        // A hard ceiling on the process's own memory, whatever the estimates say: past 40% of the machine's RAM (TBD_STREAM_RAM_PCT)
        // no new asset starts until uploads and evictions bring it down. Paging the whole PC is never worth a farther building.
        static const uint64_t ramCap = [] {
            const uint64_t total = tbd::platform::PhysicalMemoryBytes();
            float pct = 40.0f;
            EnvFloat("TBD_STREAM_RAM_PCT", pct);
            return total ? (uint64_t)((double)total * std::clamp(pct, 10.0f, 90.0f) / 100.0) : (6ull << 30);
        }();
        if (PrivateBytes() > ramCap) {
            if (!ramCapLogged_) { ramCapLogged_ = true; tbd::Log("Streaming: process memory over %llu MB: new loads wait", (unsigned long long)(ramCap >> 20)); }
            return;
        }
        for (;;) {
            { // only assets still on the workers count: one waiting for the GPU must not hold back the next import
                std::lock_guard<std::mutex> lock(shared_->mutex);
                if (shared_->pendingBytes > kMaxPendingBytes) return;
                if (inFlight_ - (int)shared_->done.size() - (committing_ ? 1 : 0) >= kMaxAssetsInFlight) return;
            }
            Item* best = nullptr;
            size_t bestIndex = 0;
            for (size_t i = 0; i < items_.size(); ++i) {
                Item& item = items_[i];
                if (item.state != ItemState::Unloaded || (!item.always && item.distance > loadRadius_)) continue;
                if (item.lastCost && used_ + item.lastCost > budget_ && !Evictable(item)) { item.state = ItemState::Denied; continue; }
                if (!best || item.distance < best->distance) { best = &item; bestIndex = i; }
            }
            if (!best) return;
            best->state = ItemState::Loading;
            ++inFlight_;
            StartJob(bestIndex, *best);
        }
    }

    bool Evictable(const Item& wanted) const {
        uint64_t freeable = 0;
        for (const Item& item : items_)
            if (item.state == ItemState::Loaded && !item.always && item.distance > wanted.distance) freeable += item.lastCost;
        return used_ + wanted.lastCost <= budget_ + freeable;
    }

    void StartSkinMaps() {
        std::shared_ptr<Shared> shared = shared_;
        const auto start = [&](const wchar_t* file, std::shared_ptr<tbd::MipChain> Shared::*slot) {
            g_engine.jobs->RunBackground([shared, slot, path = tbd::Join(g_engine.assetsRoot, file)] {
                std::shared_ptr<tbd::MipChain> mips;
                try {
                    std::string bytes;
                    std::vector<uint8_t> pixels;
                    int w = 0, h = 0;
                    if (tbd::ReadAllText(path, bytes) && tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), pixels, w, h))
                        mips = std::make_shared<tbd::MipChain>(tbd::BuildMipChain(pixels.data(), w, h));
                } catch (...) { mips.reset(); }
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared.get()->*slot = std::move(mips);
                ++shared->skinMapsDecoded;
            });
        };
        start(L"skin\\skin_surface.png", &Shared::skinSurface);
        start(L"skin\\skin_pores_tile.png", &Shared::poreTile);
    }

    // False until both maps are decoded and on the GPU; they live as long as the renderer.
    bool SkinMapsReady(tbd::Renderer& r) {
        if (skinMapsUploaded_) return true;
        std::shared_ptr<tbd::MipChain> surface, tile;
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            if (shared_->skinMapsDecoded < 2) return false;
            surface = std::move(shared_->skinSurface);
            tile = std::move(shared_->poreTile);
        }
        if (!g_engine.skinSurfaceTried) {
            g_engine.skinSurfaceTried = true;
            if (surface) g_engine.skinSurface = r.CreateTextureNoWait(surface->levels, surface->dims);
        }
        if (!poresTried_) {
            poresTried_ = true;
            if (tile) pores_ = r.CreateTextureNoWait(tile->levels, tile->dims);
        }
        skinMapsUploaded_ = true;
        return true;
    }

    void StartJob(size_t index, const Item& item) {
        auto job = std::make_shared<LoadJob>();
        job->item = index;
        if (item.always && item.entity >= 0) job->splitCell = kWholeMapCell;
        if (item.entity >= 0) {
            const tbd::Entity& e = g_engine.scene.entities[(size_t)item.entity];
            job->path = e.meshPath;
            job->color = e.color;
        } else {
            const tbd::InstanceSet& set = g_engine.scene.instances[(size_t)item.instanceSet];
            job->path = set.meshPath;
            job->instanceFile = tbd::Join(g_engine.assetsRoot, tbd::Utf8ToWide(set.file));
            job->instanceCellSize = tbd::InstanceCellSize(set.maxDistance);
        }
        tbd::JobSystem* jobs = g_engine.jobs.get();
        std::shared_ptr<Shared> shared = shared_;
        jobs->RunBackground([job, shared, jobs] { ImportStage(job, shared, jobs); });
    }

    // The process's private bytes, for the RAM ceiling in Launch.
    static uint64_t PrivateBytes() {
        return tbd::platform::ProcessPrivateBytes();
    }
    static void Finish(const std::shared_ptr<LoadJob>& job, const std::shared_ptr<Shared>& shared) {
        if (job->remaining.fetch_sub(1) != 1) return;
        job->model = tbd::ImportedModel{}; // the encoded images
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->pendingBytes += job->meshBytes;
        shared->done.push_back(job);
    }

    // A worker thread. Touches no engine state: its inputs are in the job, its outputs in the job and in `shared`.
    static void ImportStage(std::shared_ptr<LoadJob> job, std::shared_ptr<Shared> shared, tbd::JobSystem* jobs) {
        try { BuildAsset(*job, *shared, jobs, job, shared); }
        catch (...) { job->ok = false; tbd::Log("Streaming: import of %s threw", job->path.c_str()); }
        Finish(job, shared);
    }

    static void BuildAsset(LoadJob& job, Shared& shared, tbd::JobSystem* jobs, const std::shared_ptr<LoadJob>& self, const std::shared_ptr<Shared>& selfShared) {
        ImportResult imported;
        ImportFile(job.path, imported);
        if (!imported.ok || imported.vertices.size() < 3) {
            tbd::Log("Streaming: cannot import %s: %s", job.path.c_str(), imported.error.c_str());
            return;
        }
        std::vector<tbd::Vertex>& v = imported.vertices;
        for (tbd::Vertex& x : v) { x.r *= job.color.x; x.g *= job.color.y; x.b *= job.color.z; }
        job.model = std::move(imported.model);
        job.half = imported.halfExtents;
        job.whole = tbd::SphereAround(v.data(), v.size());
        std::copy(v.data(), v.data() + 3, job.handle);
        if (!job.instanceFile.empty()) {
            std::string bytes;
            if (!tbd::ReadAllText(job.instanceFile, bytes) || bytes.empty() || bytes.size() % (tbd::kInstanceFloats * sizeof(float)) != 0) {
                tbd::Log("Streaming: cannot use the instances of %s", job.path.c_str());
                return;
            }
            job.rows.resize(bytes.size() / sizeof(float));
            std::memcpy(job.rows.data(), bytes.data(), bytes.size());
            job.cells = tbd::BuildInstanceCells(job.rows, job.whole, job.instanceCellSize);
            job.meshBytes += bytes.size();
        }

        // The image behind each part, by content, so identical images embedded in many files are one texture.
        std::map<int, uint64_t> hashes;
        const auto hashOf = [&](int image) -> uint64_t {
            if (image < 0 || image >= (int)job.model.images.size()) return 0;
            const auto it = hashes.find(image);
            if (it != hashes.end()) return it->second;
            return hashes[image] = ContentHash(job.model.images[(size_t)image]) | 1; // never 0
        };
        std::vector<tbd::ImportedPart> parts = job.model.parts;
        if (parts.empty() || parts.back().first + parts.back().count != v.size()) { // no usable ranges: all of it, untextured
            parts.clear();
            tbd::ImportedPart all;
            all.count = v.size();
            parts.push_back(all);
        }
        // The same grouping as UploadMergedParts: one mesh per material, a very big material split into a grid for culling.
        size_t kSplitTriangles = 30000;
        float kSplitCell = job.splitCell;
        if (kSplitCell <= 128.0f) RoomSplit(tbd::SphereAround(v.data(), v.size()), kSplitTriangles, kSplitCell); // not whole-map pieces
        using Material = std::tuple<uint64_t, uint64_t, bool, bool, bool>; // the last: glass, kept whole for its panes
        std::map<Material, size_t> materialTriangles;
        std::vector<Material> partMaterial;
        std::map<int, std::shared_ptr<std::vector<uint8_t>>> imageCopies;
        for (const tbd::ImportedPart& p : parts) {
            const uint64_t albedo = hashOf(p.albedo), normal = hashOf(p.normal);
            const bool pores = p.skin && p.normal < 0;
            partMaterial.push_back({albedo, normal, p.skin, pores, p.glass});
            materialTriangles[partMaterial.back()] += p.count / 3;
            const auto bytesOf = [&](int image) { // one copy per image, shared by every part that uses it: a building has dozens of parts
                if (image < 0 || image >= (int)job.model.images.size()) return std::shared_ptr<std::vector<uint8_t>>();
                std::shared_ptr<std::vector<uint8_t>>& copy = imageCopies[image];
                if (!copy) copy = std::make_shared<std::vector<uint8_t>>(job.model.images[(size_t)image]);
                return copy;
            };
            const bool wantsMaps = !p.skin && !p.eye && p.albedo >= 0;
            job.source.push_back({p.first, p.count, albedo, normal, p.skin, pores, wantsMaps ? bytesOf(p.albedo) : nullptr, wantsMaps ? bytesOf(p.normal) : nullptr, wantsMaps ? bytesOf(p.rough) : nullptr, p.art, {}});
            if (wantsMaps) {
                tbd::surface_maps::ExtraInputs& extra = job.source.back().extra;
                extra.rough = job.source.back().roughBytes; extra.occlusion = bytesOf(p.occlusion); extra.emissive = bytesOf(p.emissive);
                std::copy(p.emissiveFactor, p.emissiveFactor + 3, extra.emissiveFactor);
                extra.metallic = p.metallic;
            }
        }
        std::map<std::tuple<Material, int, int>, std::vector<tbd::Vertex>> groups;
        for (size_t k = 0; k < parts.size(); ++k) {
            const tbd::ImportedPart& p = parts[k];
            const bool split = materialTriangles[partMaterial[k]] > kSplitTriangles;
            for (size_t t = 0; t + 3 <= p.count; t += 3) {
                const tbd::Vertex* tri = v.data() + p.first + t;
                int cx = 0, cz = 0;
                if (split) {
                    cx = (int)std::floor((tri[0].px + tri[1].px + tri[2].px) / (3.0f * kSplitCell));
                    cz = (int)std::floor((tri[0].pz + tri[1].pz + tri[2].pz) / (3.0f * kSplitCell));
                }
                std::vector<tbd::Vertex>& group = groups[{partMaterial[k], cx, cz}];
                group.insert(group.end(), tri, tri + 3);
            }
        }
        std::vector<tbd::InteriorRoom> rooms, allRooms;
        for (const auto& [key, vertices] : groups)
            if (!vertices.empty() && std::fabs(vertices[0].a - tbd::kGlassAlpha) < 0.003f) { rooms = tbd::ExtractInteriorRooms(v.data(), v.size()); break; }
        const bool box = rooms.empty() && IsShellAsset(job.path) && job.instanceFile.empty();
        tbd::Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
        for (size_t k = 0; box && k < v.size(); ++k) { const tbd::Vertex& x = v[k]; lo = {std::min(lo.x, x.px), std::min(lo.y, x.py), std::min(lo.z, x.pz)}; hi = {std::max(hi.x, x.px), std::max(hi.y, x.py), std::max(hi.z, x.pz)}; }
        std::vector<tbd::Vertex>().swap(v);
        job.parts.reserve(groups.size());
        for (auto& [key, vertices] : groups) {
            LoadedPart part;
            const Material& m = std::get<0>(key);
            part.albedo = std::get<0>(m);
            part.normal = std::get<1>(m);
            part.skin = std::get<2>(m);
            part.pores = std::get<3>(m);
            part.glass = std::get<4>(m);
            part.flatCount = vertices.size();
            part.bounds = tbd::SphereAround(vertices.data(), vertices.size());
            part.portals = tbd::ExtractWindowPortals(vertices.data(), vertices.size());
            if (allRooms.empty()) allRooms = rooms;
            if (!part.portals.empty()) part.rooms = std::move(rooms);
            part.interior = part.portals.empty() && (box ? PartInsideShellBox(part.bounds, lo, hi) : PartIsInterior(part.bounds, allRooms));
            tbd::WeldVertices(vertices.data(), (uint32_t)vertices.size(), part.vertices, part.indices);
            std::vector<tbd::Vertex>().swap(vertices);
            std::vector<tbd::LodLevel> levels; // instanced assets draw only the full level, so they skip the work
            if (part.portals.empty() && !part.interior && job.instanceFile.empty()) levels = LodLevelsFor(job.path, part.vertices, part.indices, part.bounds.radius);
            part.lodChain = !levels.empty();
            if (part.lodChain) part.lodCoarser = levels[0].error;
            LoadedPart base;
            base.bounds = part.bounds; base.albedo = part.albedo; base.normal = part.normal; base.skin = part.skin; base.pores = part.pores; base.glass = part.glass;
            job.meshBytes += part.vertices.size() * sizeof(tbd::Vertex) + part.indices.size() * sizeof(uint32_t);
            job.parts.push_back(std::move(part));
            for (size_t l = 0; l < levels.size(); ++l) {
                LoadedPart lod = base;
                lod.lodChain = true;
                lod.lodError = levels[l].error;
                lod.lodCoarser = l + 1 < levels.size() ? levels[l + 1].error : -1.0f;
                lod.flatCount = levels[l].indices.size();
                lod.vertices = std::move(levels[l].vertices);
                lod.indices = std::move(levels[l].indices);
                job.meshBytes += lod.vertices.size() * sizeof(tbd::Vertex) + lod.indices.size() * sizeof(uint32_t);
                job.parts.push_back(std::move(lod));
            }
        }
        job.sourceParts.resize(job.source.size());
        for (size_t k = 0; k < job.source.size(); ++k) { job.sourceParts[k].first = job.source[k].first; job.sourceParts[k].count = job.source[k].count; }
        job.ok = true;

        // Reference every image; decode the ones nobody has yet, each its own job.
        std::vector<std::pair<int, uint64_t>> toDecode;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            std::set<uint64_t> mine;
            for (const auto& [image, hash] : hashes) {
                if (!mine.insert(hash).second) continue;
                job.textures.push_back(hash);
                TextureEntry& entry = shared.textures[hash];
                if (++entry.refs == 1 && entry.state == TextureEntry::Decoding) toDecode.push_back({image, hash});
            }
        }
        const std::shared_ptr<tbd::ImportedModel> model = std::make_shared<tbd::ImportedModel>(std::move(job.model));
        for (const auto& [image, hash] : toDecode) {
            job.remaining.fetch_add(1);
            const int img = image;
            const uint64_t content = hash;
            jobs->RunBackground([self, selfShared, model, img, content] {
                auto mips = std::make_shared<tbd::MipChain>();
                bool ok = false;
                try {
                    const std::vector<uint8_t>& bytes = model->images[(size_t)img];
                    std::vector<uint8_t> rgba;
                    int w = 0, h = 0;
                    ok = tbd::DecodeImage(bytes.data(), bytes.size(), rgba, w, h);
                    if (ok) *mips = tbd::BuildMipChain(rgba.data(), w, h);
                    if (ok) {
                        bool opaque = true;
                        for (size_t a = 3; a < rgba.size() && opaque; a += 4) opaque = rgba[a] >= 103;
                        mips->opaque = opaque;
                    }
                } catch (...) { ok = false; }
                {
                    std::lock_guard<std::mutex> lock(selfShared->mutex);
                    TextureEntry& entry = selfShared->textures[content];
                    entry.state = ok ? TextureEntry::Ready : TextureEntry::Failed;
                    if (ok) {
                        entry.bytes = MipBytes(*mips);
                        entry.opaque = mips->opaque;
                        entry.mips = std::move(mips);
                        selfShared->pendingBytes += entry.bytes;
                    }
                }
                Finish(self, selfShared);
            });
        }
    }

    void DrainDone(tbd::Renderer& r) {
        std::vector<std::shared_ptr<LoadJob>> done;
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            done.swap(shared_->done);
        }
        for (const std::shared_ptr<LoadJob>& job : done) Drop(r, job);
    }

    // A finished job dropped at scene close: the accounting is about to be cleared with everything else.
    void Drop(tbd::Renderer& r, const std::shared_ptr<LoadJob>& job) {
        for (const MatPart& p : job->made) r.RetireMesh(p.mesh);
        ReleaseTextures(r, job->textures);
    }

    void ReleaseTextures(tbd::Renderer& r, std::vector<uint64_t>& textures) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        for (uint64_t hash : textures) {
            const auto it = shared_->textures.find(hash);
            if (it == shared_->textures.end() || --it->second.refs > 0) continue;
            if (it->second.state == TextureEntry::Uploaded) {
                r.RetireTexture(it->second.id);
                used_ -= std::min(used_, it->second.bytes);
            } else if (it->second.state == TextureEntry::Ready) {
                shared_->pendingBytes -= std::min(shared_->pendingBytes, it->second.bytes);
            }
            shared_->textures.erase(it);
        }
        textures.clear();
    }

    void Unload(tbd::Renderer& r, Item& item, bool mayRetry) {
        for (tbd::MeshId id : item.partMeshes) r.RetireMesh(id);
        item.partMeshes.clear();
        if (item.handle != tbd::kInvalidMesh) {
            g_engine.matParts.erase(item.handle);
            g_engine.sourceParts.erase(item.handle);
            if (item.handle < g_engine.meshBounds.size()) g_engine.meshBounds[item.handle] = tbd::Sphere{};
            r.RetireMesh(item.handle);
            if (item.entity >= 0 && item.entity < (int)g_engine.meshes.size()) g_engine.meshes[(size_t)item.entity] = tbd::kInvalidMesh;
            item.handle = tbd::kInvalidMesh;
        }
        ReleaseTextures(r, item.textures);
        used_ -= std::min(used_, item.meshBytes);
        item.meshBytes = 0;
        item.state = ItemState::Unloaded;
        if (mayRetry) // room appeared: what the budget turned away may fit now
            for (Item& other : items_)
                if (other.state == ItemState::Denied) other.state = ItemState::Unloaded;
    }

    // Takes finished jobs, nearest first, and creates their GPU objects until the frame's time is spent.
    void Pump(tbd::Renderer& r, const int64_t& start) {
        const auto spent = [&] {
            int64_t now;
            now = tbd::platform::Ticks();
            return Seconds(start, now) * 1000.0 >= pumpBudgetMs_;
        };
        for (;;) {
            {
                // Always the nearest finished asset, even if that sets a farther one aside half made: it keeps its progress.
                std::lock_guard<std::mutex> lock(shared_->mutex);
                size_t best = shared_->done.size();
                for (size_t i = 0; i < shared_->done.size(); ++i)
                    if (best == shared_->done.size() || items_[shared_->done[i]->item].distance < items_[shared_->done[best]->item].distance) best = i;
                if (best != shared_->done.size() && (!committing_ || items_[shared_->done[best]->item].distance < items_[committing_->item].distance)) {
                    std::shared_ptr<LoadJob> next = shared_->done[best];
                    shared_->done.erase(shared_->done.begin() + (ptrdiff_t)best);
                    if (committing_) shared_->done.push_back(committing_);
                    committing_ = std::move(next);
                }
                if (!committing_) return;
            }
            LoadJob& job = *committing_;
            Item& item = items_[job.item];
            if (item.state == ItemState::Pinned) { // the editor took or deleted the entity while it loaded
                Abandon(r, job);
                continue;
            }
            if (!job.ok) {
                item.state = ItemState::Failed;
                Abandon(r, job);
                continue;
            }
            if (!job.budgetChecked) {
                job.budgetChecked = true;
                if (!item.always && item.distance > loadRadius_ * kUnloadFactor) { // the camera left while it loaded
                    item.state = ItemState::Unloaded;
                    Abandon(r, job);
                    continue;
                }
                const uint64_t cost = EstimateCost(job);
                item.lastCost = cost;
                int64_t m0, m1;
                m0 = tbd::platform::Ticks();
                const bool room = MakeRoom(r, item, cost);
                m1 = tbd::platform::Ticks();
                if (Seconds(m0, m1) > 0.008) tbd::Log("Streaming: slow eviction for %s: %.1f ms", job.path.c_str(), Seconds(m0, m1) * 1000.0);
                if (!room) {
                    item.state = ItemState::Denied;
                    tbd::Log("Streaming: budget: no room for %s (%llu MB)", job.path.c_str(), (unsigned long long)(cost >> 20));
                    Abandon(r, job);
                    continue;
                }
            }
            bool usesSkin = false;
            for (const SourceRange& s : job.source) usesSkin |= s.skin;
            if (usesSkin && !SkinMapsReady(r)) return;
            // Textures first; an image another job is still decoding holds this one back a frame.
            while (job.nextTexture < job.textures.size()) {
                std::shared_ptr<tbd::MipChain> mips;
                {
                    std::lock_guard<std::mutex> lock(shared_->mutex);
                    TextureEntry& entry = shared_->textures[job.textures[job.nextTexture]];
                    if (entry.state == TextureEntry::Decoding) return;
                    if (entry.state != TextureEntry::Ready) { ++job.nextTexture; continue; }
                    mips = entry.mips;
                }
                int64_t t0, t1;
                t0 = tbd::platform::Ticks();
                const tbd::TextureId id = r.CreateTextureNoWait(mips->levels, mips->dims);
                t1 = tbd::platform::Ticks();
                pumpTextureMs_ += Seconds(t0, t1) * 1000.0;
                ++pumpTextures_;
                if (Seconds(t0, t1) > 0.03) tbd::Log("Streaming: slow texture %dx%d: %.1f ms", mips->dims[0].first, mips->dims[0].second, Seconds(t0, t1) * 1000.0);
                if (id == tbd::kNoTexture) OnGpuAllocationFailed("texture");
                {
                    std::lock_guard<std::mutex> lock(shared_->mutex);
                    TextureEntry& entry = shared_->textures[job.textures[job.nextTexture]];
                    entry.id = id;
                    entry.state = id == tbd::kNoTexture ? TextureEntry::Failed : TextureEntry::Uploaded;
                    entry.mips.reset();
                    shared_->pendingBytes -= std::min(shared_->pendingBytes, entry.bytes);
                }
                if (id != tbd::kNoTexture) { uint64_t bytes = MipBytes(*mips); used_ += bytes; }
                ++job.nextTexture;
                if (spent()) return;
            }
            while (job.nextPart < job.parts.size()) {
                LoadedPart& part = job.parts[job.nextPart];
                int64_t t0, t1;
                t0 = tbd::platform::Ticks();
                const tbd::MeshId id = r.CreateMeshNoWait(part.vertices.data(), (uint32_t)part.vertices.size(), part.indices.data(), (uint32_t)part.indices.size());
                t1 = tbd::platform::Ticks();
                pumpMeshMs_ += Seconds(t0, t1) * 1000.0;
                ++pumpMeshes_;
                if (Seconds(t0, t1) > 0.03) tbd::Log("Streaming: slow mesh, %zu vertices: %.1f ms", part.vertices.size(), Seconds(t0, t1) * 1000.0);
                const uint64_t bytes = part.vertices.size() * sizeof(tbd::Vertex) + part.indices.size() * sizeof(uint32_t);
                if (id == tbd::kInvalidMesh) OnGpuAllocationFailed("mesh");
                else {
                    MatPart made;
                    made.mesh = id;
                    made.count = part.flatCount;
                    made.bounds = part.bounds;
                    made.portals = std::move(part.portals);
                    made.rooms = std::move(part.rooms);
                    made.caster = made.portals.empty() && !part.interior; // glass lets the sun into a room; furniture is in the shell's shade
                    made.interior = part.interior;
                    made.lodChain = part.lodChain;
                    made.lodError = part.lodError;
                    made.lodCoarser = part.lodCoarser;
                    made.albedo = TextureOf(part.albedo);
                    // Only what can never be cut out or discarded goes into the depth pre-pass (see SetScenePrepass).
                    made.opaque = !part.glass && !part.skin && !part.pores && made.portals.empty() && (!part.albedo || AlbedoOpaque(part.albedo));
                    made.normal = part.pores ? PoreTile(r) : TextureOf(part.normal);
                    if (part.skin) made.surface = SkinSurfaceFor(job.path); // a character folder or head may carry its own maps (main.cpp)
                    job.made.push_back(made);
                    item.meshBytes += bytes;
                    used_ += bytes;
                }
                {
                    std::lock_guard<std::mutex> lock(shared_->mutex);
                    shared_->pendingBytes -= std::min(shared_->pendingBytes, bytes);
                }
                // Freed on a worker: giving back tens of megabytes is milliseconds of the main thread's time.
                deadVertices_.push_back(std::move(part.vertices));
                deadIndices_.push_back(std::move(part.indices));
                part.vertices = {};
                part.indices = {};
                ++job.nextPart;
                if (spent()) return;
            }
            int64_t p0, p1;
            p0 = tbd::platform::Ticks();
            Publish(r, job, item);
            p1 = tbd::platform::Ticks();
            pumpPublishMs_ += Seconds(p0, p1) * 1000.0;
            if (Seconds(p0, p1) > 0.008) tbd::Log("Streaming: slow publish of %s, %zu source parts: %.1f ms", job.path.c_str(), job.source.size(), Seconds(p0, p1) * 1000.0);
            {
                std::lock_guard<std::mutex> lock(shared_->mutex);
                shared_->pendingBytes -= std::min<uint64_t>(shared_->pendingBytes, job.rows.size() * sizeof(float));
            }
            FinishJob();
            if (spent()) return;
        }
    }

    void FinishJob() {
        // A big job holds tens of thousands of ranges and their shared image bytes: it is freed on a worker, not here.
        if (committing_ && committing_.use_count() == 1 && g_engine.jobs) g_engine.jobs->RunBackground([dead = std::move(committing_)]() mutable { dead.reset(); });
        committing_.reset();
        if (inFlight_ > 0) --inFlight_;
    }

    void Abandon(tbd::Renderer& r, LoadJob& job) {
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            shared_->pendingBytes -= std::min(shared_->pendingBytes, job.meshBytes);
        }
        ReleaseTextures(r, job.textures);
        FinishJob();
    }

    uint64_t EstimateCost(const LoadJob& job) {
        uint64_t cost = job.meshBytes;
        std::lock_guard<std::mutex> lock(shared_->mutex);
        for (uint64_t hash : job.textures) {
            const auto it = shared_->textures.find(hash);
            if (it != shared_->textures.end() && it->second.state == TextureEntry::Ready) cost += it->second.bytes;
        }
        return cost;
    }

    // Unloads the farthest items that are farther than this one until `cost` fits. Whole-map pieces always fit.
    bool MakeRoom(tbd::Renderer& r, const Item& wanted, uint64_t cost) {
        while (used_ + cost > budget_) {
            Item* farthest = nullptr;
            for (Item& item : items_)
                if (item.state == ItemState::Loaded && !item.always && item.distance > wanted.distance && (!farthest || item.distance > farthest->distance)) farthest = &item;
            if (!farthest) return wanted.always;
            Unload(r, *farthest, false);
        }
        return true;
    }

    void OnGpuAllocationFailed(const char* what) {
        tbd::Log("Streaming: the GPU refused a %s at %llu MB estimated; the budget drops to that", what, (unsigned long long)(used_ >> 20));
        budget_ = std::min(budget_, used_);
    }

    bool AlbedoOpaque(uint64_t hash) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        const auto it = shared_->textures.find(hash);
        return it != shared_->textures.end() && it->second.state == TextureEntry::Uploaded && it->second.opaque;
    }

    tbd::TextureId TextureOf(uint64_t hash) {
        if (!hash) return tbd::kNoTexture;
        std::lock_guard<std::mutex> lock(shared_->mutex);
        const auto it = shared_->textures.find(hash);
        return it != shared_->textures.end() && it->second.state == TextureEntry::Uploaded ? it->second.id : tbd::kNoTexture;
    }

    // Skin without its own normal map gets the project's tiling pore detail, as PartTextures does.
    tbd::TextureId PoreTile(tbd::Renderer&) { return pores_; }

    void Publish(tbd::Renderer& r, LoadJob& job, Item& item) {
        const tbd::MeshId handle = job.made.empty() ? tbd::kInvalidMesh : r.CreateMeshNoWait(job.handle, 3, kHandleIndices, 3);
        if (handle == tbd::kInvalidMesh) {
            for (const MatPart& p : job.made) r.RetireMesh(p.mesh);
            used_ -= std::min(used_, item.meshBytes);
            item.meshBytes = 0;
            ReleaseTextures(r, job.textures);
            item.state = ItemState::Failed;
            return;
        }
        SetMeshBounds(handle, job.whole);
        int64_t q0, q1, q2;
        q0 = tbd::platform::Ticks();
        // A street mesh has tens of thousands of source ranges but a handful of materials: each is looked up once, and the
        // parts themselves were made by the worker.
        std::vector<MatPart>& source = job.sourceParts;
        struct Looked { tbd::TextureId albedo, normal; };
        std::map<std::tuple<uint64_t, uint64_t, bool>, Looked> looked;
        std::set<std::pair<tbd::TextureId, tbd::TextureId>> requested;
        uint64_t lastKey[2] = {~0ull, ~0ull};
        bool lastPores = false;
        Looked last = {};
        for (size_t k = 0; k < job.source.size() && k < source.size(); ++k) {
            const SourceRange& s = job.source[k];
            MatPart& p = source[k];
            if (s.albedo != lastKey[0] || s.normal != lastKey[1] || s.pores != lastPores) { // neighbours mostly share one
                const auto key = std::make_tuple(s.albedo, s.normal, s.pores);
                auto it = looked.find(key);
                if (it == looked.end()) it = looked.emplace(key, Looked{TextureOf(s.albedo), s.pores ? PoreTile(r) : TextureOf(s.normal)}).first;
                last = it->second;
                lastKey[0] = s.albedo; lastKey[1] = s.normal; lastPores = s.pores;
                if (s.albedoBytes && requested.insert({last.albedo, last.normal}).second)
                    surface_async::Request(last.albedo, last.normal, s.albedoBytes, s.normalBytes, s.roughBytes, s.art, s.extra);
            }
            p.albedo = last.albedo;
            p.normal = last.normal;
            if (s.skin) p.surface = SkinSurfaceFor(job.path);
        }
        q1 = tbd::platform::Ticks();
        g_engine.sourceParts[handle] = std::move(source);
        q2 = tbd::platform::Ticks();
        if (Seconds(q0, q2) > 0.008) tbd::Log("Streaming: publish of %zu source parts: textures %.1f ms, store %.1f ms", job.source.size(), Seconds(q0, q1) * 1000.0, Seconds(q1, q2) * 1000.0);
        item.partMeshes.clear();
        for (const MatPart& p : job.made) item.partMeshes.push_back(p.mesh);
        g_engine.matParts[handle] = std::move(job.made);
        item.handle = handle;
        item.textures = std::move(job.textures);
        item.state = ItemState::Loaded;
        if (item.entity >= 0) {
            g_engine.meshes[(size_t)item.entity] = handle;
            tbd::ImportedHalfExtents()[g_engine.scene.entities[(size_t)item.entity].meshPath] = job.half;
        } else {
            const tbd::InstanceSet& set = g_engine.scene.instances[(size_t)item.instanceSet];
            InstancedAsset asset;
            asset.mesh = handle;
            asset.maxDistance = set.maxDistance;
            asset.shadowDistance = set.shadowDistance;
            asset.cells = std::move(job.cells);
            asset.buffer = r.CreateInstanceBufferNoWait(job.rows.data(), (uint32_t)(job.rows.size() / tbd::kInstanceFloats));
            g_engine.instanced.push_back(std::move(asset));
        }
    }

    static constexpr uint32_t kHandleIndices[3] = {0, 1, 2};

    bool active_ = false;
    std::vector<Item> items_;
    std::shared_ptr<Shared> shared_;
    std::shared_ptr<LoadJob> committing_;
    float loadRadius_ = 1000.0f;
    uint64_t budget_ = 0, used_ = 0;
    int inFlight_ = 0;
    bool ramCapLogged_ = false;
    tbd::TextureId pores_ = tbd::kNoTexture;
    bool poresTried_ = false, skinMapsUploaded_ = false;
    int64_t loadStart_ = {}, lastFrame_ = {};
    int64_t lastProgress_ = {};
    double pumpBudgetMs_ = kPumpMs, lastInterval_ = 100.0;
    double worstFrame_ = 0, worstPump_ = 0, lastPump_ = 0;
    bool firstFrameLogged_ = false;
    uint32_t frames_ = 0, hitches33_ = 0, hitches100_ = 0;
    bool idleLogged_ = false;
    size_t lastIdleLoaded_ = 0;
    double pumpTextureMs_ = 0, pumpMeshMs_ = 0, pumpPublishMs_ = 0; // this frame's pump, by kind
    std::vector<std::vector<tbd::Vertex>> deadVertices_; // uploaded this frame, freed on a worker
    std::vector<std::vector<uint32_t>> deadIndices_;
    uint32_t pumpTextures_ = 0, pumpMeshes_ = 0;
    std::vector<uint8_t> failedLogged_;
};

// TBD_SHOT2: once the first spot has settled, the test camera travels to the second and streaming follows it.
struct ShotTour {
    float to[3] = {}, speed = 0.0f;
    bool wanted = false, moving = false, finished = false;
    int settledFrames = 0;
    ShotTour() {
        char v[96] = {};
        wanted = tbd::platform::GetEnvA("TBD_SHOT2", v, sizeof(v)) > 0 && sscanf(v, "%f %f %f %f", to, to + 1, to + 2, &speed) == 4;
    }
    bool Done() const { return !wanted || finished; }
    void Advance(float* target, float dt, bool idle) {
        if (!wanted || finished) return;
        if (!moving) {
            if (idle && ++settledFrames > 60) { moving = true; tbd::Log("Streaming: test camera leaves for %.0f %.0f %.0f at %.0f m/s", to[0], to[1], to[2], speed); }
            return;
        }
        const float d[3] = {to[0] - target[0], to[1] - target[1], to[2] - target[2]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float step = speed > 0.0f ? speed * dt : len;
        if (len <= step) { std::copy(to, to + 3, target); finished = true; return; }
        for (int i = 0; i < 3; ++i) target[i] += d[i] / len * step;
    }
};

} // namespace world_streaming

world_streaming::Streamer g_stream;
