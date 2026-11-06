// The scene: a list of entities with transforms, plus ray picking. Renderer-agnostic.
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "procmesh.h"
#include "render/renderer.h"
#include "vecmath.h"

namespace tbd {

enum class MeshKind { Cube, Plane, Sphere, Tree, Rock, Imported };

// An entity that becomes an AI agent in Play (src/ai/npc_system.h). The entity marks where it starts and is hidden while Play runs.
struct NpcInfo {
    bool enabled = false;
    std::string routine = "townsfolk"; // townsfolk | guard | loiterer | follower | skittish; see npc_system.cpp
    float wanderRadius = 14.0f;        // wandering stays this near the start point
    float reactRadius = 6.0f;          // the player this near is turned to face (or fled from, for skittish)
    bool hasIndoors = false;           // townsfolk go here at night and are not seen until morning
    Vec3 indoors = {0, 0, 0};
    std::vector<Vec3> patrol;          // guards walk these in order, then back
};

// A Luau script attached to an entity (path relative to the project's Scripts folder) and its edited property values.
struct EntityScript {
    std::string path;
    std::string props; // "key=value;key=value", applied over the script's declared defaults
};

struct Entity {
    std::string name;
    NpcInfo npc;
    MeshKind mesh = MeshKind::Cube;
    std::string meshPath; // Imported only: the .obj / .gltf / .glb file (UTF-8, absolute)
    Vec3 position = {0, 0, 0};
    Quat rotation;
    Vec3 scale = {1, 1, 1};
    Vec3 color = {0.8f, 0.8f, 0.8f};
    std::string faceClip; // .tbdface file (UTF-8, absolute) whose head rotation drives this entity; empty = none
    Quat faceBase;        // rotation the face clip is applied on top of (rotation itself is overwritten while driven)
    std::string animClip; // glTF file (UTF-8, absolute) whose first animation poses this entity's skin; empty = none
    std::vector<EntityScript> scripts; // Luau scripts run on this entity in Play (src/script); saved as S| lines

    Mat4 Model() const { return TRS(position, rotation, scale); }
};

// Half-size of the pickable box in the entity's own space (before scale).
inline Vec3 LocalHalfExtents(MeshKind kind) {
    switch (kind) {
    case MeshKind::Plane: return {0.5f, 0.02f, 0.5f};
    case MeshKind::Tree: return {0.36f, 0.5f, 0.41f};
    case MeshKind::Rock: return {0.53f, 0.4f, 0.45f};
    default: return {0.5f, 0.5f, 0.5f};
    }
}

// Pickable half-size of each imported file's (unit-box normalized) mesh, filled in when the mesh is loaded.
inline std::map<std::string, Vec3>& ImportedHalfExtents() {
    static std::map<std::string, Vec3> extents;
    return extents;
}

inline const char* MeshKindName(MeshKind kind) {
    switch (kind) {
    case MeshKind::Imported: return "Imported";
    case MeshKind::Cube: return "Cube";
    case MeshKind::Plane: return "Plane";
    case MeshKind::Sphere: return "Sphere";
    case MeshKind::Tree: return "Tree";
    case MeshKind::Rock: return "Rock";
    }
    return "Cube";
}

inline Vec3 LocalHalfExtents(const Entity& e) {
    if (e.mesh != MeshKind::Imported) return LocalHalfExtents(e.mesh);
    const auto it = ImportedHalfExtents().find(e.meshPath);
    return it != ImportedHalfExtents().end() ? it->second : Vec3{0.5f, 0.5f, 0.5f};
}

// Ray against the entity's oriented, scaled box. On a hit, tOut is the distance along ray.dir.
inline bool RayHitsEntity(const Ray& ray, const Entity& e, float& tOut) {
    auto safeInv = [](float s) { return std::fabs(s) > 1e-6f ? 1.0f / s : 1e6f; };
    const Vec3 inv = {safeInv(e.scale.x), safeInv(e.scale.y), safeInv(e.scale.z)};
    const Quat toLocal = Conjugate(e.rotation);
    Vec3 o = Rotate(toLocal, ray.origin - e.position);
    Vec3 d = Rotate(toLocal, ray.dir);
    o = {o.x * inv.x, o.y * inv.y, o.z * inv.z};
    d = {d.x * inv.x, d.y * inv.y, d.z * inv.z};

    const Vec3 h = LocalHalfExtents(e);
    const float os[3] = {o.x, o.y, o.z}, ds[3] = {d.x, d.y, d.z}, hs[3] = {h.x, h.y, h.z};
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(ds[i]) < 1e-9f) {
            if (os[i] < -hs[i] || os[i] > hs[i]) return false;
            continue;
        }
        float t1 = (-hs[i] - os[i]) / ds[i], t2 = (hs[i] - os[i]) / ds[i];
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::fmax(tmin, t1);
        tmax = std::fmin(tmax, t2);
        if (tmin > tmax) return false;
    }
    if (tmax < 0) return false;
    tOut = tmin >= 0 ? tmin : tmax;
    return true;
}

// One asset scattered many times: its matrices live in a binary file (packed float[12] rows, a float3x4 row by row).
struct InstanceSet {
    std::string name;
    std::string meshPath;
    std::string file;              // relative to the project's assets folder
    float maxDistance = 1500.0f;   // meters from the camera
    float shadowDistance = 80.0f;  // meters from the shadow focus; 0 = casts no shadow
};

