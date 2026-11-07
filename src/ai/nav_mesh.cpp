// See nav_mesh.h. The translation unit that includes Recast and Detour for tiles and queries.
#define NOMINMAX
#include "ai/nav_mesh.h"

#include "platform/platform.h"

#include <filesystem>
#undef min
#undef max

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "Recast.h"
#include "ai/ai_world.h"

namespace tbd::ai {
namespace {

constexpr float kBuildRadius = 75.0f;   // tiles whose centre is this close to the focus are built (their collision must have loaded)
constexpr float kUnloadRadius = 150.0f; // and removed past this
constexpr int kMaxBuilds = 3;           // jobs in flight
constexpr uint32_t kCacheVersion = 3;   // bump when the build settings below change
constexpr float kCell = 0.25f, kCellHeight = 0.2f;
constexpr int kMaxPath = 256;

int64_t Key(int x, int z) { return ((int64_t)x << 32) | (uint32_t)z; }

struct Built {
    int64_t key = 0;
    int x = 0, z = 0;
    uint32_t generation = 0;
    unsigned char* data = nullptr; // dtAlloc'd, handed to the Detour mesh
    int size = 0;
    bool fromCache = false, empty = false, failed = false;
};

std::wstring CacheDir() {
    static const std::wstring dir = [] {
        const std::filesystem::path p = tbd::platform::CacheRoot() / L"navcache";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p.wstring();
    }();
    return dir;
}

// Order independent: Jolt hands the triangles over in tree order, which depends on when each body arrived.
uint64_t HashTriangles(const std::vector<float>& v, int tx, int tz) {
    uint64_t sum = 0;
    for (size_t t = 0; t + 9 <= v.size(); t += 9) {
        uint64_t h = 14695981039346656037ull;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v[t]);
        for (size_t i = 0; i < 36; ++i) h = (h ^ p[i]) * 1099511628211ull;
        h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ull; h ^= h >> 32;
        sum += h;
    }
    uint64_t h = sum ^ ((uint64_t)(v.size() / 9) * 0x9e3779b97f4a7c15ull);
    h ^= (uint64_t)(uint32_t)tx * 0xff51afd7ed558ccdull ^ (uint64_t)(uint32_t)tz * 0xc4ceb9fe1a85ec53ull ^ kCacheVersion;
    return h;
}

bool ReadCache(const std::wstring& path, unsigned char*& data, int& size) {
    FILE* f = tbd::platform::OpenFile(path, "rb");
    if (!f) return false;
    uint32_t head[2] = {};
    bool ok = fread(head, sizeof(head), 1, f) == 1 && head[0] == 0x56414e54u && head[1] > 0 && head[1] < (64u << 20);
    if (ok) {
        data = (unsigned char*)dtAlloc((size_t)head[1], DT_ALLOC_PERM);
        ok = data && fread(data, 1, head[1], f) == head[1];
        if (ok) size = (int)head[1]; else if (data) { dtFree(data); data = nullptr; }
    }
    fclose(f);
    return ok;
}

void WriteCache(const std::wstring& path, const unsigned char* data, int size) {
    const std::wstring temp = path + L".tmp";
    FILE* f = tbd::platform::OpenFile(temp, "wb");
    if (!f) return;
    const uint32_t head[2] = {0x56414e54u, (uint32_t)size};
    const bool ok = fwrite(head, sizeof(head), 1, f) == 1 && fwrite(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    std::error_code ec;
    if (ok) std::filesystem::rename(temp, path, ec); // replaces an existing file, like MoveFileExW(REPLACE_EXISTING)
    else std::filesystem::remove(temp, ec);
}

// Rasterizes the triangles (AI space, nine floats each) into one tile's navmesh data. Returns false when there is nothing to walk on.
bool BuildTileData(const std::vector<float>& tris, int tx, int tz, unsigned char*& outData, int& outSize) {
    const int triCount = (int)(tris.size() / 9), vertCount = triCount * 3;
    if (triCount == 0) return false;
    rcContext ctx(false);
    rcConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cs = kCell;
    cfg.ch = kCellHeight;
    cfg.walkableSlopeAngle = kAgentSlope;
    cfg.walkableHeight = (int)std::ceil(kAgentHeight / kCellHeight);
    cfg.walkableClimb = (int)std::floor(kAgentStep / kCellHeight);
    cfg.walkableRadius = (int)std::ceil(kAgentRadius / kCell);
    cfg.maxEdgeLen = (int)(12.0f / kCell);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = rcSqr(6);
    cfg.mergeRegionArea = rcSqr(20);
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = kCell * 6.0f;
    cfg.detailSampleMaxError = kCellHeight;
    cfg.tileSize = (int)std::ceil(kTileSize / kCell);
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    float lo = 1e30f, hi = -1e30f;
    for (size_t i = 1; i < tris.size(); i += 3) { lo = std::min(lo, tris[i]); hi = std::max(hi, tris[i]); }
    const float border = cfg.borderSize * kCell;
    cfg.bmin[0] = tx * kTileSize - border; cfg.bmin[1] = lo - 0.5f; cfg.bmin[2] = tz * kTileSize - border;
    cfg.bmax[0] = (tx + 1) * kTileSize + border; cfg.bmax[1] = hi + 0.5f; cfg.bmax[2] = (tz + 1) * kTileSize + border;

    rcHeightfield* solid = rcAllocHeightfield();
    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    rcContourSet* cset = rcAllocContourSet();
    rcPolyMesh* pmesh = rcAllocPolyMesh();
    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    std::vector<int> indices((size_t)vertCount);
    for (int i = 0; i < vertCount; ++i) indices[(size_t)i] = i;
    std::vector<unsigned char> areas((size_t)triCount, 0);
    bool ok = solid && chf && cset && pmesh && dmesh;
    ok = ok && rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch);
    if (ok) {
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, tris.data(), vertCount, indices.data(), triCount, areas.data());
        ok = rcRasterizeTriangles(&ctx, tris.data(), vertCount, indices.data(), areas.data(), triCount, *solid, cfg.walkableClimb);
    }
    if (ok) {
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);
        ok = rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf) &&
             rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf) && rcBuildDistanceField(&ctx, *chf) &&
             rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea) &&
             rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset);
    }
    bool built = false;
    if (ok && cset->nconts > 0) {
        ok = rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh) && rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh);
        if (ok && pmesh->npolys > 0 && pmesh->npolys < 0xffff) {
            for (int i = 0; i < pmesh->npolys; ++i) {
                if (pmesh->areas[i] == RC_WALKABLE_AREA) pmesh->areas[i] = 0;
                pmesh->flags[i] = 1;
            }
            dtNavMeshCreateParams p;
            std::memset(&p, 0, sizeof(p));
            p.verts = pmesh->verts; p.vertCount = pmesh->nverts;
            p.polys = pmesh->polys; p.polyAreas = pmesh->areas; p.polyFlags = pmesh->flags; p.polyCount = pmesh->npolys; p.nvp = pmesh->nvp;
            p.detailMeshes = dmesh->meshes; p.detailVerts = dmesh->verts; p.detailVertsCount = dmesh->nverts;
            p.detailTris = dmesh->tris; p.detailTriCount = dmesh->ntris;
            p.walkableHeight = kAgentHeight; p.walkableRadius = kAgentRadius; p.walkableClimb = kAgentStep;
            rcVcopy(p.bmin, pmesh->bmin); rcVcopy(p.bmax, pmesh->bmax);
            p.cs = cfg.cs; p.ch = cfg.ch; p.buildBvTree = true;
            p.tileX = tx; p.tileY = tz; p.tileLayer = 0;
            built = dtCreateNavMeshData(&p, &outData, &outSize);
        }
    }
    rcFreeHeightField(solid);
    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);
    rcFreePolyMesh(pmesh);
    rcFreePolyMeshDetail(dmesh);
    return built;
}

