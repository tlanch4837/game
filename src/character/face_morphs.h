// Character creator, data side: the face maker's identity morphs (gameenginetools\face_maker\out\morphs.json, written by
// build_morphs.py) applied to Wyatt's head on the CPU. No UI and no renderer calls here, so it runs headless (tests, TBD_CHARPRESET).
//
// morphs.json: 30 signed sliders, each a sparse list of per-vertex deltas keyed by the GLB's vertex index (POSITION accessor of
// the head's primitives, shared by M_Skin and both M_Eye). final = rest + sum(value_i * delta_i), linear and order-free.
// The engine's import is a flat, unindexed triangle list with the node transform (and, without #native, a unit-box fit) baked
// in, plus hair cards appended by the importer. HeadMorpher rebuilds the flat -> GLB index map by walking the GLB's primitives in
// the importer's order, fits the affine map GLB -> flat space (checked to a tight residual), and then writes, per change:
//   skin/eye vertices: rest + A * delta, normals = authored + (welded normals after - before) (build_morphs.py `morphed`)
//   hair card vertices: follow the displacement of the skin vertex nearest them at rest (the same binding FaceRig's BindHair
//   makes), so the cards re-seat themselves after forehead, brow or ear sliders without a separate re-bind step.
// The eye UVs ApplyEyeMaps projected at rest stay right: the eyeballs only move rigidly in every slider.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "../mesh_import.h"

namespace tbd::character {

using meshimport_detail::Json;
using meshimport_detail::JsonParser;

// ---------------------------------------------------------------- SHA-1 (morphs.json rest_checksum)
class Sha1 {
public:
    void Add(const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        total_ += n;
        while (n) {
            const size_t take = std::min(n, (size_t)64 - fill_);
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take; p += take; n -= take;
            if (fill_ == 64) { Block(buf_); fill_ = 0; }
        }
    }
    std::string HexDigest() {
        const uint64_t bits = total_ * 8;
        const uint8_t one = 0x80, zero = 0;
        Add(&one, 1);
        while (fill_ != 56) Add(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        Add(len, 8);
        static const char* hex = "0123456789abcdef";
        std::string s;
        for (uint32_t w : h_)
            for (int i = 28; i >= 0; i -= 4) s += hex[(w >> i) & 15];
        return s;
    }

private:
    uint32_t h_[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t buf_[64] = {};
    size_t fill_ = 0;
    uint64_t total_ = 0;
    static uint32_t Rol(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
    void Block(const uint8_t* b) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999u; }
            else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = bb ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(bb, 30); bb = a; a = t;
        }
        h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d; h_[4] += e;
    }
};

inline bool ReadBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

// ---------------------------------------------------------------- morphs.json
struct MorphSlider {
    std::string name, group, label, minus, plus;
    float lo = -0.8f, hi = 0.8f, sigma = 0.35f, maxDispMm = 0.0f; // believable range, Randomize spread
    std::vector<uint16_t> idx; // GLB vertex indices
    std::vector<float> d;      // metres at +1, xyz per index
};

struct MorphSet {
    int vertexCount = 0;
    std::string mesh, restChecksum;
    std::vector<std::string> groups;
    std::vector<MorphSlider> sliders;
    float groupRmsCap = 0.6f, dispCapMm = 7.5f;
    std::vector<std::pair<int, float>> ageOffsets; // slider, offset at age 1

    bool Valid() const { return vertexCount > 0 && !sliders.empty(); }
    int Find(const std::string& name) const {
        for (size_t i = 0; i < sliders.size(); ++i)
            if (sliders[i].name == name) return (int)i;
        return -1;
    }

    static bool ReadJson(const std::wstring& path, Json& doc, std::string* err) {
        std::vector<uint8_t> bytes;
        if (!ReadBytes(path, bytes)) { if (err) *err = "cannot read the file"; return false; }
        if (!JsonParser((const char*)bytes.data(), bytes.size()).Parse(doc) || doc.type != Json::Object) { if (err) *err = "not JSON"; return false; }
        if (!doc.Find("version") || doc.Find("version")->Int() != 1) { if (err) *err = "version is not 1"; return false; }
        return true;
    }
    bool Load(const std::wstring& path, std::string* err) {
        Json doc;
        return ReadJson(path, doc, err) && Parse(doc, "", err);
    }

