// Mesh importer: reads OBJ and glTF 2.0 (.gltf / .glb) into the engine's flat, unindexed Vertex list, the same
// shape the procedural meshes in scene.h produce, so the result goes straight to Renderer::CreateMesh.
// No third-party code: a small JSON reader and the two format parsers live here. glTF node transforms are baked
// into the vertices; the material's base color factor becomes the vertex color. Both formats are right-handed
// with counter-clockwise front faces, which matches the engine's primitives, so no winding flip is applied.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "image_decode.h"
#include "render/renderer.h"
#include "character/hair_vertex.h"

namespace tbd {

struct ImportOptions {
    bool normalizeToUnitBox = true; // recenter on the origin and scale so the longest side is 1 (entity scale sizes it)
    bool generateFlatNormals = true; // for triangles whose source has no normals
};

// What an import found besides triangles: the vertex list is split into parts, one per glTF primitive, and each part
// may name a base-color image and a normal-map image (indices into `images`, whose entries hold the encoded file
// bytes, PNG or JPEG, decoded on the engine side).
struct ImportedPart {
    size_t first = 0, count = 0; // vertex range in the flat list
    int albedo = -1, normal = -1, rough = -1; // rough: metallicRoughnessTexture
    int occlusion = -1, emissive = -1; // occlusionTexture (R), emissiveTexture
    float emissiveFactor[3] = {0, 0, 0}, metallic = 0; // metallic: the factor the shader scales the map's B by
    bool eye = false;  // material named M_Eye
    bool skin = false; // material named M_Skin: its vertices carry alpha 0.5, which the shaders read as "scatters light"
    bool ground = false; // material named M_Ground: a tiling surface
    bool unnamed = false; // its material has no name
    bool groundSplat = false; // Western ground material: COLOR_0 carries splat weights
    bool art = false; // its own picture (a sign, lettering), not a tiling material: no relief, no weathering
    bool hair = false; // made by the importer, not in the file: hair cards (AddHairCards, AddScalpCards); vertex data in character/hair_vertex.h
    bool brow = false; // the brow cards (brow_cards.json): the Character Creator's brow maker rewrites them
    bool glass = false; // a window pane (alpha 0.645): its albedo slot carries the wear mask, not a colour picture
    // Camera distance (m) this part draws at; lodMax 0 = always. Hair detail levels (AddScalpCards) are parts with adjacent ranges.
    float lodMin = 0.0f, lodMax = 0.0f;
};
// One flat vertex's skin binding: indices into SkinJoint order, weights summing to 1 (all zero = not skinned, stays put).
struct VertexSkin {
    uint16_t joints[4] = {0, 0, 0, 0};
    float weights[4] = {0, 0, 0, 0};
};
// A joint of a glTF skin, reordered so that a parent always comes before its children.
struct SkinJoint {
    std::string name;
    int parent = -1;            // index in this list; -1 = its parent is outside the skin
    float t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1}; // rest pose, local
    float inverseBind[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float rootParent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // for parent == -1: the non-joint nodes above it
};
// One glTF animation, addressed by joint name so a clip file can drive any mesh file with the same names.
struct AnimTrack {
    std::vector<float> times;
    std::vector<float> values; // 3 or 4 floats per time
    bool step = false;
    // A translation track that never moves more than 1 mm from its first key: only a bone length, and the clip's skeleton's
    // length at that (skeletal_anim.h plays the mesh's own length instead below the pelvis).
    bool still = false;
};
struct AnimJointTracks {
    AnimTrack translation, rotation, scale; // empty = the joint's rest value
};
struct AnimClip {
    std::string name;
    float duration = 0;
    std::map<std::string, AnimJointTracks> joints;
};
struct ImportedModel {
    std::vector<ImportedPart> parts;
    std::vector<std::vector<uint8_t>> images;
    // Filled when the file has one skin and its skinned nodes carry no transform (nothing was baked into the vertices).
    std::vector<SkinJoint> joints;
    std::vector<VertexSkin> vertexSkin; // parallel to the flat vertex list
    std::vector<AnimClip> clips;
};

namespace meshimport_detail {

// ---------------------------------------------------------------- JSON
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json* Find(const char* key) const {
        if (type != Object) return nullptr;
        for (const auto& kv : o)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    const Json* At(size_t i) const { return type == Array && i < a.size() ? &a[i] : nullptr; }
    double Num(double fallback = 0) const { return type == Number ? n : fallback; }
    int Int(int fallback = 0) const { return type == Number ? (int)n : fallback; }
};

class JsonParser {
public:
    JsonParser(const char* text, size_t size) : p_(text), end_(text + size) {}
    bool Parse(Json& out) {
        if (!Value(out)) return false;
        Skip();
        return true;
    }

private:
    const char* p_;
    const char* end_;

    void Skip() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }
    bool Literal(const char* word) {
        const size_t len = std::strlen(word);
        if ((size_t)(end_ - p_) < len || std::strncmp(p_, word, len) != 0) return false;
        p_ += len;
        return true;
    }
    static void AppendUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
    bool Str(std::string& out) {
        if (p_ >= end_ || *p_ != '"') return false;
        ++p_;
        while (p_ < end_ && *p_ != '"') {
            if (*p_ != '\\') { out += *p_++; continue; }
            if (++p_ >= end_) return false;
            switch (*p_++) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (end_ - p_ < 4) return false;
                uint32_t cp = (uint32_t)std::strtoul(std::string(p_, 4).c_str(), nullptr, 16);
                p_ += 4;
                AppendUtf8(out, cp);
                break;
            }
            default: out += p_[-1]; break; // " \ /
            }
        }
        if (p_ >= end_) return false;
        ++p_;
        return true;
    }
    bool Value(Json& v) {
        Skip();
        if (p_ >= end_) return false;
        switch (*p_) {
        case '{': {
            v.type = Json::Object;
            ++p_;
            Skip();
            if (p_ < end_ && *p_ == '}') { ++p_; return true; }
            for (;;) {
                Skip();
                std::string key;
                if (!Str(key)) return false;
                Skip();
                if (p_ >= end_ || *p_++ != ':') return false;
                Json child;
                if (!Value(child)) return false;
                v.o.emplace_back(std::move(key), std::move(child));
                Skip();
                if (p_ >= end_) return false;
                if (*p_ == ',') { ++p_; continue; }
                if (*p_ == '}') { ++p_; return true; }
                return false;
            }
        }
        case '[': {
            v.type = Json::Array;
            ++p_;
            Skip();
            if (p_ < end_ && *p_ == ']') { ++p_; return true; }
            for (;;) {
                Json child;
                if (!Value(child)) return false;
                v.a.push_back(std::move(child));
                Skip();
                if (p_ >= end_) return false;
                if (*p_ == ',') { ++p_; continue; }
                if (*p_ == ']') { ++p_; return true; }
                return false;
            }
        }
        case '"': v.type = Json::String; return Str(v.s);
        case 't': v.type = Json::Bool; v.b = true; return Literal("true");
        case 'f': v.type = Json::Bool; v.b = false; return Literal("false");
        case 'n': v.type = Json::Null; return Literal("null");
        default: {
            char* stop = nullptr;
            v.n = std::strtod(p_, &stop);
            if (stop == p_) return false;
            v.type = Json::Number;
            p_ = stop;
            return true;
        }
        }
    }
};

// ---------------------------------------------------------------- shared helpers
// Paths are UTF-8 (that is how scenes store them); Windows needs UTF-16 to open non-ASCII names.
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(Utf8ToWide(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0);
    out.resize((size_t)size);
    if (size > 0) f.read((char*)out.data(), size);
    return (bool)f;
}

inline std::string DirOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

inline std::string LowerExt(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string() : path.substr(dot);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

inline void FlatNormals(std::vector<Vertex>& v, size_t from) {
    for (size_t i = from; i + 2 < v.size(); i += 3) {
        const float ax = v[i + 1].px - v[i].px, ay = v[i + 1].py - v[i].py, az = v[i + 1].pz - v[i].pz;
        const float bx = v[i + 2].px - v[i].px, by = v[i + 2].py - v[i].py, bz = v[i + 2].pz - v[i].pz;
        float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-20f) { nx /= len; ny /= len; nz /= len; }
        for (size_t k = 0; k < 3; ++k) { v[i + k].nx = nx; v[i + k].ny = ny; v[i + k].nz = nz; }
    }
}

inline void NormalizeToUnitBox(std::vector<Vertex>& v) {
    if (v.empty()) return;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const Vertex& x : v) {
        const float p[3] = {x.px, x.py, x.pz};
        for (int i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], p[i]); hi[i] = std::max(hi[i], p[i]); }
    }
    const float longest = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
    if (longest < 1e-12f) return;
    const float s = 1.0f / longest;
    const float c[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    for (Vertex& x : v) { x.px = (x.px - c[0]) * s; x.py = (x.py - c[1]) * s; x.pz = (x.pz - c[2]) * s; }
}

// ---------------------------------------------------------------- OBJ
inline bool LoadObj(const std::string& path, std::vector<Vertex>& out, bool genNormals, std::string& error) {
    std::ifstream f(Utf8ToWide(path));
    if (!f) { error = "cannot open " + path; return false; }

    struct V3 { float x, y, z; };
    std::vector<V3> pos, nrm;
    std::vector<std::pair<float, float>> uv;
    struct Corner { int v = 0, t = 0, n = 0; };

    // OBJ indices are 1-based, negative counts back from the end of what has been read so far.
    auto resolve = [](int i, size_t count) { return i > 0 ? i - 1 : i < 0 ? (int)count + i : -1; };

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") { V3 p{}; ls >> p.x >> p.y >> p.z; pos.push_back(p); }
        else if (tag == "vn") { V3 n{}; ls >> n.x >> n.y >> n.z; nrm.push_back(n); }
        else if (tag == "vt") { float u = 0, v = 0; ls >> u >> v; uv.emplace_back(u, 1.0f - v); } // OBJ v points up
        else if (tag == "f") {
            std::vector<Corner> face;
            std::string tok;
            while (ls >> tok) {
                Corner c;
                int a = 0, b = 0, d = 0;
                if (std::sscanf(tok.c_str(), "%d/%d/%d", &a, &b, &d) == 3) {}
                else if (std::sscanf(tok.c_str(), "%d//%d", &a, &d) == 2) b = 0;
                else if (std::sscanf(tok.c_str(), "%d/%d", &a, &b) == 2) d = 0;
                else if (std::sscanf(tok.c_str(), "%d", &a) == 1) { b = 0; d = 0; }
                else continue;
                c.v = resolve(a, pos.size());
                c.t = b ? resolve(b, uv.size()) : -1;
                c.n = d ? resolve(d, nrm.size()) : -1;
                if (c.v < 0 || c.v >= (int)pos.size()) { error = "OBJ face references a missing vertex"; return false; }
                face.push_back(c);
            }
            for (size_t i = 1; i + 1 < face.size(); ++i) { // triangle fan
                const size_t first = out.size();
                bool hasNormals = true;
                for (const Corner* c : {&face[0], &face[i], &face[i + 1]}) {
                    Vertex v{};
                    v.px = pos[c->v].x; v.py = pos[c->v].y; v.pz = pos[c->v].z;
                    if (c->n >= 0 && c->n < (int)nrm.size()) { v.nx = nrm[c->n].x; v.ny = nrm[c->n].y; v.nz = nrm[c->n].z; }
                    else hasNormals = false;
                    if (c->t >= 0 && c->t < (int)uv.size()) { v.u = uv[c->t].first; v.v = uv[c->t].second; }
                    v.r = v.g = v.b = 0.8f; v.a = 1.0f;
                    out.push_back(v);
                }
                if (!hasNormals && genNormals) FlatNormals(out, first);
            }
        }
    }
    if (out.empty()) { error = "OBJ contains no faces"; return false; }
    return true;
}

// ---------------------------------------------------------------- glTF
struct M4 { float m[16]; }; // column-major, as glTF stores it

inline M4 Identity() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
inline M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
inline M4 FromTRS(const float t[3], const float q[4], const float s[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    M4 r = Identity();
    r.m[0] = (1 - 2 * (y * y + z * z)) * s[0]; r.m[1] = (2 * (x * y + z * w)) * s[0]; r.m[2] = (2 * (x * z - y * w)) * s[0];
    r.m[4] = (2 * (x * y - z * w)) * s[1]; r.m[5] = (1 - 2 * (x * x + z * z)) * s[1]; r.m[6] = (2 * (y * z + x * w)) * s[1];
    r.m[8] = (2 * (x * z + y * w)) * s[2]; r.m[9] = (2 * (y * z - x * w)) * s[2]; r.m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    r.m[12] = t[0]; r.m[13] = t[1]; r.m[14] = t[2];
    return r;
}

inline bool DecodeBase64(const std::string& in, std::vector<uint8_t>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(acc >> bits)); }
    }
    return true;
}

class GltfLoader {
public:
    GltfLoader(const std::string& path, std::vector<Vertex>& out, bool genNormals, std::string& error, ImportedModel* model = nullptr)
        : path_(path), out_(out), genNormals_(genNormals), error_(error), model_(model) {}

