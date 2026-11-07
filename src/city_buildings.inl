// ------------------------------------------------------------------- city buildings (Track R, phases R1-R4, R8)
// Included by main.cpp after city_stress.inl. Zoned buildings are no longer scene entities: every lot is a row of a per-model
// instanced draw, and how a lot is drawn is decided per 128 m CityCell (city/city_cells.h), from the cell's size on screen:
//   Near   the real meshes: one GPU mesh per model path (uploaded once), instances gathered per frame, one draw per material part
//          of each model in view (R1). They cast shadows (DrawShadowInstancedTransient).
//   Mid    the cell's chunk bake (R2, five orthographic faces in the chunk atlas) drawn by ChunkVolPS as one box per cell, all such
//   Far    cells in one draw (R4); or, with TBD_CHUNK_FAR=merged, the cell's merged proxy: every building in one simplified mesh
//          coloured by its textures' averages (R8, mesh_lod.h), one draw per cell. A cell whose bake or proxy is not ready yet
//          stays Near.
// The tier has ~12% hysteresis. Bakes are scheduled (R3): the cells that need one, biggest on screen first; per frame the top and
// the side facing the camera of one cell, else one remaining face, at most ~1 ms of recording and never while the camera moves fast;
// a finished bake is written to cache\chunks keyed by the cell's content, and read back from there instead of baking again.
//
// Volumes come in three tiers from the same bake (R5): high (Mid cells: 24 march steps, 4 bisections, the full shadow filter), medium
// (Far cells: 12 and 2) and low (Far cells under TBD_CHUNK_LOW_PX, 45: 6 and 1, one shadow tap); TBD_CHUNK_QUALITY=low|medium|high
// caps them. A cell that changes representation shows the old and the new one for TBD_CHUNK_FADE_MS (350), each hiding the pixels the
// other shows (R6, Renderer::SetDrawFade). The governor (R7) keeps the real meshes within TBD_CHUNK_DRAW_BUDGET draws (80) and
// TBD_CHUNK_TRI_BUDGET triangles (300k): Near cells biggest on screen first, the rest drawn as volumes; the camera's own cell always Near.
// Debug: TBD_CHUNK_FORCE=near|vol|merged forces every cell (Ctrl+Shift+B cycles it at run time); TBD_CHUNK_NEAR_PX / TBD_CHUNK_MID_PX the tier thresholds (pixels of the
// cell's radius); TBD_CHUNK_TILE (64) and TBD_CHUNK_ATLAS (4096) the atlas; TBD_CHUNK_FACES (2) faces per frame; TBD_ZONE_ENTITIES=1
// the old path (one scene entity per lot, for comparisons). TBD_CHUNK_CHECK=1 with TBD_CITY_STRESS: Checkpoint B (CityChunkCheck).

// (its headers, <future> and render/chunk_faces.h among them, are included at the top of main.cpp: this file sits in its anonymous namespace)

namespace city_bld {
using tbd::Vec3;

enum Tier : uint8_t { kNear, kMid, kFar };
enum Mode : int { kAuto = 0, kModeNear, kModeVol, kModeMerged };

inline int EnvI(const char* name, int fallback) {
    char v[32] = {};
    return GetEnvironmentVariableA(name, v, sizeof(v)) > 0 ? atoi(v) : fallback;
}
inline int forceRuntime = -2; // -2: not read yet; CityBuildingsCycleForce changes it
inline int ForcedMode() {
    if (forceRuntime != -2) return forceRuntime;
    return forceRuntime = [] {
        char v[16] = {};
        if (GetEnvironmentVariableA("TBD_CHUNK_FORCE", v, sizeof(v)) == 0) return (int)kAuto;
        const std::string s = v;
        return s == "near" ? (int)kModeNear : s == "vol" ? (int)kModeVol : s == "merged" ? (int)kModeMerged : (int)kAuto;
    }();
}
// The highest volume tier (R5): TBD_CHUNK_QUALITY=low|medium|high (default high; phones want medium).
inline int QualityCap() {
    static const int q = [] {
        char v[16] = {};
        if (GetEnvironmentVariableA("TBD_CHUNK_QUALITY", v, sizeof(v)) == 0) return 2;
        const std::string s = v;
        return s == "low" ? 0 : s == "medium" ? 1 : 2;
    }();
    return q;
}
inline bool FarMerged() {
    static const bool on = [] { char v[16] = {}; return GetEnvironmentVariableA("TBD_CHUNK_FAR", v, sizeof(v)) > 0 && std::string(v) == "merged"; }();
    return on;
}

struct Model {
    std::string path;          // the GLB, without "#native"
    tbd::MeshId mesh = tbd::kInvalidMesh; // UploadEntityMesh's handle; textured models draw their matParts
    Vec3 half{4, 4, 4};
    float minY = 0.0f, maxY = 8.0f, reach = 6.0f; // measured from its own vertices: the bake's box must hold all of it
    int parts = 1;      // draws one instanced batch of it costs (its material parts), for the governor
    uint32_t tris = 0;  // triangles of one instance
    bool ok = false;
};
inline std::vector<Model> models;
inline std::unordered_map<std::string, int> modelOf;

struct Rec {
    int model = -1, cell = -1;
    float row[12] = {};
    float sy = -1.0f;   // the construction's height scale (0 hidden, 1 built)
    bool built = false;
    uint32_t gen = 0;
};
inline std::unordered_map<uint64_t, Rec> recs;
inline uint32_t gen = 0;

struct Cell {
    tbd::city::CellKey key;
    std::vector<uint64_t> lots;
    bool dirty = true;
    Vec3 lo{}, hi{};       // what the built lots fill (a little more than the 128 m square where they reach over it)
    uint64_t content = 0;  // hash of the built lots' models and placements: what a bake or a proxy shows
    int built = 0;
    uint8_t tier = kFar;
    float px = 0.0f;
    bool visible = false;
    int force = kAuto;     // per-cell override (the checkpoint)
    int forceQ = -1;       // per-cell volume tier override (the checkpoint)
    int quality = 2;       // the volume tier this frame (R5): 0 low, 1 medium, 2 high
    bool govNear = false;  // the governor (R7) lets this Near cell draw its real meshes
    // the dithered transition (R6): what it shows now, what it showed before, and how far the change has got (1 = done)
    int shownAs = -1, fadeFrom = -1;
    float fadeT = 1.0f;
    // the chunk bake
    int slot = -1;
    uint64_t bakedFor = 0;
    uint8_t faces = 0;     // bit f: face f holds bakedFor
    bool saved = false;
    uint64_t lastUse = 0;
    // the merged proxy
    tbd::MeshId merged = tbd::kInvalidMesh;
    uint64_t mergedFor = 0;
    uint32_t mergedTris = 0;
};
inline std::vector<Cell> cells;
inline std::unordered_map<uint64_t, int> cellOf;
inline int isolate = -1; // the checkpoint: only this cell's buildings are drawn

inline const tbd::Renderer* owner = nullptr;
inline uint64_t frame = 0;
inline bool atlasOk = false, atlasTried = false;
inline uint32_t tile = 64, atlasW = 4096, atlasH = 4096, slotCols = 0, slotRows = 0;
inline std::vector<int> slotCell; // which cell owns each slot (-1 free)
inline tbd::MeshId volMesh = tbd::kInvalidMesh;
inline uint64_t volKey = 0;
inline uint32_t volCells = 0;
inline Vec3 lastEye{};
inline bool cameraFast = false;
// The per-frame instance buckets: rows per model, for the camera (frustum culled) and the sun (every Near lot).
inline std::vector<std::vector<float>> camRows, sunRows;
inline uint64_t sunKey = 0;
inline std::vector<std::pair<int, float>> mergedDraw; // cells drawn as merged proxies this frame, with their fade
struct FadeBucket { int model; float keep; std::vector<float> rows; };
inline std::vector<FadeBucket> fadeRows; // Near lots of cells in a transition, by model and (quantized) fade
inline tbd::MeshId fadeVolMesh = tbd::kInvalidMesh; // this frame's boxes of volume cells in a transition
inline uint32_t statGovDenied = 0, statFading = 0;
// numbers for the log and the checkpoint
inline std::unordered_map<uint64_t, bool> missing; // content hashes known to have no cache file (the bake waits for this answer)
inline uint32_t statNear = 0, statVol = 0, statMerged = 0, statBakedFaces = 0, statCacheLoads = 0, statCacheSaves = 0;
inline double statBakeMs = 0.0;

inline uint64_t Mix(uint64_t h, const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t k = 0; k < bytes; ++k) h = (h ^ p[k]) * 1099511628211ull;
    return h;
}