double NowMs() {
    return tbd::platform::NowMs();
}

void AtomicMax(std::atomic<double>& a, double v) { double c = a.load(); while (v > c && !a.compare_exchange_weak(c, v)) {} }
void AtomicAdd(std::atomic<double>& a, double v) { double c = a.load(); while (!a.compare_exchange_weak(c, c + v)) {} }

uint32_t g_random = 1;
float RandomFloat() {
    g_random = g_random * 1664525u + 1013904223u;
    return (float)(g_random >> 8) / 16777216.0f;
}

dtQueryFilter MakeFilter() {
    dtQueryFilter f;
    f.setIncludeFlags(1);
    f.setExcludeFlags(0);
    return f;
}

Vec3 V(const float* p) { return {p[0], p[1], p[2]}; }

// The search itself, shared by the main thread and the workers. `nav` must not change meanwhile.
PathResult SearchPath(const dtNavMesh* nav, dtNavMeshQuery* q, Vec3 fromAi, Vec3 toAi) {
    PathResult r;
    const dtQueryFilter filter = MakeFilter();
    const float ext[3] = {2.0f, 4.0f, 2.0f};
    const float from[3] = {fromAi.x, fromAi.y, fromAi.z};
    float to[3] = {toAi.x, toAi.y, toAi.z};
    dtPolyRef startRef = 0, endRef = 0;
    float startPt[3] = {}, endPt[3] = {};
    q->findNearestPoly(from, ext, &filter, &startRef, startPt);
    if (!startRef) return r;
    q->findNearestPoly(to, ext, &filter, &endRef, endPt);
    bool partial = false;
    if (!endRef) { // an unloaded tile, or off the walkable ground: go for the nearest point of a tile that is loaded
        partial = true;
        float best = 1e30f, bx = 0, bz = 0;
        for (int i = 0; i < nav->getMaxTiles(); ++i) {
            const dtMeshTile* tile = nav->getTile(i);
            if (!tile || !tile->header) continue;
            const float x0 = tile->header->bmin[0] + 1.0f, x1 = tile->header->bmax[0] - 1.0f, z0 = tile->header->bmin[2] + 1.0f, z1 = tile->header->bmax[2] - 1.0f;
            const float cx = std::clamp(to[0], x0, x1), cz = std::clamp(to[2], z0, z1);
            const float d = (cx - to[0]) * (cx - to[0]) + (cz - to[2]) * (cz - to[2]);
            if (d < best) { best = d; bx = cx; bz = cz; }
        }
        if (best >= 1e29f) return r;
        const float edge[3] = {bx, fromAi.y, bz};
        const float wide[3] = {8.0f, 400.0f, 8.0f};
        q->findNearestPoly(edge, wide, &filter, &endRef, endPt);
        if (!endRef) { // the tile has walkable ground only elsewhere: aim for the point nearest to the destination among polys around
            const float huge[3] = {kTileSize * 0.5f, 400.0f, kTileSize * 0.5f};
            q->findNearestPoly(edge, huge, &filter, &endRef, endPt);
        }
        if (!endRef) return r;
    }
    dtPolyRef polys[kMaxPath];
    int polyCount = 0;
    q->findPath(startRef, endRef, startPt, endPt, &filter, polys, &polyCount, kMaxPath);
    if (polyCount == 0) return r;
    if (polys[polyCount - 1] != endRef) {
        partial = true;
        float closest[3];
        bool over = false;
        if (dtStatusSucceed(q->closestPointOnPoly(polys[polyCount - 1], endPt, closest, &over))) std::memcpy(endPt, closest, sizeof(closest));
    }
    float straight[kMaxPath * 3];
    unsigned char flags[kMaxPath];
    dtPolyRef refs[kMaxPath];
    int n = 0;
    q->findStraightPath(startPt, endPt, polys, polyCount, straight, flags, refs, &n, kMaxPath, 0);
    for (int i = 0; i < n; ++i) r.points.push_back(FromAi(V(&straight[i * 3])));
    r.found = n > 0;
    r.partial = partial;
    return r;
}

} // namespace

