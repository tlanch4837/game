// GPU vegetation: rules read from <assets>\vegetation\<scene>.vegrules become buffers for shaders/vegetation.hlsl, which places and
// culls instances around the camera each frame; the meshes draw indirectly from three LOD sets generated here. See the shader for
// the reference. Included by main.cpp, after the mesh upload helpers. TBD_GPUVEG=0 keeps the scene's scattered sets instead;
// TBD_GPUVEG=2 runs the compute but draws nothing, to measure it alone.
namespace vegetation {

struct Layer {
    float cell = 1, reach = 80, lod1 = 12, lod2 = 35, shadowDistance = 0, scaleMin = 0.8f, scaleMax = 1.2f, density = 1;
    float splat[4] = {0, 1, 0, 0};
    float slopeMin = 0, slopeMax = 1, heightMin = -1e6f, heightMax = 1e6f;
    float noiseFrequency = 0, noiseLo = 0, noiseHi = 1, align = 0.6f;
    uint32_t capacity = 2000;
    bool stones = false;
    float keep1 = 0.35f, keep2 = 0.12f;
    std::vector<std::vector<std::string>> variants; // each a list of scene instance-set names drawn together
};

struct GpuLayer {
    float cell, reach, lod1, lod2, shadowDistance, scaleMin, scaleMax, density, splat[4], slopeMin, slopeMax, heightMin, heightMax, noiseFrequency, noiseLo, noiseHi, align;
    uint32_t bucketFirst, variantCount, casterFirst, radiusCm;
};
static_assert(sizeof(GpuLayer) == 96);
struct GpuBucket { uint32_t first, capacity; float scale, pad; };
struct Record { tbd::MeshId mesh; tbd::TextureId albedo, normal, surface; uint32_t bucket; bool caster; bool farSet; float shadowReach = 0.0f; };
inline bool Flag(const char* name) { char b[8] = {}; return !(GetEnvironmentVariableA(name, b, 8) > 0 && b[0] == '0'); } // test switches: on unless set to 0

inline char Mode() { static const char m = [] { char b[8] = {}; return GetEnvironmentVariableA("TBD_GPUVEG", b, 8) > 0 ? b[0] : '1'; }(); return m; }

// Less geometry for the far sets: whole blades go (a blade is a connected piece), and the ones kept grow wider so the tuft's cover
// stays about the same. A mesh of few pieces (a branching shrub) loses triangles instead.
std::vector<tbd::Vertex> Thin(const tbd::Vertex* v, size_t count, float keep) {
    if (keep >= 1.0f) return std::vector<tbd::Vertex>(v, v + count);
    const size_t tris = count / 3;
    std::vector<uint32_t> parent(tris);
    for (uint32_t i = 0; i < tris; ++i) parent[i] = i;
    const auto find = [&](uint32_t x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
    std::map<std::tuple<int, int, int>, uint32_t> seen;
    for (uint32_t t = 0; t < tris; ++t)
        for (int k = 0; k < 3; ++k) {
            const tbd::Vertex& p = v[t * 3 + k];
            const auto key = std::make_tuple((int)std::lround(p.px * 1000), (int)std::lround(p.py * 1000), (int)std::lround(p.pz * 1000));
            const auto [it, added] = seen.try_emplace(key, t);
            if (!added) parent[find(t)] = find(it->second);
        }
    std::map<uint32_t, std::vector<uint32_t>> pieces;
    for (uint32_t t = 0; t < tris; ++t) pieces[find(t)].push_back(t);
    std::vector<tbd::Vertex> out;
    uint32_t hash = 2166136261u;
    const auto kept = [&] { hash = (hash ^ 0x9E37u) * 16777619u; hash ^= hash >> 13; return (hash & 0xFFFF) / 65536.0f < keep; };
    if (pieces.size() >= 8) {
        const float widen = std::min(2.2f, 1.0f / std::sqrt(keep));
        float meshLow = 1e30f, meshHigh = -1e30f;
        for (size_t i = 0; i < count; ++i) { meshLow = std::min(meshLow, v[i].py); meshHigh = std::max(meshHigh, v[i].py); }
        const float meshHeight = meshHigh - meshLow;
        for (const auto& [root, list] : pieces) {
            if (!kept()) continue;
            float cx = 0, cz = 0;
            for (uint32_t t : list) for (int k = 0; k < 3; ++k) { cx += v[t * 3 + k].px; cz += v[t * 3 + k].pz; }
            cx /= list.size() * 3.0f; cz /= list.size() * 3.0f;
            float low = 1e30f, high = -1e30f;
            for (uint32_t t : list) for (int k = 0; k < 3; ++k) { low = std::min(low, v[t * 3 + k].py); high = std::max(high, v[t * 3 + k].py); }
            const float grow = high - low > meshHeight * 0.25f ? widen : 1.0f; // a blade; a seed head's little cards stay as they are
            for (uint32_t t : list) for (int k = 0; k < 3; ++k) {
                tbd::Vertex x = v[t * 3 + k];
                x.px = cx + (x.px - cx) * grow;
                x.pz = cz + (x.pz - cz) * grow;
                out.push_back(x);
            }
        }
    } else {
        for (uint32_t t = 0; t < tris; ++t)
            if (kept()) out.insert(out.end(), v + t * 3, v + t * 3 + 3);
    }
    if (out.empty()) out.assign(v, v + 3);
    return out;
}

inline tbd::Vec3 Transform(const tbd::Mat4& m, const tbd::Vertex& x) {
    tbd::Vec3 o;
    o.x = m.At(0, 0) * x.px + m.At(0, 1) * x.py + m.At(0, 2) * x.pz + m.At(0, 3);
    o.y = m.At(1, 0) * x.px + m.At(1, 1) * x.py + m.At(1, 2) * x.pz + m.At(1, 3);
    o.z = m.At(2, 0) * x.px + m.At(2, 1) * x.py + m.At(2, 2) * x.pz + m.At(2, 3);
    return o;
}

class System {
public:
    bool Active() const { return active_; }
    bool Consumes(const std::string& setName) const { return active_ && consumed_.count(setName) > 0; }

    // Before the scene's instance sets upload: what the rules place, those sets no longer do.
    void Load() {
        Unload();
        if (Mode() == '0' || !g_engine.renderer || g_engine.scenePath.empty()) return;
        std::wstring stem = g_engine.scenePath.substr(g_engine.scenePath.find_last_of(L"\\/") + 1);
        stem = stem.substr(0, stem.find_last_of(L'.'));
        std::string text;
        if (!tbd::ReadAllText(tbd::Join(g_engine.assetsRoot, L"vegetation\\" + stem + L".vegrules"), text)) return;
        std::vector<Layer> layers;
        if (!Parse(text, layers) || !BuildMap(layers) || !BuildGpu(layers)) { Unload(); return; }
        active_ = true;
    }

    void Unload() {
        if (g_engine.renderer) {
            for (tbd::GpuBufferId b : {map_, layers_, buckets_, records_, rows_, counters_, args_}) if (b != tbd::kNoGpuBuffer) g_engine.renderer->DestroyGpuBuffer(b);
            for (const Record& r : draw_) g_engine.renderer->DestroyMesh(r.mesh);
        }
        Clear();
    }
    void Forget() { // the renderer has gone, and every id with it
        Clear();
        reset_ = place_ = argsShader_ = tbd::kNoComputeShader;
    }

    void Compute(tbd::Renderer& r, const tbd::Mat4& viewProj, const tbd::Vec3& eye) {
        if (!active_) return;
        struct Constants { float planes[4][4]; float eye[4]; float mapMin[4]; int32_t tile[4]; float misc[4]; } c = {};
        static_assert(sizeof(Constants) == 128);
        for (int p = 0; p < 4; ++p) {
            const int axis = p / 2;
            const float sign = (p & 1) ? -1.0f : 1.0f;
            float length = 0;
            for (int k = 0; k < 4; ++k) c.planes[p][k] = viewProj.At(3, k) + sign * viewProj.At(axis, k);
            for (int k = 0; k < 3; ++k) length += c.planes[p][k] * c.planes[p][k];
            length = std::sqrt(length);
            for (int k = 0; k < 4; ++k) c.planes[p][k] /= length;
        }
        c.eye[0] = eye.x; c.eye[1] = eye.y; c.eye[2] = eye.z;
        c.mapMin[0] = mapX_; c.mapMin[1] = mapZ_; c.mapMin[2] = mapCell_;
        c.tile[2] = mapW_; c.tile[3] = mapH_;
        const tbd::GpuBufferId reads[4] = {map_, layers_, buckets_, records_}, writes[3] = {rows_, counters_, args_};
        r.BeginCompute();
        r.Dispatch(reset_, &c, sizeof(c), reads, writes, (bucketTotal_ + 63) / 64, 1, 1);
        for (size_t l = 0; l < layerInfo_.size(); ++l) {
            const Layer& L = layerInfo_[l];
            const float tile = L.cell * 16.0f;
            const float reach = std::max(L.reach, L.shadowDistance);
            const int x0 = (int)std::floor((eye.x - reach) / tile), z0 = (int)std::floor((eye.z - reach) / tile);
            const int x1 = (int)std::floor((eye.x + reach) / tile), z1 = (int)std::floor((eye.z + reach) / tile);
            c.eye[3] = (float)l;
            c.tile[0] = x0 * 16; c.tile[1] = z0 * 16;
            r.Dispatch(place_, &c, sizeof(c), reads, writes, (uint32_t)(x1 - x0 + 1), (uint32_t)(z1 - z0 + 1), 1);
        }
        c.tile[0] = (int32_t)draw_.size();
        r.Dispatch(argsShader_, &c, sizeof(c), reads, writes, ((uint32_t)draw_.size() + 63) / 64, 1, 1);
        r.EndCompute();
    }

    tbd::Mat4 Params(float time) const {
        tbd::Mat4 p;
        for (float& x : p.m) x = 0;
        p.At(3, 1) = 1.0f;
        p.At(0, 0) = maxReach_;
        static const float timeOffset = [] { char b[32] = {}; return GetEnvironmentVariableA("TBD_VEGTIME", b, 32) > 0 ? (float)std::atof(b) : 0.0f; }(); // test: another moment of the wind
        p.At(0, 1) = time + timeOffset;
        p.At(0, 2) = 0.82f; // the wind's direction and strength
        p.At(0, 3) = 0.57f;
        for (int c = 0; c < 3; ++c) for (int k = 0; k < 4; ++k) if (k != 1) p.At(c + 1, k) = characters_[c][k];
        return p;
    }

    // Who bends the plants: the three animated entities nearest the camera. TBD_BEND="x y z radius" adds a test character first.
    void UpdateCharacters(const tbd::Vec3& eye) {
        std::memset(characters_, 0, sizeof(characters_));
        int count = 0;
        static const std::vector<float> test = [] {
            char b[96] = {};
            std::vector<float> out(4);
            return GetEnvironmentVariableA("TBD_BEND", b, 96) > 0 && sscanf_s(b, "%f %f %f %f", &out[0], &out[1], &out[2], &out[3]) == 4 ? out : std::vector<float>{};
        }();
        if (test.size() == 4) { std::copy(test.begin(), test.end(), characters_[0]); count = 1; }
        std::vector<std::pair<float, tbd::Vec3>> closest;
        for (const tbd::Entity& e : g_engine.scene.entities) {
            if (e.animClip.empty()) continue;
            const tbd::Mat4 m = e.Model();
            const tbd::Vec3 p = {m.At(0, 3), m.At(1, 3), m.At(2, 3)};
            const tbd::Vec3 d = p - eye;
            closest.push_back({tbd::Dot(d, d), p});
        }
        std::sort(closest.begin(), closest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < closest.size() && count < 3; ++i, ++count) {
            characters_[count][0] = closest[i].second.x; characters_[count][1] = closest[i].second.y; characters_[count][2] = closest[i].second.z;
            characters_[count][3] = 1.8f; // meters of reach
        }
    }

    template <class UseMaterial>
    void Draw(tbd::Renderer& r, const tbd::Mat4& viewProj, float time, const UseMaterial& useMaterial) {
        if (!active_ || Mode() == '2') return;
        static const bool twoPass = [] { char b[8] = {}; return GetEnvironmentVariableA("TBD_VEGPREPASS", b, 8) > 0 && b[0] == '1'; }(); // measured slower here: off unless asked
        static const bool farPlain = Flag("TBD_VEGFAR"), tilt = Flag("TBD_VEGTILT"), ecotope = Flag("TBD_VEGECO");
        tbd::Mat4 params = Params(time), farParams = params;
        for (tbd::Mat4* m : {&params, &farParams}) {
            m->At(1, 1) = tilt ? 0.55f : 0.0f;
            m->At(2, 1) = ecotope ? 0.8f : 0.0f;
            m->At(3, 1) = 1.0f;
        }
        if (farPlain) { // the far set: no animation and no normal map
            farParams.At(3, 1) = 0.0f;
        }
        for (int pass = twoPass ? 1 : 0; pass <= (twoPass ? 2 : 0); ++pass) {
            r.SetVegetationPass(pass);
            for (size_t i = 0; i < draw_.size(); ++i) {
                if (draw_[i].caster) continue;
                const bool plain = farPlain && draw_[i].farSet;
                useMaterial(draw_[i].albedo, plain ? tbd::kNoTexture : draw_[i].normal, draw_[i].surface);
                r.DrawVegetationIndirect(draw_[i].mesh, rows_, args_, (uint32_t)i * 20, viewProj, plain ? farParams : params);
            }
        }
        r.SetVegetationPass(0);
    }

    void DrawShadows(tbd::Renderer& r, float time) {
        if (!active_ || Mode() == '2') return;
        tbd::Mat4 params = Params(time);
        for (size_t i = 0; i < draw_.size(); ++i)
            if (draw_[i].caster) {
                params.At(1, 1) = draw_[i].shadowReach;
                r.DrawVegetationShadowIndirect(draw_[i].mesh, rows_, args_, (uint32_t)i * 20, params);
            }
    }

    // A capture's numbers: placed instances and the triangles they make.
    void LogCounts(tbd::Renderer& r) {
        if (!active_) return;
        std::vector<uint32_t> args(draw_.size() * 5);
        if (!r.ReadGpuBuffer(args_, args.data(), args.size() * 4)) return;
        uint64_t tris = 0, shadowTris = 0;
        std::map<uint32_t, uint32_t> bucketCount;
        for (size_t i = 0; i < draw_.size(); ++i) {
            (draw_[i].caster ? shadowTris : tris) += (uint64_t)args[i * 5] / 3 * args[i * 5 + 1];
            if (!draw_[i].caster) bucketCount[draw_[i].bucket] = args[i * 5 + 1];
        }
        uint32_t total = 0, capped = 0;
        for (const auto& [b, n] : bucketCount) { total += n; capped += n >= capacities_[b] ? 1 : 0; }
        tbd::Log("Vegetation: %u instances in view, %llu triangles, %llu shadow triangles, %zu indirect draws, %u buckets at capacity", total, (unsigned long long)tris, (unsigned long long)shadowTris, draw_.size(), capped);
    }

private:
    void Clear() {
        map_ = layers_ = buckets_ = records_ = rows_ = counters_ = args_ = tbd::kNoGpuBuffer;
        draw_.clear(); consumed_.clear(); layerInfo_.clear(); capacities_.clear();
        bucketTotal_ = 0;
        active_ = false;
    }

    static float Field(const std::string& word, const char* key, float fallback) {
        const std::string k = std::string(key) + "=";
        return word.rfind(k, 0) == 0 ? (float)std::atof(word.c_str() + k.size()) : fallback;
    }

    bool Parse(const std::string& text, std::vector<Layer>& layers) {
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
            std::vector<std::string> words;
            size_t w = 0;
            while (w < line.size()) {
                const size_t s = line.find_first_not_of(" \t\r", w);
                if (s == std::string::npos) break;
                size_t e = line.find_first_of(" \t\r", s);
                if (e == std::string::npos) e = line.size();
                words.push_back(line.substr(s, e - s));
                w = e;
            }
            if (words.empty()) continue;
            if (words[0] == "ground" && words.size() > 1) groundName_ = words[1];
            else if (words[0] == "meters_per_texel" && words.size() > 1) mapCell_ = (float)std::atof(words[1].c_str());
            else if (words[0] == "authored_radius" && words.size() > 1) authoredRadius_ = (float)std::atof(words[1].c_str());
            else if (words[0] == "layer") {
                Layer L;
                for (const std::string& f : words) {
                    L.cell = Field(f, "cell", L.cell); L.reach = Field(f, "reach", L.reach); L.lod1 = Field(f, "lod1", L.lod1); L.lod2 = Field(f, "lod2", L.lod2);
                    L.shadowDistance = Field(f, "shadow", L.shadowDistance); L.scaleMin = Field(f, "scale_min", L.scaleMin); L.scaleMax = Field(f, "scale_max", L.scaleMax);
                    L.density = Field(f, "density", L.density);
                    L.splat[0] = Field(f, "earth", L.splat[0]); L.splat[1] = Field(f, "grass", L.splat[1]); L.splat[2] = Field(f, "rock", L.splat[2]); L.splat[3] = Field(f, "base", L.splat[3]);
                    L.slopeMin = Field(f, "slope_min", L.slopeMin); L.slopeMax = Field(f, "slope_max", L.slopeMax);
                    L.heightMin = Field(f, "height_min", L.heightMin); L.heightMax = Field(f, "height_max", L.heightMax);
                    L.noiseFrequency = Field(f, "noise", L.noiseFrequency); L.noiseLo = Field(f, "noise_lo", L.noiseLo); L.noiseHi = Field(f, "noise_hi", L.noiseHi);
                    L.align = Field(f, "align", L.align); L.capacity = (uint32_t)Field(f, "capacity", (float)L.capacity);
                    L.stones = Field(f, "stones", 0.0f) > 0.5f || L.stones; L.keep1 = Field(f, "keep1", L.keep1); L.keep2 = Field(f, "keep2", L.keep2);
                }
                layers.push_back(L);
            } else if (words[0] == "variant" && !layers.empty() && words.size() > 1) {
                layers.back().variants.emplace_back(words.begin() + 1, words.end());
            }
        }
        layers.erase(std::remove_if(layers.begin(), layers.end(), [](const Layer& L) { return L.variants.empty(); }), layers.end());
        return !layers.empty() && !groundName_.empty();
    }

    // The ground as a map of height, material weights and slope; texels under anything standing on the ground are excluded.
    bool BuildMap(const std::vector<Layer>& layers) {
        const tbd::Entity* ground = nullptr;
        for (const tbd::Entity& e : g_engine.scene.entities) if (e.name == groundName_) ground = &e;
        if (!ground) { tbd::Log("Vegetation: no ground entity %s", groundName_.c_str()); return false; }
        const std::vector<tbd::Vertex>& v = ImportedVertices(ground->meshPath);
        if (v.size() < 3) return false;
        const tbd::Mat4 model = ground->Model();
        float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
        std::vector<tbd::Vec3> positions(v.size());
        for (size_t i = 0; i < v.size(); ++i) {
            const tbd::Vec3 p = Transform(model, v[i]);
            positions[i] = p;
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z);
        }
        mapX_ = minX; mapZ_ = minZ;
        mapW_ = (int)std::ceil((maxX - minX) / mapCell_) + 1;
        mapH_ = (int)std::ceil((maxZ - minZ) / mapCell_) + 1;
        std::vector<float> height((size_t)mapW_ * mapH_, -1e30f);
        std::vector<uint32_t> packed((size_t)mapW_ * mapH_, 0x80000000u); // no ground here: excluded
        const auto toByte = [](float f) { return (uint32_t)std::clamp(f * 255.0f + 0.5f, 0.0f, 255.0f); };
        for (size_t t = 0; t + 3 <= v.size(); t += 3) {
            const tbd::Vec3 a = positions[t], b = positions[t + 1], c = positions[t + 2];
            const float area = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
            if (std::fabs(area) < 1e-6f) continue;
            const int x0 = std::max(0, (int)std::floor((std::min({a.x, b.x, c.x}) - minX) / mapCell_));
            const int x1 = std::min(mapW_ - 1, (int)std::ceil((std::max({a.x, b.x, c.x}) - minX) / mapCell_));
            const int z0 = std::max(0, (int)std::floor((std::min({a.z, b.z, c.z}) - minZ) / mapCell_));
            const int z1 = std::min(mapH_ - 1, (int)std::ceil((std::max({a.z, b.z, c.z}) - minZ) / mapCell_));
            const tbd::Vec3 ab = b - a, ac = c - a;
            const float nx = ab.y * ac.z - ab.z * ac.y, ny = ab.z * ac.x - ab.x * ac.z, nz = ab.x * ac.y - ab.y * ac.x;
            const float slope = 1.0f - std::fabs(ny) / std::max(1e-6f, std::sqrt(nx * nx + ny * ny + nz * nz));
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) {
                    const float px = minX + x * mapCell_, pz = minZ + z * mapCell_;
                    const float w1 = ((px - a.x) * (c.z - a.z) - (c.x - a.x) * (pz - a.z)) / area;
                    const float w2 = ((b.x - a.x) * (pz - a.z) - (px - a.x) * (b.z - a.z)) / area;
                    const float w0 = 1.0f - w1 - w2;
                    if (w0 < -0.01f || w1 < -0.01f || w2 < -0.01f) continue;
                    float cr = v[t].r * w0 + v[t + 1].r * w1 + v[t + 2].r * w2;
                    float cg = v[t].g * w0 + v[t + 1].g * w1 + v[t + 2].g * w2;
                    float cb = v[t].b * w0 + v[t + 1].b * w1 + v[t + 2].b * w2;
                    const float total = cr + cg + cb;
                    if (total > 1e-4f) { cr /= total; cg /= total; cb /= total; } else { cr = 0.7f; cg = 0.2f; cb = 0.1f; } // as the ground's shader falls back
                    const size_t i = (size_t)z * mapW_ + x;
                    height[i] = a.y * w0 + b.y * w1 + c.y * w2;
                    packed[i] = toByte(cr) | (toByte(cg) << 8) | (toByte(cb) << 16) | (std::min(127u, (uint32_t)(slope * 127.0f + 0.5f)) << 24);
                }
        }
        // What stands on the ground: triangles of any other entity, within a little of the ground's height, close their texels.
        uint32_t closed = 0;
        for (const tbd::Entity& e : g_engine.scene.entities) {
            if (&e == ground || e.mesh != tbd::MeshKind::Imported || !e.animClip.empty()) continue;
            if (e.name.rfind("Terrain/map_", 0) == 0) continue;
            const std::vector<tbd::Vertex>& ev = ImportedVertices(e.meshPath);
            const tbd::Mat4 m = e.Model();
            for (size_t t = 0; t + 3 <= ev.size(); t += 3) {
                const tbd::Vec3 a = Transform(m, ev[t]), b = Transform(m, ev[t + 1]), c = Transform(m, ev[t + 2]);
                const int x0 = std::max(0, (int)std::floor((std::min({a.x, b.x, c.x}) - minX) / mapCell_) - 1);
                const int x1 = std::min(mapW_ - 1, (int)std::ceil((std::max({a.x, b.x, c.x}) - minX) / mapCell_) + 1);
                const int z0 = std::max(0, (int)std::floor((std::min({a.z, b.z, c.z}) - minZ) / mapCell_) - 1);
                const int z1 = std::min(mapH_ - 1, (int)std::ceil((std::max({a.z, b.z, c.z}) - minZ) / mapCell_) + 1);
                if (e.name.rfind("Terrain/", 0) == 0) { // the plaza is ground, not an object on it: things stand on its surface
                    const float area = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
                    if (std::fabs(area) < 1e-6f) continue;
                    for (int z = z0; z <= z1; ++z)
                        for (int x = x0; x <= x1; ++x) {
                            const float px = minX + x * mapCell_, pz = minZ + z * mapCell_;
                            const float w1 = ((px - a.x) * (c.z - a.z) - (c.x - a.x) * (pz - a.z)) / area;
                            const float w2 = ((b.x - a.x) * (pz - a.z) - (px - a.x) * (b.z - a.z)) / area;
                            if (w1 < -0.02f || w2 < -0.02f || w1 + w2 > 1.02f) continue;
                            const float top = a.y + w1 * (b.y - a.y) + w2 * (c.y - a.y);
                            height[(size_t)z * mapW_ + x] = std::max(height[(size_t)z * mapW_ + x], top);
                        }
                    continue;
                }
                const float low = std::min({a.y, b.y, c.y});
                for (int z = z0; z <= z1; ++z)
                    for (int x = x0; x <= x1; ++x) {
                        const size_t i = (size_t)z * mapW_ + x;
                        if (!(packed[i] & 0x80000000u) && low < height[i] + 2.0f) { packed[i] |= 0x80000000u; ++closed; }
                    }
            }
        }
        // Where the authored scatter left the ground bare (streets, yards) stays bare: plants only grow near where its sets placed one.
        for (uint32_t& t : packed) t &= ~0x10000u; // bit 16, rock's lowest: bare of authored plants
        if (authoredRadius_ > 0.0f) {
            std::vector<uint8_t> allowed((size_t)mapW_ * mapH_, 0);
            const int reach = (int)std::ceil(authoredRadius_ / mapCell_);
            for (const Layer& L : layers)
                for (const auto& names : L.variants)
                    for (const std::string& name : names)
                        for (const tbd::InstanceSet& set : g_engine.scene.instances) {
                            if (L.stones || set.name != name) continue;
                            std::string bytes;
                            if (!tbd::ReadAllText(tbd::Join(g_engine.assetsRoot, tbd::Utf8ToWide(set.file)), bytes)) continue;
                            const float* f = (const float*)bytes.data();
                            for (size_t i = 0; i < bytes.size() / 48; ++i) {
                                const int cx = (int)((f[i * 12 + 3] - minX) / mapCell_), cz = (int)((f[i * 12 + 11] - minZ) / mapCell_);
                                for (int z = std::max(0, cz - reach); z <= std::min(mapH_ - 1, cz + reach); ++z)
                                    for (int x = std::max(0, cx - reach); x <= std::min(mapW_ - 1, cx + reach); ++x)
                                        if ((x - cx) * (x - cx) + (z - cz) * (z - cz) <= reach * reach) allowed[(size_t)z * mapW_ + x] = 1;
                            }
                        }
            for (size_t i = 0; i < allowed.size(); ++i) if (!allowed[i]) packed[i] |= 0x10000u; else packed[i] &= ~0x10000u;
        }
        std::vector<uint32_t> data((size_t)mapW_ * mapH_ * 2);
        for (size_t i = 0; i < height.size(); ++i) {
            std::memcpy(&data[i * 2], &height[i], 4);
            data[i * 2 + 1] = packed[i];
        }
        map_ = g_engine.renderer->CreateGpuBuffer(tbd::GpuBufferUse::Read, data.size() * 4, data.data());
        tbd::Log("Vegetation: ground map %d x %d at %.2f m, %u texels closed by objects", mapW_, mapH_, mapCell_, closed);
        return map_ != tbd::kNoGpuBuffer;
    }

    bool BuildGpu(const std::vector<Layer>& layers) {
        tbd::Renderer& r = *g_engine.renderer;
        if (reset_ == tbd::kNoComputeShader) {
            reset_ = r.CreateComputeShader("CSReset");
            place_ = r.CreateComputeShader("CSPlace");
            argsShader_ = r.CreateComputeShader("CSArgs");
        }
        if (reset_ == tbd::kNoComputeShader || place_ == tbd::kNoComputeShader || argsShader_ == tbd::kNoComputeShader) return false;
        std::vector<GpuLayer> gpuLayers;
        std::vector<GpuBucket> buckets;
        std::vector<uint32_t> recordBuckets, args;
        uint32_t nextRow = 0;
        maxReach_ = 0;
        const auto addBucket = [&](uint32_t capacity, float scale) {
            buckets.push_back({nextRow, capacity, scale, 0});
            nextRow += capacity;
            return (uint32_t)buckets.size() - 1;
        };
        const auto addMesh = [&](const std::vector<tbd::Vertex>& v, const MatPart& part, uint32_t bucket, bool caster, bool farSet = false) {
            if (v.size() < 3) return;
            const tbd::MeshId id = r.CreateMesh(v.data(), (uint32_t)v.size(), tbd::Topology::Triangles);
            draw_.push_back({id, part.albedo, part.normal, part.surface, bucket, caster, farSet});
            recordBuckets.push_back(bucket);
            const uint32_t a[5] = {r.MeshIndexCount(id), 0, 0, 0, buckets[bucket].first};
            args.insert(args.end(), a, a + 5);
        };
        struct Piece { const tbd::Vertex* v; MatPart part; };
        for (const Layer& L : layers) {
            GpuLayer g = {};
            std::memcpy(&g, &L.cell, sizeof(float) * 8); if (L.stones) g.density = -L.density; // CSPlace reads the sign
            std::memcpy(g.splat, L.splat, sizeof(g.splat));
            g.slopeMin = L.slopeMin; g.slopeMax = L.slopeMax; g.heightMin = L.heightMin; g.heightMax = L.heightMax;
            g.noiseFrequency = L.noiseFrequency; g.noiseLo = L.noiseLo; g.noiseHi = L.noiseHi; g.align = L.align;
            g.bucketFirst = (uint32_t)buckets.size();
            g.variantCount = (uint32_t)L.variants.size();
            float radius = 1.0f;
            std::vector<std::vector<Piece>> variantPieces;
            std::vector<float> variantScale;
            for (const std::vector<std::string>& names : L.variants) {
                std::vector<Piece> pieces;
                float scale = 1.0f;
                for (const std::string& name : names) {
                    const tbd::InstanceSet* set = nullptr;
                    for (const tbd::InstanceSet& s : g_engine.scene.instances) if (s.name == name) set = &s;
                    if (!set) { tbd::Log("Vegetation: no instance set %s", name.c_str()); continue; }
                    consumed_.insert(name);
                    scale = AuthoredScale(*set);
                    const std::vector<tbd::Vertex>& v = ImportedVertices(set->meshPath);
                    if (v.empty()) continue;
                    const tbd::Sphere bounds = tbd::SphereAround(v.data(), v.size());
                    radius = std::max(radius, bounds.radius + std::sqrt(tbd::Dot(bounds.center, bounds.center)));
                    const tbd::ImportedModel& model = ImportedModels()[set->meshPath];
                    const auto& textures = PartTextures(set->meshPath);
                    if (model.parts.empty()) {
                        MatPart whole; whole.first = 0; whole.count = v.size();
                        pieces.push_back({v.data(), whole});
                    }
                    for (size_t k = 0; k < model.parts.size(); ++k) {
                        MatPart p;
                        p.first = model.parts[k].first; p.count = model.parts[k].count;
                        if (k < textures.size()) { p.albedo = textures[k].first; p.normal = textures[k].second; }
                        pieces.push_back({v.data() + p.first, p});
                    }
                }
                variantPieces.push_back(std::move(pieces));
                variantScale.push_back(scale);
            }
            g.radiusCm = (uint32_t)(radius * 100.0f);
            const float keeps[3] = {1.0f, L.keep1, L.keep2};
            for (size_t i = 0; i < variantPieces.size(); ++i)
                for (int lod = 0; lod < 3; ++lod) {
                    const uint32_t bucket = addBucket(L.capacity >> (lod == 0 ? 2 : (lod == 1 ? 1 : 0)), variantScale[i]);
                    for (const Piece& piece : variantPieces[i]) addMesh(Thin(piece.v, piece.part.count, keeps[lod]), piece.part, bucket, false, lod == 2);
                }
            g.casterFirst = (uint32_t)buckets.size();
            for (size_t i = 0; i < variantPieces.size(); ++i) {
                if (L.shadowDistance <= 0.0f) { addBucket(1, variantScale[i]); continue; }
                const uint32_t bucket = addBucket(L.capacity >> 1, variantScale[i]);
                for (const Piece& piece : variantPieces[i]) {
                    addMesh(Thin(piece.v, piece.part.count, L.keep1), piece.part, bucket, true);
                    draw_.back().shadowReach = L.shadowDistance;
                }
            }
            maxReach_ = std::max(maxReach_, L.reach);
            gpuLayers.push_back(g);
            layerInfo_.push_back(L);
        }
        bucketTotal_ = (uint32_t)buckets.size();
        for (const GpuBucket& b : buckets) capacities_.push_back(b.capacity);
        std::vector<uint32_t> zero(bucketTotal_, 0);
        layers_ = r.CreateGpuBuffer(tbd::GpuBufferUse::Read, gpuLayers.size() * sizeof(GpuLayer), gpuLayers.data());
        buckets_ = r.CreateGpuBuffer(tbd::GpuBufferUse::Read, buckets.size() * sizeof(GpuBucket), buckets.data());
        records_ = r.CreateGpuBuffer(tbd::GpuBufferUse::Read, recordBuckets.size() * 4, recordBuckets.data());
        rows_ = r.CreateGpuBuffer(tbd::GpuBufferUse::Instances, (uint64_t)nextRow * tbd::kInstanceFloats * sizeof(float), nullptr);
        counters_ = r.CreateGpuBuffer(tbd::GpuBufferUse::Args, zero.size() * 4, zero.data());
        args_ = r.CreateGpuBuffer(tbd::GpuBufferUse::Args, args.size() * 4, args.data());
        tbd::Log("Vegetation: %zu layers, %u buckets, %zu indirect draws, room for %u instances (%.1f MB)", layers.size(), bucketTotal_, draw_.size(), nextRow, nextRow * 48.0 / 1e6);
        return layers_ != tbd::kNoGpuBuffer && buckets_ != tbd::kNoGpuBuffer && records_ != tbd::kNoGpuBuffer && rows_ != tbd::kNoGpuBuffer && counters_ != tbd::kNoGpuBuffer && args_ != tbd::kNoGpuBuffer;
    }

    // The size this set was authored at: the mean scale of its scattered instances.
    float AuthoredScale(const tbd::InstanceSet& set) const {
        std::string bytes;
        if (!tbd::ReadAllText(tbd::Join(g_engine.assetsRoot, tbd::Utf8ToWide(set.file)), bytes) || bytes.size() < 48) return 1.0f;
        const float* f = (const float*)bytes.data();
        const size_t count = bytes.size() / 48;
        double sum = 0;
        for (size_t i = 0; i < count; ++i) sum += std::sqrt(f[i * 12 + 1] * f[i * 12 + 1] + f[i * 12 + 5] * f[i * 12 + 5] + f[i * 12 + 9] * f[i * 12 + 9]); // the y axis
        return (float)(sum / count);
    }

    bool active_ = false;
    std::set<std::string> consumed_;
    std::string groundName_;
    std::vector<Layer> layerInfo_;
    std::vector<Record> draw_;
    std::vector<uint32_t> capacities_;
    float characters_[3][4] = {};
    float authoredRadius_ = 0.0f;
    float mapCell_ = 0.75f, mapX_ = 0, mapZ_ = 0, maxReach_ = 0;
    int mapW_ = 0, mapH_ = 0;
    uint32_t bucketTotal_ = 0;
    tbd::GpuBufferId map_ = tbd::kNoGpuBuffer, layers_ = tbd::kNoGpuBuffer, buckets_ = tbd::kNoGpuBuffer, records_ = tbd::kNoGpuBuffer, rows_ = tbd::kNoGpuBuffer, counters_ = tbd::kNoGpuBuffer, args_ = tbd::kNoGpuBuffer;
    tbd::ComputeShaderId reset_ = tbd::kNoComputeShader, place_ = tbd::kNoComputeShader, argsShader_ = tbd::kNoComputeShader;
};

} // namespace vegetation
vegetation::System g_vegetation;