inline void Reset() {
    models.clear();
    modelOf.clear();
    recs.clear();
    cells.clear();
    cellOf.clear();
    slotCell.clear();
    volMesh = tbd::kInvalidMesh;
    volKey = 0;
    atlasOk = atlasTried = false;
}

inline int ModelFor(const std::string& path) {
    const auto it = modelOf.find(path);
    if (it != modelOf.end()) return it->second;
    Model m;
    m.path = path;
    tbd::Entity e;
    e.mesh = tbd::MeshKind::Imported;
    e.meshPath = path + "#native";
    e.color = {1, 1, 1};
    if (!ImportedVertices(e.meshPath).empty()) { // the one upload this model ever gets, however many lots show it
        m.mesh = UploadEntityMesh(e);
        m.ok = m.mesh != tbd::kInvalidMesh;
        const auto h = tbd::ImportedHalfExtents().find(e.meshPath);
        if (h != tbd::ImportedHalfExtents().end()) m.half = h->second;
        float lo = 1e9f, hi = -1e9f, r2 = 0.0f;
        for (const tbd::Vertex& v : ImportedVertices(e.meshPath)) lo = std::min(lo, v.py), hi = std::max(hi, v.py), r2 = std::max(r2, v.px * v.px + v.pz * v.pz);
        m.minY = lo, m.maxY = hi, m.reach = std::sqrt(r2);
        if (m.ok) {
            const auto parts = g_engine.matParts.find(m.mesh);
            if (parts == g_engine.matParts.end()) m.tris = (uint32_t)(ImportedVertices(e.meshPath).size() / 3);
            else {
                m.parts = 0;
                for (const MatPart& p : parts->second)
                    if (p.lodError == 0.0f) ++m.parts, m.tris += g_engine.renderer->MeshIndexCount(p.mesh) / 3;
                m.parts = std::max(m.parts, 1);
            }
        }
    }
    models.push_back(m);
    return modelOf[path] = (int)models.size() - 1;
}

inline int CellFor(float x, float z) {
    const tbd::city::CellKey k = tbd::city::CellKeyAt(x, z);
    const uint64_t p = tbd::city::PackKey(k);
    const auto it = cellOf.find(p);
    if (it != cellOf.end()) return it->second;
    Cell c;
    c.key = k;
    cells.push_back(c);
    return cellOf[p] = (int)cells.size() - 1;
}

// ZonesPlace's matrix as an instance row (float3x4 row by row).
inline void LotRow(const tbd::city::Lot& l, float sy, float* row) {
    const tbd::Mat4 m = tbd::TRS({l.c.x, l.y, l.c.z}, tbd::QuatAxisAngle({0, 1, 0}, std::atan2(l.front.x, l.front.z)), {1, std::max(sy, 0.02f), 1});
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) row[r * 4 + c] = m.At(r, c);
}

inline void Refresh(Cell& c) {
    c.dirty = false;
    c.built = 0;
    c.lo = {(float)c.key.x * tbd::city::kCityCellSize, 1e9f, (float)c.key.z * tbd::city::kCityCellSize};
    c.hi = {c.lo.x + tbd::city::kCityCellSize, -1e9f, c.lo.z + tbd::city::kCityCellSize};
    std::vector<uint64_t> ids = c.lots;
    std::sort(ids.begin(), ids.end());
    uint64_t h = 14695981039346656037ull;
    for (uint64_t id : ids) {
        const Rec& r = recs[id];
        if (!r.built || r.model < 0 || !models[(size_t)r.model].ok) continue;
        const Model& m = models[(size_t)r.model];
        ++c.built;
        h = Mix(h, m.path.data(), m.path.size());
        h = Mix(h, r.row, sizeof(r.row));
        const float reach = m.reach;
        const float x = r.row[3], y = r.row[7], z = r.row[11];
        c.lo = {std::min(c.lo.x, x - reach), std::min(c.lo.y, y + m.minY - 0.5f), std::min(c.lo.z, z - reach)};
        c.hi = {std::max(c.hi.x, x + reach), std::max(c.hi.y, y + m.maxY + 0.5f), std::max(c.hi.z, z + reach)};
    }
    if (c.built == 0) c.lo.y = 0, c.hi.y = 1;
    c.content = c.built ? h : 0;
}

inline uint32_t AllocSlot(int cell) {
    for (uint32_t s = 0; s < slotCell.size(); ++s)
        if (slotCell[s] < 0) { slotCell[s] = cell; return s; }
    // full: the least recently used cell gives its slot up
    uint32_t best = 0;
    uint64_t oldest = ~0ull;
    for (uint32_t s = 0; s < slotCell.size(); ++s) {
        const Cell& o = cells[(size_t)slotCell[s]];
        if (o.lastUse < oldest && o.lastUse + 2 < frame) oldest = o.lastUse, best = s;
    }
    if (oldest == ~0ull) return UINT32_MAX;
    Cell& o = cells[(size_t)slotCell[best]];
    o.slot = -1, o.faces = 0, o.bakedFor = 0, o.saved = false;
    slotCell[best] = cell;
    return best;
}
inline void SlotTexel(int slot, uint32_t& x, uint32_t& y) {
    x = (uint32_t)(slot % (int)slotCols) * tile * tbd::kChunkFaceCount;
    y = (uint32_t)(slot / (int)slotCols) * tile;
}