struct NavMesh::Impl {
    enum class State { Waiting, Building, Loaded, Empty, Failed };
    struct Entry { State state = State::Waiting; dtTileRef ref = 0; int retryAt = 0; };

    dtNavMesh* nav = nullptr;
    dtNavMeshQuery* query = nullptr; // main thread
    std::shared_mutex lock;          // writers: tile add / remove on the main thread; readers: async path jobs
    std::unordered_map<int64_t, Entry> tiles;
    std::mutex doneMutex;
    std::vector<Built> done;
    std::atomic<uint32_t> generation{1};
    int inflight = 0;
    int frame = 0;
    bool settled = false;

    std::mutex pathMutex;
    std::unordered_map<uint32_t, PathResult> ready;
    std::atomic<uint32_t> nextTicket{1};

    uint32_t reachableVersion = 0;
    size_t reachableSeeds = 0;
    std::unordered_set<dtPolyRef> reachable;
};

NavMesh::NavMesh() : impl_(std::make_unique<Impl>()) {}
NavMesh::~NavMesh() {
    Reset();
    dtFreeNavMeshQuery(impl_->query);
    dtFreeNavMesh(impl_->nav);
}

void NavMesh::Reset() {
    Impl& w = *impl_;
    std::unique_lock<std::shared_mutex> lock(w.lock);
    w.generation.fetch_add(1);
    {
        std::lock_guard<std::mutex> g(w.doneMutex);
        for (Built& b : w.done) if (b.data) dtFree(b.data);
        w.done.clear();
    }
    w.tiles.clear();
    w.inflight = 0; // jobs from the old generation drop their result when they finish
    w.reachable.clear();
    w.reachableVersion = 0;
    if (w.query) dtFreeNavMeshQuery(w.query);
    if (w.nav) dtFreeNavMesh(w.nav);
    w.nav = dtAllocNavMesh();
    dtNavMeshParams params;
    std::memset(&params, 0, sizeof(params));
    params.tileWidth = params.tileHeight = kTileSize;
    params.maxTiles = 256;
    params.maxPolys = 16384;
    w.nav->init(&params);
    w.query = dtAllocNavMeshQuery();
    w.query->init(w.nav, 4096);
    version_.fetch_add(1);
}

