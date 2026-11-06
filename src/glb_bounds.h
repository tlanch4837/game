// A .glb's bounding box from its JSON chunk alone (accessor min/max under the node transforms), so a scene can know where
// everything is without reading gigabytes of geometry and images.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "mesh_import.h"
#include "vecmath.h"

namespace tbd {

namespace glb_bounds_detail {

using meshimport_detail::Json;
using meshimport_detail::M4;

inline M4 NodeLocal(const Json& n) {
    M4 local = meshimport_detail::Identity();
    if (const Json* m = n.Find("matrix")) {
        for (int i = 0; i < 16 && i < (int)m->a.size(); ++i) local.m[i] = (float)m->a[(size_t)i].Num();
        return local;
    }
    float t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
    if (const Json* j = n.Find("translation")) for (int i = 0; i < 3 && i < (int)j->a.size(); ++i) t[i] = (float)j->a[(size_t)i].Num();
    if (const Json* j = n.Find("rotation")) for (int i = 0; i < 4 && i < (int)j->a.size(); ++i) q[i] = (float)j->a[(size_t)i].Num();
    if (const Json* j = n.Find("scale")) for (int i = 0; i < 3 && i < (int)j->a.size(); ++i) s[i] = (float)j->a[(size_t)i].Num();
    return meshimport_detail::FromTRS(t, q, s);
}

inline bool Visit(const Json& doc, int node, const M4& parent, int depth, Vec3& lo, Vec3& hi, bool& any) {
    const Json* nodes = doc.Find("nodes");
    const Json* n = nodes ? nodes->At((size_t)node) : nullptr;
    if (!n || depth > 64) return false;
    const M4 world = meshimport_detail::Mul(parent, NodeLocal(*n));
    if (const Json* meshRef = n->Find("mesh")) {
        const Json* meshes = doc.Find("meshes");
        const Json* accessors = doc.Find("accessors");
        const Json* mesh = meshes ? meshes->At((size_t)meshRef->Int()) : nullptr;
        const Json* prims = mesh ? mesh->Find("primitives") : nullptr;
        if (!mesh || !accessors) return false;
        if (prims)
            for (const Json& prim : prims->a) {
                const Json* attrs = prim.Find("attributes");
                const Json* pos = attrs ? attrs->Find("POSITION") : nullptr;
                if (!pos) continue;
                const Json* acc = accessors->At((size_t)pos->Int());
                const Json* mn = acc ? acc->Find("min") : nullptr;
                const Json* mx = acc ? acc->Find("max") : nullptr;
                if (!mn || !mx || mn->a.size() < 3 || mx->a.size() < 3) return false; // the spec requires them for POSITION
                for (int c = 0; c < 8; ++c) {
                    const float x = (float)((c & 1 ? mx : mn)->a[0].Num()), y = (float)((c & 2 ? mx : mn)->a[1].Num()), z = (float)((c & 4 ? mx : mn)->a[2].Num());
                    const Vec3 w = {world.m[0] * x + world.m[4] * y + world.m[8] * z + world.m[12],
                                    world.m[1] * x + world.m[5] * y + world.m[9] * z + world.m[13],
                                    world.m[2] * x + world.m[6] * y + world.m[10] * z + world.m[14]};
                    lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
                    hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
                    any = true;
                }
            }
    }
    if (const Json* kids = n->Find("children"))
        for (const Json& k : kids->a)
            if (!Visit(doc, k.Int(), world, depth + 1, lo, hi, any)) return false;
    return true;
}

} // namespace glb_bounds_detail

// False when the file isn't a readable .glb with the bounds it should carry; the caller must then assume nothing about it.
inline bool ReadGlbBounds(const std::string& path, Vec3& lo, Vec3& hi) {
    using namespace glb_bounds_detail;
    FILE* f = _wfopen(meshimport_detail::Utf8ToWide(path).c_str(), L"rb");
    if (!f) return false;
    uint32_t head[5] = {};
    std::vector<char> text;
    const bool read = fread(head, sizeof(head), 1, f) == 1 && head[0] == 0x46546C67u && head[4] == 0x4E4F534Au && head[3] > 0 && head[3] < (1u << 30);
    if (read) text.resize(head[3]);
    const bool ok = read && fread(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (!ok) return false;
    Json doc;
    if (!meshimport_detail::JsonParser(text.data(), text.size()).Parse(doc)) return false;
    const Json* scenes = doc.Find("scenes");
    const Json* sceneRef = doc.Find("scene");
    const Json* scene = scenes ? scenes->At(sceneRef ? (size_t)sceneRef->Int() : 0) : nullptr;
    const Json* roots = scene ? scene->Find("nodes") : nullptr;
    if (!roots) return false;
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (const Json& r : roots->a)
        if (!Visit(doc, r.Int(), meshimport_detail::Identity(), 0, lo, hi, any)) return false;
    return any;
}

} // namespace tbd