// ---- the disk cache (R3): the three channels of a cell's strip, keyed by what it shows
inline std::wstring CacheFile(const Cell& c) {
    static const std::wstring dir = [] {
        std::wstring d = tbd::ExeDirectory() + L"\\..\\cache";
        CreateDirectoryW(d.c_str(), nullptr);
        d += L"\\chunks";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    constexpr uint32_t kBakeVersion = 2; // bump when the bake's look changes
    uint64_t h = Mix(c.content, &tile, sizeof(tile));
    h = Mix(h, &kBakeVersion, sizeof(kBakeVersion));
    wchar_t name[40];
    swprintf(name, 40, L"\\%016llx.chunk", (unsigned long long)h);
    return dir + name;
}
inline bool CacheLoad(tbd::Renderer& r, Cell& c) {
    FILE* f = _wfopen(CacheFile(c).c_str(), L"rb");
    if (!f) return false;
    const size_t bytes = (size_t)tile * tbd::kChunkFaceCount * tile * 4;
    std::vector<uint8_t> data(bytes * 3);
    const bool read = fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!read) return false;
    uint32_t x, y;
    SlotTexel(c.slot, x, y);
    for (int ch = 0; ch < 3; ++ch)
        if (!r.WriteChunkAtlas(ch, x, y, tile * tbd::kChunkFaceCount, tile, data.data() + bytes * (size_t)ch)) return false;
    c.faces = 0x1F, c.bakedFor = c.content, c.saved = true;
    ++statCacheLoads;
    return true;
}
inline bool ReadStrip(tbd::Renderer& r, const Cell& c, std::vector<uint8_t> (&out)[3]) {
    uint32_t x, y;
    SlotTexel(c.slot, x, y);
    for (int ch = 0; ch < 3; ++ch)
        if (!r.ReadChunkAtlas(ch, x, y, tile * tbd::kChunkFaceCount, tile, out[ch])) return false;
    return true;
}
inline void CacheSave(tbd::Renderer& r, Cell& c) {
    std::vector<uint8_t> ch[3];
    c.saved = true;
    if (!ReadStrip(r, c, ch)) { tbd::Log("City buildings: could not read cell %d,%d back for the cache", c.key.x, c.key.z); return; }
    const std::wstring file = CacheFile(c);
    FILE* f = _wfopen((file + L".tmp").c_str(), L"wb");
    if (!f) return;
    for (const auto& v : ch) fwrite(v.data(), 1, v.size(), f);
    fclose(f);
    MoveFileExW((file + L".tmp").c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    ++statCacheSaves;
}

// ---- the merged proxy (R8): every built lot of the cell in one mesh, coloured by its textures' averages, simplified
inline const tbd::Vec3& ImageAverage(const std::string& key, const tbd::ImportedModel& model, int image) {
    static std::unordered_map<std::string, tbd::Vec3> avg;
    const std::string k = key + "#" + std::to_string(image);
    const auto it = avg.find(k);
    if (it != avg.end()) return it->second;
    tbd::Vec3 a{0.7f, 0.7f, 0.7f};
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (image >= 0 && image < (int)model.images.size() && tbd::DecodeImage(model.images[(size_t)image].data(), model.images[(size_t)image].size(), px, w, h) && w > 0 && h > 0) {
        double s[3] = {};
        const size_t n = (size_t)w * h, step = std::max<size_t>(1, n / 4096);
        size_t count = 0;
        for (size_t i = 0; i < n; i += step, ++count)
            for (int c = 0; c < 3; ++c) s[c] += px[i * 4 + c] / 255.0;
        if (count) a = {(float)(s[0] / count), (float)(s[1] / count), (float)(s[2] / count)};
    }
    return avg[k] = a;
}
struct MergeJob {
    int cell = -1;
    uint64_t content = 0;
    std::future<std::pair<std::vector<tbd::Vertex>, std::vector<uint32_t>>> result;
    double gatherMs = 0.0;
    uint32_t sourceTris = 0;
};
inline MergeJob mergeJob;
inline uint32_t MergedTarget() { static const uint32_t t = (uint32_t)std::max(200, EnvI("TBD_CHUNK_MERGED_TRIS", 4000)); return t; }
inline void StartMerge(int ci) {
    Cell& c = cells[(size_t)ci];
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    std::vector<tbd::Vertex> flat;
    for (uint64_t id : c.lots) {
        const Rec& r = recs[id];
        if (!r.built || r.model < 0 || !models[(size_t)r.model].ok) continue;
        const std::string key = models[(size_t)r.model].path + "#native";
        const std::vector<tbd::Vertex>& v = ImportedVertices(key);
        const tbd::ImportedModel& model = ImportedModels()[key];
        const float* m = r.row;
        for (size_t p = 0; p < model.parts.size(); ++p) {
            const tbd::ImportedPart& part = model.parts[p];
            if (part.hair || part.count == 0) continue;
            const tbd::Vec3 avg = ImageAverage(key, model, part.albedo);
            for (size_t k = part.first; k < part.first + part.count && k < v.size(); ++k) {
                tbd::Vertex x = v[k];
                const float a = x.a;
                tbd::Vec3 col = avg;
                if (std::fabs(a - 0.665f) < 0.003f || std::fabs(a - 0.70f) < 0.003f || std::fabs(a - 0.645f) < 0.003f) col = {0.16f, 0.19f, 0.23f};
                else if (!(a > 0.79f && a < 0.825f)) col = {avg.x * std::clamp(x.r, 0.0f, 1.0f), avg.y * std::clamp(x.g, 0.0f, 1.0f), avg.z * std::clamp(x.b, 0.0f, 1.0f)};
                const tbd::Vec3 p0{x.px, x.py, x.pz}, n0{x.nx, x.ny, x.nz};
                x.px = m[0] * p0.x + m[1] * p0.y + m[2] * p0.z + m[3];
                x.py = m[4] * p0.x + m[5] * p0.y + m[6] * p0.z + m[7];
                x.pz = m[8] * p0.x + m[9] * p0.y + m[10] * p0.z + m[11];
                tbd::Vec3 n{m[0] * n0.x + m[1] * n0.y + m[2] * n0.z, m[4] * n0.x + m[5] * n0.y + m[6] * n0.z, m[8] * n0.x + m[9] * n0.y + m[10] * n0.z};
                const float len = std::sqrt(tbd::Dot(n, n));
                if (len > 1e-6f) n = n * (1.0f / len);
                x.nx = n.x, x.ny = n.y, x.nz = n.z;
                x.r = col.x, x.g = col.y, x.b = col.z;
                x.a = 0.8997f; // PbrFromAlpha: roughness 0.8, not metal
                x.u = x.v = 0.0f;
                flat.push_back(x);
            }
        }
    }
    QueryPerformanceCounter(&t1);
    mergeJob.cell = ci;
    mergeJob.content = c.content;
    mergeJob.gatherMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / g_engine.tickFreq.QuadPart;
    mergeJob.sourceTris = (uint32_t)(flat.size() / 3);
    const float radius = 0.5f * std::sqrt(tbd::Dot(c.hi - c.lo, c.hi - c.lo));
    const uint32_t target = MergedTarget();
    mergeJob.result = std::async(std::launch::async, [flat = std::move(flat), radius, target]() mutable {
        std::vector<tbd::Vertex> v;
        std::vector<uint32_t> idx;
        tbd::WeldVertices(flat.data(), (uint32_t)flat.size(), v, idx);
        if (idx.size() / 3 <= target) return std::make_pair(std::move(v), std::move(idx));
        // Colours split vertices that share a position: weld by position for the simplifier, so walls of one colour collapse.
        std::vector<tbd::LodLevel> levels = tbd::BuildLodChain(v, idx, 0.5f, radius);
        if (levels.empty()) levels = tbd::BuildPieceLevels(v, idx, radius);
        for (const tbd::LodLevel& l : levels)
            if (l.indices.size() / 3 <= target) return std::make_pair(l.vertices, l.indices);
        if (!levels.empty()) return std::make_pair(levels.back().vertices, levels.back().indices);
        return std::make_pair(std::move(v), std::move(idx));
    });
}
inline void PollMerge(tbd::Renderer& r) {
    if (mergeJob.cell < 0 || !mergeJob.result.valid()) return;
    if (mergeJob.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto [v, idx] = mergeJob.result.get();
    Cell& c = cells[(size_t)mergeJob.cell];
    if (c.merged != tbd::kInvalidMesh) r.RetireMesh(c.merged);
    c.merged = v.empty() ? tbd::kInvalidMesh : r.CreateMeshNoWait(v.data(), (uint32_t)v.size(), idx.data(), (uint32_t)idx.size());
    c.mergedFor = mergeJob.content;
    c.mergedTris = (uint32_t)(idx.size() / 3);
    tbd::Log("City buildings: merged proxy of cell %d,%d: %u -> %u triangles (gather %.1f ms on the main thread)", c.key.x, c.key.z, mergeJob.sourceTris,
             c.mergedTris, mergeJob.gatherMs);
    mergeJob.cell = -1;
}

inline int ModeOf(const Cell& c) {
    const int f = c.force != kAuto ? c.force : ForcedMode();
    if (f != kAuto) return f;
    if (c.tier == kNear && c.govNear) return kModeNear;
    return FarMerged() || !atlasOk ? kModeMerged : kModeVol;
}
inline bool VolReady(const Cell& c) { return atlasOk && c.slot >= 0 && c.bakedFor == c.content && (c.faces & 1) && (c.faces & 0x1E); }
inline bool MergedReady(const Cell& c) { return c.merged != tbd::kInvalidMesh && c.mergedFor == c.content; }
// What the cell is drawn as this frame: its mode, unless that one is not ready (then the real meshes).
inline int DrawnAs(const Cell& c) {
    const int m = ModeOf(c);
    if (m == kModeVol && !VolReady(c)) return kModeNear;
    if (m == kModeMerged && !MergedReady(c)) return kModeNear;
    return m;
}

// ChunkVolVS reads the tier and the fade from the vertex alpha: 0.851 + 0.001 * quality + 0.00045 * (fade + 1).
inline float VolCode(int quality, float fade) { return 0.851f + 0.001f * (float)quality + 0.00045f * (std::clamp(fade, -1.0f, 1.0f) + 1.0f); }
inline void PushBox(std::vector<tbd::Vertex>& v, std::vector<uint32_t>& idx, const Cell& c, float fade = 1.0f) {
    uint32_t sx, sy;
    SlotTexel(c.slot, sx, sy);
    const Vec3 lo = c.lo, size = c.hi - c.lo;
    const uint32_t base = (uint32_t)v.size();
    for (int k = 0; k < 8; ++k) {
        tbd::Vertex x{};
        x.px = lo.x + ((k & 1) ? size.x : 0.0f);
        x.py = lo.y + ((k & 2) ? size.y : 0.0f);
        x.pz = lo.z + ((k & 4) ? size.z : 0.0f);
        x.nx = lo.x, x.ny = lo.y, x.nz = lo.z;
        x.r = size.x, x.g = size.y, x.b = size.z, x.a = VolCode(c.quality, fade);
        x.u = (float)sx, x.v = (float)sy;
        v.push_back(x);
    }
    static const uint32_t q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : q)
        for (uint32_t t : {f[0], f[1], f[2], f[0], f[2], f[3]}) idx.push_back(base + t);
}

} // namespace city_bld