dtNavMesh* NavMesh::Raw() const { return impl_->nav; }
size_t NavMesh::TileCount() const {
    size_t n = 0;
    for (const auto& [key, e] : impl_->tiles) n += e.state == Impl::State::Loaded;
    return n;
}
bool NavMesh::Settled() const { return impl_->settled; }

bool NavMesh::HasTileAt(Vec3 world) const {
    const Vec3 a = ToAi(world);
    const auto it = impl_->tiles.find(Key((int)std::floor(a.x / kTileSize), (int)std::floor(a.z / kTileSize)));
    return it != impl_->tiles.end() && it->second.state == Impl::State::Loaded;
}

void NavMesh::Update(PhysicsWorld& physics, JobSystem& jobs, Vec3 focusWorld) {
    Impl& w = *impl_;
    if (!w.nav) Reset();
    ++w.frame;
    const Vec3 focus = ToAi(focusWorld);

    // Finished tiles into the Detour mesh.
    std::vector<Built> finished;
    {
        std::lock_guard<std::mutex> g(w.doneMutex);
        finished.swap(w.done);
    }
    for (Built& b : finished) {
        if (b.generation != w.generation.load()) { if (b.data) dtFree(b.data); continue; }
        --w.inflight;
        Impl::Entry& e = w.tiles[b.key];
        if (b.failed) { e.state = Impl::State::Failed; continue; }
        if (b.empty) { e.state = Impl::State::Empty; continue; }
        std::unique_lock<std::shared_mutex> lock(w.lock);
        dtTileRef ref = 0;
        if (dtStatusFailed(w.nav->addTile(b.data, b.size, DT_TILE_FREE_DATA, 0, &ref))) {
            dtFree(b.data);
            e.state = Impl::State::Failed;
            Log("Nav: tile %d %d was refused by Detour", b.x, b.z);
            continue;
        }
        e.state = Impl::State::Loaded;
        e.ref = ref;
        version_.fetch_add(1);
    }

    // Far tiles out.
    for (auto it = w.tiles.begin(); it != w.tiles.end();) {
        const int x = (int)(it->first >> 32), z = (int)(uint32_t)it->first;
        const float dx = (x + 0.5f) * kTileSize - focus.x, dz = (z + 0.5f) * kTileSize - focus.z;
        if (it->second.state != Impl::State::Building && std::sqrt(dx * dx + dz * dz) > kUnloadRadius) {
            if (it->second.state == Impl::State::Loaded) {
                std::unique_lock<std::shared_mutex> lock(w.lock);
                w.nav->removeTile(it->second.ref, nullptr, nullptr);
                version_.fetch_add(1);
            }
            it = w.tiles.erase(it);
        } else ++it;
    }

    // New tiles: nearest first, one collision check and one triangle gather per frame.
    const int span = (int)std::ceil(kBuildRadius / kTileSize) + 1;
    const int cx = (int)std::floor(focus.x / kTileSize), cz = (int)std::floor(focus.z / kTileSize);
    struct Candidate { int x, z; float d; };
    std::vector<Candidate> want;
    for (int z = cz - span; z <= cz + span; ++z)
        for (int x = cx - span; x <= cx + span; ++x) {
            const float dx = (x + 0.5f) * kTileSize - focus.x, dz = (z + 0.5f) * kTileSize - focus.z;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d <= kBuildRadius) want.push_back({x, z, d});
        }
    std::sort(want.begin(), want.end(), [](const Candidate& a, const Candidate& b) { return a.d < b.d; });
    bool anyWaiting = false;
    int gathered = 0;
    for (const Candidate& c : want) {
        Impl::Entry& e = w.tiles[Key(c.x, c.z)];
        if (e.state != Impl::State::Waiting) continue;
        anyWaiting = true;
        if (gathered >= 1 || w.inflight >= kMaxBuilds || e.retryAt > w.frame) continue;
        const Vec3 centreAi = {(c.x + 0.5f) * kTileSize, 0, (c.z + 0.5f) * kTileSize};
        const Vec3 centre = FromAi(centreAi);
        if (!physics.ReadyAround(centre, kTileSize * 0.72f + 3.0f)) { e.retryAt = w.frame + 20; continue; }
        ++gathered;
        const float margin = 3.0f;
        const Vec3 minAi = {c.x * kTileSize - margin, -5000.0f, c.z * kTileSize - margin};
        const Vec3 maxAi = {(c.x + 1) * kTileSize + margin, 5000.0f, (c.z + 1) * kTileSize + margin};
        auto tris = std::make_shared<std::vector<float>>();
        physics.CollectStaticTriangles(FromAi(minAi), FromAi(maxAi), *tris);
        const Vec3 off = WorldOffset();
        if (off.x != 0.0f || off.y != 0.0f || off.z != 0.0f)
            for (size_t i = 0; i + 2 < tris->size(); i += 3) { (*tris)[i] -= off.x; (*tris)[i + 1] -= off.y; (*tris)[i + 2] -= off.z; }
        e.state = Impl::State::Building;
        ++w.inflight;
        NavMesh* self = this;
        Impl* impl = &w;
        const uint32_t generation = w.generation.load();
        const int tx = c.x, tz = c.z;
        jobs.RunBackground([self, impl, tris, tx, tz, generation] {
            Built b;
            b.key = Key(tx, tz);
            b.x = tx; b.z = tz;
            b.generation = generation;
            self->stats_.buildJobsRunning.fetch_add(1);
            const double t0 = NowMs();
            try {
                if (tris->empty()) b.empty = true;
                else {
                    char name[40];
                    snprintf(name, sizeof(name), "\\%016llx.nav", (unsigned long long)HashTriangles(*tris, tx, tz));
                    const std::wstring path = CacheDir() + std::wstring(name, name + strlen(name));
                    if (ReadCache(path, b.data, b.size)) {
                        b.fromCache = true;
                        self->stats_.tilesFromCache.fetch_add(1);
                        AtomicAdd(self->stats_.cacheMsTotal, NowMs() - t0);
                    } else if (BuildTileData(*tris, tx, tz, b.data, b.size)) {
                        WriteCache(path, b.data, b.size);
                        const double ms = NowMs() - t0;
                        self->stats_.tilesBuilt.fetch_add(1);
                        AtomicAdd(self->stats_.buildMsTotal, ms);
                        AtomicMax(self->stats_.buildMsMax, ms);
                        Log("Nav: tile %d %d built in %.0f ms (%zu triangles, %d bytes)", tx, tz, ms, tris->size() / 9, b.size);
                    } else {
                        b.empty = true; // triangles, but nothing walkable
                        self->stats_.tilesEmpty.fetch_add(1);
                    }
                }
            } catch (...) { b.failed = true; self->stats_.tilesFailed.fetch_add(1); }
            self->stats_.buildJobsRunning.fetch_sub(1);
            std::lock_guard<std::mutex> g(impl->doneMutex);
            impl->done.push_back(b);
        });
    }
    w.settled = w.inflight == 0 && !anyWaiting;
}