    bool Load() {
        std::vector<uint8_t> file;
        if (!ReadFile(path_, file)) return Fail("cannot open " + path_);

        const char* jsonText = (const char*)file.data();
        size_t jsonSize = file.size();
        if (file.size() >= 12 && std::memcmp(file.data(), "glTF", 4) == 0) { // GLB container
            size_t at = 12;
            jsonText = nullptr;
            while (at + 8 <= file.size()) {
                uint32_t len, type;
                std::memcpy(&len, &file[at], 4);
                std::memcpy(&type, &file[at + 4], 4);
                at += 8;
                if (at + len > file.size()) return Fail("GLB chunk runs past the end of the file");
                if (type == 0x4E4F534A) { jsonText = (const char*)&file[at]; jsonSize = len; }                   // JSON
                else if (type == 0x004E4942 && glbBin_.empty()) glbBin_.assign(file.begin() + at, file.begin() + at + len); // BIN
                at += (len + 3) & ~3u;
            }
            if (!jsonText) return Fail("GLB has no JSON chunk");
        }
        if (!JsonParser(jsonText, jsonSize).Parse(doc_) || doc_.type != Json::Object) return Fail("invalid glTF JSON");

        if (!LoadBuffers()) return false;

        const Json* scenes = doc_.Find("scenes");
        const Json* nodes = doc_.Find("nodes");
        if (nodes && nodes->type == Json::Array) {
            std::vector<int> roots;
            const Json* scene = scenes ? scenes->At((size_t)doc_.Find("scene") ? doc_.Find("scene")->Int() : 0) : nullptr;
            if (const Json* r = scene ? scene->Find("nodes") : nullptr)
                for (const Json& n : r->a) roots.push_back(n.Int());
            if (roots.empty()) // no scene: treat every node nobody else lists as a child as a root
                for (int i = 0; i < (int)nodes->a.size(); ++i) {
                    bool child = false;
                    for (const Json& other : nodes->a)
                        if (const Json* c = other.Find("children"))
                            for (const Json& k : c->a) child |= k.Int() == i;
                    if (!child) roots.push_back(i);
                }
            for (int r : roots)
                if (!Visit(r, Identity(), 0)) return false;
        } else if (const Json* meshes = doc_.Find("meshes")) { // meshes without nodes
            for (size_t i = 0; i < meshes->a.size(); ++i)
                if (!EmitMesh((int)i, Identity())) return false;
        }
        if (out_.empty()) return Fail("glTF contains no triangle meshes");
        if (model_) {
            LoadSkin();
            SoftenWrists();
            // characters\regular is the generic default human: none of Wyatt's beard bake or beard cards. Head hair comes from the
            // character's own hair folder (HairDir), which the generic human has only when someone gave it one.
            if (!IsGenericHuman()) {
                ApplyBeardBake();
                AddHairCards();
            }
            AddHairCap();
            AddHairShell();
            AddScalpCards();
            AddHairCards(L"brow_cards.json", L"brow_strands.png"); // gameenginetools/brow_cards: brows and lashes, on the generic human too
            AddHairCards(L"lash_cards.json", L"lash_strands.png");
            ApplyBrowBase();
            ApplyEyeMaps();
            ApplyGlassWear();
            LoadClips();
        }
        return true;
    }

private:
    // ---- hair helpers ------------------------------------------------------------------------------------------------------------
    // The head's skin vertices in a uniform grid: nearest-vertex queries for binding cards to joints and for the skin under a card.
    struct SkinGrid {
        float lo[3] = {0, 0, 0}, cell = 0.008f;
        int dim[3] = {0, 0, 0};
        std::vector<std::vector<uint32_t>> bins;
        const std::vector<Vertex>* v = nullptr;
        bool empty() const { return bins.empty(); }
        void Build(const std::vector<Vertex>& verts, const std::vector<size_t>& which) {
            v = &verts;
            bins.clear();
            if (which.empty()) return;
            float hi[3] = {-1e30f, -1e30f, -1e30f};
            lo[0] = lo[1] = lo[2] = 1e30f;
            for (size_t k : which) {
                const float p[3] = {verts[k].px, verts[k].py, verts[k].pz};
                for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], p[c]); hi[c] = std::max(hi[c], p[c]); }
            }
            for (int c = 0; c < 3; ++c) dim[c] = std::clamp((int)((hi[c] - lo[c]) / cell) + 1, 1, 512);
            bins.assign((size_t)dim[0] * dim[1] * dim[2], {});
            for (size_t k : which) bins[Bin(verts[k].px, verts[k].py, verts[k].pz)].push_back((uint32_t)k);
        }
        int Axis(float x, int c) const { return std::clamp((int)((x - lo[c]) / cell), 0, dim[c] - 1); }
        size_t Bin(float x, float y, float z) const { return ((size_t)Axis(z, 2) * dim[1] + Axis(y, 1)) * dim[0] + Axis(x, 0); }
        // Nearest vertex: rings of cells outward until no closer one can exist. SIZE_MAX when the grid is empty.
        size_t Nearest(float x, float y, float z) const {
            if (bins.empty()) return SIZE_MAX;
            const int cx = Axis(x, 0), cy = Axis(y, 1), cz = Axis(z, 2);
            size_t best = SIZE_MAX;
            float bestD = 1e30f;
            const int maxR = std::max(dim[0], std::max(dim[1], dim[2]));
            for (int r = 0; r <= maxR; ++r) {
                for (int dz = -r; dz <= r; ++dz)
                    for (int dy = -r; dy <= r; ++dy)
                        for (int dx = -r; dx <= r; ++dx) {
                            if (std::max(std::abs(dx), std::max(std::abs(dy), std::abs(dz))) != r) continue;
                            const int X = cx + dx, Y = cy + dy, Z = cz + dz;
                            if (X < 0 || Y < 0 || Z < 0 || X >= dim[0] || Y >= dim[1] || Z >= dim[2]) continue;
                            for (uint32_t k : bins[((size_t)Z * dim[1] + Y) * dim[0] + X]) {
                                const Vertex& q = (*v)[k];
                                const float d = (q.px - x) * (q.px - x) + (q.py - y) * (q.py - y) + (q.pz - z) * (q.pz - z);
                                if (d < bestD) { bestD = d; best = k; }
                            }
                        }
                if (best != SIZE_MAX && bestD <= (r * cell) * (r * cell)) break; // anything in ring r+1 is at least r cells away
            }
            return best;
        }
    };
    SkinGrid skinGrid_;
    const SkinGrid& HeadSkin() {
        if (skinGrid_.empty()) {
            std::vector<size_t> which;
            for (const ImportedPart& part : model_->parts)
                if (part.skin)
                    for (size_t k = part.first; k < part.first + part.count; ++k) which.push_back(k);
            skinGrid_.Build(out_, which);
        }
        return skinGrid_;
    }

    bool IsGenericHuman() const {
        return path_.find("\\characters\\regular\\") != std::string::npos || path_.find("/characters/regular/") != std::string::npos;
    }
    // A character's head hair: a "hair" folder beside its glb (scalp_cards*.json, hair_cap.json, hairline_cards.json,
    // hair_look.json), else Wyatt's Assets\hair\wyatt for his head. Empty: this character has no head hair (the generic human
    // without a folder of its own). Ends in a backslash.
    std::wstring HairDir() const {
        std::string file = path_.substr(0, path_.find('#'));
        const size_t slash = file.find_last_of("\\/");
        if (slash != std::string::npos) {
            const std::wstring own = std::wstring(file.begin(), file.begin() + (std::ptrdiff_t)slash + 1) + L"hair\\";
            if (std::ifstream(own + L"hair_style.json") || std::ifstream(own + L"scalp_cards.json") || std::ifstream(own + L"hair_cap.json")) return own;
        }
        return IsGenericHuman() ? std::wstring() : ExeDirectory() + L"\\..\\Assets\\hair\\wyatt\\";
    }
    // Where the head hair's cards, cap and hairline come from: hair_style.json in the hair folder ({"style": "side_part"}, written
    // by the Character Creator's HAIR STYLE picker) names a style in Assets\hair\styles\ (baked by hair_groom/groom.py --install and
    // cap.py); "none" is bald. Without it, the hair folder holds them itself. Empty: no head hair.
    std::wstring ScalpDir() const {
        const std::wstring own = HairDir();
        if (own.empty()) return own;
        std::ifstream f(own + L"hair_style.json");
        if (!f) return own;
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        Json doc;
        if (!JsonParser(text.data(), text.size()).Parse(doc)) return own;
        const Json* style = doc.Find("style");
        if (!style || style->s.empty()) return own;
        if (style->s == "none") return std::wstring();
        const std::wstring dir = ExeDirectory() + L"\\..\\Assets\\hair\\styles\\" + std::wstring(style->s.begin(), style->s.end()) + L"\\";
        return std::ifstream(dir + L"scalp_cards.json") ? dir : own;
    }

    // Per character hair colour (character/hair_vertex.h HairTint), from hair_look.json in the character's hair folder (HairDir):
    //   {"scalp": {"melanin": 0.55, "red": 0.16, "grey": 0.0}, "beard": {...}}. A missing file or key keeps the defaults below.
    HairTint HairLook(const char* which) {
        HairTint tint;
        if (std::string(which) == "beard") { tint.melanin = 0.6f; tint.red = 0.24f; tint.grey = 0.04f; }
        else { tint.melanin = 0.55f; tint.red = 0.16f; tint.grey = 0.0f; }
        std::vector<uint8_t> bytes;
        Json doc;
        std::wstring dir = HairDir();
        if (dir.empty()) dir = ExeDirectory() + L"\\..\\Assets\\hair\\wyatt\\";
        if (!ReadWhole(dir + L"hair_look.json", bytes) || !JsonParser((const char*)bytes.data(), bytes.size()).Parse(doc))
            return tint;
        if (const Json* o = doc.Find(which)) {
            if (const Json* x = o->Find("melanin")) tint.melanin = (float)x->Num(tint.melanin);
            if (const Json* x = o->Find("red")) tint.red = (float)x->Num(tint.red);
            if (const Json* x = o->Find("grey")) tint.grey = (float)x->Num(tint.grey);
        }
        return tint;
    }

    // How deep each hair vertex sits inside the hair volume: its height over the skin against the highest hair within about a
    // centimetre. 0 on the outside of the hairdo .. 1 at `thick` metres below it. Written into green's fraction (hair_vertex.h).
    void HairVolumeDepth(std::vector<Vertex>& verts, float thick) {
        const SkinGrid& skin = HeadSkin();
        if (skin.empty() || verts.empty()) return;
        std::vector<float> h(verts.size(), 0.0f);
        for (size_t i = 0; i < verts.size(); ++i) {
            const Vertex& x = verts[i];
            const size_t k = skin.Nearest(x.px, x.py, x.pz);
            if (k == SIZE_MAX) continue;
            const Vertex& s = out_[k];
            h[i] = std::max(0.0f, (x.px - s.px) * s.nx + (x.py - s.py) * s.ny + (x.pz - s.pz) * s.nz);
        }
        const float cell = 0.01f;
        std::map<std::tuple<int, int, int>, float> top;
        const auto key = [&](const Vertex& x, int dx, int dy, int dz) {
            return std::make_tuple((int)std::floor(x.px / cell) + dx, (int)std::floor(x.py / cell) + dy, (int)std::floor(x.pz / cell) + dz);
        };
        for (size_t i = 0; i < verts.size(); ++i) { float& t = top[key(verts[i], 0, 0, 0)]; t = std::max(t, h[i]); }
        for (size_t i = 0; i < verts.size(); ++i) {
            float H = h[i];
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (const auto it = top.find(key(verts[i], dx, dy, dz)); it != top.end()) H = std::max(H, it->second);
            verts[i].g = std::floor(verts[i].g) + std::clamp((H - h[i]) / thick, 0.0f, 1.0f) * 0.999f;
        }
    }

    // The generated strand atlas for `name` (hair or beard, gameenginetools\hair_atlas): data (R brightness, G root-tip, B strand
    // random, A coverage) and material (R occlusion, G roughness, B specular shift). Both or nothing.
    bool HairAtlas(const wchar_t* name, std::vector<uint8_t>& data, std::vector<uint8_t>& material, Json* layout = nullptr) {
        const std::wstring gen = ExeDirectory() + L"\\..\\Assets\\hair\\generated\\" + name;
        if (!ReadWhole(gen + L"_data.png", data) || !ReadWhole(gen + L"_material.png", material)) return false;
        if (layout) {
            std::vector<uint8_t> bytes;
            if (!ReadWhole(gen + L"_layout.json", bytes) || !JsonParser((const char*)bytes.data(), bytes.size()).Parse(*layout)) return false;
        }
        return true;
    }

    // Wyatt's authored brow and beard cards (from the hair maker in WesternGameIOS, copied to Assets\hair\wyatt): strips that
    // follow a flow direction. Positions are the hair maker's decimetres on the same head mesh, so x0.1 puts them on this one. Each
    // card rides the skin vertex nearest its root and is drawn both sides with one outward normal (hair is lit as a volume, not as
    // a sheet). Beard cards sample the generated beard atlas and take their colour from melanin (hair_look.json); brows and lashes
    // keep their own coloured strand sheets. Vertex data: character/hair_vertex.h. `sheet`: a card file with its own strand sheet
    // (brow_cards.json / lash_cards.json from gameenginetools/brow_cards, each card's rect into that sheet).
    static bool ReadWhole(const std::wstring& path, std::vector<uint8_t>& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return !out.empty();
    }

    void AddHairCards(const wchar_t* file = L"beard_cards.json", const wchar_t* sheet = nullptr) {
        const bool brows = sheet != nullptr;
        if (path_.find("body_head") == std::string::npos) return; // the cards are authored on Wyatt's head
        using F3 = std::array<float, 3>;
        const std::wstring dir = ExeDirectory() + L"\\..\\Assets\\hair\\wyatt\\";
        std::vector<uint8_t> jsonBytes, png, material;
        // A head whose eyes and brows sit elsewhere than the base default's carries its own card file beside the mesh
        // (lash_cards.py --head); the strand sheets still come from the shared folder.
        const std::string meshFile = path_.substr(0, path_.find('#'));
        const size_t meshSlash = meshFile.find_last_of("\\/");
        const std::wstring own = meshSlash == std::string::npos ? std::wstring() : Utf8ToWide(meshFile.substr(0, meshSlash + 1)) + file;
        if ((own.empty() || !ReadWhole(own, jsonBytes)) && !ReadWhole(dir + file, jsonBytes)) return;
        Json doc;
        if (!JsonParser((const char*)jsonBytes.data(), jsonBytes.size()).Parse(doc)) return;
        const Json* cards = doc.Find("cards");
        if (!cards || cards->a.empty()) return;
        // Beard: the generated atlas, each card picking one of its short lock cells from the card's seed. Without it, the old
        // coloured sheet (strand_cards.png) and the cards' own rects.
        Json layout;
        std::vector<const Json*> lockRects;
        if (!brows && HairAtlas(L"beard", png, material, &layout))
            if (const Json* cells = layout.Find("cells"))
                for (const Json& cell : cells->a)
                    if (const Json* kind = cell.Find("kind"); kind && (kind->s == "beard_lock" || kind->s == "beard_curl"))
                        if (const Json* r = cell.Find("rect")) lockRects.push_back(r);
        if (lockRects.empty()) {
            material.clear();
            if (!ReadWhole(dir + (brows ? std::wstring(sheet) : std::wstring(L"strand_cards.png")), png)) return;
        }
        const HairTint beardTint = HairLook("beard");
        const HairTint* tint = lockRects.empty() ? nullptr : &beardTint; // null: colour from the sheet
        const int cls = brows ? kHairFine : kHairBeard;
        const Json* K = doc.Find("K");
        const auto knob = [&](const char* name) { const Json* v = K ? K->Find(name) : nullptr; return v ? (float)v->Num(1.0) : 1.0f; };
        const auto vec = [](const Json* a) { return a && a->a.size() >= 3 ? F3{(float)a->a[0].Num(), (float)a->a[1].Num(), (float)a->a[2].Num()} : F3{0, 0, 0}; };
        const auto add = [](F3 a, F3 b, float s = 1.0f) { return F3{a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s}; };
        const auto cross = [](F3 a, F3 b) { return F3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
        const auto norm = [](F3 a) { const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); return l > 1e-12f ? F3{a[0] / l, a[1] / l, a[2] / l} : F3{0, 1, 0}; };
        const auto scale = [](F3 a, float s) { return F3{a[0] * s, a[1] * s, a[2] * s}; };

        // The skin the cards are bound to, and a check that the cards belong to this mesh at all.
        const SkinGrid& skin = HeadSkin();
        if (skin.empty()) return;
        const F3 lo = {skin.lo[0], skin.lo[1], skin.lo[2]};
        const F3 hi = {skin.lo[0] + skin.dim[0] * skin.cell, skin.lo[1] + skin.dim[1] * skin.cell, skin.lo[2] + skin.dim[2] * skin.cell};
        size_t inside = 0;
        for (const Json& c : cards->a) {
            const Json* sp = c.Find("strip");
            const F3 p = scale(vec(sp && !sp->a.empty() ? &sp->a[0] : c.Find("p")), 0.1f);
            inside += p[0] > lo[0] - 0.02f && p[0] < hi[0] + 0.02f && p[1] > lo[1] - 0.02f && p[1] < hi[1] + 0.02f && p[2] > lo[2] - 0.02f && p[2] < hi[2] + 0.02f;
        }
        if (inside * 2 < cards->a.size()) return;

        const bool skinned = model_->vertexSkin.size() == out_.size();
        std::vector<Vertex> made;
        std::vector<VertexSkin> madeSkin;
        float cardRand = 0.0f;
        int cardCls = cls; // a card may say it is another class ("cls": kHairWet for the caruncle and tear line)
        const auto emit = [&](F3 p, F3 n, float t, float u, float v, size_t bindTo) {
            Vertex x{};
            x.px = p[0]; x.py = p[1]; x.pz = p[2];
            x.nx = n[0]; x.ny = n[1]; x.nz = n[2];
            x.r = HairPackR(t, cardRand); x.g = HairPackG(cardCls, 0.0f); x.b = HairPackB(tint); x.a = kHairAlpha;
            x.u = u; x.v = v;
            made.push_back(x);
            if (skinned) madeSkin.push_back(model_->vertexSkin[bindTo]);
        };
        const auto nearestSkin = [&](F3 p) { const size_t k = skin.Nearest(p[0], p[1], p[2]); return k == SIZE_MAX ? (size_t)0 : k; };
        // front and back share the outward normal: the back is the same strands seen from the other side
        static const int front[6] = {0, 2, 1, 1, 2, 3}, back[6] = {0, 1, 2, 1, 3, 2};
        uint32_t cardIndex = 0;
        for (const Json& c : cards->a) {
            ++cardIndex;
            const Json* seedJ = c.Find("seed");
            cardRand = seedJ ? (float)seedJ->Num(0.0) : (float)((cardIndex * 2654435761u) >> 16) / 65536.0f;
            cardRand -= std::floor(cardRand);
            cardCls = c.Find("cls") ? std::clamp((int)c.Find("cls")->Num(cls), 0, kHairWet) : cls;
            const Json* rect = c.Find("rect");
            // Strip card (lash_cards.py): one ribbon along a polyline of roots (the lid edge), each root swept out in `seg` steps
            // from its start direction n[i] toward its curl direction f[i] over len[i]; u runs along the strip, v along the lashes.
            if (const Json* strip = c.Find("strip"); strip && rect && strip->a.size() >= 2) {
                const Json *ns = c.Find("n"), *fs = c.Find("f"), *ls = c.Find("len");
                const size_t m = strip->a.size();
                if (!ns || !fs || !ls || ns->a.size() != m || fs->a.size() != m || ls->a.size() != m) continue;
                const int seg = std::clamp((int)(c.Find("seg") ? c.Find("seg")->Num(4) : 4), 1, 8);
                const float u0 = (float)rect->Find("u0")->Num(), u1 = (float)rect->Find("u1")->Num(), v0 = (float)rect->Find("v0")->Num(), v1 = (float)rect->Find("v1")->Num();
                std::vector<std::vector<F3>> grid(m);
                std::vector<float> arc(m, 0.0f);
                std::vector<size_t> binds(m);
                std::vector<F3> outward(m);
                for (size_t i = 0; i < m; ++i) {
                    const F3 root = scale(vec(&strip->a[i]), 0.1f), n = norm(vec(&ns->a[i])), f = norm(vec(&fs->a[i]));
                    const float len = (float)ls->a[i].Num(0.05) * knob("len") * 0.1f;
                    binds[i] = nearestSkin(root);
                    outward[i] = n;
                    grid[i].push_back(root);
                    for (int s = 1; s <= seg; ++s) {
                        const float tm = ((float)s - 0.5f) / (float)seg;
                        grid[i].push_back(add(grid[i].back(), norm(add(scale(n, 1.0f - tm), f, tm)), len / (float)seg));
                    }
                    if (i) { const F3 d = add(root, grid[i - 1][0], -1.0f); arc[i] = arc[i - 1] + std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); }
                }
                for (size_t i = 0; i + 1 < m; ++i)
                    for (int s = 0; s < seg; ++s) {
                        F3 corner[4];
                        float uu[4], vv[4], tt[4];
                        for (int k = 0; k < 4; ++k) {
                            const size_t col = i + (k & 1);
                            const int row = s + (k >> 1);
                            corner[k] = grid[col][(size_t)row];
                            uu[k] = u0 + (u1 - u0) * arc[col] / std::max(arc[m - 1], 1e-6f);
                            tt[k] = (float)row / (float)seg;
                            vv[k] = 1.0f - (v0 + (v1 - v0) * tt[k]);
                        }
                        F3 nm = norm(cross(add(corner[1], corner[0], -1.0f), add(corner[2], corner[0], -1.0f)));
                        const F3 o = add(outward[i], outward[i + 1]);
                        if (nm[0] * o[0] + nm[1] * o[1] + nm[2] * o[2] < 0.0f) nm = scale(nm, -1.0f);
                        for (int k = 0; k < 6; ++k) emit(corner[front[k]], nm, tt[front[k]], uu[front[k]], vv[front[k]], binds[i + (front[k] & 1)]);
                        for (int k = 0; k < 6; ++k) emit(corner[back[k]], nm, tt[back[k]], uu[back[k]], vv[back[k]], binds[i + (back[k] & 1)]);
                    }
                continue;
            }
            if (!rect || !c.Find("p") || !c.Find("n") || !c.Find("f")) continue;
            if (!lockRects.empty()) rect = lockRects[std::min((size_t)(cardRand * (float)lockRects.size()), lockRects.size() - 1)];
            const F3 root = scale(vec(c.Find("p")), 0.1f), n = norm(vec(c.Find("n"))), f = norm(vec(c.Find("f")));
            const auto num = [&](const char* name, double fallback) { const Json* v = c.Find(name); return v ? (float)v->Num(fallback) : (float)fallback; };
            const float len = num("len", 0.1) * knob("len") * 0.1f, wid = num("wid", 0.05) * knob("wid") * 0.1f;
            const int seg = std::clamp((int)num("seg", 3), 1, 8);
            const bool hasRise = c.Find("rise") != nullptr;
            const float rise = num("rise", 0) * knob("rise"), bend = num("bend", 0) * knob("bend"), lay = num("lay", 0.7), grav = num("grav", 0);
            const float wob = num("wob", 0), wf = num("wf", 1), ph = num("ph", 0), yaw = num("yaw", 0) * knob("yaw");
            const float u0 = (float)rect->Find("u0")->Num(), u1 = (float)rect->Find("u1")->Num(), v0 = (float)rect->Find("v0")->Num(), v1 = (float)rect->Find("v1")->Num();
            F3 side = cross(n, f);
            if (side[0] * side[0] + side[1] * side[1] + side[2] * side[2] < 1e-5f) side = {1, 0, 0};
            side = norm(side);
            // Standoff keeps the strip off the skin's curve; the hair maker measured the skin along the card instead (liftAlong).
            // brow / lash cards already sit on the skin segment by segment (brow_cards.py snaps each joint): only a hair's lift
            const auto stand = [brows](float t) { return brows ? 0.0002f + 0.0003f * t : 0.0004f + 0.0012f * t; };
            std::vector<F3> pts = {add(root, n, 0.0001f)};
            for (int i = 1; i <= seg; ++i) {
                const float t = (float)i / (float)seg, tm = ((float)i - 0.5f) / (float)seg;
                F3 d;
                if (hasRise) d = add(scale(n, rise), f, 2.0f * bend * tm);
                else { const float a = std::min(1.0f, lay + (1.0f - lay) * std::pow(t, 0.7f)); d = add(scale(n, 1.0f - a), f, a); }
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1e-10f) d = f;
                d = norm(add(norm(d), F3{0, -1, 0}, grav * t));
                F3 q = add(pts[(size_t)i - 1], d, len / (float)seg);
                if (wob != 0.0f) q = add(q, side, wob * 0.1f * (std::sin(t * wf * 6.2831853f + ph) - std::sin((float)(i - 1) / (float)seg * wf * 6.2831853f + ph)));
                pts.push_back(add(q, n, stand(t)));
            }
            std::vector<F3> dirs((size_t)seg + 1);
            for (int i = 1; i <= seg; ++i) dirs[(size_t)i] = norm(add(pts[(size_t)i], pts[(size_t)i - 1], -1.0f));
            dirs[0] = dirs[1];
            if (std::fabs(yaw) > 1e-4f) { // Rodrigues: turn the card about its first direction
                const float cs = std::cos(yaw), sn = std::sin(yaw);
                const F3 k = dirs[0], kx = cross(k, side);
                const float kd = k[0] * side[0] + k[1] * side[1] + k[2] * side[2];
                side = norm(add(add(scale(side, cs), kx, sn), k, kd * (1.0f - cs)));
            }
            const size_t bind = nearestSkin(root); // the card rides that vertex's joints
            for (int i = 0; i < seg; ++i) {
                F3 corner[4], nm[4];
                float tt[4], vv[4];
                for (int j = 0; j < 2; ++j) {
                    const int ring = i + j;
                    const float t = (float)ring / (float)seg, w = wid * (1.0f - 0.1f * t) * 0.5f;
                    F3 nring = norm(add(scale(cross(dirs[(size_t)ring], side), 0.35f), n, 0.65f));
                    if (nring[0] * n[0] + nring[1] * n[1] + nring[2] * n[2] < 0.0f) nring = scale(nring, -1.0f);
                    corner[j * 2] = add(pts[(size_t)ring], side, -w);
                    corner[j * 2 + 1] = add(pts[(size_t)ring], side, w);
                    nm[j * 2] = nm[j * 2 + 1] = nring;
                    tt[j * 2] = tt[j * 2 + 1] = t;
                    vv[j * 2] = vv[j * 2 + 1] = 1.0f - (v0 + (v1 - v0) * t); // the sheet's v runs up the image; the engine's runs down
                }
                const float uu[4] = {u0, u1, u0, u1};
                for (int k = 0; k < 6; ++k) emit(corner[front[k]], nm[front[k]], tt[front[k]], uu[front[k]], vv[front[k]], bind);
                for (int k = 0; k < 6; ++k) emit(corner[back[k]], nm[back[k]], tt[back[k]], uu[back[k]], vv[back[k]], bind);
            }
        }
        if (made.empty()) return;
        if (!brows) HairVolumeDepth(made, 0.008f); // a beard is a thinner volume than a head of hair
        ImportedPart part;
        part.first = out_.size();
        part.count = made.size();
        part.albedo = (int)model_->images.size();
        part.hair = true;
        part.brow = brows && std::wstring(file) == L"brow_cards.json";
        model_->images.push_back(std::move(png));
        if (!material.empty()) { part.normal = (int)model_->images.size(); model_->images.push_back(std::move(material)); }
        out_.insert(out_.end(), made.begin(), made.end());
        if (skinned) model_->vertexSkin.insert(model_->vertexSkin.end(), madeSkin.begin(), madeSkin.end());
        model_->parts.push_back(part);
    }

    // Splits a glass part into panes (vertices that share a position, to 1 cm, are one pane) and writes each vertex's place in its pane
    // into the colour GlassPS ignores otherwise: r, g = 0..1 across the pane's two larger axes, b = a per-pane random seed.
    // Interpolated over a rectangle this gives the shader pane-local coordinates, for dirt gathered at corners and edges.
    void TagPanes(size_t first, size_t last) {
        const size_t n = last - first;
        std::vector<size_t> parent(n);
        for (size_t k = 0; k < n; ++k) parent[k] = k;
        const auto find = [&](size_t k) { while (parent[k] != k) k = parent[k] = parent[parent[k]]; return k; };
        std::map<std::tuple<int, int, int>, size_t> seen;
        for (size_t k = 0; k < n; ++k) {
            const Vertex& v = out_[first + k];
            const auto key = std::make_tuple((int)std::lround(v.px * 100.0f), (int)std::lround(v.py * 100.0f), (int)std::lround(v.pz * 100.0f));
            const auto it = seen.emplace(key, k);
            if (!it.second) parent[find(k)] = find(it.first->second);
        }
        for (size_t k = 0; k + 2 < n; k += 3) { // corners of one triangle are one pane
            parent[find(k + 1)] = find(k);
            parent[find(k + 2)] = find(k);
        }
        struct Box { float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}; };
        std::map<size_t, Box> boxes;
        for (size_t k = 0; k < n; ++k) {
            const Vertex& v = out_[first + k];
            const float p[3] = {v.px, v.py, v.pz};
            Box& b = boxes[find(k)];
            for (int a = 0; a < 3; ++a) { b.lo[a] = std::min(b.lo[a], p[a]); b.hi[a] = std::max(b.hi[a], p[a]); }
        }
        for (size_t k = 0; k < n; ++k) {
            Vertex& v = out_[first + k];
            const Box& b = boxes[find(k)];
            int thin = 0;
            for (int a = 1; a < 3; ++a) if (b.hi[a] - b.lo[a] < b.hi[thin] - b.lo[thin]) thin = a;
            const int a0 = thin == 0 ? 1 : 0, a1 = thin == 2 ? 1 : 2;
            const float p[3] = {v.px, v.py, v.pz};
            const auto frac01 = [&](int a) { return std::clamp((p[a] - b.lo[a]) / std::max(b.hi[a] - b.lo[a], 1e-4f), 0.0f, 1.0f); };
            // y is the vertical of a wall pane: put it in g so "down" is g = 0
            const int across = a0 == 1 ? a1 : a0, up = a0 == 1 ? a0 : a1;
            uint32_t h = (uint32_t)std::lround((b.lo[0] + b.hi[0]) * 50.0f) * 73856093u ^ (uint32_t)std::lround((b.lo[1] + b.hi[1]) * 50.0f) * 19349663u ^
                         (uint32_t)std::lround((b.lo[2] + b.hi[2]) * 50.0f) * 83492791u;
            h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            v.r = frac01(across);
            v.g = frac01(up);
            v.b = (float)(h & 0xFFFFu) / 65535.0f;
        }
    }

    // Window panes share one grayscale wear mask (Assets\glass\glass_wear_mask.png: scratches, fingerprints, drips, grime; brighter = dirtier).
    // GlassPS reads it from the albedo slot, so no extra binding is needed on either backend.
    void ApplyGlassWear() {
        bool any = false;
        for (const ImportedPart& part : model_->parts) any |= part.glass;
        std::vector<uint8_t> png;
        if (!any || !ReadWhole(ExeDirectory() + L"\\..\\Assets\\glass\\glass_wear_mask.png", png)) return;
        const int image = (int)model_->images.size();
        model_->images.push_back(std::move(png));
        for (ImportedPart& part : model_->parts)
            if (part.glass) part.albedo = image;
    }

    // Scalp cards from gameenginetools\hair_groom\groom.py --install (or the older generate_styles.py): strips in the head's own
    // frame, two vertices per ring. Up to three detail levels, each its own part with a camera distance range:
    //   scalp_cards.json (LOD 0, full), scalp_cards_lod1.json, scalp_cards_lod2.json; past the last level the painted scalp carries it.
    // Format v2 adds per vertex `card`, `rand` (a card's random look, the same at every level) and `layer`; v1 files get a random
    // per card from their triangle connectivity. Textured from the generated hair atlas (data + material), coloured by melanin
    // (hair_look.json "scalp"), each vertex rides its nearest skin vertex, drawn both sides with one outward normal: the skin's,
    // bent a little toward the card's own, so the hair shades as one smooth volume. Delete scalp_cards.json to remove them.
    void AddScalpCards() {
        if (path_.find("body_head") == std::string::npos) return;
        const std::wstring dir = ScalpDir();
        if (dir.empty()) return;
        std::vector<uint8_t> data, material;
        if (!HairAtlas(L"hair", data, material)) return;
        const SkinGrid& skin = HeadSkin();
        if (skin.empty()) return;
        const HairTint tint = HairLook("scalp");
        const bool skinned = model_->vertexSkin.size() == out_.size();
        // distance bands: full detail for close-ups and the character creator, medium for a conversation, low for gameplay range
        static const wchar_t* kFiles[3] = {L"scalp_cards.json", L"scalp_cards_lod1.json", L"scalp_cards_lod2.json"};
        static const float kNear[3] = {0.0f, 2.5f, 7.0f}, kFar[3] = {2.5f, 7.0f, 45.0f};
        int dataImage = -1, materialImage = -1, levels = 0;
        for (int lod = 0; lod < 3; ++lod) {
            std::vector<uint8_t> jsonBytes;
            Json doc;
            if (!ReadWhole(dir + kFiles[lod], jsonBytes) || !JsonParser((const char*)jsonBytes.data(), jsonBytes.size()).Parse(doc)) {
                if (lod == 0) return;
                break;
            }
            std::vector<Vertex> made;
            std::vector<VertexSkin> madeSkin;
            if (!ScalpLevel(doc, skin, tint, skinned, made, madeSkin)) { if (lod == 0) return; break; }
            HairVolumeDepth(made, 0.014f);
            if (dataImage < 0) {
                dataImage = (int)model_->images.size();
                model_->images.push_back(data);
                materialImage = (int)model_->images.size();
                model_->images.push_back(material);
            }
            ImportedPart part;
            part.first = out_.size();
            part.count = made.size();
            part.albedo = dataImage;
            part.normal = materialImage;
            part.hair = true;
            part.lodMin = kNear[lod];
            part.lodMax = kFar[lod];
            out_.insert(out_.end(), made.begin(), made.end());
            if (skinned) model_->vertexSkin.insert(model_->vertexSkin.end(), madeSkin.begin(), madeSkin.end());
            model_->parts.push_back(part);
            ++levels;
        }
        // One level only: it draws at every distance, as before levels existed.
        if (levels == 1) model_->parts.back().lodMin = model_->parts.back().lodMax = 0.0f;
        else if (levels > 1) model_->parts.back().lodMax = kFar[2];
        // The fine hairline (cap.py): short, soft cards rooted in the skin along the cap's edge, so the hairline frays into single
        // hairs instead of ending in card edges. Close range only: past the medium level the fuzz is under a pixel.
        std::vector<uint8_t> lineBytes;
        Json line;
        if (dataImage >= 0 && ReadWhole(dir + L"hairline_cards.json", lineBytes) && JsonParser((const char*)lineBytes.data(), lineBytes.size()).Parse(line)) {
            std::vector<Vertex> made;
            std::vector<VertexSkin> madeSkin;
            if (ScalpLevel(line, skin, tint, skinned, made, madeSkin)) {
                ImportedPart part;
                part.first = out_.size();
                part.count = made.size();
                part.albedo = dataImage;
                part.normal = materialImage;
                part.hair = true;
                part.lodMin = 0.0f;
                part.lodMax = levels > 1 ? kFar[1] : 0.0f;
                out_.insert(out_.end(), made.begin(), made.end());
                if (skinned) model_->vertexSkin.insert(model_->vertexSkin.end(), madeSkin.begin(), madeSkin.end());
                model_->parts.push_back(part);
            }
        }
    }

    // The hair cap (gameenginetools\hair_groom\cap.py): the clay hair volume, decimated and shrunk to sit under the cards, so a gap
    // between cards shows more hair, not scalp. One-sided, facing out; each vertex rides its nearest skin vertex. Drawn at every
    // distance: past the last card level it is the hair. Its colour is the scalp tint; HairCardPS draws strands along its flow.
    void AddHairCap() {
        if (path_.find("body_head") == std::string::npos) return;
        std::vector<uint8_t> jsonBytes, data, material;
        Json doc;
        const std::wstring dir = ScalpDir();
        if (dir.empty() || !ReadWhole(dir + L"hair_cap.json", jsonBytes) ||
            !JsonParser((const char*)jsonBytes.data(), jsonBytes.size()).Parse(doc) || !HairAtlas(L"hair", data, material))
            return;
        const Json *pos = doc.Find("pos"), *nrm = doc.Find("nrm"), *idx = doc.Find("idx"), *cov = doc.Find("cov"), *uv = doc.Find("uv");
        if (!pos || !nrm || !idx || !cov || !uv || pos->a.size() % 3 || nrm->a.size() != pos->a.size() || cov->a.size() * 3 != pos->a.size() ||
            uv->a.size() * 3 != pos->a.size() * 2 || idx->a.size() % 3)
            return;
        const SkinGrid& skin = HeadSkin();
        if (skin.empty()) return;
        const size_t nv = cov->a.size();
        const HairTint tint = HairLook("scalp");
        const bool skinned = model_->vertexSkin.size() == out_.size();
        std::vector<Vertex> verts(nv);
        std::vector<size_t> bind(nv, 0);
        for (size_t i = 0; i < nv; ++i) {
            Vertex& x = verts[i];
            x.px = (float)pos->a[i * 3].Num(); x.py = (float)pos->a[i * 3 + 1].Num(); x.pz = (float)pos->a[i * 3 + 2].Num();
            x.nx = (float)nrm->a[i * 3].Num(); x.ny = (float)nrm->a[i * 3 + 1].Num(); x.nz = (float)nrm->a[i * 3 + 2].Num();
            x.r = HairPackR((float)cov->a[i].Num(), 0.5f); x.g = HairPackG(kHairCap, 0.7f); x.b = HairPackB(&tint); x.a = kHairAlpha;
            x.u = (float)uv->a[i * 2].Num(); x.v = (float)uv->a[i * 2 + 1].Num();
            const size_t k = skin.Nearest(x.px, x.py, x.pz);
            bind[i] = k == SIZE_MAX ? 0 : k;
        }
        std::vector<Vertex> made;
        std::vector<VertexSkin> madeSkin;
        for (size_t f = 0; f + 2 < idx->a.size(); f += 3)
            for (int k = 0; k < 3; ++k) {
                const size_t v = (size_t)idx->a[f + (size_t)k].Num();
                if (v >= nv) return;
                made.push_back(verts[v]);
                if (skinned) madeSkin.push_back(model_->vertexSkin[bind[v]]);
            }
        if (made.empty()) return;
        ImportedPart part;
        part.first = out_.size();
        part.count = made.size();
        part.albedo = (int)model_->images.size();
        part.hair = true;
        model_->images.push_back(std::move(data));
        out_.insert(out_.end(), made.begin(), made.end());
        if (skinned) model_->vertexSkin.insert(model_->vertexSkin.end(), madeSkin.begin(), madeSkin.end());
        model_->parts.push_back(part);
    }

    // Fur shells on the hairline (docs/hair_maker_spec.md section 7): hairline_shell.json from cap.py is the scalp along the hairline
    // with a fur density and flow coordinates; it is stacked kShells times from 0.3 mm to kThick off the skin. HairCardPS draws fine
    // single hairs through the stack (leaning along the flow), so close up the hairline frays into real hairs. Close range only: the
    // part draws within kFar metres (the shader fades the fur out before that), so it costs nothing at gameplay distance.
    void AddHairShell() {
        if (path_.find("body_head") == std::string::npos) return;
        const std::wstring dir = ScalpDir();
        std::vector<uint8_t> jsonBytes, data, material;
        Json doc;
        if (dir.empty() || !ReadWhole(dir + L"hairline_shell.json", jsonBytes) || !JsonParser((const char*)jsonBytes.data(), jsonBytes.size()).Parse(doc) ||
            !HairAtlas(L"hair", data, material))
            return;
        const Json *pos = doc.Find("pos"), *nrm = doc.Find("nrm"), *idx = doc.Find("idx"), *den = doc.Find("density"), *uv = doc.Find("uv");
        if (!pos || !nrm || !idx || !den || !uv || pos->a.size() % 3 || nrm->a.size() != pos->a.size() || den->a.size() * 3 != pos->a.size() ||
            uv->a.size() * 3 != pos->a.size() * 2 || idx->a.size() % 3)
            return;
        const SkinGrid& skin = HeadSkin();
        if (skin.empty()) return;
        constexpr int kShells = 12; // over a ribbon of polys on the hairline only (cap.py hairline_ribbon): ~5k triangles in all
        constexpr float kThick = 0.0035f, kFar = 1.6f;
        const size_t nv = den->a.size();
        const HairTint tint = HairLook("scalp");
        const bool skinned = model_->vertexSkin.size() == out_.size();
        std::vector<size_t> bind(nv, 0);
        for (size_t i = 0; i < nv; ++i) {
            const size_t k = skin.Nearest((float)pos->a[i * 3].Num(), (float)pos->a[i * 3 + 1].Num(), (float)pos->a[i * 3 + 2].Num());
            bind[i] = k == SIZE_MAX ? 0 : k;
        }
        std::vector<Vertex> made;
        std::vector<VertexSkin> madeSkin;
        made.reserve(idx->a.size() * kShells);
        for (int sh = 0; sh < kShells; ++sh) {
            const float t = ((float)sh + 0.5f) / (float)kShells, lift = 0.0003f + t * kThick;
            for (size_t f = 0; f < idx->a.size(); ++f) {
                const size_t i = (size_t)idx->a[f].Num();
                if (i >= nv) return;
                Vertex x{};
                x.nx = (float)nrm->a[i * 3].Num(); x.ny = (float)nrm->a[i * 3 + 1].Num(); x.nz = (float)nrm->a[i * 3 + 2].Num();
                x.px = (float)pos->a[i * 3].Num() + x.nx * lift; x.py = (float)pos->a[i * 3 + 1].Num() + x.ny * lift; x.pz = (float)pos->a[i * 3 + 2].Num() + x.nz * lift;
                x.r = HairPackR(t, 0.5f); x.g = HairPackG(kHairShell, (float)den->a[i].Num()); x.b = HairPackB(&tint); x.a = kHairAlpha;
                x.u = (float)uv->a[i * 2].Num(); x.v = (float)uv->a[i * 2 + 1].Num();
                made.push_back(x);
                if (skinned) madeSkin.push_back(model_->vertexSkin[bind[i]]);
            }
        }
        if (made.empty()) return;
        ImportedPart part;
        part.first = out_.size();
        part.count = made.size();
        part.albedo = (int)model_->images.size();
        part.hair = true;
        part.lodMin = 0.0f;
        part.lodMax = kFar;
        model_->images.push_back(std::move(data));
        out_.insert(out_.end(), made.begin(), made.end());
        if (skinned) model_->vertexSkin.insert(model_->vertexSkin.end(), madeSkin.begin(), madeSkin.end());
        model_->parts.push_back(part);
    }

    // One scalp_cards file into two-sided hair vertices. False when the file is malformed or does not belong to this head.
    bool ScalpLevel(const Json& doc, const SkinGrid& skin, const HairTint& tint, bool skinned, std::vector<Vertex>& made, std::vector<VertexSkin>& madeSkin) {
        const Json *pos = doc.Find("pos"), *uv = doc.Find("uv"), *tt = doc.Find("t"), *idx = doc.Find("idx");
        if (!pos || !uv || !tt || !idx || pos->a.size() % 3 || pos->a.size() / 3 != tt->a.size() || uv->a.size() != tt->a.size() * 2 || idx->a.size() % 3) return false;
        const size_t nv = tt->a.size();
        const Json *cardJ = doc.Find("card"), *randJ = doc.Find("rand");
        const bool perVertex = cardJ && randJ && cardJ->a.size() == nv && randJ->a.size() == nv;
        size_t inside = 0;
        for (size_t i = 0; i < nv; ++i) {
            const float x = (float)pos->a[i * 3].Num(), y = (float)pos->a[i * 3 + 1].Num(), z = (float)pos->a[i * 3 + 2].Num();
            inside += x > skin.lo[0] - 0.02f && x < skin.lo[0] + skin.dim[0] * skin.cell + 0.02f && y > skin.lo[1] - 0.02f &&
                      y < skin.lo[1] + skin.dim[1] * skin.cell + 0.02f && z > skin.lo[2] - 0.02f && z < skin.lo[2] + skin.dim[2] * skin.cell + 0.02f;
        }
        if (inside * 2 < nv) return false; // the cards do not belong to this mesh
        // v1 files: a card is a connected set of triangles (union-find over the index list); its random comes from its first vertex.
        std::vector<float> rand(nv, 0.0f);
        if (perVertex) {
            for (size_t i = 0; i < nv; ++i) rand[i] = (float)randJ->a[i].Num();
        } else {
            std::vector<size_t> parent(nv);
            for (size_t k = 0; k < nv; ++k) parent[k] = k;
            const auto find = [&](size_t k) { while (parent[k] != k) k = parent[k] = parent[parent[k]]; return k; };
            for (size_t f = 0; f + 2 < idx->a.size(); f += 3) {
                const size_t a = (size_t)idx->a[f].Num(), b = (size_t)idx->a[f + 1].Num(), c = (size_t)idx->a[f + 2].Num();
                if (a < nv && b < nv && c < nv) { parent[find(b)] = find(a); parent[find(c)] = find(a); }
            }
            for (size_t i = 0; i < nv; ++i) { uint32_t h = (uint32_t)find(i) * 2654435761u; h ^= h >> 15; rand[i] = (float)(h & 0xFFFFu) / 65536.0f; }
        }
        std::vector<size_t> bind(nv, 0);
        std::vector<Vertex> verts(nv);
        for (size_t i = 0; i < nv; ++i) {
            Vertex& x = verts[i];
            x.px = (float)pos->a[i * 3].Num(); x.py = (float)pos->a[i * 3 + 1].Num(); x.pz = (float)pos->a[i * 3 + 2].Num();
            x.r = HairPackR((float)tt->a[i].Num(), rand[i]); x.g = HairPackG(kHairScalp, 0.0f); x.b = HairPackB(&tint); x.a = kHairAlpha;
            x.u = (float)uv->a[i * 2].Num(); x.v = 1.0f - (float)uv->a[i * 2 + 1].Num(); // the atlas's v runs up the image; the engine's runs down
            const size_t k = skin.Nearest(x.px, x.py, x.pz);
            bind[i] = k == SIZE_MAX ? 0 : k;
        }
        made.reserve(idx->a.size() * 2);
        for (size_t f = 0; f + 2 < idx->a.size(); f += 3) {
            size_t t3[3];
            for (int k = 0; k < 3; ++k) { t3[k] = (size_t)idx->a[f + (size_t)k].Num(); if (t3[k] >= nv) return false; }
            const Vertex &A = verts[t3[0]], &B = verts[t3[1]], &C = verts[t3[2]];
            const float e1[3] = {B.px - A.px, B.py - A.py, B.pz - A.pz}, e2[3] = {C.px - A.px, C.py - A.py, C.pz - A.pz};
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (l < 1e-12f) continue;
            for (float& c : n) c /= l;
            // the skin normal under the triangle tells outward from inward
            const Vertex& under = out_[bind[t3[0]]];
            const float sn[3] = {under.nx, under.ny, under.nz};
            if (n[0] * sn[0] + n[1] * sn[1] + n[2] * sn[2] < 0.0f) for (float& c : n) c = -c;
            for (int side = 0; side < 2; ++side)
                for (int k = 0; k < 3; ++k) {
                    const size_t src = t3[side ? 2 - k : k];
                    Vertex x = verts[src];
                    const Vertex& s = out_[bind[src]];
                    float m[3] = {s.nx * 0.7f + n[0] * 0.3f, s.ny * 0.7f + n[1] * 0.3f, s.nz * 0.7f + n[2] * 0.3f};
                    const float ml = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                    if (ml > 1e-6f) { x.nx = m[0] / ml; x.ny = m[1] / ml; x.nz = m[2] / ml; } else { x.nx = n[0]; x.ny = n[1]; x.nz = n[2]; }
                    made.push_back(x);
                    if (skinned) madeSkin.push_back(model_->vertexSkin[bind[src]]);
                }
        }
        return !made.empty();
    }

    // Wyatt's eyes take the generated map (gameenginetools\eye_atlas): each ball gets a planar front UV, r = 1 at its silhouette, and
    // alpha 0.975 sends it to EyePS in mesh.hlsl. Without the map the eyes keep their own texture and LitEye.
    void ApplyEyeMaps() {
        if (path_.find("body_head") == std::string::npos) return;
        std::vector<uint8_t> png;
        if (!ReadWhole(ExeDirectory() + L"\\..\\Assets\\eyes\\eye_colour.png", png)) return;
        float midX = 0;
        size_t total = 0;
        for (const ImportedPart& part : model_->parts)
            if (part.eye)
                for (size_t k = part.first; k < part.first + part.count; ++k) { midX += out_[k].px; ++total; }
        if (!total) return;
        midX /= (float)total;
        for (const bool side : {false, true}) {
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            for (const ImportedPart& part : model_->parts)
                if (part.eye)
                    for (size_t k = part.first; k < part.first + part.count; ++k)
                        if ((out_[k].px >= midX) == side) {
                            lo[0] = std::min(lo[0], out_[k].px); hi[0] = std::max(hi[0], out_[k].px);
                            lo[1] = std::min(lo[1], out_[k].py); hi[1] = std::max(hi[1], out_[k].py);
                        }
            const float radius = std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]) * 0.5f, 1e-6f);
            const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f;
            for (const ImportedPart& part : model_->parts)
                if (part.eye)
                    for (size_t k = part.first; k < part.first + part.count; ++k)
                        if ((out_[k].px >= midX) == side) {
                            out_[k].u = 0.5f + (out_[k].px - cx) / (2 * radius);
                            out_[k].v = 0.5f - (out_[k].py - cy) / (2 * radius);
                            out_[k].a = 0.975f;
                        }
        }
        const int image = (int)model_->images.size();
        model_->images.push_back(std::move(png));
        for (ImportedPart& part : model_->parts)
            if (part.eye) part.albedo = image;
    }

    // The hair maker bakes a hand-painted beard (Assets\hair\wyatt\beard_texture.png, placed by beard_texture.json in the skin
    // texture's UV rectangle) into the skin under the beard cards; without it the cards leave bare patches. Same bake here:
    // alpha-over into the decoded albedo, nearest resample. Returns true when the pixels changed.
    bool CompositeBeard(std::vector<uint8_t>& px, int w, int h) {
        const std::wstring dir = ExeDirectory() + L"\\..\\Assets\\hair\\wyatt\\";
        std::vector<uint8_t> png, json;
        if (!ReadWhole(dir + L"beard_texture.png", png) || !ReadWhole(dir + L"beard_texture.json", json)) return false;
        Json rect;
        if (!JsonParser((const char*)json.data(), json.size()).Parse(rect)) return false;
        std::vector<uint8_t> bp;
        int bw = 0, bh = 0;
        if (!DecodeImage(png.data(), png.size(), bp, bw, bh) || bw < 2 || bh < 2) return false;
        const auto get = [&](const char* k) { const Json* v = rect.Find(k); return v ? (float)v->Num() : 0.0f; };
        const float u0 = get("u0"), v0 = get("v0"), u1 = get("u1"), v1 = get("v1");
        // The beard was painted on the hair maker's skin atlas, whose rows sit about 70 px (of 2048) lower than this file's: shiftY.
        const int X = (int)std::floor(u0 * (float)w), Y0 = (int)std::floor((1.0f - v1) * (float)h);
        const int Wd = (int)std::ceil(u1 * (float)w) - X, Hd = (int)std::ceil((1.0f - v0) * (float)h) - Y0;
        const int Y = Y0 + (int)std::lround(get("shiftY") * (float)h);
        if (Wd < 2 || Hd < 2) return false;
        for (int y = std::max(Y, 0); y < std::min(Y + Hd, h); ++y)
            for (int x = std::max(X, 0); x < std::min(X + Wd, w); ++x) {
                const int sx = std::min(bw - 1, (x - X) * bw / Wd), sy = std::min(bh - 1, (y - Y) * bh / Hd);
                const uint8_t* b = &bp[((size_t)sy * (size_t)bw + (size_t)sx) * 4];
                uint8_t* d = &px[((size_t)y * (size_t)w + (size_t)x) * 4];
                const float a = b[3] / 255.0f;
                for (int c = 0; c < 3; ++c) d[c] = (uint8_t)std::lround(d[c] * (1.0f - a) + b[c] * a);
            }
        return true;
    }

    // Uncompressed 24-bit BMP of top-down RGBA (alpha dropped: skin is opaque), which WIC reads back like any other image.
    static void WriteBmp(const std::vector<uint8_t>& px, int w, int h, std::vector<uint8_t>& out) {
        const size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3, size = 54 + stride * (size_t)h;
        out.assign(size, 0);
        const auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&out[at], &v, 4); };
        out[0] = 'B'; out[1] = 'M';
        put32(2, (uint32_t)size); put32(10, 54); put32(14, 40); put32(18, (uint32_t)w); put32(22, (uint32_t)h);
        out[26] = 1; out[28] = 24; put32(34, (uint32_t)(stride * (size_t)h));
        for (int y = 0; y < h; ++y) {
            uint8_t* row = &out[54 + stride * (size_t)(h - 1 - y)]; // bottom-up
            for (int x = 0; x < w; ++x) {
                const uint8_t* s = &px[((size_t)y * (size_t)w + (size_t)x) * 4];
                row[x * 3] = s[2]; row[x * 3 + 1] = s[1]; row[x * 3 + 2] = s[0];
            }
        }
    }

    // The head's own albedo has light, hard-edged decal patches on the face (cheek, brow) that the beard cannot hide. Tone-match:
    // find skin blocks clearly brighter than the face's typical skin, pull them toward it with a smooth gain field, so the
    // patch edges fade instead of cutting. Only darkens; hair and shadow blocks are left alone. Face island rect is this file's.
    static void ToneMatchFace(std::vector<uint8_t>& px, int w, int h) {
        const int B = 16, x0 = (int)(0.62f * (float)w), x1 = (int)(0.99f * (float)w), y0 = (int)(0.14f * (float)h), y1 = (int)(0.83f * (float)h);
        const int bw = (x1 - x0) / B, bh = (y1 - y0) / B;
        if (bw < 4 || bh < 4) return;
        const size_t N = (size_t)bw * (size_t)bh;
        std::vector<float> C(N * 3), L(N), skin;
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                float s[3] = {};
                for (int y = 0; y < B; y += 2) for (int x = 0; x < B; x += 2) {
                    const uint8_t* p = &px[((size_t)(y0 + by * B + y) * (size_t)w + (size_t)(x0 + bx * B + x)) * 4];
                    for (int c = 0; c < 3; ++c) s[c] += p[c];
                }
                const size_t k = (size_t)by * (size_t)bw + (size_t)bx;
                for (int c = 0; c < 3; ++c) C[k * 3 + (size_t)c] = s[c] / 64.0f;
                L[k] = 0.3f * C[k * 3] + 0.59f * C[k * 3 + 1] + 0.11f * C[k * 3 + 2];
                if (L[k] > 120.0f) skin.push_back(L[k]);
            }
        if (skin.size() < 32) return;
        std::nth_element(skin.begin(), skin.begin() + skin.size() * 2 / 5, skin.end());
        const float ref = skin[skin.size() * 2 / 5];
        float refC[3] = {}; int nref = 0; // typical skin: the mean colour of blocks near the reference tone
        for (size_t k = 0; k < N; ++k)
            if (std::fabs(L[k] - ref) < 12.0f) { for (int c = 0; c < 3; ++c) refC[c] += C[k * 3 + (size_t)c]; ++nref; }
        if (!nref) return;
        for (float& v : refC) v /= (float)nref;
        std::vector<float> g(N * 3, 1.0f), t(g);
        for (size_t k = 0; k < N; ++k) {
            if (L[k] <= ref + 8.0f) continue;
            const float u = std::clamp((L[k] - ref - 8.0f) / 22.0f, 0.0f, 1.0f), wt = u * u * (3.0f - 2.0f * u);
            for (int c = 0; c < 3; ++c) g[k * 3 + (size_t)c] = std::clamp(1.0f + wt * (refC[c] / std::max(C[k * 3 + (size_t)c], 1.0f) - 1.0f), 0.6f, 1.0f);
        }
        for (int pass = 0; pass < 3; ++pass) { // soften the field: box blur, radius 2 blocks
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx)
                    for (int c = 0; c < 3; ++c) {
                        float s = 0; int n = 0;
                        for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
                            const int X = bx + dx, Y = by + dy;
                            if (X >= 0 && X < bw && Y >= 0 && Y < bh) { s += g[((size_t)Y * (size_t)bw + (size_t)X) * 3 + (size_t)c]; ++n; }
                        }
                        t[((size_t)by * (size_t)bw + (size_t)bx) * 3 + (size_t)c] = s / (float)n;
                    }
            g.swap(t);
        }
        for (int y = y0; y < y0 + bh * B; ++y)
            for (int x = x0; x < x0 + bw * B; ++x) {
                const float fx = std::clamp(((float)(x - x0) + 0.5f) / (float)B - 0.5f, 0.0f, (float)(bw - 1)), fy = std::clamp(((float)(y - y0) + 0.5f) / (float)B - 0.5f, 0.0f, (float)(bh - 1));
                const int ix = std::min((int)fx, bw - 2), iy = std::min((int)fy, bh - 2);
                const float ax = fx - (float)ix, ay = fy - (float)iy;
                uint8_t* p = &px[((size_t)y * (size_t)w + (size_t)x) * 4];
                for (int c = 0; c < 3; ++c) {
                    const auto G = [&](int X, int Y) { return g[((size_t)Y * (size_t)bw + (size_t)X) * 3 + (size_t)c]; };
                    const float gain = (G(ix, iy) * (1 - ax) + G(ix + 1, iy) * ax) * (1 - ay) + (G(ix, iy + 1) * (1 - ax) + G(ix + 1, iy + 1) * ax) * ay;
                    p[c] = (uint8_t)std::lround(p[c] * gain);
                }
            }
    }

    // The beard bake on its own (no geometry): the skin albedo with the hair maker's painted beard composited in.
    // A soft brow-coloured shadow under the brow cards (gameenginetools/brow_cards/brow_base.py -> Assets/hair/wyatt/brow_base.png,
    // RGB tint, A strength, over the head's UV atlas at any size), multiplied into the skin albedo so the brow still reads when the thin
    // cards drop out with distance. Missing file: nothing.
    void ApplyBrowBase() {
        if (path_.find("body_head") == std::string::npos) return;
        std::vector<uint8_t> png, mask;
        int mw = 0, mh = 0;
        if (!ReadWhole(ExeDirectory() + L"\\..\\Assets\\hair\\wyatt\\brow_base.png", png) || !DecodeImage(png.data(), png.size(), mask, mw, mh) || mw < 2 || mh < 2) return;
        for (const ImportedPart& part : model_->parts) {
            if (!part.skin || part.albedo < 0 || part.albedo >= (int)model_->images.size() || model_->images[(size_t)part.albedo].empty()) continue;
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            if (!DecodeImage(model_->images[(size_t)part.albedo].data(), model_->images[(size_t)part.albedo].size(), px, w, h) || w < 16 || h < 16) continue;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const uint8_t* m = &mask[((size_t)(y * mh / h) * mw + (size_t)(x * mw / w)) * 4];
                    if (m[3] == 0) continue;
                    const float a = m[3] / 255.0f;
                    uint8_t* p = &px[((size_t)y * w + x) * 4];
                    for (int c = 0; c < 3; ++c) p[c] = (uint8_t)(p[c] * (1.0f - a + a * m[c] / 255.0f) + 0.5f);
                }
            WriteBmp(px, w, h, model_->images[(size_t)part.albedo]);
            return;
        }
    }

    // Exported MakeHuman bodies (and the clothing made on them) bind every wrist vertex wholly to lowerarm_* or hand_*: a hinge,
    // so a bent wrist folds along one ring. This spreads the change over ~9 cm (6 cm of forearm, 3 cm of hand) by smoothing the
    // weights over the mesh graph, the band's ends held so nothing steps where it stops. Same as
    // gameenginetools/body_weights/soften_wrists.py, done at load so a fresh re-export cannot bring the hinge back; files that
    // tool already rewrote (asset.extras.tbd_wrist_soften) are left alone. Files whose arm joints are not really bound (jeans,
    // boots: identity bind matrices) fail the arm-length check and are skipped. TBD_WRIST_SOFTEN=0 turns it off.
    void SoftenWrists() {
        if (const char* e = std::getenv("TBD_WRIST_SOFTEN")) if (e[0] == '0') return;
        if (model_->joints.empty() || model_->vertexSkin.size() != out_.size()) return;
        if (const Json* a = doc_.Find("asset"))
            if (const Json* x = a->Find("extras"))
                if (const Json* m = x->Find("tbd_wrist_soften"); m && m->Num() != 0) return;
        const size_t JN = model_->joints.size();
        auto jointPos = [&](size_t j, float p[3]) { // the joint's bind position: inverse of the inverse bind matrix (column major)
            const float* m = model_->joints[j].inverseBind;
            const double a00 = m[0], a10 = m[1], a20 = m[2], a01 = m[4], a11 = m[5], a21 = m[6], a02 = m[8], a12 = m[9], a22 = m[10];
            const double c00 = a11 * a22 - a12 * a21, c01 = a02 * a21 - a01 * a22, c02 = a01 * a12 - a02 * a11;
            const double det = a00 * c00 + a10 * c01 + a20 * c02;
            if (std::fabs(det) < 1e-12) return false;
            const double i00 = c00 / det, i01 = c01 / det, i02 = c02 / det;
            const double i10 = (a12 * a20 - a10 * a22) / det, i11 = (a00 * a22 - a02 * a20) / det, i12 = (a02 * a10 - a00 * a12) / det;
            const double i20 = (a10 * a21 - a11 * a20) / det, i21 = (a01 * a20 - a00 * a21) / det, i22 = (a00 * a11 - a01 * a10) / det;
            const double tx = m[12], ty = m[13], tz = m[14];
            p[0] = (float)-(i00 * tx + i01 * ty + i02 * tz);
            p[1] = (float)-(i10 * tx + i11 * ty + i12 * tz);
            p[2] = (float)-(i20 * tx + i21 * ty + i22 * tz);
            return true;
        };
        auto find = [&](const char* name) -> int {
            const size_t len = std::strlen(name);
            for (size_t j = 0; j < JN; ++j) {
                const std::string& nm = model_->joints[j].name;
                if (nm.size() != len) continue;
                bool same = true;
                for (size_t c = 0; c < len && same; ++c) same = (char)std::tolower((unsigned char)nm[c]) == name[c];
                if (same) return (int)j;
            }
            return -1;
        };
        struct Arm { float a[3], dir[3], len; };
        Arm arms[2];
        const char* armNames[2][2] = {{"upperarm_l", "hand_l"}, {"upperarm_r", "hand_r"}};
        for (int s = 0; s < 2; ++s) {
            const int ju = find(armNames[s][0]), jh = find(armNames[s][1]);
            float pu[3], ph[3];
            if (ju < 0 || jh < 0 || !jointPos((size_t)ju, pu) || !jointPos((size_t)jh, ph)) return;
            const float d[3] = {ph[0] - pu[0], ph[1] - pu[1], ph[2] - pu[2]};
            const float L = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (!(L > 0.3f && L < 0.9f)) return; // not a real arm in metres
            for (int c = 0; c < 3; ++c) { arms[s].a[c] = pu[c]; arms[s].dir[c] = d[c] / L; }
            arms[s].len = L;
        }
        constexpr float kLo = -0.06f, kHi = 0.03f; // metres from the hand joint along the arm
        // Weld by position so seams move together; only vertices in the band take part.
        const size_t n = out_.size();
        std::vector<int> weld(n, -1);
        std::vector<float> dist; // per welded vertex
        std::vector<size_t> firstOf;
        std::map<std::tuple<long, long, long>, int> byKey;
        for (size_t i = 0; i < n; ++i) {
            const Vertex& v = out_[i];
            float d = 1e9f;
            for (const Arm& arm : arms) {
                const float r[3] = {v.px - arm.a[0], v.py - arm.a[1], v.pz - arm.a[2]};
                const float t = r[0] * arm.dir[0] + r[1] * arm.dir[1] + r[2] * arm.dir[2];
                const float q[3] = {r[0] - t * arm.dir[0], r[1] - t * arm.dir[1], r[2] - t * arm.dir[2]};
                const float radial = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                if ((v.px > 0) == (arm.dir[0] > 0) && radial < 0.09f) d = t - arm.len; // that arm, not the legs or the body
            }
            if (d < kLo || d > kHi) continue;
            const auto key = std::make_tuple(std::lround(v.px * 20000.0f), std::lround(v.py * 20000.0f), std::lround(v.pz * 20000.0f));
            auto it = byKey.find(key);
            if (it == byKey.end()) {
                it = byKey.emplace(key, (int)firstOf.size()).first;
                firstOf.push_back(i);
                dist.push_back(d);
            }
            weld[i] = it->second;
        }
        const size_t nu = firstOf.size();
        if (nu == 0) return;
        // The joints the band uses; dense weights per welded vertex over just those.
        std::vector<int> local(JN, -1);
        std::vector<uint16_t> used;
        for (size_t u = 0; u < nu; ++u) {
            const VertexSkin& vs = model_->vertexSkin[firstOf[u]];
            for (int c = 0; c < 4; ++c)
                if (vs.weights[c] > 0 && vs.joints[c] < JN && local[vs.joints[c]] < 0) {
                    local[vs.joints[c]] = (int)used.size();
                    used.push_back(vs.joints[c]);
                }
        }
        const size_t K = used.size();
        if (K < 2) return;
        std::vector<float> w(nu * K, 0.0f), acc(nu * K);
        for (size_t u = 0; u < nu; ++u) {
            const VertexSkin& vs = model_->vertexSkin[firstOf[u]];
            for (int c = 0; c < 4; ++c)
                if (vs.weights[c] > 0 && vs.joints[c] < JN) w[u * K + (size_t)local[vs.joints[c]]] += vs.weights[c];
        }
        // Graph edges from the triangles, both ends in the band.
        std::vector<std::pair<int, int>> edges;
        for (size_t t = 0; t + 2 < n; t += 3)
            for (size_t e = 0; e < 3; ++e) {
                const int a = weld[t + e], b = weld[t + (e + 1) % 3];
                if (a >= 0 && b >= 0 && a != b) edges.emplace_back(std::min(a, b), std::max(a, b));
            }
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        if (edges.empty()) return;
        std::vector<float> deg(nu, 0.0f), keep(nu);
        for (const auto& e : edges) { deg[(size_t)e.first] += 1; deg[(size_t)e.second] += 1; }
        for (size_t u = 0; u < nu; ++u) { // held at the band's ends, free in the middle
            const float x = std::clamp((std::fabs(dist[u] + 0.015f) - 0.018f) / 0.012f, 0.0f, 1.0f);
            keep[u] = 0.5f + 0.5f * x * x * (3 - 2 * x);
        }
        for (int it = 0; it < 14; ++it) {
            std::fill(acc.begin(), acc.end(), 0.0f);
            for (const auto& e : edges)
                for (size_t k = 0; k < K; ++k) {
                    acc[(size_t)e.first * K + k] += w[(size_t)e.second * K + k];
                    acc[(size_t)e.second * K + k] += w[(size_t)e.first * K + k];
                }
            for (size_t u = 0; u < nu; ++u) {
                if (deg[u] == 0) continue;
                for (size_t k = 0; k < K; ++k) w[u * K + k] = w[u * K + k] * keep[u] + acc[u * K + k] / deg[u] * (1 - keep[u]);
            }
        }
        // Back to four joints per vertex, written to every flat vertex of the weld.
        std::vector<VertexSkin> result(nu);
        std::vector<std::pair<float, uint16_t>> order;
        for (size_t u = 0; u < nu; ++u) {
            order.clear();
            for (size_t k = 0; k < K; ++k)
                if (w[u * K + k] >= 1e-4f) order.emplace_back(w[u * K + k], used[k]);
            std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            float sum = 0;
            for (size_t c = 0; c < 4 && c < order.size(); ++c) sum += order[c].first;
            if (sum <= 0) { result[u] = model_->vertexSkin[firstOf[u]]; continue; }
            for (size_t c = 0; c < 4 && c < order.size(); ++c) {
                result[u].joints[c] = order[c].second;
                result[u].weights[c] = order[c].first / sum;
            }
        }
        size_t changed = 0;
        for (size_t i = 0; i < n; ++i)
            if (weld[i] >= 0) { model_->vertexSkin[i] = result[(size_t)weld[i]]; ++changed; }
        Log("Skinning: softened the wrist weights of %zu vertices (hard hinge in the file)", changed);
    }

    void ApplyBeardBake() {
        if (path_.find("body_head") == std::string::npos) return;
        for (const ImportedPart& part : model_->parts) {
            if (!part.skin || part.albedo < 0 || part.albedo >= (int)model_->images.size() || model_->images[(size_t)part.albedo].empty()) continue;
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            if (!DecodeImage(model_->images[(size_t)part.albedo].data(), model_->images[(size_t)part.albedo].size(), px, w, h) || w < 16 || h < 16) continue;
            if (w >= 1024 && h >= 1024) ToneMatchFace(px, w, h);
            CompositeBeard(px, w, h);
            WriteBmp(px, w, h, model_->images[(size_t)part.albedo]);
            return;
        }
    }

    int skinUsed_ = -1;          // the one skin the skinned nodes share
    bool skinUsable_ = true;     // false: several skins, or a skinned node with a transform that was baked in
    bool anySkinnedVertex_ = false;
    std::string path_;
    std::vector<Vertex>& out_;
    bool genNormals_;
    std::string& error_;
    ImportedModel* model_;
    Json doc_;
    std::vector<uint8_t> glbBin_;
    std::vector<std::vector<uint8_t>> buffers_;

    bool Fail(const std::string& why) { error_ = why; return false; }

    // Encoded bytes of glTF image `index` into model_->images (loaded once); false when unreadable.
    bool LoadImage(int index) {
        const Json* imgs = doc_.Find("images");
        const Json* img = imgs ? imgs->At((size_t)index) : nullptr;
        if (!img || !model_) return false;
        if (model_->images.size() < imgs->a.size()) model_->images.resize(imgs->a.size());
        std::vector<uint8_t>& dst = model_->images[(size_t)index];
        if (!dst.empty()) return true;
        if (const Json* bv = img->Find("bufferView")) {
            const Json* views = doc_.Find("bufferViews");
            const Json* view = views ? views->At((size_t)bv->Int()) : nullptr;
            if (!view) return false;
            const int buf = view->Find("buffer") ? view->Find("buffer")->Int() : 0;
            if (buf < 0 || buf >= (int)buffers_.size()) return false;
            const size_t off = view->Find("byteOffset") ? (size_t)view->Find("byteOffset")->Num() : 0;
            const size_t len = view->Find("byteLength") ? (size_t)view->Find("byteLength")->Num() : 0;
            if (off + len > buffers_[(size_t)buf].size()) return false;
            dst.assign(buffers_[(size_t)buf].begin() + off, buffers_[(size_t)buf].begin() + off + len);
        } else if (const Json* uri = img->Find("uri")) {
            if (uri->s.rfind("data:", 0) == 0) {
                const size_t comma = uri->s.find(',');
                if (comma == std::string::npos || !DecodeBase64(uri->s.substr(comma + 1), dst)) return false;
            } else {
                std::string rel = uri->s;
                for (size_t at; (at = rel.find("%20")) != std::string::npos;) rel.replace(at, 3, " ");
                if (!ReadFile(DirOf(path_) + rel, dst)) return false;
            }
        }
        return !dst.empty();
    }

    // Image index behind a texture slot of a material (`texInfo` = {"index": textureIndex}), or -1.
    int ImageOf(const Json* texInfo) {
        const Json* ti = texInfo ? texInfo->Find("index") : nullptr;
        const Json* texs = doc_.Find("textures");
        const Json* tex = ti && texs ? texs->At((size_t)ti->Int()) : nullptr;
        const Json* src = tex ? tex->Find("source") : nullptr;
        return src && LoadImage(src->Int()) ? src->Int() : -1;
    }

    bool LoadBuffers() {
        const Json* bufs = doc_.Find("buffers");
        if (!bufs) return true;
        for (const Json& b : bufs->a) {
            std::vector<uint8_t> data;
            const Json* uri = b.Find("uri");
            if (!uri) data = glbBin_;
            else if (uri->s.rfind("data:", 0) == 0) {
                const size_t comma = uri->s.find(',');
                if (comma == std::string::npos || !DecodeBase64(uri->s.substr(comma + 1), data)) return Fail("bad data URI buffer");
            } else {
                std::string rel = uri->s; // percent-escapes: only %20 is common in practice
                for (size_t at; (at = rel.find("%20")) != std::string::npos;) rel.replace(at, 3, " ");
                if (!ReadFile(DirOf(path_) + rel, data)) return Fail("cannot open buffer " + rel);
            }
            buffers_.push_back(std::move(data));
        }
        return true;
    }

    // Reads accessor `index` as floats (`comps` per element), applying normalization; returns false if absent/invalid.
    bool ReadFloats(int index, int comps, std::vector<float>& out) {
        const Json* accs = doc_.Find("accessors");
        const Json* views = doc_.Find("bufferViews");
        const Json* acc = accs ? accs->At((size_t)index) : nullptr;
        if (!acc || !views) return false;
        const int count = acc->Find("count") ? acc->Find("count")->Int() : 0;
        const int ctype = acc->Find("componentType") ? acc->Find("componentType")->Int() : 0;
        const bool normalized = acc->Find("normalized") && acc->Find("normalized")->b;
        const std::string type = acc->Find("type") ? acc->Find("type")->s : "";
        const int have = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : 0;
        if (have < comps || count <= 0) return false;
        const int csize = (ctype == 5120 || ctype == 5121) ? 1 : (ctype == 5122 || ctype == 5123) ? 2 : 4;

        out.assign((size_t)count * comps, 0.0f);
        const Json* viewRef = acc->Find("bufferView");
        if (!viewRef) return true; // all zeros (sparse not supported)
        const Json* view = views->At((size_t)viewRef->Int());
        if (!view) return false;
        const int buf = view->Find("buffer") ? view->Find("buffer")->Int() : 0;
        if (buf < 0 || buf >= (int)buffers_.size()) return false;
        const size_t base = (view->Find("byteOffset") ? (size_t)view->Find("byteOffset")->Num() : 0) +
                            (acc->Find("byteOffset") ? (size_t)acc->Find("byteOffset")->Num() : 0);
        size_t stride = view->Find("byteStride") ? (size_t)view->Find("byteStride")->Num() : 0;
        if (stride == 0) stride = (size_t)csize * have;
        const std::vector<uint8_t>& data = buffers_[(size_t)buf];
        if (base + (size_t)(count - 1) * stride + (size_t)csize * have > data.size()) return false;

        for (int i = 0; i < count; ++i)
            for (int c = 0; c < comps; ++c) {
                const uint8_t* at = &data[base + (size_t)i * stride + (size_t)c * csize];
                float v = 0;
                switch (ctype) {
                case 5120: v = (float)(int8_t)*at; if (normalized) v = std::max(v / 127.0f, -1.0f); break;
                case 5121: v = (float)*at; if (normalized) v /= 255.0f; break;
                case 5122: { int16_t x; std::memcpy(&x, at, 2); v = (float)x; if (normalized) v = std::max(v / 32767.0f, -1.0f); break; }
                case 5123: { uint16_t x; std::memcpy(&x, at, 2); v = (float)x; if (normalized) v /= 65535.0f; break; }
                case 5125: { uint32_t x; std::memcpy(&x, at, 4); v = (float)x; break; }
                case 5126: std::memcpy(&v, at, 4); break;
                default: return false;
                }
                out[(size_t)i * comps + c] = v;
            }
        return true;
    }

    bool ReadIndices(int index, std::vector<uint32_t>& out) {
        std::vector<float> f;
        if (!ReadFloats(index, 1, f)) return false;
        out.resize(f.size());
        for (size_t i = 0; i < f.size(); ++i) out[i] = (uint32_t)f[i]; // exact below 2^24 (16M vertices)
        return true;
    }

    static void NodeTRS(const Json& n, float t[3], float q[4], float s[3]) {
        if (const Json* j = n.Find("translation")) for (int i = 0; i < 3 && i < (int)j->a.size(); ++i) t[i] = (float)j->a[(size_t)i].Num();
        if (const Json* j = n.Find("rotation")) for (int i = 0; i < 4 && i < (int)j->a.size(); ++i) q[i] = (float)j->a[(size_t)i].Num();
        if (const Json* j = n.Find("scale")) for (int i = 0; i < 3 && i < (int)j->a.size(); ++i) s[i] = (float)j->a[(size_t)i].Num();
    }
    static M4 NodeLocal(const Json& n) {
        M4 local = Identity();
        if (const Json* m = n.Find("matrix")) {
            for (int i = 0; i < 16 && i < (int)m->a.size(); ++i) local.m[i] = (float)m->a[(size_t)i].Num();
        } else {
            float t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
            NodeTRS(n, t, q, s);
            local = FromTRS(t, q, s);
        }
        return local;
    }

    bool Visit(int node, const M4& parent, int depth) {
        const Json* nodes = doc_.Find("nodes");
        const Json* n = nodes ? nodes->At((size_t)node) : nullptr;
        if (!n || depth > 64) return Fail("bad node reference or hierarchy too deep");
        const M4 world = Mul(parent, NodeLocal(*n));
        if (const Json* mesh = n->Find("mesh")) {
            int skin = -1;
            if (const Json* sk = n->Find("skin")) {
                skin = sk->Int();
                if (skinUsed_ >= 0 && skinUsed_ != skin) skinUsable_ = false;
                skinUsed_ = skin;
                const M4 identity = Identity();
                for (int i = 0; i < 16; ++i) // the importer bakes node transforms; a skin's bind pose must not be moved
                    if (std::fabs(world.m[i] - identity.m[i]) > 1e-5f) skinUsable_ = false;
            }
            if (!EmitMesh(mesh->Int(), world, skin >= 0)) return false;
        }
        if (const Json* kids = n->Find("children"))
            for (const Json& k : kids->a)
                if (!Visit(k.Int(), world, depth + 1)) return false;
        return true;
    }

    bool EmitMesh(int meshIndex, const M4& w, bool skinned = false) {
        const Json* meshes = doc_.Find("meshes");
        const Json* mesh = meshes ? meshes->At((size_t)meshIndex) : nullptr;
        if (!mesh) return Fail("bad mesh reference");
        const Json* prims = mesh->Find("primitives");
        if (!prims) return true;

        // Normals transform by the inverse transpose; for the rotation+uniform-scale case the upper 3x3 plus
        // renormalizing is exact, and good enough for non-uniform scale on imported props.
        for (const Json& prim : prims->a) {
            const Json* mode = prim.Find("mode");
            if (mode && mode->Int() != 4) continue; // triangles only
            const Json* attrs = prim.Find("attributes");
            const Json* posRef = attrs ? attrs->Find("POSITION") : nullptr;
            if (!posRef) continue;

            std::vector<float> pos, nrm, uv, col;
            if (!ReadFloats(posRef->Int(), 3, pos)) return Fail("cannot read POSITION");
            const bool hasN = attrs->Find("NORMAL") && ReadFloats(attrs->Find("NORMAL")->Int(), 3, nrm);
            const bool hasUv = attrs->Find("TEXCOORD_0") && ReadFloats(attrs->Find("TEXCOORD_0")->Int(), 2, uv);
            int colorComps = 4;
            bool hasCol = attrs->Find("COLOR_0") && ReadFloats(attrs->Find("COLOR_0")->Int(), 4, col);
            if (!hasCol && attrs->Find("COLOR_0") && ReadFloats(attrs->Find("COLOR_0")->Int(), 3, col)) { hasCol = true; colorComps = 3; }

            std::vector<float> skinJ, skinW;
            const bool hasSkin = skinned && model_ && attrs->Find("JOINTS_0") && attrs->Find("WEIGHTS_0") &&
                                 ReadFloats(attrs->Find("JOINTS_0")->Int(), 4, skinJ) && ReadFloats(attrs->Find("WEIGHTS_0")->Int(), 4, skinW) &&
                                 skinJ.size() == pos.size() / 3 * 4 && skinW.size() == skinJ.size();

            float base[4] = {0.8f, 0.8f, 0.8f, 1.0f};
            int fabricKind = 0; // 1 cloth, 2 leather
            bool glass = false;
            bool interior = false; // vehicle window with a ray-cast saloon behind it: material name contains "interior"; alpha 0.70, see InteriorPS in mesh.hlsl
            bool roomglass = false; // a building window: a room behind scratched glass; material name contains "roomglass"; alpha 0.665, see RoomGlassPS
            bool spom = false; // a silhouette-POM volume (balcony, canopy): material name contains "spom"; alpha 0.8, see SpomPS in mesh.hlsl
            float roughness = 0.72f, metallic = 0.0f; // glTF's defaults are 1 and 1; a file without factors keeps the engine's older look
            ImportedPart part;
            part.first = out_.size();
            if (const Json* mat = prim.Find("material")) {
                const Json* mats = doc_.Find("materials");
                const Json* m = mats ? mats->At((size_t)mat->Int()) : nullptr;
                const Json* pbr = m ? m->Find("pbrMetallicRoughness") : nullptr;
                if (const Json* bc = pbr ? pbr->Find("baseColorFactor") : nullptr)
                    for (int i = 0; i < 4 && i < (int)bc->a.size(); ++i) base[i] = (float)bc->a[(size_t)i].Num();
                if (const Json* rf = pbr ? pbr->Find("roughnessFactor") : nullptr) roughness = (float)rf->Num();
                bool metallicGiven = false;
                if (const Json* mf = pbr ? pbr->Find("metallicFactor") : nullptr) { metallic = (float)mf->Num(); metallicGiven = true; }
                if (const Json* am = m ? m->Find("alphaMode") : nullptr) glass = am->s == "BLEND" && roughness < 0.2f; // window panes
                if (model_) {
                    part.albedo = ImageOf(pbr ? pbr->Find("baseColorTexture") : nullptr);
                    part.normal = ImageOf(m ? m->Find("normalTexture") : nullptr);
                    part.rough = ImageOf(pbr ? pbr->Find("metallicRoughnessTexture") : nullptr);
                    if (part.rough >= 0 && !metallicGiven) metallic = 1.0f; // glTF's default factor is 1: the map's B alone decides
                    part.occlusion = ImageOf(m ? m->Find("occlusionTexture") : nullptr);
                    part.emissive = ImageOf(m ? m->Find("emissiveTexture") : nullptr);
                    if (const Json* ef = m ? m->Find("emissiveFactor") : nullptr)
                        for (int i = 0; i < 3 && i < (int)ef->a.size(); ++i) part.emissiveFactor[i] = (float)ef->a[(size_t)i].Num();
                    part.metallic = metallic;
                    const Json* extras = m ? m->Find("extras") : nullptr;
                    const Json* spec = extras ? extras->Find("spec") : nullptr;
                    part.art = extras && part.albedo >= 0 && part.normal < 0 && !(spec && spec->Find("map"));
                }
                if (const Json* name = m ? m->Find("name") : nullptr) {
                    part.skin = name->s == "M_Skin";
                    part.ground = name->s == "M_Ground";
                    part.groundSplat = name->s == "M_PerditionGround";
                    part.eye = name->s == "M_Eye";
                    std::string low = name->s;
                    for (char& ch : low) ch = (char)std::tolower((unsigned char)ch);
                    const auto has = [&](const char* w) { return low.find(w) != std::string::npos; };
                    spom = has("spom");
                    roomglass = has("roomglass");
                    interior = has("interior");
                    if (spom) fabricKind = 0;
                    else if (has("jeans") || has("shirt") || has("flannel") || has("coat") || has("trouser") || has("pants") || has("vest") || has("cloth") || has("fabric")) fabricKind = 1;
                    else if ((has("boot") || has("hat") || has("leather") || has("saddle") || has("strap") || has("belt") || has("holster")) && !has("buckle") && !has("sole")) fabricKind = 2;
                } else if (m) part.unnamed = true;
            }

            std::vector<uint32_t> idx;
            if (const Json* ir = prim.Find("indices")) {
                if (!ReadIndices(ir->Int(), idx)) return Fail("cannot read indices");
            } else {
                idx.resize(pos.size() / 3);
                for (size_t i = 0; i < idx.size(); ++i) idx[i] = (uint32_t)i;
            }

            const size_t vcount = pos.size() / 3;
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                const size_t first = out_.size();
                for (size_t k = 0; k < 3; ++k) {
                    const size_t i = idx[t + k];
                    if (i >= vcount) return Fail("index out of range");
                    Vertex v{};
                    const float* p = &pos[i * 3];
                    v.px = w.m[0] * p[0] + w.m[4] * p[1] + w.m[8] * p[2] + w.m[12];
                    v.py = w.m[1] * p[0] + w.m[5] * p[1] + w.m[9] * p[2] + w.m[13];
                    v.pz = w.m[2] * p[0] + w.m[6] * p[1] + w.m[10] * p[2] + w.m[14];
                    if (hasN) {
                        const float* q = &nrm[i * 3];
                        float nx = w.m[0] * q[0] + w.m[4] * q[1] + w.m[8] * q[2];
                        float ny = w.m[1] * q[0] + w.m[5] * q[1] + w.m[9] * q[2];
                        float nz = w.m[2] * q[0] + w.m[6] * q[1] + w.m[10] * q[2];
                        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (len > 1e-20f) { nx /= len; ny /= len; nz /= len; }
                        v.nx = nx; v.ny = ny; v.nz = nz;
                    }
                    if (hasUv) { v.u = uv[i * 2]; v.v = uv[i * 2 + 1]; }
                    v.r = base[0]; v.g = base[1]; v.b = base[2]; v.a = base[3];
                    if (hasCol) {
                        const float* c = &col[i * colorComps];
                        v.r *= c[0]; v.g *= c[1]; v.b *= c[2];
                        if (colorComps == 4) v.a *= c[3];
                    }
                    out_.push_back(v);
                    if (hasSkin) {
                        model_->vertexSkin.resize(out_.size());
                        VertexSkin& vs = model_->vertexSkin.back();
                        float sum = 0;
                        for (int c = 0; c < 4; ++c) sum += skinW[i * 4 + c];
                        if (sum > 1e-6f)
                            for (int c = 0; c < 4; ++c) {
                                vs.joints[c] = (uint16_t)skinJ[i * 4 + c];
                                vs.weights[c] = skinW[i * 4 + c] / sum;
                            }
                        anySkinnedVertex_ = true;
                    }
                }
                if (!hasN && genNormals_) FlatNormals(out_, first);
            }
            part.count = out_.size() - part.first;
            if (part.skin)
                for (size_t k = part.first; k < out_.size(); ++k) out_[k].a = 0.5f;
            const bool paneOnly = glass && !part.skin && !part.eye && !part.ground && !part.groundSplat;
            if (part.eye) // alpha 0.95: the shader adds the wet cornea
                for (size_t k = part.first; k < out_.size(); ++k) out_[k].a = 0.95f;
            if (part.ground) // alpha 0.75: the shader hides this material's tiling
                for (size_t k = part.first; k < out_.size(); ++k) out_[k].a = 0.75f;
            if (part.unnamed && hasCol && path_.find("Terrain") != std::string::npos) part.groundSplat = true; // the export's unnamed terrain pieces, such as the plaza
            if (part.groundSplat) // sentinel alpha identifies this textured splat path in the shared shader
                for (size_t k = part.first; k < out_.size(); ++k) out_[k].a = 0.2f;
            if (interior && !part.skin && !part.eye && !part.ground && !part.groundSplat) {
                for (size_t k = part.first; k < out_.size(); ++k) out_[k].a = 0.70f; // rgb = seat fabric, uv = style/pane/variant
            } else if (roomglass && !part.skin && !part.eye && !part.ground && !part.groundSplat) {
                for (size_t k = part.first; k < out_.size(); ++k) out_[k].a = 0.665f; // rgb = room width, height, seed; uv = metres on the plane
            } else if (spom && !part.skin && !part.eye && !part.ground && !part.groundSplat) {
                for (size_t k = part.first; k < out_.size(); ++k) out_[k].a = 0.8f; // rgb and uv carry the volume's frame: the shader reads them raw
            } else if (!part.skin && !part.eye && !part.ground && !part.groundSplat && fabricKind != 0) {
                // Fabric and leather, told apart by material name: alpha 0.61 and 0.63, read by PSMain in mesh.hlsl.
                for (size_t k = part.first; k < out_.size(); ++k) out_[k].a = fabricKind == 1 ? 0.61f : 0.63f;
            } else if (!part.skin && !part.eye && !part.ground && !part.groundSplat) {
                // Other materials: alpha 0.86..0.91 packs roughness (32 steps) and metal or not; see PbrFromAlpha in mesh.hlsl.
                const int rough = (int)std::lround(std::clamp(roughness, 0.0f, 1.0f) * 31.0f);
                const float a = 0.86f + 0.05f * (float)(rough * 2 + (metallic >= 0.5f ? 1 : 0)) / 63.0f;
                for (size_t k = part.first; k < out_.size(); ++k) out_[k].a = a;
            }
            if (paneOnly) { // alpha 0.645: GlassPS in mesh.hlsl
                for (size_t k = part.first; k < out_.size(); ++k) out_[k].a = 0.645f;
                TagPanes(part.first, out_.size());
            }
            part.glass = paneOnly;
            if (model_ && part.count) model_->parts.push_back(part);
        }
        return true;
    }

    // Reorders the used skin's joints parents-first and reads their rest pose and inverse bind matrices.
    void LoadSkin() {
        const Json* skins = doc_.Find("skins");
        const Json* nodes = doc_.Find("nodes");
        const Json* skin = skins && skinUsed_ >= 0 ? skins->At((size_t)skinUsed_) : nullptr;
        const Json* jointList = skin ? skin->Find("joints") : nullptr;
        if (!jointList || !nodes || !skinUsable_ || !anySkinnedVertex_) {
            model_->vertexSkin.clear();
            return;
        }
        const size_t jointCount = jointList->a.size();
        std::vector<int> nodeParent(nodes->a.size(), -1);
        for (size_t i = 0; i < nodes->a.size(); ++i)
            if (const Json* kids = nodes->a[i].Find("children"))
                for (const Json& k : kids->a)
                    if (k.Int() >= 0 && k.Int() < (int)nodeParent.size()) nodeParent[(size_t)k.Int()] = (int)i;
        std::vector<int> skinIndexOfNode(nodes->a.size(), -1);
        for (size_t j = 0; j < jointCount; ++j) {
            const int node = jointList->a[j].Int();
            if (node < 0 || node >= (int)nodes->a.size()) { model_->vertexSkin.clear(); return; }
            skinIndexOfNode[(size_t)node] = (int)j;
        }
        std::vector<float> ibm;
        if (const Json* ref = skin->Find("inverseBindMatrices")) ReadMat4s(ref->Int(), ibm);
        // Parents first: repeatedly take the joints whose parent joint is already placed.
        std::vector<int> newIndex(jointCount, -1);
        std::vector<SkinJoint> sorted;
        sorted.reserve(jointCount);
        while (sorted.size() < jointCount) {
            const size_t before = sorted.size();
            for (size_t j = 0; j < jointCount; ++j) {
                if (newIndex[j] >= 0) continue;
                const int node = jointList->a[j].Int();
                int above = nodeParent[(size_t)node];
                while (above >= 0 && skinIndexOfNode[(size_t)above] < 0) above = nodeParent[(size_t)above];
                const int parentJoint = above >= 0 ? skinIndexOfNode[(size_t)above] : -1;
                if (parentJoint >= 0 && newIndex[(size_t)parentJoint] < 0) continue;
                SkinJoint out;
                const Json& n = nodes->a[(size_t)node];
                if (const Json* nm = n.Find("name")) out.name = nm->s;
                NodeTRS(n, out.t, out.q, out.s);
                if (n.Find("matrix")) { // a joint stored as a matrix: split it, ignoring shear
                    const M4 l = NodeLocal(n);
                    out.t[0] = l.m[12]; out.t[1] = l.m[13]; out.t[2] = l.m[14];
                    MatrixToRS(l, out.q, out.s);
                }
                out.parent = parentJoint >= 0 ? newIndex[(size_t)parentJoint] : -1;
                if (ibm.size() >= (j + 1) * 16) std::memcpy(out.inverseBind, &ibm[j * 16], sizeof(out.inverseBind));
                if (parentJoint < 0) { // the non-joint nodes above a root joint
                    M4 up = Identity();
                    for (int a = nodeParent[(size_t)node]; a >= 0; a = nodeParent[(size_t)a]) up = Mul(NodeLocal(nodes->a[(size_t)a]), up);
                    std::memcpy(out.rootParent, up.m, sizeof(out.rootParent));
                }
                newIndex[j] = (int)sorted.size();
                sorted.push_back(std::move(out));
            }
            if (sorted.size() == before) { model_->vertexSkin.clear(); return; } // a cycle: not a valid skin
        }
        model_->joints = std::move(sorted);
        model_->vertexSkin.resize(out_.size());
        for (VertexSkin& vs : model_->vertexSkin)
            for (int c = 0; c < 4; ++c)
                if (vs.weights[c] > 0.0f) {
                    if (vs.joints[c] >= jointCount) { vs.weights[c] = 0.0f; continue; }
                    vs.joints[c] = (uint16_t)newIndex[vs.joints[c]];
                }
    }

    static void MatrixToRS(const M4& l, float q[4], float s[3]) {
        for (int c = 0; c < 3; ++c) s[c] = std::sqrt(l.m[c * 4] * l.m[c * 4] + l.m[c * 4 + 1] * l.m[c * 4 + 1] + l.m[c * 4 + 2] * l.m[c * 4 + 2]);
        const float sx = std::max(s[0], 1e-9f), sy = std::max(s[1], 1e-9f), sz = std::max(s[2], 1e-9f);
        const float m00 = l.m[0] / sx, m10 = l.m[1] / sx, m20 = l.m[2] / sx, m01 = l.m[4] / sy, m11 = l.m[5] / sy, m21 = l.m[6] / sy,
                    m02 = l.m[8] / sz, m12 = l.m[9] / sz, m22 = l.m[10] / sz;
        const float tr = m00 + m11 + m22;
        if (tr > 0) { const float r = std::sqrt(1 + tr) * 2; q[3] = 0.25f * r; q[0] = (m21 - m12) / r; q[1] = (m02 - m20) / r; q[2] = (m10 - m01) / r; }
        else if (m00 > m11 && m00 > m22) { const float r = std::sqrt(1 + m00 - m11 - m22) * 2; q[3] = (m21 - m12) / r; q[0] = 0.25f * r; q[1] = (m01 + m10) / r; q[2] = (m02 + m20) / r; }
        else if (m11 > m22) { const float r = std::sqrt(1 + m11 - m00 - m22) * 2; q[3] = (m02 - m20) / r; q[0] = (m01 + m10) / r; q[1] = 0.25f * r; q[2] = (m12 + m21) / r; }
        else { const float r = std::sqrt(1 + m22 - m00 - m11) * 2; q[3] = (m10 - m01) / r; q[0] = (m02 + m20) / r; q[1] = (m12 + m21) / r; q[2] = 0.25f * r; }
    }

    // MAT4 accessors are not covered by ReadFloats' scalar..VEC4 types.
    bool ReadMat4s(int index, std::vector<float>& out) {
        const Json* accs = doc_.Find("accessors");
        const Json* views = doc_.Find("bufferViews");
        const Json* acc = accs ? accs->At((size_t)index) : nullptr;
        if (!acc || !views || !acc->Find("bufferView") || !acc->Find("type") || acc->Find("type")->s != "MAT4") return false;
        if (!acc->Find("componentType") || acc->Find("componentType")->Int() != 5126) return false;
        const Json* view = views->At((size_t)acc->Find("bufferView")->Int());
        if (!view) return false;
        const int buf = view->Find("buffer") ? view->Find("buffer")->Int() : 0;
        if (buf < 0 || buf >= (int)buffers_.size()) return false;
        const size_t count = acc->Find("count") ? (size_t)acc->Find("count")->Num() : 0;
        const size_t base = (view->Find("byteOffset") ? (size_t)view->Find("byteOffset")->Num() : 0) +
                            (acc->Find("byteOffset") ? (size_t)acc->Find("byteOffset")->Num() : 0);
        size_t stride = view->Find("byteStride") ? (size_t)view->Find("byteStride")->Num() : 0;
        if (stride == 0) stride = 64;
        if (count == 0 || base + (count - 1) * stride + 64 > buffers_[(size_t)buf].size()) return false;
        out.resize(count * 16);
        for (size_t i = 0; i < count; ++i) std::memcpy(&out[i * 16], &buffers_[(size_t)buf][base + i * stride], 64);
        return true;
    }

    void LoadClips() {
        const Json* anims = doc_.Find("animations");
        const Json* nodes = doc_.Find("nodes");
        if (!anims || !nodes) return;
        for (const Json& a : anims->a) {
            AnimClip clip;
            if (const Json* nm = a.Find("name")) clip.name = nm->s;
            const Json* samplers = a.Find("samplers");
            const Json* channels = a.Find("channels");
            if (!samplers || !channels) continue;
            for (const Json& ch : channels->a) {
                const Json* target = ch.Find("target");
                const Json* samplerRef = ch.Find("sampler");
                const Json* nodeRef = target ? target->Find("node") : nullptr;
                const Json* path = target ? target->Find("path") : nullptr;
                const Json* sampler = samplerRef ? samplers->At((size_t)samplerRef->Int()) : nullptr;
                const Json* node = nodeRef ? nodes->At((size_t)nodeRef->Int()) : nullptr;
                const Json* name = node ? node->Find("name") : nullptr;
                if (!path || !sampler || !name || !sampler->Find("input") || !sampler->Find("output")) continue;
                const size_t comps = path->s == "rotation" ? 4 : (path->s == "translation" || path->s == "scale") ? 3 : 0;
                if (comps == 0) continue; // morph weights
                AnimTrack track;
                std::vector<float> values;
                if (!ReadFloats(sampler->Find("input")->Int(), 1, track.times) || !ReadFloats(sampler->Find("output")->Int(), (int)comps, values)) continue;
                const std::string interp = sampler->Find("interpolation") ? sampler->Find("interpolation")->s : "LINEAR";
                track.step = interp == "STEP";
                const size_t keys = track.times.size();
                if (interp == "CUBICSPLINE") { // in-tangent, value, out-tangent per key: plays the values linearly
                    if (values.size() != keys * comps * 3) continue;
                    track.values.resize(keys * comps);
                    for (size_t k = 0; k < keys; ++k)
                        for (size_t c = 0; c < comps; ++c) track.values[k * comps + c] = values[k * comps * 3 + comps + c];
                } else {
                    if (values.size() != keys * comps) continue;
                    track.values = std::move(values);
                }
                if (!track.times.empty()) clip.duration = std::max(clip.duration, track.times.back());
                if (comps == 3 && !track.values.empty()) {
                    float drift = 0;
                    for (size_t k = 0; k < track.values.size(); ++k) drift = std::max(drift, std::fabs(track.values[k] - track.values[k % 3]));
                    track.still = drift <= 0.001f;
                }
                AnimJointTracks& jt = clip.joints[name->s];
                (path->s == "rotation" ? jt.rotation : path->s == "translation" ? jt.translation : jt.scale) = std::move(track);
            }
            if (!clip.joints.empty()) model_->clips.push_back(std::move(clip));
        }
    }
};

} // namespace meshimport_detail

// Loads `path` (.obj, .gltf or .glb) into `out` as unindexed triangles. Returns false and fills `error` on failure.
inline bool ImportMesh(const std::string& path, std::vector<Vertex>& out, std::string* error = nullptr,
                       const ImportOptions& options = {}, ImportedModel* model = nullptr) {
    using namespace meshimport_detail;
    std::string err;
    out.clear();
    const std::string ext = LowerExt(path);
    bool ok = false;
    if (ext == ".obj") ok = LoadObj(path, out, options.generateFlatNormals, err);
    else if (ext == ".gltf" || ext == ".glb") ok = GltfLoader(path, out, options.generateFlatNormals, err, model).Load();
    else err = "unsupported mesh format '" + ext + "' (use .obj, .gltf or .glb; convert others with Blender or Assimp)";
    if (!ok) {
        out.clear();
        if (error) *error = err;
        return false;
    }
    if (options.normalizeToUnitBox) NormalizeToUnitBox(out);
    return true;
}

} // namespace tbd