// Before the frame opens (after ZonesSync, with this frame's camera): lots into cells, tiers, the cache, proxies, instance buckets.
void CityBuildingsSync(tbd::Renderer& r, const tbd::Mat4& viewProj, const tbd::Vec3& eye) {
    using namespace city_bld;
    if (g_zoneEntities) return;
    if (owner != &r) { Reset(); owner = &r; }
    ++frame;
    if (!atlasTried) {
        atlasTried = true;
        tile = (uint32_t)std::clamp(EnvI("TBD_CHUNK_TILE", 64), 16, 512);
        atlasW = atlasH = (uint32_t)std::clamp(EnvI("TBD_CHUNK_ATLAS", 4096), 512, 16384);
        atlasOk = r.CreateChunkAtlas(tile, atlasW, atlasH);
        slotCols = atlasW / (tile * tbd::kChunkFaceCount);
        slotRows = atlasH / tile;
        slotCell.assign(atlasOk ? (size_t)slotCols * slotRows : 0, -1);
        if (!atlasOk) tbd::Log("City buildings: no chunk atlas on %s: far cells use merged proxies", r.Name());
    }
    // ---- lots into records and cells
    ++gen;
    for (const tbd::city::Lot& l : g_zones.lots) {
        if (!l.alive) continue;
        auto it = recs.find(l.id);
        if (it == recs.end()) {
            Rec rec;
            rec.cell = CellFor(l.c.x, l.c.z);
            cells[(size_t)rec.cell].lots.push_back(l.id);
            it = recs.emplace(l.id, rec).first;
        }
        Rec& rec = it->second;
        rec.gen = gen;
        if (rec.model < 0 || models[(size_t)rec.model].path != l.path) { // new, or levelled to another model
            rec.model = ModelFor(l.path);
            rec.sy = -1.0f;
        }
        if (rec.sy == 1.0f && l.Built()) continue; // settled: nothing moves
        const float sy = l.Built() ? 1.0f : ZoneBuildScale(l, ZoneModelHeight(l.path));
        if (sy != rec.sy) {
            LotRow(l, sy, rec.row);
            const bool wasBuilt = rec.built;
            rec.sy = sy;
            rec.built = sy >= 1.0f;
            if (rec.built || wasBuilt) cells[(size_t)rec.cell].dirty = true;
        }
    }
    for (auto it = recs.begin(); it != recs.end();) {
        if (it->second.gen == gen) { ++it; continue; }
        Cell& c = cells[(size_t)it->second.cell];
        c.lots.erase(std::remove(c.lots.begin(), c.lots.end(), it->first), c.lots.end());
        c.dirty = true;
        it = recs.erase(it);
    }
    // ---- tiers: the cell's radius on screen, with hysteresis
    const tbd::ViewVolume view = tbd::ViewVolume::FromMatrix(viewProj, false);
    const float pixelScale = (float)std::max(g_engine.vpH, 1) / (2.0f * std::tan(g_engine.camera.fovY * 0.5f));
    static const float nearPx = (float)EnvI("TBD_CHUNK_NEAR_PX", 420), midPx = (float)EnvI("TBD_CHUNK_MID_PX", 140);
    const float moved = std::sqrt(tbd::Dot(eye - lastEye, eye - lastEye));
    cameraFast = moved > std::max(2.0f, g_engine.camera.distance * 0.02f); // more than 2% of the view's distance in a frame
    lastEye = eye;
    for (size_t ci = 0; ci < cells.size(); ++ci) {
        Cell& c = cells[ci];
        if (c.dirty) Refresh(c);
        if (c.lots.empty()) { c.visible = false; continue; }
        const Vec3 mid = (c.lo + c.hi) * 0.5f, half = (c.hi - c.lo) * 0.5f;
        const float radius = std::sqrt(tbd::Dot(half, half));
        c.visible = (isolate < 0 || (int)ci == isolate) && view.Touches({mid, radius});
        const float d = std::max(std::sqrt(tbd::Dot(mid - eye, mid - eye)) - radius * 0.5f, 1.0f);
        c.px = radius * pixelScale / d;
        constexpr float kH = 1.12f;
        const uint8_t was = c.tier;
        uint8_t t = c.px >= nearPx ? kNear : c.px >= midPx ? kMid : kFar;
        if (was == kNear && t != kNear && c.px >= nearPx / kH) t = kNear;
        if (was != kNear && t == kNear && c.px < nearPx * kH) t = was;
        if (was == kMid && t == kFar && c.px >= midPx / kH) t = kMid;
        if (was == kFar && t == kMid && c.px < midPx * kH) t = kFar;
        c.tier = t;
        if (c.visible) c.lastUse = frame;
    }
    // ---- the bakes' disk cache: before any bake of the frame (a bake that is already on disk is never redone), loads biggest on
    // screen first for up to ~2 ms; one save a frame while the camera is still
    PollMerge(r);
    if (atlasOk) {
        static std::vector<int> want;
        want.clear();
        int saveBest = -1;
        for (size_t ci = 0; ci < cells.size(); ++ci) {
            const Cell& c = cells[ci];
            if (!c.visible || !c.built || ModeOf(c) != kModeVol) continue;
            if (c.bakedFor != c.content || c.faces != 0x1F) { if (!missing.count(c.content)) want.push_back((int)ci); }
            else if (!c.saved && saveBest < 0) saveBest = (int)ci;
        }
        std::sort(want.begin(), want.end(), [](int a, int b) { return cells[(size_t)a].px > cells[(size_t)b].px; });
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        for (int ci : want) {
            QueryPerformanceCounter(&t1);
            if ((t1.QuadPart - t0.QuadPart) * 1000.0 / g_engine.tickFreq.QuadPart > 2.0) break;
            Cell& c = cells[(size_t)ci];
            if (c.slot < 0) { const uint32_t s = AllocSlot(ci); if (s == UINT32_MAX) break; c.slot = (int)s; c.faces = 0; c.bakedFor = 0; }
            if (!CacheLoad(r, c)) missing[c.content] = true;
        }
        static uint32_t saveSkipsFast = 0;
        if (saveBest >= 0 && cameraFast && ++saveSkipsFast % 300 == 0) tbd::Log("City buildings: %u cache saves put off while the camera moved", saveSkipsFast);
        if (saveBest >= 0 && !cameraFast) {
            CacheSave(r, cells[(size_t)saveBest]);
            missing.erase(cells[(size_t)saveBest].content);
        }
    }
    // ---- merged proxies: one at a time, on a worker
    if (mergeJob.cell < 0) {
        int best = -1;
        for (size_t ci = 0; ci < cells.size(); ++ci) {
            const Cell& c = cells[ci];
            if (c.visible && c.built && ModeOf(c) == kModeMerged && !MergedReady(c) && (best < 0 || c.px > cells[(size_t)best].px)) best = (int)ci;
        }
        if (best >= 0) StartMerge(best);
    }
    // ---- the governor (R7): Near cells, biggest on screen first (the ones already Near a little ahead, so the choice holds), get
    // their real meshes while the zoned draws and triangles stay within budget; the cell the camera stands in always does
    static const int drawBudget = EnvI("TBD_CHUNK_DRAW_BUDGET", 80), triBudget = EnvI("TBD_CHUNK_TRI_BUDGET", 300000);
    static const int qCap = QualityCap();
    static const float lowPx = (float)EnvI("TBD_CHUNK_LOW_PX", 45);
    {
        static std::vector<int> want;
        static std::vector<uint8_t> modelIn;
        static std::vector<int> fresh;
        want.clear();
        modelIn.assign(models.size(), 0);
        for (size_t ci = 0; ci < cells.size(); ++ci) {
            Cell& c = cells[ci];
            c.govNear = false;
            c.quality = c.forceQ >= 0 ? c.forceQ : std::min(qCap, c.tier == kFar && c.px < lowPx ? 0 : c.tier == kFar ? 1 : 2);
            if (c.visible && c.tier == kNear && !c.lots.empty()) want.push_back((int)ci);
        }
        const auto priority = [](const Cell& c) { return c.px * (c.shownAs == kModeNear ? 1.12f : 1.0f); };
        std::sort(want.begin(), want.end(), [&](int a, int b) { return priority(cells[(size_t)a]) > priority(cells[(size_t)b]); });
        int draws = 0;
        int64_t tris = 0;
        statGovDenied = 0;
        for (int ci : want) {
            Cell& c = cells[(size_t)ci];
            int addDraws = 0;
            int64_t addTris = 0;
            fresh.clear();
            for (uint64_t id : c.lots) {
                const Rec& rec = recs[id];
                if (rec.model < 0 || !models[(size_t)rec.model].ok || rec.sy <= 0.0f) continue;
                const Model& m = models[(size_t)rec.model];
                addTris += m.tris;
                if (!modelIn[(size_t)rec.model]) modelIn[(size_t)rec.model] = 2, fresh.push_back(rec.model), addDraws += m.parts;
            }
            const bool camIn = eye.x >= c.lo.x && eye.x <= c.hi.x && eye.z >= c.lo.z && eye.z <= c.hi.z;
            if (camIn || draws == 0 || (draws + addDraws <= drawBudget && tris + addTris <= triBudget)) {
                c.govNear = true;
                draws += addDraws, tris += addTris;
                for (int m : fresh) modelIn[(size_t)m] = 1;
            } else {
                for (int m : fresh) modelIn[(size_t)m] = 0;
                ++statGovDenied;
            }
        }
    }
    // ---- this frame's buckets and the volume meshes; a cell whose representation changed shows both for a moment, dithered (R6)
    static LARGE_INTEGER lastTick = {};
    LARGE_INTEGER tick;
    QueryPerformanceCounter(&tick);
    const float dt = lastTick.QuadPart ? std::min(0.1f, (float)((tick.QuadPart - lastTick.QuadPart) / (double)g_engine.tickFreq.QuadPart)) : 0.0f;
    lastTick = tick;
    static const float fadeSeconds = (float)EnvI("TBD_CHUNK_FADE_MS", 350) / 1000.0f;
    camRows.resize(models.size());
    sunRows.resize(models.size());
    for (auto& v : camRows) v.clear();
    for (auto& v : sunRows) v.clear();
    fadeRows.clear();
    static std::unordered_map<uint32_t, size_t> fadeIndex;
    fadeIndex.clear();
    mergedDraw.clear();
    statNear = statVol = statMerged = statFading = 0;
    uint64_t vk = 14695981039346656037ull, sk = 14695981039346656037ull;
    static std::vector<int> volList;
    static std::vector<std::pair<int, float>> fadeVol;
    volList.clear();
    fadeVol.clear();
    const auto available = [](const Cell& c, int as) { return as == kModeNear || (as == kModeVol && VolReady(c)) || (as == kModeMerged && MergedReady(c)); };
    for (size_t ci = 0; ci < cells.size(); ++ci) {
        Cell& c = cells[ci];
        if (c.lots.empty() || (isolate >= 0 && (int)ci != isolate)) continue;
        const int as = DrawnAs(c);
        if (!c.visible || c.shownAs < 0 || fadeSeconds <= 0.0f || isolate >= 0) c.shownAs = as, c.fadeT = 1.0f;
        else if (as != c.shownAs) c.fadeFrom = c.shownAs, c.shownAs = as, c.fadeT = 0.0f;
        else if (c.fadeT < 1.0f) c.fadeT = std::min(1.0f, c.fadeT + dt / fadeSeconds);
        const bool fading = c.fadeT < 1.0f && c.fadeFrom >= 0 && c.fadeFrom != c.shownAs && available(c, c.fadeFrom);
        const float t = std::clamp(c.fadeT, 0.002f, 0.998f);
        const std::pair<int, float> reps[2] = {{c.shownAs, fading ? t : 1.0f}, {fading ? c.fadeFrom : -1, -t}};
        statFading += fading;
        bool anyNear = false;
        float nearKeep = 1.0f;
        for (const auto& [rep, keep] : reps) {
            if (rep == kModeVol && c.visible) {
                if (keep >= 1.0f) volList.push_back((int)ci), vk = Mix(vk, &ci, sizeof(ci)), vk = Mix(vk, &c.content, sizeof(c.content)), vk = Mix(vk, &c.slot, sizeof(c.slot)), vk = Mix(vk, &c.quality, sizeof(c.quality));
                else fadeVol.push_back({(int)ci, keep});
                ++statVol;
            }
            if (rep == kModeMerged && c.visible) mergedDraw.push_back({(int)ci, keep}), ++statMerged;
            if (rep == kModeNear) anyNear = true, nearKeep = keep;
        }
        if (anyNear) sk = Mix(sk, &ci, sizeof(ci)), sk = Mix(sk, &c.content, sizeof(c.content)), statNear += c.visible;
        // a fade quantized to 32 steps, so the cells in a transition share their batches
        const float qKeep = nearKeep >= 1.0f ? 1.0f : std::clamp(std::round(nearKeep * 32.0f) / 32.0f, -0.99f, 0.99f);
        for (uint64_t id : c.lots) {
            const Rec& rec = recs[id];
            if (rec.model < 0 || !models[(size_t)rec.model].ok || rec.sy <= 0.0f) continue;
            if (!anyNear && rec.built) continue; // the bake or the proxy shows it
            sunRows[(size_t)rec.model].insert(sunRows[(size_t)rec.model].end(), rec.row, rec.row + 12);
            if (!rec.built) sk = Mix(sk, rec.row, sizeof(rec.row));
            if (!c.visible) continue;
            const Model& m = models[(size_t)rec.model];
            const float hy = 0.5f * (m.maxY - m.minY), reach = std::sqrt(m.reach * m.reach + hy * hy);
            if (!view.Touches({{rec.row[3], rec.row[7] + m.minY + hy, rec.row[11]}, reach})) continue;
            if (qKeep >= 1.0f || !rec.built) { camRows[(size_t)rec.model].insert(camRows[(size_t)rec.model].end(), rec.row, rec.row + 12); continue; }
            const uint32_t key = (uint32_t)rec.model * 128u + (uint32_t)(int)std::lround(qKeep * 32.0f + 64.0f);
            const auto [it, made] = fadeIndex.try_emplace(key, fadeRows.size());
            if (made) fadeRows.push_back({rec.model, qKeep, {}});
            fadeRows[it->second].rows.insert(fadeRows[it->second].rows.end(), rec.row, rec.row + 12);
        }
    }
    sunKey = sk;
    if (vk != volKey) {
        volKey = vk;
        if (volMesh != tbd::kInvalidMesh) r.RetireMesh(volMesh);
        volMesh = tbd::kInvalidMesh;
        std::vector<tbd::Vertex> v;
        std::vector<uint32_t> idx;
        for (int ci : volList) PushBox(v, idx, cells[(size_t)ci]);
        if (!v.empty()) volMesh = r.CreateMeshNoWait(v.data(), (uint32_t)v.size(), idx.data(), (uint32_t)idx.size());
        volCells = (uint32_t)volList.size();
    }
    if (fadeVolMesh != tbd::kInvalidMesh) r.RetireMesh(fadeVolMesh);
    fadeVolMesh = tbd::kInvalidMesh;
    if (!fadeVol.empty()) { // small and short-lived: remade every frame of a transition
        std::vector<tbd::Vertex> v;
        std::vector<uint32_t> idx;
        for (const auto& [ci, keep] : fadeVol) PushBox(v, idx, cells[(size_t)ci], keep);
        fadeVolMesh = r.CreateMeshNoWait(v.data(), (uint32_t)v.size(), idx.data(), (uint32_t)idx.size());
    }
}

