// See physics.h. The one translation unit that includes Jolt.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "platform/platform.h"
#undef min
#undef max

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

#include "culling.h"
#include "glb_bounds.h"
#include "physics.h"
#include "floating_origin.h"
#include "scene.h"

namespace tbd {
namespace {

namespace Layers { constexpr JPH::ObjectLayer Static = 0, Moving = 1; }
namespace BroadLayers { const JPH::BroadPhaseLayer Static(0), Moving(1); }

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return layer == Layers::Static ? BroadLayers::Static : BroadLayers::Moving; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "layer"; }
#endif
};
class ObjectVsBroad final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override { return a == Layers::Moving || b == BroadLayers::Moving; }
};
class ObjectVsObject final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == Layers::Moving || b == Layers::Moving; }
};

// Jolt's jobs on the engine's workers, following Jolt's own JobSystemThreadPool. The caller also helps at the barrier.
class EngineJobs final : public JPH::JobSystemWithBarrier {
public:
    explicit EngineJobs(tbd::JobSystem& jobs) : JobSystemWithBarrier(8), jobs_(jobs) { free_.Init(2048, 2048); }
    int GetMaxConcurrency() const override { return (int)jobs_.WorkerCount() + 1; }
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& fn, JPH::uint32 dependencies) override {
        JPH::uint32 index;
        while ((index = free_.ConstructObject(name, color, this, fn, dependencies)) == Free::cInvalidObjectIndex) std::this_thread::yield();
        Job* job = &free_.Get(index);
        JobHandle handle(job);
        if (dependencies == 0) QueueJob(job);
        return handle;
    }
protected:
    void QueueJob(Job* job) override {
        job->AddRef();
        jobs_.Run([job] { job->Execute(); job->Release(); });
    }
    void QueueJobs(Job** jobs, JPH::uint count) override { for (JPH::uint i = 0; i < count; ++i) QueueJob(jobs[i]); }
    void FreeJob(Job* job) override { free_.DestructObject(job); }
private:
    using Free = JPH::FixedSizeFreeList<Job>;
    tbd::JobSystem& jobs_;
    Free free_;
};