    // `meshName` empty: morphs.json (one mesh, deltas on each slider). Otherwise one mesh of a multi-mesh file such as
    // body_morphs.json: doc.meshes[meshName] = {vertex_count, rest_checksum}, each slider's deltas for it in
    // slider.targets[] = {mesh, count, scale, indices, deltas} (a slider without a target for it moves nothing there).
    bool Parse(const Json& doc, const std::string& meshName, std::string* err) {
        const auto str = [](const Json* j) { return j && j->type == Json::String ? j->s : std::string(); };
        const Json* meshInfo = &doc;
        if (!meshName.empty()) {
            const Json* ms = doc.Find("meshes");
            meshInfo = ms ? ms->Find(meshName.c_str()) : nullptr;
            if (!meshInfo) { if (err) *err = "no meshes." + meshName; return false; }
        }
        vertexCount = meshInfo->Find("vertex_count") ? meshInfo->Find("vertex_count")->Int() : 0;
        mesh = meshName.empty() ? str(doc.Find("mesh")) : meshName;
        restChecksum = str(meshInfo->Find("rest_checksum"));
        groups.clear();
        if (const Json* g = doc.Find("groups"))
            for (const Json& x : g->a) groups.push_back(x.s);
        sliders.clear();
        const Json* sl = doc.Find("sliders");
        if (!sl || sl->type != Json::Array) { if (err) *err = "morphs.json has no sliders"; return false; }
        for (const Json& s : sl->a) {
            MorphSlider m;
            m.name = str(s.Find("name")); m.group = str(s.Find("group")); m.label = str(s.Find("label"));
            m.minus = str(s.Find("minus")); m.plus = str(s.Find("plus"));
            if (const Json* b = s.Find("believable"); b && b->a.size() == 2) { m.lo = (float)b->a[0].Num(-0.8); m.hi = (float)b->a[1].Num(0.8); }
            if (const Json* v = s.Find("sigma")) m.sigma = (float)v->Num(0.35);
            if (const Json* v = s.Find("max_disp_mm")) m.maxDispMm = (float)v->Num();
            const Json* src = &s;
            if (!meshName.empty()) {
                src = nullptr;
                if (const Json* ts = s.Find("targets"))
                    for (const Json& t : ts->a)
                        if (str(t.Find("mesh")) == meshName) src = &t;
                if (!src) { sliders.push_back(std::move(m)); continue; }
            }
            const int count = src->Find("count") ? src->Find("count")->Int() : 0;
            const double scale = src->Find("scale") ? src->Find("scale")->Num(1.0) : 1.0;
            std::vector<uint8_t> ib, db;
            if (!meshimport_detail::DecodeBase64(str(src->Find("indices")), ib) || !meshimport_detail::DecodeBase64(str(src->Find("deltas")), db) ||
                ib.size() != (size_t)count * 2 || db.size() != (size_t)count * 6) {
                if (err) *err = "slider " + m.name + ": bad delta data";
                return false;
            }
            m.idx.resize((size_t)count);
            m.d.resize((size_t)count * 3);
            for (int i = 0; i < count; ++i) {
                m.idx[(size_t)i] = (uint16_t)(ib[(size_t)i * 2] | ib[(size_t)i * 2 + 1] << 8);
                if (m.idx[(size_t)i] >= vertexCount) { if (err) *err = "slider " + m.name + ": vertex out of range"; return false; }
            }
            for (int i = 0; i < count * 3; ++i) {
                const int16_t q = (int16_t)(uint16_t)(db[(size_t)i * 2] | db[(size_t)i * 2 + 1] << 8);
                m.d[(size_t)i] = (float)(q * scale);
            }
            sliders.push_back(std::move(m));
        }
        if (const Json* lim = doc.Find("limiter")) {
            if (const Json* v = lim->Find("group_rms_cap")) groupRmsCap = (float)v->Num(0.6);
            if (const Json* v = lim->Find("disp_cap_mm")) dispCapMm = (float)v->Num(7.5);
        }
        ageOffsets.clear();
        if (const Json* ac = doc.Find("age_curve"))
            if (const Json* off = ac->Find("offsets_at_age_1"))
                for (const auto& [k, v] : off->o)
                    if (const int i = Find(k); i >= 0) ageOffsets.push_back({i, (float)v.Num()});
        return true;
    }