PathResult NavMesh::FindPath(Vec3 from, Vec3 to) const {
    Impl& w = *impl_;
    stats_.pathRequests.fetch_add(1);
    PathResult r;
    if (w.nav && w.query) r = SearchPath(w.nav, w.query, ToAi(from), ToAi(to));
    r.goal = to;
    if (!r.found) stats_.pathFailures.fetch_add(1);
    else if (r.partial) stats_.pathPartial.fetch_add(1);
    return r;
}

bool NavMesh::Raycast(Vec3 from, Vec3 to, Vec3& hit) const {
    Impl& w = *impl_;
    hit = to;
    if (!w.nav) return false;
    const dtQueryFilter filter = MakeFilter();
    const float ext[3] = {2.0f, 4.0f, 2.0f};
    const Vec3 a = ToAi(from), b = ToAi(to);
    const float fa[3] = {a.x, a.y, a.z}, fb[3] = {b.x, b.y, b.z};
    dtPolyRef ref = 0;
    float nearest[3];
    w.query->findNearestPoly(fa, ext, &filter, &ref, nearest);
    if (!ref) return false;
    float t = 0, normal[3];
    dtPolyRef path[16];
    int n = 0;
    w.query->raycast(ref, nearest, fb, &filter, &t, normal, path, &n, 16);
    if (t >= 1.0f) return false;
    hit = FromAi({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t});
    return true;
}