// Ctrl+Shift+B (R6 debug): every cell as the tiers choose, all real meshes, all chunk volumes, all merged proxies, in turn.
void CityBuildingsCycleForce() {
    using namespace city_bld;
    static const char* const kName[4] = {"automatic", "real meshes", "chunk volumes", "merged proxies"};
    forceRuntime = (ForcedMode() + 1) % 4;
    g_engine.toast = std::string("Zoned buildings: ") + kName[forceRuntime];
    g_engine.toastUntil = GetTickCount64() + 2000;
    tbd::Log("City buildings: forced to %s", kName[forceRuntime]);
}

uint64_t CityBuildingsShadowKey() { return g_zoneEntities ? 0 : city_bld::sunKey; }

// Into the sun's shadow pass: every Near lot (and the ones still being built), one instanced caster per material part.
void CityBuildingsCasters(tbd::Renderer& r) {
    using namespace city_bld;
    if (g_zoneEntities) return;
    for (size_t m = 0; m < sunRows.size() && m < models.size(); ++m) {
        if (sunRows[m].empty()) continue;
        const uint32_t n = (uint32_t)(sunRows[m].size() / 12);
        const auto it = g_engine.matParts.find(models[m].mesh);
        if (it == g_engine.matParts.end()) { r.DrawShadowInstancedTransient(models[m].mesh, sunRows[m].data(), n); continue; }
        for (const MatPart& p : it->second)
            if (p.lodError == 0.0f && p.caster) r.DrawShadowInstancedTransient(p.mesh, sunRows[m].data(), n);
    }
}