    // Sum of value * delta per GLB vertex (metres), into `out` (vertexCount * 3).
    void Compose(const std::vector<float>& values, std::vector<float>& out) const {
        out.assign((size_t)vertexCount * 3, 0.0f);
        for (size_t s = 0; s < sliders.size() && s < values.size(); ++s) {
            const float v = values[s];
            if (v == 0.0f) continue;
            const MorphSlider& m = sliders[s];
            for (size_t k = 0; k < m.idx.size(); ++k) {
                float* o = &out[(size_t)m.idx[k] * 3];
                o[0] += v * m.d[k * 3]; o[1] += v * m.d[k * 3 + 1]; o[2] += v * m.d[k * 3 + 2];
            }
        }
    }
    float MaxDisplacementMm(const std::vector<float>& values) const {
        std::vector<float> d;
        Compose(values, d);
        float m = 0.0f;
        for (size_t i = 0; i + 2 < d.size(); i += 3) m = std::max(m, d[i] * d[i] + d[i + 1] * d[i + 1] + d[i + 2] * d[i + 2]);
        return std::sqrt(m) * 1000.0f;
    }

    // morphs.json "limiter": clamp to believable, cap each group's RMS, cap the largest displacement.
    void Limit(std::vector<float>& v) const {
        v.resize(sliders.size(), 0.0f);
        for (size_t i = 0; i < sliders.size(); ++i) v[i] = std::clamp(v[i], sliders[i].lo, sliders[i].hi);
        for (const std::string& g : groups) {
            double sum = 0; int n = 0;
            for (size_t i = 0; i < sliders.size(); ++i)
                if (sliders[i].group == g) { sum += (double)v[i] * v[i]; ++n; }
            const double rms = n ? std::sqrt(sum / n) : 0.0;
            if (rms > groupRmsCap)
                for (size_t i = 0; i < sliders.size(); ++i)
                    if (sliders[i].group == g) v[i] *= (float)(groupRmsCap / rms);
        }
        const float dm = MaxDisplacementMm(v);
        if (dm > dispCapMm)
            for (float& x : v) x *= dispCapMm / dm;
    }

    // Same generator as face_maker\index.html (mulberry32(seed * 7919 + 13), Box-Muller): a seed gives the browser's face.
    // With age > 0 the age curve is added before the limiter, as build_morphs.py randomize(seed, age=...) does.
    // `n` standard normals from mulberry32(seed * 7919 + 13) and Box-Muller, one pair of draws each (index.html's gauss()).
    static std::vector<double> Normals(uint32_t seed, size_t n) {
        uint32_t a = seed * 7919u + 13u;
        const auto next = [&a]() -> double {
            a += 0x6D2B79F5u;
            uint32_t t = (a ^ (a >> 15)) * (1u | a);
            t = (t + (t ^ (t >> 7)) * (61u | t)) ^ t;
            return (double)(t ^ (t >> 14)) / 4294967296.0;
        };
        std::vector<double> z(n);
        for (double& x : z) {
            double u = 0;
            while (u == 0) u = next();
            const double w = next();
            x = std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * w);
        }
        return z;
    }
    std::vector<float> Randomize(uint32_t seed, float age) const {
        const std::vector<double> z = Normals(seed, sliders.size());
        std::vector<float> v(sliders.size());
        for (size_t i = 0; i < sliders.size(); ++i) v[i] = (float)(z[i] * sliders[i].sigma);
        AddAge(v, age);
        Limit(v);
        return v;
    }
    void AddAge(std::vector<float>& v, float age) const {
        v.resize(sliders.size(), 0.0f);
        for (const auto& [i, k] : ageOffsets) v[(size_t)i] = std::clamp(v[(size_t)i] + k * age, -1.0f, 1.0f);
    }
};

// ---------------------------------------------------------------- the head in the engine
class HeadMorpher {
public:
    std::string status; // why Bind failed, or what it found