bool NavMesh::NearestPoint(Vec3 p, Vec3 halfExtents, Vec3& out) const {
    Impl& w = *impl_;
    if (!w.nav) return false;
    const dtQueryFilter filter = MakeFilter();
    const Vec3 a = ToAi(p);
    const float fa[3] = {a.x, a.y, a.z}, ext[3] = {halfExtents.x, halfExtents.y, halfExtents.z};
    dtPolyRef ref = 0;
    float nearest[3];
    w.query->findNearestPoly(fa, ext, &filter, &ref, nearest);
    if (!ref) return false;
    out = FromAi(V(nearest));
    return true;
}

bool NavMesh::RandomPointAround(Vec3 from, float radius, Vec3& out, uint32_t seed) const {
    Impl& w = *impl_;
    if (!w.nav) return false;
    const dtQueryFilter filter = MakeFilter();
    const Vec3 a = ToAi(from);
    const float fa[3] = {a.x, a.y, a.z}, ext[3] = {2.0f, 4.0f, 2.0f};
    dtPolyRef ref = 0, picked = 0;
    float nearest[3], point[3];
    w.query->findNearestPoly(fa, ext, &filter, &ref, nearest);
    if (!ref) return false;
    g_random = seed * 2654435761u + 12345u;
    if (dtStatusFailed(w.query->findRandomPointAroundCircle(ref, nearest, radius, &filter, RandomFloat, &picked, point)) || !picked) return false;
    out = FromAi(V(point));
    return true;
}

uint32_t NavMesh::FindPathAsync(JobSystem& jobs, Vec3 from, Vec3 to) {
    Impl* impl = impl_.get();
    NavMesh* self = this;
    const uint32_t ticket = impl->nextTicket.fetch_add(1);
    const uint32_t generation = impl->generation.load();
    stats_.pathRequests.fetch_add(1);
    jobs.Run([impl, self, ticket, generation, from, to] {
        PathResult r;
        {
            std::shared_lock<std::shared_mutex> lock(impl->lock);
            if (impl->generation.load() == generation && impl->nav) {
                thread_local dtNavMeshQuery* query = nullptr; // one per worker, never freed
                thread_local uint32_t queryGeneration = 0;
                if (!query) query = dtAllocNavMeshQuery();
                if (queryGeneration != generation) { query->init(impl->nav, 2048); queryGeneration = generation; }
                r = SearchPath(impl->nav, query, ToAi(from), ToAi(to));
            }
        }
        r.goal = to;
        if (!r.found) self->stats_.pathFailures.fetch_add(1);
        else if (r.partial) self->stats_.pathPartial.fetch_add(1);
        std::lock_guard<std::mutex> g(impl->pathMutex);
        if (impl->generation.load() == generation) impl->ready[ticket] = std::move(r);
    });
    return ticket;
}

bool NavMesh::TakePath(uint32_t ticket, PathResult& out) {
    Impl& w = *impl_;
    std::lock_guard<std::mutex> g(w.pathMutex);
    const auto it = w.ready.find(ticket);
    if (it == w.ready.end()) return false;
    out = std::move(it->second);
    w.ready.erase(it);
    return true;
}