namespace city_bld {
// One face of one cell into the atlas (inside a bake).
inline void BakeFace(tbd::Renderer& r, Cell& c, int face) {
    static std::vector<std::vector<float>> rows;
    static std::vector<tbd::Renderer::ChunkBakeItem> items;
    rows.assign(models.size(), {});
    items.clear();
    for (uint64_t id : c.lots) {
        const Rec& rec = recs[id];
        if (!rec.built || rec.model < 0 || !models[(size_t)rec.model].ok) continue;
        rows[(size_t)rec.model].insert(rows[(size_t)rec.model].end(), rec.row, rec.row + 12);
    }
    for (size_t m = 0; m < rows.size(); ++m) {
        if (rows[m].empty()) continue;
        const uint32_t n = (uint32_t)(rows[m].size() / 12);
        const auto it = g_engine.matParts.find(models[m].mesh);
        if (it == g_engine.matParts.end()) { items.push_back({models[m].mesh, tbd::kNoTexture, rows[m].data(), n}); continue; }
        for (const MatPart& p : it->second)
            if (p.lodError == 0.0f) items.push_back({p.mesh, p.albedo, rows[m].data(), n});
    }
    uint32_t x, y;
    SlotTexel(c.slot, x, y);
    r.ChunkBakeFace(items.data(), (uint32_t)items.size(), tbd::ChunkFaceMatrix(face, c.lo, c.hi - c.lo), x + (uint32_t)face * tile, y);
    c.faces |= (uint8_t)(1u << face);
    ++statBakedFaces;
}
inline int FacingSide(const Cell& c, const Vec3& eye) {
    const Vec3 mid = (c.lo + c.hi) * 0.5f;
    const float dx = eye.x - mid.x, dz = eye.z - mid.z;
    return std::fabs(dx) >= std::fabs(dz) ? (dx > 0 ? 1 : 2) : (dz > 0 ? 3 : 4);
}
inline bool bakeAll = false; // the checkpoint: every face of the cells that want one, now
} // namespace city_bld