    // `glbPath`: the file (no #native tag). `flat`: the entity's rest vertices as the engine imported them (EntityVertices).
    // `model`: its parts (skin / eye / hair flags and ranges).
    bool Bind(const MorphSet& set, const std::string& glbPath, const std::vector<Vertex>& flat, const ImportedModel& model) {
        bound_ = false;
        set_ = &set;
        flatRest_ = flat;
        if (!ReadHead(glbPath)) return false;
        if ((int)glbPos_.size() / 3 != set.vertexCount) { status = "vertex count " + std::to_string(glbPos_.size() / 3) + " is not morphs.json's " + std::to_string(set.vertexCount); return false; }
        checksum_ = Checksum();
        checksumOk_ = set.restChecksum.empty() || checksum_ == set.restChecksum;
        if (!checksumOk_) { status = "rest_checksum " + checksum_ + " differs from morphs.json " + set.restChecksum + ": another head"; return false; }
        if (flatToGlb_.size() > flat.size()) { status = "the engine's mesh has fewer vertices than the file's triangles"; return false; }
        if (!FitAffine()) return false;
        BuildWeld();
        BindHair(model);
        bound_ = true;
        status = "bound: " + std::to_string(set.vertexCount) + " head vertices, " + std::to_string(hair_.size()) + " hair card vertices, checksum ok";
        return true;
    }
    bool Bound() const { return bound_; }
    const std::string& RestChecksum() const { return checksum_; }
    const std::vector<Vertex>& Rest() const { return flatRest_; }
    // Replaces rest vertices [first, first + v.size()) (the brow maker's baked cards); hair-card vertices keep their skin binding.
    void SetRest(size_t first, const std::vector<Vertex>& v) { if (first + v.size() <= flatRest_.size()) std::copy(v.begin(), v.end(), flatRest_.begin() + (ptrdiff_t)first); }
    const std::vector<uint32_t>& FlatToGlb() const { return flatToGlb_; } // flat vertex -> GLB index (UINT32_MAX: not from the file)

    // The entity's vertices with `values` applied (same layout and count as the rest list).
    // `extra`: a second morph set on the same GLB vertices (body_maker's head part, the neck), added to this one's deltas.
    void Deform(const std::vector<float>& values, std::vector<Vertex>& out, const MorphSet* extra = nullptr, const std::vector<float>* extraValues = nullptr) {
        out = flatRest_;
        if (!bound_) return;
        set_->Compose(values, delta_);
        if (extra && extraValues && extra->vertexCount == set_->vertexCount) {
            extra->Compose(*extraValues, extraDelta_);
            for (size_t i = 0; i < delta_.size(); ++i) delta_[i] += extraDelta_[i];
        }
        // welded area-weighted skin normals after the move; before = n0_
        for (size_t i = 0; i < glbPos_.size(); ++i) moved_[i] = glbPos_[i] + delta_[i];
        WeldNormals(moved_, n1_);
        for (size_t k = 0; k < flatToGlb_.size(); ++k) {
            const uint32_t g = flatToGlb_[k];
            if (g == UINT32_MAX) continue;
            const float* d = &delta_[(size_t)g * 3];
            if (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f) continue;
            Vertex& v = out[k];
            v.px += A_[0] * d[0] + A_[1] * d[1] + A_[2] * d[2];
            v.py += A_[3] * d[0] + A_[4] * d[1] + A_[5] * d[2];
            v.pz += A_[6] * d[0] + A_[7] * d[1] + A_[8] * d[2];
            if (eye_[g]) continue; // the eyes keep their authored normals, as build_morphs.py does
            const float dn[3] = {n1_[(size_t)g * 3] - n0_[(size_t)g * 3], n1_[(size_t)g * 3 + 1] - n0_[(size_t)g * 3 + 1], n1_[(size_t)g * 3 + 2] - n0_[(size_t)g * 3 + 2]};
            float nx = v.nx + R_[0] * dn[0] + R_[1] * dn[1] + R_[2] * dn[2];
            float ny = v.ny + R_[3] * dn[0] + R_[4] * dn[1] + R_[5] * dn[2];
            float nz = v.nz + R_[6] * dn[0] + R_[7] * dn[1] + R_[8] * dn[2];
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-12f) { v.nx = nx / len; v.ny = ny / len; v.nz = nz / len; }
        }
        for (const auto& [k, g] : hair_) {
            const float* d = &delta_[(size_t)g * 3];
            Vertex& v = out[k];
            v.px += A_[0] * d[0] + A_[1] * d[1] + A_[2] * d[2];
            v.py += A_[3] * d[0] + A_[4] * d[1] + A_[5] * d[2];
            v.pz += A_[6] * d[0] + A_[7] * d[1] + A_[8] * d[2];
        }
    }