class Scene {
public:
    std::vector<Entity> entities;
    std::vector<InstanceSet> instances;
    int selected = -1;
    // How much sky, probe and ground-bounce light reaches surfaces (1 = as computed). Below 1 for a lit-by-one-source look,
    // a studio key with little fill. Saved as "L|<fill>|<exposure>" when either is set; the mesh shaders read it from lightDebug.w.
    float ambientFill = 1.0f;
    // A fixed exposure (the tone map's multiplier before brightness), as a photographer locks it for a dark shot; 0 = auto.
    float exposure = 0.0f;
    // The time of day the scene opens at (Sun::hour, day cycle stopped); below 0 = keep the editor's. Third field of "L|".
    float hour = -1.0f;

    // A box on a plane.
    Scene() {
        Entity ground;
        ground.name = "Ground";
        ground.mesh = MeshKind::Plane;
        ground.scale = {20, 1, 20};
        ground.color = {0.36f, 0.33f, 0.29f};
        entities.push_back(ground);

        Entity box;
        box.name = "Box";
        box.mesh = MeshKind::Cube;
        box.position = {0, 0.5f, 0};
        box.color = {0.85f, 0.56f, 0.28f};
        entities.push_back(box);
        selected = 1;
    }

    Entity* Selected() { return selected >= 0 && selected < (int)entities.size() ? &entities[selected] : nullptr; }

    // Adds an entity and selects it. Returns its index.
    int Add(const Entity& e) {
        entities.push_back(e);
        selected = (int)entities.size() - 1;
        return selected;
    }

    // Copies the selected entity, nudged sideways, and selects the copy. Returns it, or null.
    const Entity* DuplicateSelected() {
        const Entity* src = Selected();
        if (!src) return nullptr;
        Entity copy = *src;
        copy.name += " Copy";
        copy.position.x += std::fmax(1.0f, copy.scale.x);
        Add(copy);
        return &entities[selected];
    }

    // Removes the selected entity. Returns its index, or -1 when nothing was selected.
    int DeleteSelected() {
        if (!Selected()) return -1;
        const int index = selected;
        entities.erase(entities.begin() + index);
        selected = entities.empty() ? -1 : std::min(index, (int)entities.size() - 1);
        return index;
    }

    // Index of the nearest entity under the ray, or -1.
    int Pick(const Ray& ray) const {
        int best = -1;
        float bestT = 1e30f;
        for (int i = 0; i < (int)entities.size(); ++i) {
            float t;
            if (RayHitsEntity(ray, entities[i], t) && t < bestT) {
                bestT = t;
                best = i;
            }
        }
        return best;
    }
};

// Unit-size meshes (the entity's scale sizes them), flat-shaded, baked with a color.
inline std::vector<Vertex> MakeCubeMesh(Vec3 c) {
    struct Face {
        Vec3 n, u, v;
    };
    const Face faces[6] = {{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                           {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                           {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
    std::vector<Vertex> out;
    for (const Face& f : faces) {
        const Vec3 p[4] = {f.n * 0.5f - f.u * 0.5f - f.v * 0.5f, f.n * 0.5f + f.u * 0.5f - f.v * 0.5f,
                           f.n * 0.5f + f.u * 0.5f + f.v * 0.5f, f.n * 0.5f - f.u * 0.5f + f.v * 0.5f};
        for (int i : {0, 1, 2, 0, 2, 3})
            out.push_back({p[i].x, p[i].y, p[i].z, f.n.x, f.n.y, f.n.z, c.x, c.y, c.z, 1.0f});
    }
    return out;
}

inline std::vector<Vertex> MakePlaneMesh(Vec3 c) {
    const float p[4][3] = {{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}, {-0.5f, 0, 0.5f}};
    std::vector<Vertex> out;
    for (int i : {0, 1, 2, 0, 2, 3}) out.push_back({p[i][0], p[i][1], p[i][2], 0, 1, 0, c.x, c.y, c.z, 1.0f});
    return out;
}

// Diameter 1, smooth normals.
inline std::vector<Vertex> MakeSphereMesh(Vec3 c) {
    const int rings = 12, sectors = 20;
    auto point = [&](int ring, int sector) {
        const float phi = kPi * (float)ring / (float)rings, theta = 2.0f * kPi * (float)sector / (float)sectors;
        return Vec3{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
    };
    std::vector<Vertex> out;
    auto push = [&](Vec3 n) { out.push_back({n.x * 0.5f, n.y * 0.5f, n.z * 0.5f, n.x, n.y, n.z, c.x, c.y, c.z, 1.0f}); };
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < sectors; ++s) {
            const Vec3 a = point(r, s), b = point(r, s + 1), d = point(r + 1, s), e = point(r + 1, s + 1);
            if (r > 0) { push(a); push(d); push(b); }
            if (r < rings - 1) { push(b); push(d); push(e); }
        }
    return out;
}

inline std::vector<Vertex> MakeMesh(MeshKind kind, Vec3 color) {
    switch (kind) {
    case MeshKind::Plane: return MakePlaneMesh(color);
    case MeshKind::Sphere: return MakeSphereMesh(color);
    case MeshKind::Tree: return MakeTreeMesh(color);
    case MeshKind::Rock: return MakeRockMesh(color);
    default: return MakeCubeMesh(color);
    }
}

} // namespace tbd