// After the shadow passes (and the capture), before the scene: this frame's bakes (R3).
void CityBuildingsBake(tbd::Renderer& r, const tbd::Vec3& eye) {
    using namespace city_bld;
    if (g_zoneEntities || !atlasOk) return;
    if (cameraFast && !bakeAll) return;
    static const int perFrame = std::max(1, EnvI("TBD_CHUNK_FACES", 2));
    LARGE_INTEGER t0, now;
    QueryPerformanceCounter(&t0);
    const auto ms = [&] { QueryPerformanceCounter(&now); return (now.QuadPart - t0.QuadPart) * 1000.0 / g_engine.tickFreq.QuadPart; };
    bool open = false;
    int done = 0;
    for (int round = 0; round < 64 && (bakeAll || (done < perFrame && ms() < 1.0)); ++round) {
        // The cell most in need: biggest on screen among those whose top or facing side is missing; else one with any face missing.
        int best = -1;
        bool primary = false;
        for (size_t ci = 0; ci < cells.size(); ++ci) {
            const Cell& c = cells[ci];
            if (!c.visible || !c.built || ModeOf(c) != kModeVol) continue;
            const bool stale = c.bakedFor != c.content;
            if (stale && !missing.count(c.content)) continue; // the disk cache has not been asked yet (CityBuildingsSync does, first)
            const uint8_t have = stale ? 0 : c.faces;
            if (have == 0x1F) continue;
            const bool needPrimary = !(have & 1) || !(have & (1u << FacingSide(c, eye)));
            if (best < 0 || (needPrimary && !primary) || (needPrimary == primary && c.px > cells[(size_t)best].px)) best = (int)ci, primary = needPrimary;
        }
        if (best < 0) break;
        Cell& c = cells[(size_t)best];
        if (c.slot < 0) {
            const uint32_t s = AllocSlot(best);
            if (s == UINT32_MAX) break;
            c.slot = (int)s;
        }
        if (c.bakedFor != c.content) c.faces = 0, c.bakedFor = c.content, c.saved = false;
        if (!open) { if (!r.BeginChunkBake()) return; open = true; }
        int faceList[5] = {0, FacingSide(c, eye), -1, -1, -1};
        int n = 2;
        for (int f = 1; f < 5; ++f) if (f != faceList[1]) faceList[n++] = f;
        int baked = 0;
        for (int k = 0; k < 5; ++k) {
            const int f = faceList[k];
            if (c.faces & (1u << f)) continue;
            if (!bakeAll && baked > 0 && !primary) break;      // one remaining face a frame
            if (!bakeAll && primary && k >= 2) break;          // the two that show first
            BakeFace(r, c, f);
            ++baked, ++done;
        }
        if (!bakeAll && !primary) break;
    }
    if (open) r.EndChunkBake();
    if (done) statBakeMs += ms();
}

// The scene: Near instances, the chunk volumes, the merged proxies. Counted as ZONED BUILDINGS.
void CityBuildingsDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    using namespace city_bld;
    if (g_zoneEntities) return;
    for (size_t m = 0; m < camRows.size() && m < models.size(); ++m) {
        if (camRows[m].empty()) continue;
        const uint32_t n = (uint32_t)(camRows[m].size() / 12);
        const auto it = g_engine.matParts.find(models[m].mesh);
        if (it == g_engine.matParts.end()) {
            r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
            r.DrawInstancedTransient(models[m].mesh, camRows[m].data(), n, viewProj);
            continue;
        }
        for (const MatPart& p : it->second) {
            if (p.lodError != 0.0f) continue; // the full level only
            r.SetMaterial(p.albedo, p.normal, p.surface);
            r.DrawInstancedTransient(p.mesh, camRows[m].data(), n, viewProj);
        }
    }
    for (const FadeBucket& b : fadeRows) { // Near lots in a transition, screen-door faded against the other representation
        const uint32_t n = (uint32_t)(b.rows.size() / 12);
        const auto it = g_engine.matParts.find(models[(size_t)b.model].mesh);
        if (it == g_engine.matParts.end()) {
            r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
            r.SetDrawFade(b.keep);
            r.DrawInstancedTransient(models[(size_t)b.model].mesh, b.rows.data(), n, viewProj);
            continue;
        }
        for (const MatPart& p : it->second) {
            if (p.lodError != 0.0f) continue;
            r.SetMaterial(p.albedo, p.normal, p.surface);
            r.SetDrawFade(b.keep);
            r.DrawInstancedTransient(p.mesh, b.rows.data(), n, viewProj);
        }
    }
    r.SetDrawFade(0.0f);
    if (volMesh != tbd::kInvalidMesh) r.DrawChunkVolumes(volMesh, viewProj);
    if (fadeVolMesh != tbd::kInvalidMesh) r.DrawChunkVolumes(fadeVolMesh, viewProj);
    if (!mergedDraw.empty()) {
        r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
        for (const auto& [ci, keep] : mergedDraw) {
            r.SetDrawFade(keep >= 1.0f ? 0.0f : keep);
            r.Draw(cells[(size_t)ci].merged, viewProj, tbd::Mat4::Identity(), true, true);
        }
        r.SetDrawFade(0.0f);
    }
    r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
    static uint32_t logFrame = 0;
    if (++logFrame % 600 == 0) {
        int complete = 0, unsaved = 0, stale = 0;
        for (const Cell& c : cells) {
            if (c.slot < 0) continue;
            const bool full = c.bakedFor == c.content && c.faces == 0x1F;
            complete += full, unsaved += full && !c.saved, stale += c.bakedFor != c.content;
        }
        tbd::Log("City buildings: slots: %d complete (%d not on disk yet), %d stale", complete, unsaved, stale);
    }
    if (logFrame % 600 == 0)
        tbd::Log("City buildings: %zu cells (%u near, %u volume, %u merged on screen, %u fading, %u kept off Near by the budget), %zu models, %u faces baked (%.1f ms recording), cache %u loads %u saves",
                 cells.size(), statNear, statVol, statMerged, statFading, statGovDenied, models.size(), statBakedFaces, statBakeMs, statCacheLoads, statCacheSaves);
}

// ---- Checkpoint B (TBD_CHUNK_CHECK=1 with TBD_CITY_STRESS=<n>): one cell three ways. Once the stress city is up, the busiest cell
// near its centre is isolated (nothing else of the city's buildings draws), baked whole, its five faces dumped to BMP (shots\chunk_face_*),
// then shown as the real meshes, the chunk volume and the merged proxy from two distances; each is held still, its GPU scene time
// averaged over 120 frames with the zoned draws and triangles, and a screenshot written (shots\chunk_check_<mode>_<distance>.bmp).
namespace city_check {
constexpr int kCheckKinds = 5;
inline const int kCheckModes[kCheckKinds] = {city_bld::kModeNear, city_bld::kModeVol, city_bld::kModeVol, city_bld::kModeVol, city_bld::kModeMerged};
inline const int kCheckQuality[kCheckKinds] = {-1, 2, 1, 0, -1};
inline const char* const kCheckNames[kCheckKinds] = {"near", "vol_high", "vol_medium", "vol_low", "merged"};
inline int step = -1, wait = 0, cell = -1, combo = 0;
inline double gpuSum[8] = {};
inline int gpuFrames = 0;
inline uint64_t drawsSum = 0, trisSum = 0;
inline std::wstring Dir() {
    const std::wstring d = tbd::ExeDirectory() + L"\\..\\shots";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
inline void WriteBmp(const std::wstring& file, const uint8_t* bgra, int w, int h) {
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + (size_t)w * h * 4), 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER ih = {sizeof(ih), w, -h, 1, 32, BI_RGB};
    if (FILE* f = _wfopen(file.c_str(), L"wb")) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(bgra, 1, (size_t)w * h * 4, f);
        fclose(f);
    }
}
// The five faces of a cell, three channels each (relief: start in red, end in green), as one BMP per channel.
inline void DumpFaces(tbd::Renderer& r, int ci) {
    using namespace city_bld;
    std::vector<uint8_t> ch[3];
    if (!ReadStrip(r, cells[(size_t)ci], ch)) { tbd::Log("Chunk check: could not read the atlas back"); return; }
    const int w = (int)(tile * tbd::kChunkFaceCount), h = (int)tile;
    static const wchar_t* const kName[3] = {L"albedo", L"normal", L"relief"};
    for (int c = 0; c < 3; ++c) {
        std::vector<uint8_t> px((size_t)w * h * 4);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const uint8_t* s = &ch[c][i * 4];
            uint8_t rgb[3];
            if (c < 2) rgb[0] = s[0], rgb[1] = s[1], rgb[2] = s[2];
            else {
                const uint16_t start = (uint16_t)(s[0] | (s[1] << 8)), endInv = (uint16_t)(s[2] | (s[3] << 8));
                const bool empty = 65535 - endInv < start;
                rgb[0] = empty ? 0 : (uint8_t)(255 - start / 257), rgb[1] = empty ? 0 : (uint8_t)(255 - (65535 - endInv) / 257), rgb[2] = empty ? 40 : 0;
            }
            px[i * 4 + 0] = rgb[2], px[i * 4 + 1] = rgb[1], px[i * 4 + 2] = rgb[0], px[i * 4 + 3] = 255;
        }
        WriteBmp(Dir() + L"\\chunk_face_" + kName[c] + L".bmp", px.data(), w, h);
    }
    tbd::Log("Chunk check: wrote shots\\chunk_face_{albedo,normal,relief}.bmp (%d x %d: top, east, west, south, north)", w, h);
}
} // namespace city_check