private:
    const MorphSet* set_ = nullptr;
    bool bound_ = false, checksumOk_ = false;
    std::string checksum_;
    std::vector<Vertex> flatRest_;
    std::vector<float> glbPos_;          // GLB rest positions, xyz
    std::vector<uint32_t> skinTris_;     // M_Skin triangles, GLB indices
    std::vector<uint8_t> eye_;           // per GLB vertex: used by an M_Eye primitive
    std::vector<uint32_t> flatToGlb_;    // per flat vertex the importer emitted from the file (before hair): GLB index
    std::vector<uint32_t> weld_;         // GLB vertex -> welded position id
    uint32_t weldCount_ = 0;
    std::vector<float> n0_, n1_, delta_, extraDelta_, moved_, acc_;
    std::vector<std::pair<uint32_t, uint32_t>> hair_; // flat hair vertex, GLB vertex it rides
    float A_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // GLB -> flat linear part (row-major)
    float R_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // A_ without its scale, for normals

    std::string Checksum() const {
        Sha1 h;
        for (float p : glbPos_) {
            const int64_t q = (int64_t)std::nearbyint((double)p / 1e-6); // np.round(pos / 1e-6).astype(int64), half to even
            h.Add(&q, 8);
        }
        return h.HexDigest().substr(0, 16);
    }

    // The GLB's own primitives, in the importer's order (scene roots, depth first, triangles only).
    bool ReadHead(const std::string& path) {
        std::vector<uint8_t> file;
        if (!ReadBytes(meshimport_detail::Utf8ToWide(path), file) || file.size() < 20 || std::memcmp(file.data(), "glTF", 4) != 0) { status = "cannot read " + path + " as GLB"; return false; }
        const char* jsonText = nullptr;
        size_t jsonSize = 0;
        const uint8_t* bin = nullptr;
        size_t binSize = 0;
        for (size_t at = 12; at + 8 <= file.size();) {
            uint32_t len, type;
            std::memcpy(&len, &file[at], 4);
            std::memcpy(&type, &file[at + 4], 4);
            at += 8;
            if (at + len > file.size()) break;
            if (type == 0x4E4F534A) { jsonText = (const char*)&file[at]; jsonSize = len; }
            else if (type == 0x004E4942 && !bin) { bin = &file[at]; binSize = len; }
            at += (len + 3) & ~3u;
        }
        Json doc;
        if (!jsonText || !bin || !JsonParser(jsonText, jsonSize).Parse(doc)) { status = "GLB has no JSON or BIN chunk"; return false; }
        const Json* accs = doc.Find("accessors");
        const Json* views = doc.Find("bufferViews");
        const auto access = [&](int index, int& count, int& comps, int& ctype, const uint8_t*& base, size_t& stride) {
            const Json* acc = accs ? accs->At((size_t)index) : nullptr;
            const Json* view = acc && views && acc->Find("bufferView") ? views->At((size_t)acc->Find("bufferView")->Int()) : nullptr;
            if (!view) return false;
            count = acc->Find("count") ? acc->Find("count")->Int() : 0;
            ctype = acc->Find("componentType") ? acc->Find("componentType")->Int() : 0;
            const std::string t = acc->Find("type") ? acc->Find("type")->s : "";
            comps = t == "SCALAR" ? 1 : t == "VEC2" ? 2 : t == "VEC3" ? 3 : t == "VEC4" ? 4 : 0;
            const int csize = ctype == 5121 ? 1 : ctype == 5123 ? 2 : 4;
            const size_t off = (size_t)(view->Find("byteOffset") ? view->Find("byteOffset")->Num() : 0) + (size_t)(acc->Find("byteOffset") ? acc->Find("byteOffset")->Num() : 0);
            stride = view->Find("byteStride") ? (size_t)view->Find("byteStride")->Num() : 0;
            if (!stride) stride = (size_t)csize * comps;
            if (count <= 0 || off + (size_t)(count - 1) * stride + (size_t)csize * comps > binSize) return false;
            base = bin + off;
            return true;
        };
        const auto readIndices = [&](int index, std::vector<uint32_t>& out) {
            int count, comps, ctype; const uint8_t* base; size_t stride;
            if (!access(index, count, comps, ctype, base, stride) || comps != 1) return false;
            out.resize((size_t)count);
            for (int i = 0; i < count; ++i) {
                const uint8_t* p = base + (size_t)i * stride;
                if (ctype == 5121) out[(size_t)i] = *p;
                else if (ctype == 5123) { uint16_t x; std::memcpy(&x, p, 2); out[(size_t)i] = x; }
                else if (ctype == 5125) std::memcpy(&out[(size_t)i], p, 4);
                else return false;
            }
            return true;
        };
        int posAccessor = -1;
        glbPos_.clear(); skinTris_.clear(); flatToGlb_.clear();
        std::vector<uint32_t> eyeVerts, allTris; // allTris: normals for a mesh without M_Skin (body parts)
        const Json* mats = doc.Find("materials");
        const Json* meshes = doc.Find("meshes");
        const Json* nodes = doc.Find("nodes");
        const auto emitMesh = [&](int meshIndex) -> bool {
            const Json* mesh = meshes ? meshes->At((size_t)meshIndex) : nullptr;
            const Json* prims = mesh ? mesh->Find("primitives") : nullptr;
            if (!prims) return true;
            for (const Json& prim : prims->a) {
                if (const Json* mode = prim.Find("mode"); mode && mode->Int() != 4) continue;
                const Json* attrs = prim.Find("attributes");
                const Json* posRef = attrs ? attrs->Find("POSITION") : nullptr;
                if (!posRef) continue;
                int count, comps, ctype; const uint8_t* base; size_t stride;
                if (!access(posRef->Int(), count, comps, ctype, base, stride) || comps != 3 || ctype != 5126) { status = "unreadable POSITION"; return false; }
                std::vector<uint32_t> idx;
                if (const Json* ir = prim.Find("indices")) { if (!readIndices(ir->Int(), idx)) { status = "unreadable indices"; return false; } }
                else { idx.resize((size_t)count); for (int i = 0; i < count; ++i) idx[(size_t)i] = (uint32_t)i; }
                idx.resize(idx.size() / 3 * 3);
                const bool shared = posAccessor < 0 || posAccessor == posRef->Int();
                if (posAccessor < 0) {
                    posAccessor = posRef->Int();
                    glbPos_.resize((size_t)count * 3);
                    for (int i = 0; i < count; ++i) std::memcpy(&glbPos_[(size_t)i * 3], base + (size_t)i * stride, 12);
                }
                std::string matName;
                if (const Json* m = prim.Find("material"); m && mats)
                    if (const Json* mm = mats->At((size_t)m->Int()); mm && mm->Find("name")) matName = mm->Find("name")->s;
                for (uint32_t i : idx) flatToGlb_.push_back(shared && i < (uint32_t)count ? i : UINT32_MAX);
                if (!shared) continue;
                if (matName == "M_Skin") skinTris_.insert(skinTris_.end(), idx.begin(), idx.end());
                allTris.insert(allTris.end(), idx.begin(), idx.end());
                if (matName == "M_Eye") eyeVerts.insert(eyeVerts.end(), idx.begin(), idx.end());
            }
            return true;
        };
        std::vector<int> roots;
        if (nodes && nodes->type == Json::Array) {
            const Json* scenes = doc.Find("scenes");
            const Json* scene = scenes ? scenes->At((size_t)(doc.Find("scene") ? doc.Find("scene")->Int() : 0)) : nullptr;
            if (const Json* r = scene ? scene->Find("nodes") : nullptr)
                for (const Json& n : r->a) roots.push_back(n.Int());
            if (roots.empty())
                for (int i = 0; i < (int)nodes->a.size(); ++i) {
                    bool child = false;
                    for (const Json& other : nodes->a)
                        if (const Json* c = other.Find("children"))
                            for (const Json& k : c->a) child |= k.Int() == i;
                    if (!child) roots.push_back(i);
                }
            std::vector<std::pair<int, int>> stack; // node, depth (children visited in order)
            for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.push_back({*it, 0});
            while (!stack.empty()) {
                const auto [ni, depth] = stack.back();
                stack.pop_back();
                const Json* n = nodes->At((size_t)ni);
                if (!n || depth > 64) continue;
                if (const Json* m = n->Find("mesh")) if (!emitMesh(m->Int())) return false;
                if (const Json* kids = n->Find("children"))
                    for (auto it = kids->a.rbegin(); it != kids->a.rend(); ++it) stack.push_back({it->Int(), depth + 1});
            }
        } else if (meshes) {
            for (size_t i = 0; i < meshes->a.size(); ++i) if (!emitMesh((int)i)) return false;
        }
        if (skinTris_.empty()) skinTris_ = allTris;
        if (glbPos_.empty() || skinTris_.empty()) { status = "the GLB has no triangles"; return false; }
        eye_.assign(glbPos_.size() / 3, 0);
        for (uint32_t i : eyeVerts) eye_[i] = 1;
        return true;
    }

    // flat = A * glb + t, least squares over every mapped vertex, then checked vertex by vertex.
    bool FitAffine() {
        double mg[3] = {}, mf[3] = {};
        size_t n = 0;
        for (size_t k = 0; k < flatToGlb_.size(); ++k) {
            const uint32_t g = flatToGlb_[k];
            if (g == UINT32_MAX) continue;
            const Vertex& f = flatRest_[k];
            mg[0] += glbPos_[g * 3]; mg[1] += glbPos_[g * 3 + 1]; mg[2] += glbPos_[g * 3 + 2];
            mf[0] += f.px; mf[1] += f.py; mf[2] += f.pz;
            ++n;
        }
        if (n < 4) { status = "no vertices to match"; return false; }
        for (int c = 0; c < 3; ++c) { mg[c] /= (double)n; mf[c] /= (double)n; }
        double C[9] = {}, B[9] = {};
        for (size_t k = 0; k < flatToGlb_.size(); ++k) {
            const uint32_t g = flatToGlb_[k];
            if (g == UINT32_MAX) continue;
            const double gv[3] = {glbPos_[g * 3] - mg[0], glbPos_[g * 3 + 1] - mg[1], glbPos_[g * 3 + 2] - mg[2]};
            const double fv[3] = {flatRest_[k].px - mf[0], flatRest_[k].py - mf[1], flatRest_[k].pz - mf[2]};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) { C[r * 3 + c] += gv[r] * gv[c]; B[r * 3 + c] += fv[r] * gv[c]; }
        }
        const double det = C[0] * (C[4] * C[8] - C[5] * C[7]) - C[1] * (C[3] * C[8] - C[5] * C[6]) + C[2] * (C[3] * C[7] - C[4] * C[6]);
        if (std::fabs(det) < 1e-30) { status = "degenerate head"; return false; }
        double I[9] = {(C[4] * C[8] - C[5] * C[7]) / det, (C[2] * C[7] - C[1] * C[8]) / det, (C[1] * C[5] - C[2] * C[4]) / det,
                       (C[5] * C[6] - C[3] * C[8]) / det, (C[0] * C[8] - C[2] * C[6]) / det, (C[2] * C[3] - C[0] * C[5]) / det,
                       (C[3] * C[7] - C[4] * C[6]) / det, (C[1] * C[6] - C[0] * C[7]) / det, (C[0] * C[4] - C[1] * C[3]) / det};
        double A[9];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) A[r * 3 + c] = B[r * 3] * I[c] + B[r * 3 + 1] * I[3 + c] + B[r * 3 + 2] * I[6 + c];
        double t[3];
        for (int r = 0; r < 3; ++r) t[r] = mf[r] - (A[r * 3] * mg[0] + A[r * 3 + 1] * mg[1] + A[r * 3 + 2] * mg[2]);
        double worst = 0, extent = 0;
        for (size_t k = 0; k < flatToGlb_.size(); ++k) {
            const uint32_t g = flatToGlb_[k];
            if (g == UINT32_MAX) continue;
            const double gx = glbPos_[g * 3], gy = glbPos_[g * 3 + 1], gz = glbPos_[g * 3 + 2];
            const double fx = A[0] * gx + A[1] * gy + A[2] * gz + t[0], fy = A[3] * gx + A[4] * gy + A[5] * gz + t[1], fz = A[6] * gx + A[7] * gy + A[8] * gz + t[2];
            worst = std::max(worst, std::fabs(fx - flatRest_[k].px) + std::fabs(fy - flatRest_[k].py) + std::fabs(fz - flatRest_[k].pz));
            extent = std::max({extent, std::fabs(fx - mf[0]), std::fabs(fy - mf[1]), std::fabs(fz - mf[2])});
        }
        if (worst > std::max(1e-5, extent * 1e-4)) { status = "the engine's head does not line up with the GLB's vertex order (off by " + std::to_string(worst) + ")"; return false; }
        const double s = std::cbrt(std::fabs(A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6])));
        for (int i = 0; i < 9; ++i) { A_[i] = (float)A[i]; R_[i] = (float)(A[i] / std::max(s, 1e-12)); }
        return true;
    }

    void BuildWeld() {
        const size_t N = glbPos_.size() / 3;
        struct KeyHash { size_t operator()(const std::array<int64_t, 3>& k) const { return (size_t)(k[0] * 73856093 ^ k[1] * 19349663 ^ k[2] * 83492791); } };
        std::unordered_map<std::array<int64_t, 3>, uint32_t, KeyHash> ids;
        weld_.resize(N);
        for (size_t i = 0; i < N; ++i) {
            const std::array<int64_t, 3> k = {(int64_t)std::nearbyint((double)glbPos_[i * 3] / 1e-6), (int64_t)std::nearbyint((double)glbPos_[i * 3 + 1] / 1e-6),
                                              (int64_t)std::nearbyint((double)glbPos_[i * 3 + 2] / 1e-6)};
            weld_[i] = ids.emplace(k, (uint32_t)ids.size()).first->second;
        }
        weldCount_ = (uint32_t)ids.size();
        moved_.resize(glbPos_.size());
        WeldNormals(glbPos_, n0_);
    }

    void WeldNormals(const std::vector<float>& P, std::vector<float>& out) {
        acc_.assign((size_t)weldCount_ * 3, 0.0f);
        for (size_t t = 0; t + 2 < skinTris_.size(); t += 3) {
            const float* a = &P[(size_t)skinTris_[t] * 3];
            const float* b = &P[(size_t)skinTris_[t + 1] * 3];
            const float* c = &P[(size_t)skinTris_[t + 2] * 3];
            const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
            const float n[3] = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
            for (int k = 0; k < 3; ++k) {
                float* o = &acc_[(size_t)weld_[skinTris_[t + (size_t)k]] * 3];
                o[0] += n[0]; o[1] += n[1]; o[2] += n[2];
            }
        }
        out.resize(glbPos_.size());
        for (size_t i = 0; i < weld_.size(); ++i) {
            const float* a = &acc_[(size_t)weld_[i] * 3];
            const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            const float inv = l > 1e-20f ? 1.0f / l : 0.0f;
            out[i * 3] = a[0] * inv; out[i * 3 + 1] = a[1] * inv; out[i * 3 + 2] = a[2] * inv;
        }
    }

    // Each hair card vertex rides the skin vertex nearest it at rest (grid search, like FaceRig::BindHair).
    void BindHair(const ImportedModel& model) {
        hair_.clear();
        std::vector<std::pair<size_t, size_t>> ranges;
        for (const ImportedPart& p : model.parts)
            if (p.hair && p.first + p.count <= flatRest_.size()) ranges.push_back({p.first, p.count});
        if (ranges.empty()) return;
        std::vector<uint8_t> isSkin(glbPos_.size() / 3, 0);
        for (uint32_t i : skinTris_) isSkin[i] = 1;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t k = 0; k < flatToGlb_.size(); ++k) {
            const Vertex& v = flatRest_[k];
            lo[0] = std::min(lo[0], v.px); lo[1] = std::min(lo[1], v.py); lo[2] = std::min(lo[2], v.pz);
            hi[0] = std::max(hi[0], v.px); hi[1] = std::max(hi[1], v.py); hi[2] = std::max(hi[2], v.pz);
        }
        const float cell = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-4f}) / 48.0f;
        const auto key = [cell](float x, float y, float z) {
            const auto q = [cell](float a) { return (uint64_t)(int64_t)std::floor(a / cell) & 0x1FFFFF; };
            return (q(x) << 42) | (q(y) << 21) | q(z);
        };
        std::unordered_map<uint64_t, std::vector<uint32_t>> grid; // flat skin vertices
        std::vector<uint8_t> seen(glbPos_.size() / 3, 0);
        for (size_t k = 0; k < flatToGlb_.size(); ++k) {
            const uint32_t g = flatToGlb_[k];
            if (g == UINT32_MAX || !isSkin[g] || seen[g]) continue;
            seen[g] = 1;
            grid[key(flatRest_[k].px, flatRest_[k].py, flatRest_[k].pz)].push_back((uint32_t)k);
        }
        for (const auto& [first, count] : ranges)
            for (size_t i = first; i < first + count; ++i) {
                const Vertex& h = flatRest_[i];
                uint32_t best = UINT32_MAX;
                float bestD = 1e30f;
                for (int reach = 1; reach <= 6 && best == UINT32_MAX; ++reach)
                    for (int dx = -reach; dx <= reach; ++dx)
                        for (int dy = -reach; dy <= reach; ++dy)
                            for (int dz = -reach; dz <= reach; ++dz) {
                                const auto it = grid.find(key(h.px + (float)dx * cell, h.py + (float)dy * cell, h.pz + (float)dz * cell));
                                if (it == grid.end()) continue;
                                for (const uint32_t s : it->second) {
                                    const Vertex& b = flatRest_[s];
                                    const float d = (b.px - h.px) * (b.px - h.px) + (b.py - h.py) * (b.py - h.py) + (b.pz - h.pz) * (b.pz - h.pz);
                                    if (d < bestD) { bestD = d; best = s; }
                                }
                            }
                if (best != UINT32_MAX) hair_.push_back({(uint32_t)i, flatToGlb_[best]});
            }
    }
};

} // namespace tbd::character