inline JPH::Vec3 J(Vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
inline JPH::Quat J(Quat q) { return JPH::Quat(q.x, q.y, q.z, q.w).Normalized(); }
inline Vec3 T(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
inline float Len(Vec3 v) { return std::sqrt(Dot(v, v)); }

enum class Collision { None, Simple, Complex };
enum AssetState { Idle, Building, Ready, Failed };

// One shape, shared by every placement of the file. A tile's triangles stay in memory, so its shape can be dropped and remade.
struct Asset {
    std::string path;
    Collision kind = Collision::Complex;
    std::atomic<int> state{Idle};
    JPH::RefConst<JPH::Shape> shape;
    std::vector<JPH::Float3> tile; // three per triangle, relative to the tile's centre; empty for a file
    int users = 0;
};

struct Item {
    std::shared_ptr<Asset> asset;
    Vec3 position = {0, 0, 0};
    Quat rotation;
    Vec3 scale = {1, 1, 1};
    Sphere world;
    bool big = false;     // a whole-map file not yet split into tiles
    bool started = false; // big: the split job is running or done
    bool failed = false;
    JPH::BodyID body;
};

constexpr float kLoadRadius = 120.0f, kUnloadRadius = 170.0f; // meters from the focus to the piece's bounding sphere
constexpr float kTileSize = 64.0f;
constexpr float kBigRadius = 150.0f;  // a file larger than this is split into tiles
constexpr float kMinInstance = 0.35f; // scattered pieces smaller than this (world radius) are walked over: pebbles
constexpr int kMaxBuilding = 4;
constexpr int kAddsPerFrame = 48;

bool EndsWithNoCase(const std::string& s, const std::string& tail) {
    if (s.size() < tail.size()) return false;
    for (size_t i = 0; i < tail.size(); ++i)
        if (std::tolower((unsigned char)s[s.size() - tail.size() + i]) != std::tolower((unsigned char)tail[i])) return false;
    return true;
}

std::string Normal(std::string s) {
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

// Reads `"file": "...", ... "collision": "..."` pairs from layout.json's assets section with a plain scan: the file also
// holds 150,000 instance rows that would be slow to parse and are not wanted here.
std::vector<std::pair<std::string, Collision>> ReadLayout(const std::string& path) {
    std::vector<std::pair<std::string, Collision>> out;
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t begin = text.find("\"assets\""), end = text.find("\"placements\"");
    if (begin == std::string::npos) return out;
    auto value = [&](size_t at, std::string& v) {
        at = text.find(':', at);
        const size_t a = text.find('"', at), b = a == std::string::npos ? a : text.find('"', a + 1);
        if (b == std::string::npos) return false;
        v = text.substr(a + 1, b - a - 1);
        return true;
    };
    for (size_t at = begin; (at = text.find("\"file\"", at)) != std::string::npos && at < end; at += 6) {
        const size_t c = text.find("\"collision\"", at), next = text.find("\"file\"", at + 6);
        std::string file, collision;
        if (c == std::string::npos || (next != std::string::npos && c > next)) continue;
        if (value(at, file) && value(c, collision))
            out.push_back({Normal(file), collision == "none" ? Collision::None : collision == "simple" ? Collision::Simple : Collision::Complex});
    }
    return out;
}

// When the layout has nothing to say.
Collision GuessCollision(const std::string& path) {
    if (path.find("/Vegetation/") != std::string::npos || path.find("map_water") != std::string::npos) return Collision::None;
    if (path.find("/Rocks/") != std::string::npos || path.find("/Props/") != std::string::npos || path.find("/Doors/") != std::string::npos) return Collision::Simple;
    return Collision::Complex;
}

std::string GlbPath(const std::string& meshPath) { return meshPath.substr(0, meshPath.rfind("#native")); }

JPH::RefConst<JPH::Shape> BuildShape(Asset& asset, PhysicsWorld::LoadFn load) {
    JPH::TriangleList triangles;
    if (!asset.tile.empty()) {
        triangles.reserve(asset.tile.size() / 3);
        for (size_t t = 0; t + 2 < asset.tile.size(); t += 3) triangles.push_back(JPH::Triangle(asset.tile[t], asset.tile[t + 1], asset.tile[t + 2]));
    } else {
        std::vector<Vertex> v;
        if (!load(asset.path, v) || v.size() < 3) return nullptr;
        if (asset.kind == Collision::Simple) {
            JPH::Array<JPH::Vec3> points;
            const size_t step = std::max<size_t>(1, v.size() / 4096); // the hull needs only the outline
            points.reserve(v.size() / step + 1);
            Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
            for (size_t i = 0; i < v.size(); ++i) {
                lo = {std::min(lo.x, v[i].px), std::min(lo.y, v[i].py), std::min(lo.z, v[i].pz)};
                hi = {std::max(hi.x, v[i].px), std::max(hi.y, v[i].py), std::max(hi.z, v[i].pz)};
                if (i % step == 0) points.push_back(JPH::Vec3(v[i].px, v[i].py, v[i].pz));
            }
            JPH::ConvexHullShapeSettings settings(points, 0.02f);
            JPH::ShapeSettings::ShapeResult hull = settings.Create();
            if (hull.IsValid()) return hull.Get();
            const Vec3 half = {std::max((hi.x - lo.x) * 0.5f, 0.03f), std::max((hi.y - lo.y) * 0.5f, 0.03f), std::max((hi.z - lo.z) * 0.5f, 0.03f)};
            JPH::Ref<JPH::Shape> box = new JPH::BoxShape(J(half), 0.0f); // flat or broken outline: its box, about its own centre
            JPH::RotatedTranslatedShapeSettings moved(JPH::Vec3((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f), JPH::Quat::sIdentity(), box);
            JPH::ShapeSettings::ShapeResult r = moved.Create();
            return r.IsValid() ? r.Get() : nullptr;
        }
        triangles.reserve(v.size() / 3);
        for (size_t t = 0; t + 2 < v.size(); t += 3)
            triangles.push_back(JPH::Triangle(JPH::Float3(v[t].px, v[t].py, v[t].pz), JPH::Float3(v[t + 1].px, v[t + 1].py, v[t + 1].pz), JPH::Float3(v[t + 2].px, v[t + 2].py, v[t + 2].pz)));
    }
    JPH::MeshShapeSettings settings(std::move(triangles));
    JPH::ShapeSettings::ShapeResult r = settings.Create();
    if (!r.IsValid()) { Log("Physics: no mesh shape for %s: %s", asset.path.c_str(), r.GetError().c_str()); return nullptr; }
    return r.Get();
}

void EnsureJolt() {
    static const bool once = [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory(); // never deleted: background jobs may still be finishing at exit
        JPH::RegisterTypes();
        return true;
    }();
    (void)once;
}

// What jobs touch. They hold a reference, so a new scene just abandons the old one to them.
struct StreamShared {
    std::mutex mutex;
    std::vector<Item> arrived; // from the setup and split jobs
    std::atomic<int> building{0};
    std::atomic<int> pendingSetup{1};
};

// Worker: reads the whole file, moves it to world space, and sorts its triangles into tiles by their centres.
void SplitBig(const Item& big, const std::shared_ptr<StreamShared>& s, PhysicsWorld::LoadFn load) {
    std::vector<Item> tiles;
    try {
        std::vector<Vertex> v;
        if (load(big.asset->path, v)) {
            const Mat4 model = TRS(big.position, big.rotation, big.scale);
            std::unordered_map<uint64_t, std::vector<Vec3>> cells;
            for (size_t t = 0; t + 2 < v.size(); t += 3) {
                Vec3 p[3];
                for (int k = 0; k < 3; ++k) {
                    const Vec4 w = model * Vec4{v[t + k].px, v[t + k].py, v[t + k].pz, 1.0f};
                    p[k] = {w.x, w.y, w.z};
                }
                const int cx = (int)std::floor((p[0].x + p[1].x + p[2].x) / 3.0f / kTileSize), cz = (int)std::floor((p[0].z + p[1].z + p[2].z) / 3.0f / kTileSize);
                std::vector<Vec3>& cell = cells[((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz];
                cell.insert(cell.end(), p, p + 3);
            }
            tiles.reserve(cells.size());
            for (auto& [key, tri] : cells) {
                Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
                for (const Vec3& p : tri) {
                    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                }
                const Vec3 centre = (lo + hi) * 0.5f;
                Item item;
                item.asset = std::make_shared<Asset>();
                item.asset->path = big.asset->path;
                item.asset->tile.reserve(tri.size());
                for (const Vec3& p : tri) item.asset->tile.push_back(JPH::Float3(p.x - centre.x, p.y - centre.y, p.z - centre.z));
                item.position = centre;
                item.world.center = centre;
                item.world.radius = Len(hi - centre);
                tiles.push_back(std::move(item));
            }
            Log("Physics: %s split into %zu tiles", big.asset->path.c_str(), tiles.size());
        } else Log("Physics: cannot read %s", big.asset->path.c_str());
    } catch (...) { Log("Physics: failed to split %s", big.asset->path.c_str()); }
    std::lock_guard<std::mutex> lock(s->mutex);
    for (Item& tile : tiles) s->arrived.push_back(std::move(tile));
    s->pendingSetup.fetch_sub(1, std::memory_order_acq_rel);
}

struct Placement { std::string path; Vec3 p; Quat r; Vec3 s; bool instance; };
struct InstanceFile { std::string path; std::wstring file; };

// Worker: the layout, every file's bounds, the rock rows. Produces the item list; reads no geometry.
void SetUp(const std::vector<Placement>& placements, const std::vector<InstanceFile>& files, const std::shared_ptr<StreamShared>& s) {
    std::vector<Item> ready;
    try {
        std::map<std::string, std::vector<std::pair<std::string, Collision>>> layouts; // by Export folder
        auto kindOf = [&](const std::string& meshPath) {
            const std::string path = Normal(GlbPath(meshPath));
            if (path.find("/Doors/") != std::string::npos) return Collision::None; // the game swings doors open (game\game.cpp)
            const size_t cut = path.find("/Export/");
            if (cut != std::string::npos) {
                const std::string root = path.substr(0, cut + 8);
                if (!layouts.count(root)) layouts[root] = ReadLayout(root + "layout.json");
                for (const auto& [file, kind] : layouts[root])
                    if (EndsWithNoCase(path, "/" + file)) return kind;
            }
            return GuessCollision(path);
        };
        std::unordered_map<std::string, std::shared_ptr<Asset>> assets;
        struct Bounds { Vec3 lo, hi; bool ok; };
        std::unordered_map<std::string, Bounds> bounds;
        auto boundsOf = [&](const std::string& meshPath) -> const Bounds& {
            auto it = bounds.find(meshPath);
            if (it != bounds.end()) return it->second;
            Bounds b{};
            b.ok = ReadGlbBounds(GlbPath(meshPath), b.lo, b.hi);
            return bounds[meshPath] = b;
        };
        auto assetOf = [&](const std::string& path) -> std::shared_ptr<Asset>& {
            std::shared_ptr<Asset>& a = assets[path];
            if (!a) { a = std::make_shared<Asset>(); a->path = path; a->kind = kindOf(path); }
            return a;
        };
        auto place = [&](const Placement& p) {
            std::shared_ptr<Asset>& asset = assetOf(p.path);
            if (asset->kind == Collision::None) return;
            const Bounds& b = boundsOf(p.path);
            if (!b.ok) return;
            Item item;
            item.position = p.p;
            item.rotation = p.r;
            item.scale = p.s;
            const Vec3 half = (b.hi - b.lo) * 0.5f, centre = (b.hi + b.lo) * 0.5f;
            item.world = TransformSphere(Sphere{centre, Len(half)}, TRS(p.p, p.r, p.s));
            if (p.instance && item.world.radius < kMinInstance) return;
            if (item.world.radius > kBigRadius && asset->kind == Collision::Complex) {
                item.big = true;
                item.asset = std::make_shared<Asset>(); // a private one: the file's triangles become tiles
                item.asset->path = asset->path;
            } else item.asset = asset;
            ready.push_back(std::move(item));
        };
        for (const Placement& p : placements) place(p);
        for (const InstanceFile& f : files) {
            if (assetOf(f.path)->kind == Collision::None) continue;
            std::ifstream in(f.file, std::ios::binary);
            if (!in) continue;
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const size_t count = bytes.size() / (12 * sizeof(float));
            for (size_t i = 0; i < count; ++i) {
                float r[12];
                std::memcpy(r, bytes.data() + i * sizeof(r), sizeof(r));
                const Vec3 col[3] = {{r[0], r[4], r[8]}, {r[1], r[5], r[9]}, {r[2], r[6], r[10]}};
                Placement p;
                p.path = f.path;
                p.instance = true;
                p.p = {r[3], r[7], r[11]};
                p.s = {Len(col[0]), Len(col[1]), Len(col[2])};
                if (p.s.x < 1e-6f || p.s.y < 1e-6f || p.s.z < 1e-6f) continue;
                const JPH::Mat44 m(JPH::Vec4(col[0].x / p.s.x, col[0].y / p.s.x, col[0].z / p.s.x, 0), JPH::Vec4(col[1].x / p.s.y, col[1].y / p.s.y, col[1].z / p.s.y, 0),
                                   JPH::Vec4(col[2].x / p.s.z, col[2].y / p.s.z, col[2].z / p.s.z, 0), JPH::Vec4(0, 0, 0, 1));
                const JPH::Quat q = m.GetQuaternion().Normalized();
                p.r.x = q.GetX(); p.r.y = q.GetY(); p.r.z = q.GetZ(); p.r.w = q.GetW();
                place(p);
            }
        }
        Log("Physics: %zu collision pieces from %zu files", ready.size(), assets.size());
    } catch (...) { Log("Physics: reading the scene's collision failed"); }
    std::lock_guard<std::mutex> lock(s->mutex);
    for (Item& item : ready) s->arrived.push_back(std::move(item));
    s->pendingSetup.fetch_sub(1, std::memory_order_acq_rel);
}

constexpr float kCapsuleRadius = 0.3f, kCapsuleHeight = 1.8f;

} // namespace

struct PhysicsWorld::Impl {
    BroadPhaseLayers broad;
    ObjectVsBroad objectVsBroad;
    ObjectVsObject objectVsObject;
    std::unique_ptr<JPH::PhysicsSystem> system;
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<EngineJobs> jolt;
    JobSystem* jobs = nullptr;
    LoadFn load = nullptr;
    std::shared_ptr<StreamShared> shared;
    std::vector<Item> items;
    WorldPosition origin; // scene-space offset; worker results remain in scene coordinates
    bool broadPhaseDirty = false;
    Vec3 lastFocus = {1e30f, 0, 0};
    size_t cursor = 0;
    bool scanAll = true;
    JPH::Ref<JPH::CharacterVirtual> character;
    Vec3 lastGround = {0, 0, 0};
    bool hasLastGround = false;
    std::vector<JPH::BodyID> balls;
    std::unordered_map<uint32_t, JPH::BodyID> kinematic; // AI agents' capsules by id
    uint32_t nextKinematic = 0;
    std::unique_ptr<RagdollWorld> ragdolls; // after `system`, so it goes first
    PhysicsStats stats;
    size_t bodies = 0;

    void Clear() {
        if (!system) return;
        JPH::BodyInterface& bi = system->GetBodyInterfaceNoLock();
        for (Item& item : items)
            if (!item.body.IsInvalid()) { bi.RemoveBody(item.body); bi.DestroyBody(item.body); }
        for (JPH::BodyID id : balls) { bi.RemoveBody(id); bi.DestroyBody(id); }
        balls.clear();
        for (auto& [id, body] : kinematic) { bi.RemoveBody(body); bi.DestroyBody(body); }
        kinematic.clear();
        if (ragdolls) ragdolls->Clear();
        items.clear();
        bodies = 0;
    }

    void Add(Item& item) {
        JPH::RefConst<JPH::Shape> shape = item.asset->shape;
        const bool scaled = std::fabs(item.scale.x - 1.0f) + std::fabs(item.scale.y - 1.0f) + std::fabs(item.scale.z - 1.0f) > 1e-4f;
        if (scaled) shape = new JPH::ScaledShape(shape, J(item.scale));
        JPH::BodyCreationSettings settings(shape, J(ToLocal(origin, {item.position.x, item.position.y, item.position.z})), J(item.rotation), JPH::EMotionType::Static, Layers::Static);
        item.body = system->GetBodyInterfaceNoLock().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (item.body.IsInvalid()) { item.failed = true; Log("Physics: out of bodies"); return; }
        ++item.asset->users;
        ++bodies;
        broadPhaseDirty = true;
    }

    void Remove(Item& item) {
        JPH::BodyInterface& bi = system->GetBodyInterfaceNoLock();
        bi.RemoveBody(item.body);
        bi.DestroyBody(item.body);
        item.body = JPH::BodyID();
        --bodies;
        if (--item.asset->users == 0 && !item.asset->tile.empty()) { // the triangles stay: the shape is remade on the way back
            item.asset->shape = nullptr;
            item.asset->state.store(Idle, std::memory_order_release);
        }
    }

    void StartBuild(const std::shared_ptr<Asset>& asset) {
        asset->state.store(Building, std::memory_order_release);
        shared->building.fetch_add(1, std::memory_order_relaxed);
        std::shared_ptr<StreamShared> s = shared;
        const LoadFn loadFn = load;
        jobs->RunBackground([asset, s, loadFn] {
            JPH::RefConst<JPH::Shape> shape;
            try { shape = BuildShape(*asset, loadFn); } catch (...) {}
            asset->shape = shape;
            asset->state.store(shape ? Ready : Failed, std::memory_order_release);
            s->building.fetch_sub(1, std::memory_order_relaxed);
        });
    }
};

PhysicsWorld::PhysicsWorld() : impl_(std::make_unique<Impl>()) {}
PhysicsWorld::~PhysicsWorld() { if (impl_) { impl_->character = nullptr; impl_->Clear(); } }

void PhysicsWorld::SetScene(JobSystem* jobs, const Scene& scene, const std::wstring& assetsRoot, LoadFn load) {
    Impl& w = *impl_;
    if (!jobs) return;
    EnsureJolt();
    if (!w.system) {
        w.temp = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
        w.jolt = std::make_unique<EngineJobs>(*jobs);
        w.system = std::make_unique<JPH::PhysicsSystem>();
        w.system->Init(65536, 0, 65536, 16384, w.broad, w.objectVsBroad, w.objectVsObject);
        w.system->SetGravity(JPH::Vec3(0, -9.81f, 0));
    }
    w.character = nullptr;
    w.hasLastGround = false;
    w.Clear();
    w.origin = {};
    w.lastFocus = {1e30f, 0, 0};
    w.jobs = jobs;
    w.load = load;
    w.shared = std::make_shared<StreamShared>();

    auto placements = std::make_shared<std::vector<Placement>>();
    auto files = std::make_shared<std::vector<InstanceFile>>();
    for (const Entity& e : scene.entities) {
        if (!e.animClip.empty() || !e.faceClip.empty()) continue; // characters
        if (e.mesh == MeshKind::Imported) { placements->push_back({e.meshPath, e.position, e.rotation, e.scale, false}); continue; }
        Item item;
        item.asset = std::make_shared<Asset>();
        item.asset->path = e.name;
        const Vec3 half = LocalHalfExtents(e.mesh);
        item.asset->shape = new JPH::BoxShape(J(half), 0.0f);
        item.asset->state.store(Ready);
        item.position = e.position;
        item.rotation = e.rotation;
        item.scale = e.scale;
        item.world = TransformSphere(Sphere{{0, 0, 0}, Len(half)}, e.Model());
        w.items.push_back(std::move(item));
    }
    for (const InstanceSet& set : scene.instances)
        files->push_back({set.meshPath, assetsRoot + L"\\" + std::wstring(set.file.begin(), set.file.end())});
    std::shared_ptr<StreamShared> s = w.shared;
    jobs->RunBackground([placements, files, s] { SetUp(*placements, *files, s); });
}

void PhysicsWorld::Update(Vec3 focus, float dt) {
    Impl& w = *impl_;
    if (!w.system || !w.shared) return;
    const int64_t t0 = tbd::platform::Ticks();
    bool arrivedAny = false;
    {
        std::lock_guard<std::mutex> lock(w.shared->mutex);
        if (!w.shared->arrived.empty()) {
            arrivedAny = true;
            w.items.insert(w.items.end(), std::make_move_iterator(w.shared->arrived.begin()), std::make_move_iterator(w.shared->arrived.end()));
            w.shared->arrived.clear();
        }
    }
    // A share of the list per frame: whole-map pieces make tens of thousands of far tiles, and walking takes frames to matter.
    // A jump (teleport) or a new batch looks at everything at once.
    int adds = 0;
    const size_t total = w.items.size();
    const bool jumped = Len(focus - w.lastFocus) > 20.0f;
    w.lastFocus = focus;
    if (jumped || arrivedAny) w.scanAll = true;
    const size_t visits = w.scanAll ? total : std::min(total, std::max<size_t>(256, total / 8));
    if (w.scanAll && total) w.scanAll = false;
    for (size_t visit = 0; visit < visits; ++visit) {
        w.cursor = w.cursor + 1 < total ? w.cursor + 1 : 0;
        Item& item = w.items[w.cursor];
        const Vec3 itemLocal = ToLocal(w.origin, {item.world.center.x, item.world.center.y, item.world.center.z});
        const Vec3 flat = {itemLocal.x - focus.x, 0, itemLocal.z - focus.z};
        const float distance = Len(flat) - item.world.radius; // height does not count: a teleport does not know the ground's yet
        if (!item.body.IsInvalid()) {
            if (distance > kUnloadRadius) w.Remove(item);
            continue;
        }
        if (item.failed || distance > kLoadRadius) continue;
        if (item.big) {
            if (item.started) continue;
            item.started = true;
            w.shared->pendingSetup.fetch_add(1, std::memory_order_acq_rel);
            std::shared_ptr<StreamShared> s = w.shared;
            const Item copy = item;
            const LoadFn load = w.load;
            w.jobs->RunBackground([copy, s, load] { SplitBig(copy, s, load); });
            continue;
        }
        const int state = item.asset->state.load(std::memory_order_acquire);
        if (state == Idle && w.shared->building.load(std::memory_order_relaxed) < kMaxBuilding) w.StartBuild(item.asset);
        else if (state == Failed) item.failed = true;
        else if (state == Ready) {
            if (adds < kAddsPerFrame) { w.Add(item); ++adds; }
            else w.scanAll = true; // more than a frame's worth: keep looking until they are all in
        }
        if (state == Idle || state == Building) w.scanAll = w.scanAll || distance < 6.0f; // what he stands on comes first
    }
    if (w.broadPhaseDirty && adds == 0 && w.shared->building.load(std::memory_order_relaxed) == 0) {
        w.system->OptimizeBroadPhase();
        w.broadPhaseDirty = false;
    }
    const bool dolls = w.ragdolls && w.ragdolls->Count() > 0;
    if (!w.balls.empty() || dolls) {
        w.system->Update(dt, dolls ? RagdollWorld::Substeps(dt) : 1, w.temp.get(), w.jolt.get());
        if (dolls) w.ragdolls->PostStep(dt);
    }
    const int64_t t1 = tbd::platform::Ticks();
    w.stats.updateMs = tbd::platform::TicksToMs(t1 - t0);
}

bool PhysicsWorld::ReadyAround(Vec3 at, float radius) const {
    const Impl& w = *impl_;
    if (!w.system || !w.shared || w.shared->pendingSetup.load(std::memory_order_acquire) != 0) return false;
    {
        std::lock_guard<std::mutex> lock(w.shared->mutex);
        if (!w.shared->arrived.empty()) return false;
    }
    for (const Item& item : w.items) {
        if (item.failed || !item.body.IsInvalid() || (item.big && item.started)) continue;
        const Vec3 center = ToLocal(w.origin, {item.world.center.x, item.world.center.y, item.world.center.z});
        const Vec3 flat = {center.x - at.x, 0, center.z - at.z};
        if (Len(flat) - item.world.radius < radius) return false;
    }
    return true;
}

void PhysicsWorld::ShiftOrigin(Vec3 offset) {
    Impl& w = *impl_;
    if (!w.system) return;
    // Update only on the main thread between Jolt updates. Background jobs produce
    // scene-space placements and therefore do not race with this operation.
    JPH::BodyInterface& bi = w.system->GetBodyInterface();
    for (const Item& item : w.items)
        if (!item.body.IsInvalid()) bi.SetPosition(item.body, bi.GetPosition(item.body) - J(offset), JPH::EActivation::DontActivate);
    for (JPH::BodyID id : w.balls)
        bi.SetPosition(id, bi.GetPosition(id) - J(offset), JPH::EActivation::DontActivate);
    if (w.ragdolls) w.ragdolls->Shift(offset);
    if (w.character) {
        w.character->SetPosition(w.character->GetPosition() - J(offset));
        // SetPosition does not translate CharacterVirtual's cached contact points.
        w.character->RefreshContacts(w.system->GetDefaultBroadPhaseLayerFilter(Layers::Moving),
                                     w.system->GetDefaultLayerFilter(Layers::Moving), {}, {}, *w.temp);
    }
    if (w.hasLastGround) w.lastGround = w.lastGround - offset;
    w.lastFocus = w.lastFocus - offset;
    w.origin = ToWorld(w.origin, offset);
    w.scanAll = true;
}

bool PhysicsWorld::Raycast(Vec3 origin, Vec3 dir, float maxDistance, RayHit& hit) const {
    const Impl& w = *impl_;
    if (!w.system) return false;
    const Vec3 unit = Normalize(dir);
    JPH::RRayCast ray(J(origin), J(unit * maxDistance));
    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> closest;
    w.system->GetNarrowPhaseQuery().CastRay(ray, settings, closest);
    if (!closest.HadHit()) return false;
    hit.distance = closest.mHit.mFraction * maxDistance;
    hit.point = origin + unit * hit.distance;
    JPH::BodyLockRead lock(w.system->GetBodyLockInterfaceNoLock(), closest.mHit.mBodyID);
    if (lock.Succeeded()) hit.normal = T(lock.GetBody().GetWorldSpaceSurfaceNormal(closest.mHit.mSubShapeID2, J(hit.point)));
    return true;
}

bool PhysicsWorld::SphereCast(Vec3 origin, Vec3 dir, float maxDistance, float radius, float& distance) const {
    const Impl& w = *impl_;
    if (!w.system) return false;
    const Vec3 unit = Normalize(dir);
    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded();
    JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(&sphere, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(J(origin)), J(unit * maxDistance));
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> closest;
    w.system->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), closest);
    if (!closest.HadHit()) return false;
    distance = closest.mHit.mFraction * maxDistance;
    return true;
}

void PhysicsWorld::CreateCharacter(Vec3 feet) {
    Impl& w = *impl_;
    if (!w.system) return;
    JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(kCapsuleHeight * 0.5f - kCapsuleRadius, kCapsuleRadius);
    JPH::RotatedTranslatedShapeSettings raised(JPH::Vec3(0, kCapsuleHeight * 0.5f, 0), JPH::Quat::sIdentity(), capsule);
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = raised.Create().Get();
    settings->mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
    settings->mMass = 75.0f;
    settings->mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -kCapsuleRadius); // only contacts low on the capsule support it
    settings->mCharacterPadding = 0.02f;
    w.character = new JPH::CharacterVirtual(settings, J(feet), JPH::Quat::sIdentity(), w.system.get());
    w.lastGround = feet;
    w.hasLastGround = false;
}

void PhysicsWorld::DestroyCharacter() { impl_->character = nullptr; }

void PhysicsWorld::TeleportCharacter(Vec3 feet) {
    Impl& w = *impl_;
    if (!w.character) return;
    w.character->SetPosition(J(feet));
    w.character->SetLinearVelocity(JPH::Vec3::sZero());
    w.hasLastGround = false;
}

CharacterState PhysicsWorld::Character() const {
    CharacterState s;
    const Impl& w = *impl_;
    if (!w.character) return s;
    s.feet = T(w.character->GetPosition());
    s.velocity = T(w.character->GetLinearVelocity());
    const JPH::CharacterBase::EGroundState g = w.character->GetGroundState();
    s.onGround = g == JPH::CharacterBase::EGroundState::OnGround;
    s.steep = g == JPH::CharacterBase::EGroundState::OnSteepGround;
    return s;
}

CharacterState PhysicsWorld::StepCharacter(const CharacterStep& step, float dt) {
    Impl& w = *impl_;
    if (!w.character) return {};
    const int64_t t0 = tbd::platform::Ticks();
    JPH::CharacterVirtual& c = *w.character;
    const JPH::Vec3 gravity = w.system->GetGravity();
    const bool grounded = c.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    float vy = c.GetLinearVelocity().GetY();
    if (step.swim) vy = std::clamp((step.swimFeetY - c.GetPosition().GetY()) * 4.0f, -2.0f, 2.0f);
    else if (grounded) vy = step.jump ? 4.6f : 0.0f; // about 1.08 m high
    else vy += gravity.GetY() * dt;
    c.SetLinearVelocity(JPH::Vec3(step.velocity.x, vy, step.velocity.z));
    JPH::CharacterVirtual::ExtendedUpdateSettings extended;
    extended.mWalkStairsStepUp = JPH::Vec3(0, 0.4f, 0);
    extended.mStickToFloorStepDown = (step.jump || step.swim) ? JPH::Vec3::sZero() : JPH::Vec3(0, -0.5f, 0);
    c.ExtendedUpdate(dt, step.swim ? JPH::Vec3::sZero() : gravity, extended, w.system->GetDefaultBroadPhaseLayerFilter(Layers::Moving),
                     w.system->GetDefaultLayerFilter(Layers::Moving), {}, {}, *w.temp);
    CharacterState s = Character();
    if (s.onGround) { w.lastGround = s.feet; w.hasLastGround = true; }
    else if (w.hasLastGround && s.feet.y < w.lastGround.y - 200.0f) { // through a hole in the world: back to where it held
        Log("Physics: the character fell out of the world at %.1f %.1f %.1f", s.feet.x, s.feet.y, s.feet.z);
        TeleportCharacter(w.lastGround + Vec3{0, 0.2f, 0});
        s = Character();
    }
    const int64_t t1 = tbd::platform::Ticks();
    w.stats.characterMs = tbd::platform::TicksToMs(t1 - t0);
    return s;
}

RagdollWorld& PhysicsWorld::Ragdolls() {
    Impl& w = *impl_;
    if (!w.ragdolls) w.ragdolls = std::make_unique<RagdollWorld>(w.system.get(), (unsigned)Layers::Moving);
    return *w.ragdolls;
}
bool PhysicsWorld::HasRagdolls() const { return impl_->ragdolls && impl_->ragdolls->Count() > 0; }

uint32_t PhysicsWorld::AddBall(Vec3 centre, float radius) {
    Impl& w = *impl_;
    if (!w.system) return 0;
    JPH::BodyCreationSettings settings(new JPH::SphereShape(radius), J(centre), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layers::Moving);
    const JPH::BodyID id = w.system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (id.IsInvalid()) return 0;
    w.balls.push_back(id);
    return (uint32_t)w.balls.size();
}

Vec3 PhysicsWorld::BallPosition(uint32_t id) const {
    const Impl& w = *impl_;
    if (id == 0 || id > w.balls.size()) return {0, 0, 0};
    return T(w.system->GetBodyInterface().GetPosition(w.balls[id - 1]));
}

void PhysicsWorld::CollectStaticTriangles(Vec3 lo, Vec3 hi, std::vector<float>& out) const {
    const Impl& w = *impl_;
    if (!w.system) return;
    const JPH::AABox box(J(lo), J(hi));
    JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> hits;
    w.system->GetNarrowPhaseQuery().CollectTransformedShapes(box, hits, JPH::SpecifiedBroadPhaseLayerFilter(BroadLayers::Static),
                                                              JPH::SpecifiedObjectLayerFilter(Layers::Static));
    JPH::Float3 tri[3 * 64];
    for (const JPH::TransformedShape& shape : hits.mHits) {
        JPH::Shape::GetTrianglesContext context;
        shape.GetTrianglesStart(context, box, JPH::Vec3::sZero());
        for (;;) {
            const int count = shape.GetTrianglesNext(context, 64, tri);
            if (count == 0) break;
            for (int i = 0; i < count * 3; ++i) { out.push_back(tri[i].x); out.push_back(tri[i].y); out.push_back(tri[i].z); }
        }
    }
}

uint32_t PhysicsWorld::CreateKinematicCapsule(Vec3 feet, float radius, float height) {
    Impl& w = *impl_;
    if (!w.system) return 0;
    JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(std::max(height * 0.5f - radius, 0.05f), radius);
    JPH::RotatedTranslatedShapeSettings raised(JPH::Vec3(0, height * 0.5f, 0), JPH::Quat::sIdentity(), capsule);
    JPH::BodyCreationSettings settings(raised.Create().Get(), J(feet), JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, Layers::Moving);
    const JPH::BodyID id = w.system->GetBodyInterfaceNoLock().CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (id.IsInvalid()) return 0;
    w.kinematic[++w.nextKinematic] = id;
    return w.nextKinematic;
}

void PhysicsWorld::MoveKinematic(uint32_t id, Vec3 feet) {
    Impl& w = *impl_;
    const auto it = w.kinematic.find(id);
    if (it != w.kinematic.end()) w.system->GetBodyInterfaceNoLock().SetPosition(it->second, J(feet), JPH::EActivation::DontActivate);
}

void PhysicsWorld::DestroyKinematic(uint32_t id) {
    Impl& w = *impl_;
    const auto it = w.kinematic.find(id);
    if (it == w.kinematic.end()) return;
    JPH::BodyInterface& bi = w.system->GetBodyInterfaceNoLock();
    bi.RemoveBody(it->second);
    bi.DestroyBody(it->second);
    w.kinematic.erase(it);
}

size_t PhysicsWorld::KinematicCount() const { return impl_->kinematic.size(); }

PhysicsStats PhysicsWorld::Stats() const {
    PhysicsStats s = impl_->stats;
    s.items = impl_->items.size();
    s.bodies = impl_->bodies;
    s.building = impl_->shared ? (size_t)impl_->shared->building.load() : 0;
    return s;
}

} // namespace tbd
