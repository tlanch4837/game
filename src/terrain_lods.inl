// Three sets of terrain data switched by distance, the idea Kojima Productions describes for Death Stranding 2 (three kinds of
// terrain data with their own LOD stages; the distances here are ours, as none is published). Near: the authored triangles. Middle
// and far: the same surface clustered onto a world grid, with skirts over the cracks between tiles and sets. The big terrain entities
// become tiles of MatParts, so culling and shadows treat them like any part. TBD_TERRAINLOD=0 keeps the whole meshes.
namespace terrain_lods {

constexpr size_t kMinTriangles = 300000;
constexpr int kTiles = 16; // per axis

inline bool Wanted(const tbd::Entity& e, const std::vector<tbd::Vertex>& v) {
    static const bool on = [] { char b[8] = {}; return !(tbd::platform::GetEnvA("TBD_TERRAINLOD", b, 8) > 0 && b[0] == '0'); }();
    return on && v.size() / 3 >= kMinTriangles && e.name.rfind("Terrain/", 0) == 0 && e.animClip.empty();
}

inline uint64_t CellKey(float x, float z, float cell) {
    return ((uint64_t)(uint32_t)(int32_t)std::floor(x / cell) << 32) | (uint32_t)(int32_t)std::floor(z / cell);
}

// `v` flat, in the entity's own space. Touches no shared state, so a scene load runs it on a worker.
inline void Build(const std::vector<tbd::Vertex>& v, tbd::TextureId albedo, tbd::TextureId normal, tbd::TextureId surface, std::vector<BuiltPart>& made) {
    float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
    for (const tbd::Vertex& x : v) { minX = std::min(minX, x.px); maxX = std::max(maxX, x.px); minZ = std::min(minZ, x.pz); maxZ = std::max(maxZ, x.pz); }
    const float spacing = std::sqrt(2.0f * (maxX - minX) * (maxZ - minZ) / (float)(v.size() / 3)); // typical edge
    const float tileX = (maxX - minX) / kTiles + 1e-3f, tileZ = (maxZ - minZ) / kTiles + 1e-3f;
    const auto tileOf = [&](const tbd::Vertex* t) {
        const int x = std::clamp((int)(((t[0].px + t[1].px + t[2].px) / 3.0f - minX) / tileX), 0, kTiles - 1);
        const int z = std::clamp((int)(((t[0].pz + t[1].pz + t[2].pz) / 3.0f - minZ) / tileZ), 0, kTiles - 1);
        return z * kTiles + x;
    };
    std::vector<std::vector<uint32_t>> tiles(kTiles * kTiles);
    for (uint32_t t = 0; t < v.size() / 3; ++t) tiles[tileOf(&v[t * 3])].push_back(t);

    const float near1 = spacing * 200.0f, near2 = spacing * 700.0f;
    const float cells[3] = {0.0f, spacing * 2.0f, spacing * 6.0f};
    const float ranges[3][2] = {{0.0f, near1}, {near1, near2}, {near2, 1e9f}};
    uint64_t triangles[3] = {};
    for (int lod = 0; lod < 3; ++lod) {
        // One vertex per occupied cell, the same for every tile, so tiles of a set share their borders.
        std::unordered_map<uint64_t, tbd::Vertex> merged;
        std::unordered_map<uint64_t, uint32_t> weight;
        if (lod > 0) {
            merged.reserve(v.size() / 8);
            for (const tbd::Vertex& x : v) {
                const uint64_t key = CellKey(x.px, x.pz, cells[lod]);
                uint32_t& n = weight[key];
                tbd::Vertex& m = merged[key];
                if (n == 0) m = x;
                else {
                    m.px += x.px; m.py += x.py; m.pz += x.pz; m.nx += x.nx; m.ny += x.ny; m.nz += x.nz;
                    m.r += x.r; m.g += x.g; m.b += x.b; m.u += x.u; m.v += x.v;
                }
                ++n;
            }
            for (auto& [key, m] : merged) {
                const float f = 1.0f / (float)weight[key];
                m.px *= f; m.py *= f; m.pz *= f; m.r *= f; m.g *= f; m.b *= f; m.u *= f; m.v *= f;
                const float len = std::sqrt(m.nx * m.nx + m.ny * m.ny + m.nz * m.nz);
                if (len > 1e-6f) { m.nx /= len; m.ny /= len; m.nz /= len; }
            }
        }
        for (const std::vector<uint32_t>& tile : tiles) {
            if (tile.empty()) continue;
            std::vector<tbd::Vertex> out;
            out.reserve(lod == 0 ? tile.size() * 3 : tile.size() / 2);
            if (lod == 0) {
                for (uint32_t t : tile) out.insert(out.end(), &v[t * 3], &v[t * 3] + 3);
            } else {
                std::map<std::pair<uint64_t, uint64_t>, int> edges; // an edge used once is a border: it gets a skirt
                std::map<std::pair<uint64_t, uint64_t>, std::pair<uint64_t, uint64_t>> directed;
                for (uint32_t t : tile) {
                    const uint64_t k[3] = {CellKey(v[t * 3].px, v[t * 3].pz, cells[lod]), CellKey(v[t * 3 + 1].px, v[t * 3 + 1].pz, cells[lod]), CellKey(v[t * 3 + 2].px, v[t * 3 + 2].pz, cells[lod])};
                    if (k[0] == k[1] || k[1] == k[2] || k[0] == k[2]) continue;
                    for (int e = 0; e < 3; ++e) {
                        out.push_back(merged[k[e]]);
                        const uint64_t a = k[e], b = k[(e + 1) % 3];
                        const auto key = std::minmax(a, b);
                        ++edges[{key.first, key.second}];
                        directed[{key.first, key.second}] = {a, b};
                    }
                }
                for (const auto& [edge, count] : edges) {
                    if (count != 1) continue;
                    const auto [ka, kb] = directed[edge];
                    tbd::Vertex a = merged[ka], b = merged[kb], la = a, lb = b;
                    la.py -= cells[lod] * 0.6f; lb.py -= cells[lod] * 0.6f;
                    const tbd::Vertex quad[6] = {b, a, la, b, la, lb};
                    out.insert(out.end(), quad, quad + 6);
                }
            }
            if (out.size() < 3) continue;
            MatPart part;
            part.albedo = albedo;
            part.normal = normal;
            part.surface = surface;
            part.count = out.size();
            part.bounds = tbd::SphereAround(out.data(), out.size());
            part.lodMin = ranges[lod][0];
            part.lodMax = ranges[lod][1];
            part.caster = lod == 0; // a coarser caster would shadow the fine surface in bands; only tiles the sun map touches draw
            triangles[lod] += out.size() / 3;
            BuiltPart built;
            built.part = part;
            tbd::WeldVertices(out.data(), (uint32_t)out.size(), built.vertices, built.indices);
            made.push_back(std::move(built));
        }
    }
    tbd::Log("Terrain sets: spacing %.1f m, tiles %d, triangles %llu / %llu / %llu, switching at %.0f and %.0f m", spacing, kTiles * kTiles,
             (unsigned long long)triangles[0], (unsigned long long)triangles[1], (unsigned long long)triangles[2], near1, near2);
}

} // namespace terrain_lods