// Before the frame (after CityStressSync): drives the checkpoint's camera and modes.
void CityChunkCheckSync() {
    using namespace city_check;
    static const bool on = city_bld::EnvI("TBD_CHUNK_CHECK", 0) != 0;
    if (!on || g_zoneEntities) return;
    if (step == -1) {
        if (city_stress::g.phase < 3) return; // the stress city is not up yet
        city_stress::g.phase = 4;            // its own view walk stops: this run holds the camera
        int best = -1;
        for (size_t ci = 0; ci < city_bld::cells.size(); ++ci) {
            const city_bld::Cell& c = city_bld::cells[ci];
            const tbd::Vec3 mid = (c.lo + c.hi) * 0.5f;
            const float d = std::hypot(mid.x - city_stress::g.centre.x, mid.z - city_stress::g.centre.z);
            if (c.built && d < 700.0f && (best < 0 || c.built > city_bld::cells[(size_t)best].built)) best = (int)ci;
        }
        if (best < 0) { tbd::Log("Chunk check: no built cell near the centre"); step = 99; return; }
        cell = best;
        city_bld::isolate = best;
        city_bld::bakeAll = true;
        city_bld::cells[(size_t)best].force = city_bld::kModeVol;
        const city_bld::Cell& c = city_bld::cells[(size_t)best];
        tbd::Log("Chunk check: cell %d,%d with %d buildings, box %.0f x %.0f x %.0f m", c.key.x, c.key.z, c.built, c.hi.x - c.lo.x, c.hi.y - c.lo.y, c.hi.z - c.lo.z);
        g_clock.paused = true;
        auto& cam = g_engine.camera;
        cam.target = {(c.lo.x + c.hi.x) * 0.5f, c.lo.y + (c.hi.y - c.lo.y) * 0.3f, (c.lo.z + c.hi.z) * 0.5f};
        cam.yaw = 0.6f, cam.pitch = 0.5f, cam.distance = 260.0f;
        step = 0, wait = 0;
        return;
    }
    if (step >= 99 || step == 0) return; // step 0: CityChunkCheckAfterFrame asks for the bake and the proxy itself
    static const float kDist[2] = {260.0f, 700.0f};
    city_bld::Cell& c = city_bld::cells[(size_t)cell];
    c.force = kCheckModes[combo % kCheckKinds];
    c.forceQ = kCheckQuality[combo % kCheckKinds];
    auto& cam = g_engine.camera;
    const tbd::Vec3 mid = (c.lo + c.hi) * 0.5f;
    cam.target = {mid.x, c.lo.y + (c.hi.y - c.lo.y) * 0.3f, mid.z};
    cam.yaw = 0.6f;
    cam.pitch = 0.5f;
    cam.distance = kDist[combo / kCheckKinds];
}

// After EndFrame: waits, measurements, the dumps and screenshots.
void CityChunkCheckAfterFrame(tbd::Renderer& r) {
    using namespace city_check;
    if (step < 0 || step >= 99) return;
    city_bld::Cell& c = city_bld::cells[(size_t)cell];
    static const int kDistM[2] = {260, 700};
    const char* const modeName = kCheckNames[combo % kCheckKinds];
    const int mode = c.force;
    if (step == 0) { // the bake (all five faces in one frame) and the proxy
        if (!city_bld::VolReady(c) || c.faces != 0x1F || !city_bld::MergedReady(c)) {
            c.force = city_bld::MergedReady(c) ? city_bld::kModeVol : city_bld::kModeMerged; // ask for whichever is missing
            if (++wait > 1800) { tbd::Log("Chunk check: bake or proxy not ready after 1800 frames (vol %d, faces %x, merged %d)", city_bld::VolReady(c), c.faces, city_bld::MergedReady(c)); step = 99; }
            return;
        }
        DumpFaces(r, cell);
        c.force = city_bld::kModeNear;
        step = 1, wait = 0;
        return;
    }
    // step 1: settle 90 frames, measure 120, shoot one
    ++wait;
    if (wait <= 90) return;
    if (wait <= 210) {
        const double* g = r.GpuPasses();
        for (int k = 0; k < 8; ++k) gpuSum[k] += g[k];
        ++gpuFrames;
        drawsSum += g_drawCatsShown.draws[kCatZoned];
        trisSum += g_drawCatsShown.tris[kCatZoned];
        if (wait == 210) r.SetFrameCapture(true);
        return;
    }
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (r.CapturedFrame(px, w, h)) {
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        wchar_t name[96];
        swprintf(name, 96, L"\\chunk_check_%hs_%dm.bmp", modeName, kDistM[combo / kCheckKinds]);
        WriteBmp(Dir() + name, px.data(), w, h);
    }
    r.SetFrameCapture(false);
    double total = 0;
    for (int k = 0; k < 8; ++k) total += gpuSum[k];
    const uint32_t tris = mode == city_bld::kModeMerged ? c.mergedTris : 0;
    tbd::Log("Chunk check: %-10s at %d m: GPU scene %.3f ms, shadow %.3f ms, frame total %.3f ms; zoned draws %.1f, zoned triangles %.0f%s%s (%s)",
             modeName, kDistM[combo / kCheckKinds], gpuSum[1] / gpuFrames, gpuSum[0] / gpuFrames, total / gpuFrames, (double)drawsSum / gpuFrames,
             (double)trisSum / gpuFrames, tris ? ", proxy " : "", tris ? std::to_string(tris).c_str() : "", r.Name());
    std::fill(gpuSum, gpuSum + 8, 0.0);
    gpuFrames = 0, drawsSum = trisSum = 0, wait = 0;
    if (++combo >= 2 * kCheckKinds) {
        tbd::Log("Chunk check: done");
        step = 99;
        if (city_bld::EnvI("TBD_CITY_STRESS_QUIT", 0)) PostQuitMessage(0);
    }
}