void NavMesh::DebugGeometry(Vec3 aroundWorld, float radius, const std::vector<Vec3>& seedsWorld, std::vector<Vertex>& triangles, std::vector<Vertex>& lines) const {
    Impl& w = *impl_;
    if (!w.nav || !w.query) return;
    const Vec3 around = ToAi(aroundWorld);
    // Which polygons can be walked to from the seeds: a flood across the polygon links, redone when tiles change.
    if (w.reachableVersion != Version() || w.reachableSeeds != seedsWorld.size()) {
        w.reachableVersion = Version();
        w.reachableSeeds = seedsWorld.size();
        w.reachable.clear();
        const dtQueryFilter filter = MakeFilter();
        const float ext[3] = {2.0f, 4.0f, 2.0f};
        std::vector<dtPolyRef> stack;
        for (Vec3 s : seedsWorld) {
            const Vec3 a = ToAi(s);
            const float fa[3] = {a.x, a.y, a.z};
            dtPolyRef ref = 0;
            float nearest[3];
            w.query->findNearestPoly(fa, ext, &filter, &ref, nearest);
            if (ref && w.reachable.insert(ref).second) stack.push_back(ref);
        }
        while (!stack.empty()) {
            const dtPolyRef ref = stack.back();
            stack.pop_back();
            const dtMeshTile* tile = nullptr;
            const dtPoly* poly = nullptr;
            if (dtStatusFailed(w.nav->getTileAndPolyByRef(ref, &tile, &poly))) continue;
            for (unsigned int l = poly->firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
                const dtPolyRef next = tile->links[l].ref;
                if (next && w.reachable.insert(next).second) stack.push_back(next);
            }
        }
    }
    const float lift = 0.12f;
    for (int ti = 0; ti < w.nav->getMaxTiles(); ++ti) {
        const dtMeshTile* tile = static_cast<const dtNavMesh*>(w.nav)->getTile(ti);
        if (!tile || !tile->header) continue;
        const float cx = (tile->header->bmin[0] + tile->header->bmax[0]) * 0.5f, cz = (tile->header->bmin[2] + tile->header->bmax[2]) * 0.5f;
        if (std::fabs(cx - around.x) > radius + kTileSize * 0.5f || std::fabs(cz - around.z) > radius + kTileSize * 0.5f) continue;
        const dtPolyRef base = w.nav->getPolyRefBase(tile);
        for (int i = 0; i < tile->header->polyCount; ++i) {
            const dtPoly& poly = tile->polys[i];
            if (poly.getType() != DT_POLYTYPE_GROUND) continue;
            const bool good = w.reachable.empty() || w.reachable.count(base | (dtPolyRef)i) != 0;
            const Vec3 fill = good ? Vec3{0.10f, 0.62f, 0.32f} : Vec3{0.55f, 0.12f, 0.12f};
            const Vec3 edge = good ? Vec3{0.75f, 0.95f, 0.80f} : Vec3{0.85f, 0.45f, 0.45f};
            const dtPolyDetail& pd = tile->detailMeshes[i];
            auto vert = [&](int idx) {
                const float* p = idx < poly.vertCount ? &tile->verts[poly.verts[idx] * 3] : &tile->detailVerts[(pd.vertBase + idx - poly.vertCount) * 3];
                return FromAi({p[0], p[1] + lift, p[2]});
            };
            const Vec3 first = vert(0);
            if (std::fabs(first.x - aroundWorld.x) > radius || std::fabs(first.z - aroundWorld.z) > radius) continue;
            for (int t = 0; t < pd.triCount; ++t) {
                const unsigned char* tri = &tile->detailTris[(pd.triBase + t) * 4];
                for (int k = 0; k < 3; ++k) {
                    const Vec3 p = vert(tri[k]);
                    triangles.push_back({p.x, p.y, p.z, 0, 0, 0, fill.x, fill.y, fill.z, 1.0f});
                }
            }
            for (int e = 0; e < poly.vertCount; ++e) {
                const Vec3 a = vert(e), b = vert((e + 1) % poly.vertCount);
                const Vec3 up = {0, 0.02f, 0};
                lines.push_back({a.x, a.y + up.y, a.z, 0, 0, 0, edge.x, edge.y, edge.z, 1.0f});
                lines.push_back({b.x, b.y + up.y, b.z, 0, 0, 0, edge.x, edge.y, edge.z, 1.0f});
            }
        }
    }
}

} // namespace tbd::ai
