// The ground under the town, baked once on a background thread after the scene has loaded: a 2x2 atlas of the dirt, gravel and wagon-track
// materials with a top-down bake in the fourth cell (G: nearness to what stands on the ground), and a height and splat raster that drives a
// dense displaced grid near the camera. No extra bindings and no hardware tessellation, so both backends run the same code. The grid is
// plain static geometry: TAA reprojects it from the depth buffer, displaced position included. Scene loads only; TBD_NODISPLACE=1 turns the grid off.
namespace ground_bake {

constexpr float kExtent = 256.0f;  // bake half size, metres around the world origin
constexpr int kBake = 1024;        // texels per axis: half a metre
constexpr int kCell = 1024;
constexpr int kGrid = 201;         // vertices per axis
constexpr float kGridHalf = 20.0f; // metres

struct Atlas { std::shared_ptr<tbd::MipChain> albedo, normal, surface; };
struct Bake {
    std::vector<float> height;  // kBake^2, -1e9 where no ground
    std::vector<uint8_t> weights; // rgb per texel
    std::vector<float> gravelHeight, dirtHeight; // 256^2, 0..1, for displacement
};
struct State {
    std::mutex mutex;
    std::shared_ptr<Atlas> atlasReady;
    std::shared_ptr<Bake> bake;
    std::vector<tbd::Vertex> gridVerts;
    std::vector<uint32_t> gridIndices;
    bool gridReady = false, gridBusy = false;
};
inline std::shared_ptr<State>& Shared() { static auto s = std::make_shared<State>(); return s; }

inline tbd::TextureId gAlbedo = tbd::kNoTexture, gNormal = tbd::kNoTexture, gSurface = tbd::kNoTexture;
inline std::set<tbd::TextureId>& GroundTextures() { static std::set<tbd::TextureId> s; return s; }
inline bool gStarted = false;
inline tbd::MeshId gGridMesh = tbd::kInvalidMesh;
inline std::vector<tbd::MeshId> gRetired;
inline float gGridX = 1e9f, gGridZ = 1e9f;

inline void NoteGround(tbd::TextureId albedo) { if (albedo != tbd::kNoTexture) GroundTextures().insert(albedo); }

inline void Apply(tbd::TextureId& albedo, tbd::TextureId& normal, tbd::TextureId& surface) {
    if (gSurface == tbd::kNoTexture || !GroundTextures().count(albedo)) return;
    albedo = gAlbedo; normal = gNormal; surface = gSurface;
}

inline void Rasterize(const tbd::Vertex* v, size_t count, const float* m, std::vector<float>& height, std::vector<uint8_t>& weights, std::vector<uint8_t>* mask, float maskTop) {
    const float cell = 2.0f * kExtent / kBake;
    const auto tx = [&](const tbd::Vertex& p, float& x, float& y, float& z) {
        x = m[0] * p.px + m[4] * p.py + m[8] * p.pz + m[12];
        y = m[1] * p.px + m[5] * p.py + m[9] * p.pz + m[13];
        z = m[2] * p.px + m[6] * p.py + m[10] * p.pz + m[14];
    };
    for (size_t t = 0; t + 3 <= count; t += 3) {
        float x[3], y[3], z[3];
        for (int k = 0; k < 3; ++k) tx(v[t + k], x[k], y[k], z[k]);
        const float minX = std::min({x[0], x[1], x[2]}), maxX = std::max({x[0], x[1], x[2]}), minZ = std::min({z[0], z[1], z[2]}), maxZ = std::max({z[0], z[1], z[2]});
        if (maxX < -kExtent || minX > kExtent || maxZ < -kExtent || minZ > kExtent) continue;
        if (mask && std::min({y[0], y[1], y[2]}) > maskTop) continue;
        const int ix0 = std::max((int)std::floor((minX + kExtent) / cell), 0), ix1 = std::min((int)std::ceil((maxX + kExtent) / cell), kBake - 1);
        const int iz0 = std::max((int)std::floor((minZ + kExtent) / cell), 0), iz1 = std::min((int)std::ceil((maxZ + kExtent) / cell), kBake - 1);
        const float det = (z[1] - z[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (z[0] - z[2]);
        if (std::abs(det) < 1e-8f) continue;
        for (int iz = iz0; iz <= iz1; ++iz)
            for (int ix = ix0; ix <= ix1; ++ix) {
                const float px = (ix + 0.5f) * cell - kExtent, pz = (iz + 0.5f) * cell - kExtent;
                const float a = ((z[1] - z[2]) * (px - x[2]) + (x[2] - x[1]) * (pz - z[2])) / det;
                const float b = ((z[2] - z[0]) * (px - x[2]) + (x[0] - x[2]) * (pz - z[2])) / det;
                const float c = 1.0f - a - b;
                const float e = mask ? -0.3f : -0.05f;
                if (a < e || b < e || c < e) continue;
                const size_t at = (size_t)iz * kBake + ix;
                if (mask) { (*mask)[at] = 255; continue; }
                const float h = a * y[0] + b * y[1] + c * y[2];
                if (h > height[at]) {
                    height[at] = h;
                    weights[at * 3] = (uint8_t)std::clamp((a * v[t].r + b * v[t + 1].r + c * v[t + 2].r) * 255.0f, 0.0f, 255.0f);
                    weights[at * 3 + 1] = (uint8_t)std::clamp((a * v[t].g + b * v[t + 1].g + c * v[t + 2].g) * 255.0f, 0.0f, 255.0f);
                    weights[at * 3 + 2] = (uint8_t)std::clamp((a * v[t].b + b * v[t + 1].b + c * v[t + 2].b) * 255.0f, 0.0f, 255.0f);
                }
            }
    }
}

inline bool ReadPng(const std::wstring& path, std::vector<uint8_t>& px, int& w, int& h) {
    std::string bytes;
    return tbd::ReadAllText(path, bytes) && tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), px, w, h);
}

struct Source { const std::vector<tbd::Vertex>* vertices; /* the import cache's own: its entries never move or change */ float m[16]; bool ground; float top; };

inline void Blit(std::vector<uint8_t>& atlas, const std::vector<uint8_t>& src, int w, int h, int cell) {
    const int ox = (cell & 1) * kCell, oy = (cell >> 1) * kCell;
    for (int y = 0; y < kCell; ++y)
        for (int x = 0; x < kCell; ++x)
            std::memcpy(&atlas[((size_t)(y + oy) * kCell * 2 + x + ox) * 4], &src[((size_t)(y * h / kCell) * w + x * w / kCell) * 4], 4);
}

inline void Work(std::shared_ptr<State> st, std::wstring root, std::vector<Source> sources) {
    const size_t total = (size_t)kCell * kCell * 16;
    std::vector<uint8_t> albedo(total), normal(total), surface(total);
    const wchar_t* names[3] = {L"dry_dirt", L"gravel", L"wagon_track_dust"};
    auto bake = std::make_shared<Bake>();
    bake->dirtHeight.resize(256 * 256);
    bake->gravelHeight.resize(256 * 256);
    for (int c = 0; c < 3; ++c) {
        std::vector<uint8_t> a, n, s;
        int aw, ah, nw, nh, sw, sh;
        if (!ReadPng(root + names[c] + L"_albedo.png", a, aw, ah) || !ReadPng(root + names[c] + L"_normal.png", n, nw, nh) || !ReadPng(root + names[c] + L"_orh.png", s, sw, sh)) {
            tbd::Log("Ground bake: missing maps in %s", tbd::WideToUtf8(root).c_str());
            return;
        }
        Blit(albedo, a, aw, ah, c);
        Blit(normal, n, nw, nh, c);
        for (size_t k = 0; k + 3 < s.size(); k += 4) { const uint8_t r = s[k], g = s[k + 1], b = s[k + 2]; s[k] = r; s[k + 1] = g; s[k + 2] = 64; s[k + 3] = b; } // occlusion, roughness, marker, height
        Blit(surface, s, sw, sh, c);
        if (c < 2) {
            std::vector<float>& to = c == 0 ? bake->dirtHeight : bake->gravelHeight;
            for (int y = 0; y < 256; ++y) for (int x = 0; x < 256; ++x) to[(size_t)y * 256 + x] = s[((size_t)(y * sh / 256) * sw + x * sw / 256) * 4 + 3] / 255.0f;
        }
    }
    bake->height.assign((size_t)kBake * kBake, -1e9f);
    bake->weights.assign((size_t)kBake * kBake * 3, 0);
    std::vector<uint8_t> mask((size_t)kBake * kBake, 0);
    for (const Source& src : sources)
        if (src.ground) Rasterize(src.vertices->data(), src.vertices->size(), src.m, bake->height, bake->weights, nullptr, 0.0f);
    for (const Source& src : sources)
        if (!src.ground) Rasterize(src.vertices->data(), src.vertices->size(), src.m, bake->height, bake->weights, &mask, src.top);
    // Nearness: a blur of the footprints, about 3 m wide, kept outside them. Footprints themselves are hidden under what stands there.
    std::vector<float> near_(mask.size());
    for (size_t k = 0; k < mask.size(); ++k) near_[k] = mask[k] / 255.0f;
    const std::vector<float> wide = tbd::surface_maps::Blur(near_, kBake, 3);
    for (int y = 0; y < kCell; ++y)
        for (int x = 0; x < kCell; ++x) {
            uint8_t* p = &surface[((size_t)(y + kCell) * kCell * 2 + x + kCell) * 4];
            const float g = wide[(size_t)(y * kBake / kCell) * kBake + x * kBake / kCell];
            p[0] = 255; p[1] = (uint8_t)std::lround(std::clamp(g * 1.9f, 0.0f, 1.0f) * 255.0f); p[2] = 64; p[3] = 255;
            uint8_t* a = &albedo[((size_t)(y + kCell) * kCell * 2 + x + kCell) * 4];
            a[0] = a[1] = a[2] = 128; a[3] = 255;
            uint8_t* n = &normal[((size_t)(y + kCell) * kCell * 2 + x + kCell) * 4];
            n[0] = n[1] = 128; n[2] = n[3] = 255;
        }
    auto atlas = std::make_shared<Atlas>();
    atlas->albedo = std::make_shared<tbd::MipChain>(tbd::BuildMipChain(albedo.data(), kCell * 2, kCell * 2));
    atlas->normal = std::make_shared<tbd::MipChain>(tbd::BuildMipChain(normal.data(), kCell * 2, kCell * 2));
    atlas->surface = std::make_shared<tbd::MipChain>(tbd::BuildMipChain(surface.data(), kCell * 2, kCell * 2));
    std::lock_guard<std::mutex> lock(st->mutex);
    st->bake = bake;
    st->atlasReady = atlas;
}

// Background: the grid around (cx, cz), displaced by the layer heights where the bake has ground. Big shapes only: POM keeps the fine ones.
inline void BuildGrid(std::shared_ptr<State> st, std::shared_ptr<Bake> b, float cx, float cz) {
    const float cell = 2.0f * kExtent / kBake;
    const auto baseAt = [&](float x, float z, float& h, float w[3]) {
        const float fx = (x + kExtent) / cell - 0.5f, fz = (z + kExtent) / cell - 0.5f;
        const int ix = (int)std::floor(fx), iz = (int)std::floor(fz);
        if (ix < 0 || iz < 0 || ix >= kBake - 1 || iz >= kBake - 1) return false;
        const float tx = fx - ix, tz = fz - iz;
        const size_t a = (size_t)iz * kBake + ix, c = a + kBake;
        const float h00 = b->height[a], h10 = b->height[a + 1], h01 = b->height[c], h11 = b->height[c + 1];
        if (h00 < -1e8f || h10 < -1e8f || h01 < -1e8f || h11 < -1e8f) return false;
        h = (h00 * (1 - tx) + h10 * tx) * (1 - tz) + (h01 * (1 - tx) + h11 * tx) * tz;
        for (int k = 0; k < 3; ++k) w[k] = b->weights[a * 3 + k] / 255.0f;
        return true;
    };
    const auto layer = [&](const std::vector<float>& t, float x, float z, float metres) {
        const float u = (x / metres - std::floor(x / metres)) * 255.99f, v = (z / metres - std::floor(z / metres)) * 255.99f;
        const int iu = (int)u, iv = (int)v, ju = (iu + 1) & 255, jv = (iv + 1) & 255;
        const float fu = u - iu, fv = v - iv;
        return (t[(size_t)iv * 256 + iu] * (1 - fu) + t[(size_t)iv * 256 + ju] * fu) * (1 - fv) + (t[(size_t)jv * 256 + iu] * (1 - fu) + t[(size_t)jv * 256 + ju] * fu) * fv;
    };
    const float step = 2.0f * kGridHalf / (kGrid - 1);
    std::vector<float> hs((size_t)kGrid * kGrid, -1e9f);
    std::vector<tbd::Vertex> verts((size_t)kGrid * kGrid);
    for (int j = 0; j < kGrid; ++j)
        for (int i = 0; i < kGrid; ++i) {
            const float x = cx + (i - kGrid / 2) * step, z = cz + (j - kGrid / 2) * step;
            float h, w[3] = {0.7f, 0.2f, 0.1f};
            tbd::Vertex& v = verts[(size_t)j * kGrid + i];
            if (baseAt(x, z, h, w)) {
                const float edge = std::min({(float)i, (float)j, (float)(kGrid - 1 - i), (float)(kGrid - 1 - j)}) / 12.0f;
                const float t = std::clamp(edge, 0.0f, 1.0f);
                // Rises only, so the coarse ground stays hidden; the outer rim sinks under it, so the grid has no visible edge.
                const float relief = layer(b->dirtHeight, x, z, 3.3f) * 0.02f + 0.03f;
                hs[(size_t)j * kGrid + i] = h + relief * t - 0.03f * (1.0f - t);
            }
            v.px = x; v.pz = z; v.r = w[0]; v.g = w[1]; v.b = w[2]; v.a = 0.2f; v.u = x / 3.3f; v.v = z / 3.3f;
        }
    std::vector<uint32_t> idx;
    idx.reserve((size_t)(kGrid - 1) * (kGrid - 1) * 6);
    for (int j = 0; j < kGrid; ++j)
        for (int i = 0; i < kGrid; ++i) {
            tbd::Vertex& v = verts[(size_t)j * kGrid + i];
            const float h = hs[(size_t)j * kGrid + i];
            v.py = h;
            const auto at = [&](int a, int b2) { const float s = hs[(size_t)std::clamp(b2, 0, kGrid - 1) * kGrid + std::clamp(a, 0, kGrid - 1)]; return s < -1e8f ? h : s; };
            const float nx = at(i - 1, j) - at(i + 1, j), nz = at(i, j - 1) - at(i, j + 1), ny = 2.0f * step, len = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            v.nx = nx * len; v.ny = ny * len; v.nz = nz * len;
            if (i + 1 < kGrid && j + 1 < kGrid && h > -1e8f && hs[(size_t)j * kGrid + i + 1] > -1e8f && hs[(size_t)(j + 1) * kGrid + i] > -1e8f && hs[(size_t)(j + 1) * kGrid + i + 1] > -1e8f) {
                const uint32_t a = (uint32_t)(j * kGrid + i), b1 = a + 1, c = a + kGrid, d = c + 1;
                idx.insert(idx.end(), {a, c, b1, b1, c, d});
            }
        }
    std::lock_guard<std::mutex> lock(st->mutex);
    st->gridVerts = std::move(verts);
    st->gridIndices = std::move(idx);
    st->gridReady = true;
    st->gridBusy = false;
}

inline void Tick() { // main thread, every frame
    if (!g_engine.renderer || !g_engine.jobs) return;
    std::shared_ptr<State> st = Shared();
    if (!gStarted) {
        if (GroundTextures().empty() || SceneLoadActive()) return;
        gStarted = true;
        std::vector<Source> sources;
        for (const tbd::Entity& e : g_engine.scene.entities) {
            if (e.mesh != tbd::MeshKind::Imported) continue;
            const bool terrain = e.name.rfind("Terrain/", 0) == 0;
            if (terrain && e.name.find("perdition_ground") == std::string::npos && e.name.find("plaza") == std::string::npos) continue;
            const std::vector<tbd::Vertex>& vertices = ImportedVertices(e.meshPath);
            if (vertices.empty()) continue;
            Source s;
            s.vertices = &vertices;
            s.ground = terrain;
            s.top = e.position.y + 0.4f; // what touches the ground, not the roof's overhang
            const tbd::Mat4 m = e.Model();
            std::memcpy(s.m, m.m, sizeof(s.m));
            sources.push_back(std::move(s));
        }
        std::wstring root = g_engine.assetsRoot;
        const size_t at = root.rfind(L"WesternGame");
        root = (at == std::wstring::npos ? root + L"\\" : root.substr(0, at)) + L"Western\\Assets\\ground\\";
        g_engine.jobs->RunBackground([st, root, sources = std::move(sources)]() mutable { try { Work(st, root, std::move(sources)); } catch (...) { tbd::Log("Ground bake failed"); } });
        return;
    }
    std::shared_ptr<Atlas> ready;
    std::shared_ptr<Bake> bake;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        ready = std::move(st->atlasReady);
        st->atlasReady = nullptr;
        bake = st->bake;
    }
    if (ready) {
        gAlbedo = g_engine.renderer->CreateTextureNoWait(ready->albedo->levels, ready->albedo->dims);
        gNormal = g_engine.renderer->CreateTextureNoWait(ready->normal->levels, ready->normal->dims);
        gSurface = g_engine.renderer->CreateTextureNoWait(ready->surface->levels, ready->surface->dims);
    }
    static const bool off = [] { char b[8] = {}; return GetEnvironmentVariableA("TBD_NODISPLACE", b, 8) > 0 && b[0] == '1'; }();
    if (off || !bake) return;
    const tbd::Vec3 focus = g_engine.camera.Eye();
    const float snap = 2.0f * kGridHalf / (kGrid - 1) * 10.0f;
    const float cx = std::round(focus.x / snap) * snap, cz = std::round(focus.z / snap) * snap;
    std::lock_guard<std::mutex> lock(st->mutex);
    if (st->gridReady) {
        st->gridReady = false;
        if (!st->gridIndices.empty()) {
            if (gGridMesh != tbd::kInvalidMesh) { gRetired.push_back(gGridMesh); if (gRetired.size() > 4) { g_engine.renderer->DestroyMesh(gRetired.front()); gRetired.erase(gRetired.begin()); } }
            gGridMesh = g_engine.renderer->CreateMeshNoWait(st->gridVerts.data(), (uint32_t)st->gridVerts.size(), st->gridIndices.data(), (uint32_t)st->gridIndices.size());
        }
    }
    if (!st->gridBusy && (std::abs(cx - gGridX) > 2.5f || std::abs(cz - gGridZ) > 2.5f)) {
        st->gridBusy = true;
        gGridX = cx; gGridZ = cz;
        g_engine.jobs->RunBackground([st, bake, cx, cz] { BuildGrid(st, bake, cx, cz); });
    }
}

// Captures wait for the bake and first grid, so they show the finished ground.
inline bool Settled() {
    static const bool off = [] { char b[8] = {}; return GetEnvironmentVariableA("TBD_NODISPLACE", b, 8) > 0 && b[0] == '1'; }();
    if (GroundTextures().empty() || !gStarted) return GroundTextures().empty();
    return gSurface != tbd::kNoTexture && (off || gGridMesh != tbd::kInvalidMesh);
}

// Inside the scene pass, with useMaterial given the ground's id so the atlas binds.
template <class UseMaterial>
inline void DrawGrid(tbd::Renderer& r, const tbd::Mat4& viewProj, UseMaterial&& useMaterial) {
    if (gGridMesh == tbd::kInvalidMesh || gSurface == tbd::kNoTexture || GroundTextures().empty()) return;
    useMaterial(*GroundTextures().begin(), tbd::kNoTexture, tbd::kNoTexture);
    r.Draw(gGridMesh, viewProj, tbd::Mat4::Identity(), true, true);
}

} // namespace ground_bake
