// Clothes Maker: the button-up shirt generator. See clothes_maker.h for the pipeline.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "clothes_maker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../mesh_import.h"
#include "hair_paint.h" // WritePngRgba

namespace tbd::character::clothes {
namespace {

// ---------------------------------------------------------------- math
struct V3 { float x = 0, y = 0, z = 0; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3& operator+=(V3& a, V3 b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(V3 a, V3 fallback = {0, 1, 0}) { const float l = Len(a); return l > 1e-12f ? a * (1.0f / l) : fallback; }
inline float Clamp01(float t) { return std::clamp(t, 0.0f, 1.0f); }
inline float Smooth(float e0, float e1, float x) { const float t = Clamp01((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
constexpr float kPi = 3.14159265f;

inline uint32_t Hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
inline float Rand(int x, int y, uint32_t seed) { return (float)(Hash((uint32_t)x * 73856093u ^ Hash((uint32_t)y * 19349663u ^ seed * 83492791u)) & 0xFFFFFF) / 16777215.0f; }
inline float Noise(float x, float y, uint32_t seed) { // value noise 0..1
    const float flx = std::floor(x), fly = std::floor(y);
    const int ix = (int)flx, iy = (int)fly;
    const float fx = x - flx, fy = y - fly, sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float a = Rand(ix, iy, seed), b = Rand(ix + 1, iy, seed), c = Rand(ix, iy + 1, seed), d = Rand(ix + 1, iy + 1, seed);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}
inline float Fbm(float x, float y, uint32_t seed, int oct = 3) {
    float s = 0, a = 0.5f, w = 0;
    for (int k = 0; k < oct; ++k) { s += a * Noise(x, y, seed + (uint32_t)k * 101u); w += a; x *= 2.03f; y *= 2.03f; a *= 0.5f; }
    return s / w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------- body
struct Body {
    std::vector<Vertex> flat;          // as imported (the skin part is exported from this)
    std::vector<VertexSkin> flatSkin;
    std::vector<uint32_t> weld;        // flat index -> welded index
    std::vector<V3> p, n;              // welded
    std::vector<VertexSkin> skin;
    std::vector<uint32_t> tri;         // welded triangles
    ImportedModel model;
    int skinPart = -1;
    V3 shoulder[2], elbow[2], hand[2], dir[2]; // side 0 = +x (the _l joints), 1 = -x
    float armLen[2] = {0.5f, 0.5f};
    V3 neck, clav;
    float pelvisY = 0, minY = 0, maxY = 0;
};

bool JointPos(const ImportedModel& m, const char* name, V3& out) {
    for (const SkinJoint& j : m.joints) {
        std::string low = j.name;
        for (char& c : low) c = (char)std::tolower((unsigned char)c);
        if (low != name) continue;
        const float* ib = j.inverseBind; // column-major; the joint sits where ib * p = 0
        const float M[3][3] = {{ib[0], ib[4], ib[8]}, {ib[1], ib[5], ib[9]}, {ib[2], ib[6], ib[10]}};
        const float t[3] = {-ib[12], -ib[13], -ib[14]};
        const float det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
        if (std::fabs(det) < 1e-12f) return false;
        const auto col = [&](int c, float r[3][3]) { for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) r[i][k] = k == c ? t[i] : M[i][k]; };
        float R[3][3];
        float x[3];
        for (int c = 0; c < 3; ++c) {
            col(c, R);
            x[c] = (R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) + R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0])) / det;
        }
        out = {x[0], x[1], x[2]};
        return true;
    }
    return false;
}

bool LoadBody(const std::string& path, Body& b, std::string& err) {
    ImportOptions o;
    o.normalizeToUnitBox = false;
    if (!ImportMesh(path, b.flat, &err, o, &b.model)) { err = "body: " + err; return false; }
    b.flatSkin = b.model.vertexSkin;
    b.flatSkin.resize(b.flat.size());
    for (size_t k = 0; k < b.model.parts.size(); ++k) if (b.model.parts[k].skin) { b.skinPart = (int)k; break; }
    std::unordered_map<uint64_t, uint32_t> map;
    map.reserve(b.flat.size());
    b.weld.resize(b.flat.size());
    b.minY = 1e9f; b.maxY = -1e9f;
    for (size_t i = 0; i < b.flat.size(); ++i) {
        const Vertex& v = b.flat[i];
        const auto q = [](float f) { return (uint64_t)(uint32_t)(int32_t)std::lround(f * 20000.0f) & 0x1FFFFF; };
        const uint64_t key = q(v.px) | (q(v.py) << 21) | (q(v.pz) << 42);
        auto [it, fresh] = map.emplace(key, (uint32_t)b.p.size());
        if (fresh) { b.p.push_back({v.px, v.py, v.pz}); b.n.push_back({}); b.skin.push_back(b.flatSkin[i]); }
        b.n[it->second] += V3{v.nx, v.ny, v.nz};
        b.weld[i] = it->second;
        b.minY = std::min(b.minY, v.py); b.maxY = std::max(b.maxY, v.py);
    }
    for (V3& n : b.n) n = Norm(n);
    for (size_t t = 0; t + 2 < b.flat.size(); t += 3) {
        const uint32_t a = b.weld[t], c = b.weld[t + 1], d = b.weld[t + 2];
        if (a == c || c == d || a == d) continue;
        b.tri.insert(b.tri.end(), {a, c, d});
    }
    V3 pelvis;
    const bool ok = JointPos(b.model, "upperarm_l", b.shoulder[0]) && JointPos(b.model, "upperarm_r", b.shoulder[1]) &&
                    JointPos(b.model, "lowerarm_l", b.elbow[0]) && JointPos(b.model, "lowerarm_r", b.elbow[1]) &&
                    JointPos(b.model, "hand_l", b.hand[0]) && JointPos(b.model, "hand_r", b.hand[1]) &&
                    JointPos(b.model, "neck_01", b.neck) && JointPos(b.model, "pelvis", pelvis);
    if (!ok) { err = "body: no upperarm/lowerarm/hand/neck_01/pelvis joints (needs the MakeHuman game_engine skeleton)"; return false; }
    if (!JointPos(b.model, "clavicle_l", b.clav)) b.clav = {0, b.shoulder[0].y + 0.03f, b.neck.z};
    b.pelvisY = pelvis.y;
    for (int s = 0; s < 2; ++s) { b.armLen[s] = std::max(0.1f, Len(b.hand[s] - b.shoulder[s])); b.dir[s] = Norm(b.hand[s] - b.shoulder[s]); }
    return true;
}

// Nearest welded body vertex (uniform grid, 3 cm cells).
struct PointGrid {
    float cell = 0.03f;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
    const std::vector<V3>* pts = nullptr;
    static uint64_t Key(int x, int y, int z) { return ((uint64_t)(uint32_t)(x + 100000) & 0x1FFFFF) | (((uint64_t)(uint32_t)(y + 100000) & 0x1FFFFF) << 21) | (((uint64_t)(uint32_t)(z + 100000) & 0x1FFFFF) << 42); }
    void Build(const std::vector<V3>& p, float c) {
        pts = &p; cell = c; cells.clear();
        for (uint32_t i = 0; i < p.size(); ++i) cells[Key((int)std::floor(p[i].x / cell), (int)std::floor(p[i].y / cell), (int)std::floor(p[i].z / cell))].push_back(i);
    }
    int Nearest(V3 q, int reach = 1) const {
        const int cx = (int)std::floor(q.x / cell), cy = (int)std::floor(q.y / cell), cz = (int)std::floor(q.z / cell);
        int best = -1; float bd = 1e30f;
        for (int r = reach; best < 0 && r <= reach + 6; r += 2)
            for (int x = cx - r; x <= cx + r; ++x) for (int y = cy - r; y <= cy + r; ++y) for (int z = cz - r; z <= cz + r; ++z) {
                const auto it = cells.find(Key(x, y, z));
                if (it == cells.end()) continue;
                for (uint32_t i : it->second) { const V3 d = (*pts)[i] - q; const float dd = Dot(d, d); if (dd < bd) { bd = dd; best = (int)i; } }
            }
        return best;
    }
};

bool RayTri(V3 o, V3 d, V3 a, V3 b, V3 c, float& t) {
    const V3 e1 = b - a, e2 = c - a, pv = Cross(d, e2);
    const float det = Dot(e1, pv);
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const V3 tv = o - a;
    const float u = Dot(tv, pv) * inv;
    if (u < 0 || u > 1) return false;
    const V3 qv = Cross(tv, e1);
    const float v = Dot(d, qv) * inv;
    if (v < 0 || u + v > 1) return false;
    t = Dot(e2, qv) * inv;
    return t > 0;
}

// ---------------------------------------------------------------- jeans: outer radius per angle and height around the waist
struct Jeans {
    bool ok = false;
    static constexpr int A = 96;
    float cz = 0, y0 = 0, dy = 0.004f;
    int H = 0;
    std::vector<float> r;         // H * A, -1 = no hit
    float top[A] = {}, rTop[A] = {};
    float R(float th, float y) const { // bilinear, -1 where unknown
        if (!ok) return -1;
        float fa = th / (2 * kPi) * A; fa -= std::floor(fa / A) * A;
        const float fh = (y - y0) / dy;
        if (fh < 0 || fh > H - 1) return -1;
        const int a0 = (int)fa % A, a1 = (a0 + 1) % A, h0 = std::min(H - 2, (int)fh);
        const float ta = fa - std::floor(fa), th2 = fh - h0;
        const float v00 = r[(size_t)h0 * A + a0], v01 = r[(size_t)h0 * A + a1], v10 = r[(size_t)(h0 + 1) * A + a0], v11 = r[(size_t)(h0 + 1) * A + a1];
        if (v00 < 0 || v01 < 0 || v10 < 0 || v11 < 0) { const float m = std::max({v00, v01, v10, v11}); return m; }
        return Lerp(Lerp(v00, v01, ta), Lerp(v10, v11, ta), th2);
    }
    float Top(float th) const { float fa = th / (2 * kPi) * A; fa -= std::floor(fa / A) * A; const int a0 = (int)fa % A, a1 = (a0 + 1) % A; return Lerp(top[a0], top[a1], fa - std::floor(fa)); }
    float RTop(float th) const { float fa = th / (2 * kPi) * A; fa -= std::floor(fa / A) * A; const int a0 = (int)fa % A, a1 = (a0 + 1) % A; return Lerp(rTop[a0], rTop[a1], fa - std::floor(fa)); }
};

bool LoadJeans(const std::string& path, Jeans& J, float cz, std::string& note) {
    std::vector<Vertex> v;
    ImportedModel m;
    ImportOptions o;
    o.normalizeToUnitBox = false;
    std::string err;
    if (path.empty() || !ImportMesh(path, v, &err, o, &m)) { note = path.empty() ? "no jeans in the scene" : "jeans: " + err; return false; }
    std::vector<V3> tris;
    float topY = -1e9f;
    for (const ImportedPart& p : m.parts) {
        if (p.skin) continue;
        for (size_t k = p.first; k + 2 < p.first + p.count; k += 3)
            for (int c = 0; c < 3; ++c) { tris.push_back({v[k + c].px, v[k + c].py, v[k + c].pz}); if (std::fabs(v[k + c].px) < 0.2f) topY = std::max(topY, v[k + c].py); }
    }
    if (tris.empty()) { note = "jeans: no cloth triangles"; return false; }
    J.cz = cz;
    J.y0 = topY - 0.18f;
    J.H = (int)((topY + 0.004f - J.y0) / J.dy) + 1;
    J.r.assign((size_t)J.H * Jeans::A, -1.0f);
    for (int h = 0; h < J.H; ++h) {
        const float y = J.y0 + h * J.dy;
        std::vector<uint32_t> band;
        for (uint32_t t = 0; t + 2 < tris.size(); t += 3) {
            const float lo = std::min({tris[t].y, tris[t + 1].y, tris[t + 2].y}), hi = std::max({tris[t].y, tris[t + 1].y, tris[t + 2].y});
            if (y >= lo && y <= hi) band.push_back(t);
        }
        for (int a = 0; a < Jeans::A; ++a) {
            const float th = a * 2 * kPi / Jeans::A;
            const V3 o3{0, y, cz}, d{std::sin(th), 0, std::cos(th)};
            float best = -1;
            for (uint32_t t : band) { float tt; if (RayTri(o3, d, tris[t], tris[t + 1], tris[t + 2], tt)) best = std::max(best, tt); }
            J.r[(size_t)h * Jeans::A + a] = best;
        }
    }
    for (int a = 0; a < Jeans::A; ++a) {
        J.top[a] = J.y0; J.rTop[a] = -1;
        for (int h = J.H - 1; h >= 0; --h) if (J.r[(size_t)h * Jeans::A + a] > 0) { J.top[a] = J.y0 + h * J.dy; J.rTop[a] = J.r[(size_t)h * Jeans::A + a]; break; }
    }
    // smooth the waistband line (belt loops stick up)
    float t2[Jeans::A];
    for (int it = 0; it < 3; ++it) {
        for (int a = 0; a < Jeans::A; ++a) t2[a] = std::min(J.top[a], 0.5f * (J.top[(a + 1) % Jeans::A] + J.top[(a + Jeans::A - 1) % Jeans::A]) + 0.002f);
        std::copy(t2, t2 + Jeans::A, J.top);
    }
    for (int a = 0; a < Jeans::A; ++a) {
        const int h = std::clamp((int)((J.top[a] - J.y0) / J.dy), 0, J.H - 1);
        for (int k = h; k >= 0; --k) if (J.r[(size_t)k * Jeans::A + a] > 0) { J.rTop[a] = J.r[(size_t)k * Jeans::A + a]; break; }
    }
    J.ok = true;
    note = "tucked into " + path.substr(path.find_last_of("\\/") + 1);
    return true;
}

// ---------------------------------------------------------------- the cut: which body points the shirt covers
enum Reason { R_None = 0, R_Sleeve, R_Hem, R_Neck, R_V };
struct Cut {
    float hemY = 0, sleeve = 0.6f;
    float neckBack = 0, neckFront = 0, neckZ = 0;
    bool v = false;
    float vBottom = 0, vTopY = 0, vTopHalf = 0;
    float NeckY(float x, float z) const {
        const float dz = z - neckZ, rh = std::sqrt(x * x + dz * dz);
        const float t = 0.5f + 0.5f * std::cos(std::atan2(x, dz));
        return neckBack - (neckBack - neckFront) * std::pow(t, 1.5f) + std::max(0.0f, rh - 0.065f) * 0.6f;
    }
    float VHalf(float y) const { return vTopHalf * Clamp01((y - vBottom) / std::max(0.001f, vTopY - vBottom)); }
};

float ArmA(const Body& b, V3 p, int* sideOut) {
    const int s = p.x >= 0 ? 0 : 1;
    if (sideOut) *sideOut = s;
    return Dot(p - b.shoulder[s], b.dir[s]) / b.armLen[s];
}

int Classify(const Body& b, const Cut& c, V3 p) {
    const float a = ArmA(b, p, nullptr);
    if (a > c.sleeve) return R_Sleeve;
    if (p.y < c.hemY) return R_Hem;
    if (a < 0.05f) {
        if (c.v && p.z > c.neckZ && p.y > c.vBottom && std::fabs(p.x) < c.VHalf(p.y)) return R_V;
        const float dz = p.z - c.neckZ;
        if (std::sqrt(p.x * p.x + dz * dz) < 0.10f && p.y > c.NeckY(p.x, p.z)) return R_Neck;
    }
    return R_None;
}

// ---------------------------------------------------------------- the shirt body mesh
struct Cloth {
    std::vector<V3> p0, n0, p, n;
    std::vector<VertexSkin> skin;
    std::vector<float> ease, ao;
    std::vector<int> reason; // boundary vertices: which cut made the edge (R_None inside)
    std::vector<uint32_t> tri;
    std::vector<std::vector<uint32_t>> nb;
    uint32_t Add(V3 p0v, V3 n0v, const VertexSkin& s) { p0.push_back(p0v); n0.push_back(n0v); skin.push_back(s); return (uint32_t)p0.size() - 1; }
};

// Half and half of two skin bindings: the four strongest joints, renormalised (subdivided points between two body vertices).
VertexSkin MixSkin(const VertexSkin& a, const VertexSkin& b) {
    uint16_t j[8];
    float w[8];
    int n = 0;
    for (const VertexSkin* s : {&a, &b})
        for (int c = 0; c < 4; ++c) {
            if (s->weights[c] <= 0) continue;
            int k = 0;
            while (k < n && j[k] != s->joints[c]) ++k;
            if (k == n) { j[n] = s->joints[c]; w[n++] = 0; }
            w[k] += 0.5f * s->weights[c];
        }
    VertexSkin o;
    float sum = 0;
    for (int c = 0; c < 4 && n > 0; ++c) {
        int best = 0;
        for (int k = 1; k < n; ++k) if (w[k] > w[best]) best = k;
        if (w[best] <= 0) break;
        o.joints[c] = j[best]; o.weights[c] = w[best]; sum += w[best];
        w[best] = -1;
    }
    if (sum > 0) for (float& x : o.weights) x /= sum;
    else o = a;
    return o;
}

// Red-green subdivision of the triangles `mark` says (their edges split; neighbours split to match, no cracks).
void Subdivide(Cloth& c, const std::function<bool(V3)>& mark) {
    std::unordered_map<uint64_t, uint32_t> mid;
    const auto key = [](uint32_t a, uint32_t b) { return a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a; };
    const size_t nt = c.tri.size() / 3;
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* v = &c.tri[t * 3];
        const V3 cen = (c.p0[v[0]] + c.p0[v[1]] + c.p0[v[2]]) * (1.0f / 3.0f);
        if (!mark(cen)) continue;
        for (int e = 0; e < 3; ++e) mid.emplace(key(v[e], v[(e + 1) % 3]), UINT32_MAX);
    }
    for (auto& [k, m] : mid) {
        const uint32_t a = (uint32_t)(k >> 32), b = (uint32_t)k;
        m = c.Add((c.p0[a] + c.p0[b]) * 0.5f, Norm(c.n0[a] + c.n0[b]), MixSkin(c.skin[a], c.skin[b]));
    }
    std::vector<uint32_t> out;
    out.reserve(c.tri.size() * 3);
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t v[3] = {c.tri[t * 3], c.tri[t * 3 + 1], c.tri[t * 3 + 2]};
        uint32_t m[3];
        int split = 0;
        for (int e = 0; e < 3; ++e) { const auto it = mid.find(key(v[e], v[(e + 1) % 3])); m[e] = it == mid.end() ? UINT32_MAX : it->second; split += m[e] != UINT32_MAX; }
        if (split == 0) { out.insert(out.end(), {v[0], v[1], v[2]}); continue; }
        if (split == 3) { out.insert(out.end(), {v[0], m[0], m[2], m[0], v[1], m[1], m[2], m[1], v[2], m[0], m[1], m[2]}); continue; }
        int e0 = 0;
        if (split == 1) { while (m[e0] == UINT32_MAX) ++e0; } // the split edge
        else { while (m[e0] != UINT32_MAX) ++e0; }           // the edge that is NOT split
        const uint32_t A = v[e0], B = v[(e0 + 1) % 3], C = v[(e0 + 2) % 3];
        if (split == 1) { const uint32_t M = m[e0]; out.insert(out.end(), {A, M, C, M, B, C}); continue; }
        const uint32_t MBC = m[(e0 + 1) % 3], MCA = m[(e0 + 2) % 3]; // edges B-C and C-A split, A-B whole
        out.insert(out.end(), {MCA, MBC, C, A, B, MBC, A, MBC, MCA});
    }
    c.tri.swap(out);
}

void Neighbours(Cloth& c) {
    c.nb.assign(c.p0.size(), {});
    for (size_t t = 0; t + 2 < c.tri.size(); t += 3)
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = c.tri[t + e], b = c.tri[t + (e + 1) % 3];
            if (std::find(c.nb[a].begin(), c.nb[a].end(), b) == c.nb[a].end()) c.nb[a].push_back(b);
            if (std::find(c.nb[b].begin(), c.nb[b].end(), a) == c.nb[b].end()) c.nb[b].push_back(a);
        }
}

void SmoothNormals(const std::vector<V3>& p, const std::vector<uint32_t>& tri, std::vector<V3>& n) {
    n.assign(p.size(), {});
    for (size_t t = 0; t + 2 < tri.size(); t += 3) {
        const V3 f = Cross(p[tri[t + 1]] - p[tri[t]], p[tri[t + 2]] - p[tri[t]]);
        for (int k = 0; k < 3; ++k) n[tri[t + k]] += f;
    }
    for (V3& x : n) x = Norm(x);
}

// ---------------------------------------------------------------- output pieces
enum ChartKind { C_Front, C_Back, C_SideL, C_SideR, C_Yoke, C_SleeveL, C_SleeveR, C_CuffL, C_CuffR, C_Collar, C_Placket, C_PocketL, C_PocketR, C_FlapL, C_FlapR, C_Count };
const char* kChartNames[C_Count] = {"front", "back", "side L", "side R", "yoke", "sleeve L", "sleeve R", "cuff L", "cuff R", "collar", "placket", "pocket L", "pocket R", "flap L", "flap R"};

struct OutVert { V3 p, n; float u = 0, v = 0; float ao = 1; VertexSkin skin; int chart = 0; };
struct Mesh { std::vector<OutVert> v; }; // unindexed triangles

struct Chart { float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f; int ox = 0, oy = 0, w = 0, h = 0; bool used = false; };

// A grid sheet (I x J points) emitted as triangles; normals from the grid, turned toward ref(i, j); winding follows the normals.
struct Grid {
    int I = 0, J = 0;
    bool wrapI = false, wrapJ = false;
    std::vector<V3> P, ref;
    std::vector<float> U, V, ao;
    V3& At(int i, int j) { return P[(size_t)i * J + j]; }
    void Resize(int i, int j) { I = i; J = j; P.assign((size_t)i * j, {}); ref.assign(P.size(), {0, 0, 1}); U.assign(P.size(), 0); V.assign(P.size(), 0); ao.assign(P.size(), 1.0f); }
    V3 NormalAt(int i, int j) const {
        const auto idx = [&](int a, int b) {
            if (wrapI) a = (a + I) % I; else a = std::clamp(a, 0, I - 1);
            if (wrapJ) b = (b + J) % J; else b = std::clamp(b, 0, J - 1);
            return (size_t)a * J + b;
        };
        const V3 di = P[idx(i + 1, j)] - P[idx(i - 1, j)], dj = P[idx(i, j + 1)] - P[idx(i, j - 1)];
        V3 n = Norm(Cross(di, dj), ref[(size_t)i * J + j]);
        if (Dot(n, ref[(size_t)i * J + j]) < 0) n = n * -1.0f;
        return n;
    }
};

void Emit(Mesh& m, const Grid& g, int chart, const std::function<VertexSkin(V3)>& skinOf, V3 offset = {}, bool flip = false, float offsetAlongN = 0) {
    const int ni = g.wrapI ? g.I : g.I - 1, nj = g.wrapJ ? g.J : g.J - 1;
    std::vector<V3> N(g.P.size());
    for (int i = 0; i < g.I; ++i) for (int j = 0; j < g.J; ++j) N[(size_t)i * g.J + j] = g.NormalAt(i, j) * (flip ? -1.0f : 1.0f);
    const auto vert = [&](int i, int j, bool wrapEndI, bool wrapEndJ) {
        const size_t k = (size_t)(i % g.I) * g.J + (j % g.J);
        OutVert o;
        o.n = N[k];
        o.p = g.P[k] + offset + o.n * offsetAlongN;
        o.u = g.U[k]; o.v = g.V[k];
        if (wrapEndI) o.u = g.U[(size_t)(g.I - 1) * g.J + (j % g.J)] + (g.U[(size_t)(g.I - 1) * g.J + (j % g.J)] - g.U[(size_t)(g.I - 2) * g.J + (j % g.J)]);
        if (wrapEndJ) o.v = g.V[(size_t)(i % g.I) * g.J + g.J - 1] + (g.V[(size_t)(i % g.I) * g.J + g.J - 1] - g.V[(size_t)(i % g.I) * g.J + g.J - 2]);
        o.ao = g.ao[k];
        o.skin = skinOf(o.p);
        o.chart = chart;
        return o;
    };
    for (int i = 0; i < ni; ++i)
        for (int j = 0; j < nj; ++j) {
            const bool ei = i + 1 == g.I, ej = j + 1 == g.J;
            const OutVert a = vert(i, j, false, false), b = vert(i + 1, j, ei, false), c = vert(i + 1, j + 1, ei, ej), d = vert(i, j + 1, false, ej);
            for (const auto& t : {std::array<const OutVert*, 3>{&a, &b, &c}, std::array<const OutVert*, 3>{&a, &c, &d}}) {
                const V3 f = Cross(t[1]->p - t[0]->p, t[2]->p - t[0]->p);
                const V3 avg = t[0]->n + t[1]->n + t[2]->n;
                if (Dot(f, avg) >= 0) { m.v.push_back(*t[0]); m.v.push_back(*t[1]); m.v.push_back(*t[2]); }
                else { m.v.push_back(*t[0]); m.v.push_back(*t[2]); m.v.push_back(*t[1]); }
            }
        }
}

// ---------------------------------------------------------------- front ray casts onto the shirt (placket, pockets, buttons)
struct FrontCaster {
    const Cloth* c = nullptr;
    float cell = 0.01f, x0 = -1, y0 = -1;
    int nx = 0, ny = 0;
    std::vector<std::vector<uint32_t>> bins;
    void Build(const Cloth& cl) {
        c = &cl;
        float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
        for (const V3& p : cl.p) { xmin = std::min(xmin, p.x); xmax = std::max(xmax, p.x); ymin = std::min(ymin, p.y); ymax = std::max(ymax, p.y); }
        x0 = xmin; y0 = ymin; nx = (int)((xmax - xmin) / cell) + 1; ny = (int)((ymax - ymin) / cell) + 1;
        bins.assign((size_t)nx * ny, {});
        for (uint32_t t = 0; t + 2 < cl.tri.size(); t += 3) {
            const V3 a = cl.p[cl.tri[t]], b = cl.p[cl.tri[t + 1]], d = cl.p[cl.tri[t + 2]];
            if (Cross(b - a, d - a).z <= 0) continue; // front-facing only
            const int ix0 = (int)((std::min({a.x, b.x, d.x}) - x0) / cell), ix1 = (int)((std::max({a.x, b.x, d.x}) - x0) / cell);
            const int iy0 = (int)((std::min({a.y, b.y, d.y}) - y0) / cell), iy1 = (int)((std::max({a.y, b.y, d.y}) - y0) / cell);
            for (int y = std::max(0, iy0); y <= std::min(ny - 1, iy1); ++y) for (int x = std::max(0, ix0); x <= std::min(nx - 1, ix1); ++x) bins[(size_t)y * nx + x].push_back(t);
        }
    }
    bool Hit(float x, float y, V3& p, V3& n) const {
        const int ix = (int)((x - x0) / cell), iy = (int)((y - y0) / cell);
        if (ix < 0 || iy < 0 || ix >= nx || iy >= ny) return false;
        float bestZ = -1e9f;
        bool any = false;
        for (uint32_t t : bins[(size_t)iy * nx + ix]) {
            const V3 a = c->p[c->tri[t]], b = c->p[c->tri[t + 1]], d = c->p[c->tri[t + 2]];
            const float den = (b.y - d.y) * (a.x - d.x) + (d.x - b.x) * (a.y - d.y);
            if (std::fabs(den) < 1e-14f) continue;
            const float w0 = ((b.y - d.y) * (x - d.x) + (d.x - b.x) * (y - d.y)) / den, w1 = ((d.y - a.y) * (x - d.x) + (a.x - d.x) * (y - d.y)) / den, w2 = 1 - w0 - w1;
            if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
            const float z = a.z * w0 + b.z * w1 + d.z * w2;
            if (z <= bestZ) continue;
            bestZ = z; any = true;
            p = {x, y, z};
            n = Norm(c->n[c->tri[t]] * w0 + c->n[c->tri[t + 1]] * w1 + c->n[c->tri[t + 2]] * w2, {0, 0, 1});
        }
        return any;
    }
};

// ---------------------------------------------------------------- GLB writer
struct Glb {
    std::vector<uint8_t> bin;
    std::vector<std::string> views, accessors;
    int View(const void* data, size_t n, int target) {
        while (bin.size() % 4) bin.push_back(0);
        const size_t off = bin.size();
        bin.insert(bin.end(), (const uint8_t*)data, (const uint8_t*)data + n);
        char b[160];
        if (target) snprintf(b, sizeof(b), "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":%d}", off, n, target);
        else snprintf(b, sizeof(b), "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", off, n);
        views.push_back(b);
        return (int)views.size() - 1;
    }
    int Acc(const void* data, size_t count, int ctype, const char* type, int comps, int target, const std::string& extra = "") {
        const size_t csize = ctype == 5126 || ctype == 5125 ? 4 : ctype == 5123 ? 2 : 1;
        const int v = View(data, count * comps * csize, target);
        char b[256];
        snprintf(b, sizeof(b), "{\"bufferView\":%d,\"componentType\":%d,\"count\":%zu,\"type\":\"%s\"", v, ctype, count, type);
        accessors.push_back(std::string(b) + extra + "}");
        return (int)accessors.size() - 1;
    }
};

std::string F(float x) { char b[32]; snprintf(b, sizeof(b), "%.6g", std::isfinite(x) ? x : 0.0f); return b; }

// ---------------------------------------------------------------- texture
struct Atlas {
    int size = 2048, pad = 6;
    float ppm = 1000; // pixels per metre
    Chart charts[C_Count];
};

bool Pack(Atlas& at) {
    for (float ppm = 1600; ppm >= 300; ppm -= 50) {
        std::vector<int> order;
        for (int c = 0; c < C_Count; ++c) if (at.charts[c].used) order.push_back(c);
        for (int c : order) { Chart& ch = at.charts[c]; ch.w = (int)std::ceil((ch.u1 - ch.u0) * ppm) + 2 * at.pad; ch.h = (int)std::ceil((ch.v1 - ch.v0) * ppm) + 2 * at.pad; }
        std::sort(order.begin(), order.end(), [&](int a, int b) { return at.charts[a].h > at.charts[b].h; });
        int x = 0, y = 0, rowH = 0;
        bool fits = true;
        for (int c : order) {
            Chart& ch = at.charts[c];
            if (ch.w > at.size) { fits = false; break; }
            if (x + ch.w > at.size) { x = 0; y += rowH; rowH = 0; }
            if (y + ch.h > at.size) { fits = false; break; }
            ch.ox = x; ch.oy = y; x += ch.w; rowH = std::max(rowH, ch.h);
        }
        if (fits) { at.ppm = ppm; return true; }
    }
    return false;
}

struct TexCtx {
    const ShirtRecipe* r;
    const Body* b;
    float sleeveEnd[2];    // metres from the shoulder
    float waistY;
};

void Fabric(const TexCtx& tc, int chart, float u, float v, float px, float out[3], float& height) {
    const ShirtRecipe& r = *tc.r;
    // stripes across u: a light pinstripe and a dark one half a repeat away
    const float sp = std::max(0.002f, r.stripeSpacing);
    const float f = u / sp - std::floor(u / sp);
    const auto line = [&](float centre, float width) {
        float d = std::fabs(f - centre); d = std::min(d, 1 - d); d *= sp;
        return Clamp01((width * 0.5f - d) / std::max(px, 1e-5f) + 0.5f);
    };
    const float la = line(0.0f, r.stripeWidth), lb = line(0.5f, r.stripeWidth * 0.7f);
    float c[3];
    for (int k = 0; k < 3; ++k) c[k] = Lerp(Lerp(r.base[k], r.stripeA[k], la * 0.55f), r.stripeB[k], lb * 0.45f);
    // chambray: blue warp along v with white weft; slubs are streaks along the thread direction
    const uint32_t sd = r.seed * 7919u + (uint32_t)chart * 131u;
    const float slub = Fbm(u * 1400.0f, v * 90.0f, sd, 2) - 0.5f;
    const float grain = Noise(u * 3000.0f, v * 3000.0f, sd + 3) - 0.5f;
    const float mottle = Fbm(u * 9.0f, v * 9.0f, sd + 5, 3) - 0.5f;
    // twill: fine diagonal ribs, the white weft showing on one side of each rib
    const float twill = 0.5f + 0.5f * std::sin((u * 0.7f + v) / 0.00075f * 2 * kPi);
    const float light = 1.0f + slub * 0.20f + grain * 0.10f + (twill - 0.5f) * 0.10f + mottle * 0.12f * (0.5f + r.wear);
    for (int k = 0; k < 3; ++k) c[k] *= light;
    // washed-out fade toward grey-white
    const float fade = r.wear * (0.10f + 0.12f * Fbm(u * 4.0f + 3.1f, v * 4.0f, sd + 9, 3));
    const float grey = (c[0] + c[1] + c[2]) / 3.0f;
    for (int k = 0; k < 3; ++k) c[k] = Lerp(c[k], grey * 1.15f + 0.05f, fade);
    // dirt and sweat by where the texel is on the garment
    float dirt = 0, sweat = 0;
    const float dustN = Fbm(u * 6.0f + 11.0f, v * 6.0f, sd + 21, 4);
    if (chart <= C_SideR) { // torso charts: v = -y
        const float y = -v;
        dirt += 0.45f * Smooth(tc.waistY + 0.12f, tc.waistY - 0.01f, y) * dustN;
        if (chart == C_SideL || chart == C_SideR) sweat += std::exp(-((y - (tc.b->shoulder[0].y - 0.10f)) * (y - (tc.b->shoulder[0].y - 0.10f)) / 0.003f + u * u / 0.004f));
    } else if (chart == C_Yoke) {
        dirt += 0.25f * dustN;
    } else if (chart == C_SleeveL || chart == C_SleeveR) {
        const float end = tc.sleeveEnd[chart == C_SleeveR];
        dirt += Smooth(end - 0.16f, end, v) * (0.4f + 0.6f * dustN);
        const float el = (v - (tc.b->elbow[0].x - tc.b->shoulder[0].x)) / 0.05f;
        dirt += 0.5f * std::exp(-el * el) * dustN;
    } else if (chart == C_CuffL || chart == C_CuffR) {
        dirt += 0.55f + 0.45f * dustN;
    } else if (chart == C_Collar) {
        sweat += std::exp(-(v - r.collarStand) * (v - r.collarStand) / 0.0002f) * 0.8f + Smooth(0.01f, 0.0f, v) * 0.6f;
        dirt += 0.3f * dustN;
    } else {
        dirt += 0.2f * dustN;
    }
    dirt = Clamp01(dirt * r.dirt * (0.6f + 0.8f * Fbm(u * 25.0f, v * 25.0f, sd + 33, 3)));
    sweat = Clamp01(sweat * r.wear * (0.5f + 0.7f * Fbm(u * 14.0f, v * 14.0f, sd + 41, 3)));
    const float dirtCol[3] = {0.62f, 0.56f, 0.48f}, sweatCol[3] = {0.90f, 0.85f, 0.70f}; // dust greys the blue; sweat only yellows it a little
    for (int k = 0; k < 3; ++k) {
        c[k] = Lerp(c[k], Lerp(c[k], dirtCol[k] * 0.55f, 0.5f), dirt * 0.5f);
        c[k] = Lerp(c[k], c[k] * sweatCol[k], sweat * 0.25f);
        out[k] = Clamp01(c[k]);
    }
    // height for the normal map: weave (twill diagonal), slubs, the stripes slightly raised
    height = twill * 0.9f + slub * 1.1f + grain * 0.6f + (la + lb) * 0.15f;
}

inline uint8_t ToSrgb8(float lin) { return (uint8_t)std::lround(std::pow(Clamp01(lin), 1.0f / 2.2f) * 255.0f); }

void MakeTextures(const Atlas& at, const TexCtx& tc, std::vector<uint8_t>& albedo, std::vector<uint8_t>& normal) {
    const int S = at.size;
    albedo.assign((size_t)S * S * 4, 0);
    normal.assign((size_t)S * S * 4, 0);
    std::vector<float> height((size_t)S * S, 0.0f);
    for (size_t k = 0; k < (size_t)S * S; ++k) { albedo[k * 4] = 90; albedo[k * 4 + 1] = 110; albedo[k * 4 + 2] = 150; albedo[k * 4 + 3] = 255; normal[k * 4] = 128; normal[k * 4 + 1] = 128; normal[k * 4 + 2] = 255; normal[k * 4 + 3] = 255; }
    std::vector<int> owner((size_t)S * S, -1);
    for (int c = 0; c < C_Count; ++c) {
        const Chart& ch = at.charts[c];
        if (!ch.used) continue;
        for (int y = ch.oy; y < std::min(S, ch.oy + ch.h); ++y) for (int x = ch.ox; x < std::min(S, ch.ox + ch.w); ++x) owner[(size_t)y * S + x] = c;
    }
    const float px = 1.0f / at.ppm;
    const unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::atomic<int> nextRow{0};
    const auto work = [&] {
        for (int y; (y = nextRow++) < S;)
            for (int x = 0; x < S; ++x) {
                const int c = owner[(size_t)y * S + x];
                if (c < 0) continue;
                const Chart& ch = at.charts[c];
                const float u = ch.u0 + (x - ch.ox - at.pad + 0.5f) * px, v = ch.v0 + (y - ch.oy - at.pad + 0.5f) * px;
                float col[3], h;
                Fabric(tc, c, u, v, px, col, h);
                const size_t k = (size_t)y * S + x;
                albedo[k * 4] = ToSrgb8(col[0]); albedo[k * 4 + 1] = ToSrgb8(col[1]); albedo[k * 4 + 2] = ToSrgb8(col[2]);
                height[k] = h;
            }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work);
    for (auto& t : pool) t.join();
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            const size_t k = (size_t)y * S + x;
            if (owner[k] < 0) continue;
            const float hx = height[(size_t)y * S + std::min(S - 1, x + 1)] - height[(size_t)y * S + std::max(0, x - 1)];
            const float hy = height[(size_t)std::min(S - 1, y + 1) * S + x] - height[(size_t)std::max(0, y - 1) * S + x];
            const V3 n = Norm({-hx * 0.55f, -hy * 0.55f, 1.0f});
            normal[k * 4] = (uint8_t)std::lround((n.x * 0.5f + 0.5f) * 255); normal[k * 4 + 1] = (uint8_t)std::lround((n.y * 0.5f + 0.5f) * 255); normal[k * 4 + 2] = (uint8_t)std::lround((n.z * 0.5f + 0.5f) * 255);
        }
}

bool ReadBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)std::max(0L, n));
    const bool ok = n <= 0 || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

void MakeDirs(const std::wstring& dir) {
    for (size_t k = 3; k <= dir.size(); ++k)
        if (k == dir.size() || dir[k] == L'\\' || dir[k] == L'/') CreateDirectoryW(dir.substr(0, k).c_str(), nullptr);
}

} // namespace

// ---------------------------------------------------------------- recipe file
bool ShirtRecipe::Save(const std::wstring& path) const {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const auto c3 = [](const float* c) { return "[" + F(c[0]) + "," + F(c[1]) + "," + F(c[2]) + "]"; };
    fprintf(f, "{\n  \"garment\": \"button_shirt\",\n  \"name\": \"%s\",\n", name.c_str());
    fprintf(f, "  \"ease\": %s, \"sleeveEase\": %s, \"thickness\": %s,\n", F(ease).c_str(), F(sleeveEase).c_str(), F(thickness).c_str());
    fprintf(f, "  \"sleeveLength\": %s, \"rolled\": %s, \"rollWidth\": %s, \"rollThick\": %s,\n", F(sleeveLength).c_str(), rolled ? "true" : "false", F(rollWidth).c_str(), F(rollThick).c_str());
    fprintf(f, "  \"tucked\": %s, \"tuckDepth\": %s, \"blouse\": %s,\n", tucked ? "true" : "false", F(tuckDepth).c_str(), F(blouse).c_str());
    fprintf(f, "  \"collarStand\": %s, \"collarLeaf\": %s, \"collarSpread\": %s, \"openButtons\": %d, \"pockets\": %s,\n", F(collarStand).c_str(), F(collarLeaf).c_str(), F(collarSpread).c_str(), openButtons, pockets ? "true" : "false");
    fprintf(f, "  \"drape\": %s, \"wrinkle\": %s, \"wrinkleScale\": %s, \"seed\": %u,\n", F(drape).c_str(), F(wrinkle).c_str(), F(wrinkleScale).c_str(), seed);
    fprintf(f, "  \"base\": %s, \"stripeA\": %s, \"stripeB\": %s, \"button\": %s,\n", c3(base).c_str(), c3(stripeA).c_str(), c3(stripeB).c_str(), c3(button).c_str());
    fprintf(f, "  \"chestEase\": %s, \"waistEase\": %s, \"tuckPull\": %s, \"backLength\": %s,\n", F(chestEase).c_str(), F(waistEase).c_str(), F(tuckPull).c_str(), F(backLength).c_str());
    fprintf(f, "  \"fabricWeight\": %s, \"stretch\": %s, \"bend\": %s, \"collarStiffness\": %s,\n", F(fabricWeight).c_str(), F(stretch).c_str(), F(bend).c_str(), F(collarStiffness).c_str());
    fprintf(f, "  \"stripeSpacing\": %s, \"stripeWidth\": %s, \"wear\": %s, \"dirt\": %s\n}\n", F(stripeSpacing).c_str(), F(stripeWidth).c_str(), F(wear).c_str(), F(dirt).c_str());
    fclose(f);
    return true;
}

bool ShirtRecipe::Load(const std::wstring& path, std::string* err) {
    std::vector<uint8_t> bytes;
    if (!ReadBytes(path, bytes)) { if (err) *err = "cannot read the recipe"; return false; }
    meshimport_detail::Json doc;
    if (!meshimport_detail::JsonParser((const char*)bytes.data(), bytes.size()).Parse(doc) || doc.type != meshimport_detail::Json::Object) { if (err) *err = "the recipe is not JSON"; return false; }
    const auto num = [&](const char* k, float& v) { if (const auto* j = doc.Find(k)) v = (float)j->Num(v); };
    const auto flag = [&](const char* k, bool& v) { if (const auto* j = doc.Find(k); j && j->type == meshimport_detail::Json::Bool) v = j->b; };
    const auto col = [&](const char* k, float* c) { if (const auto* j = doc.Find(k)) for (size_t i = 0; i < 3; ++i) if (const auto* e = j->At(i)) c[i] = (float)e->Num(c[i]); };
    if (const auto* j = doc.Find("name"); j && j->type == meshimport_detail::Json::String && !j->s.empty()) name = j->s;
    num("ease", ease); num("sleeveEase", sleeveEase); num("thickness", thickness); num("sleeveLength", sleeveLength); flag("rolled", rolled);
    num("rollWidth", rollWidth); num("rollThick", rollThick); flag("tucked", tucked); num("tuckDepth", tuckDepth); num("blouse", blouse);
    num("collarStand", collarStand); num("collarLeaf", collarLeaf); num("collarSpread", collarSpread); flag("pockets", pockets);
    if (const auto* j = doc.Find("openButtons")) openButtons = j->Int(openButtons);
    if (const auto* j = doc.Find("seed")) seed = (uint32_t)j->Num(seed);
    num("wrinkle", wrinkle); num("wrinkleScale", wrinkleScale); num("drape", drape);
    col("base", base); col("stripeA", stripeA); col("stripeB", stripeB); col("button", button);
    num("stripeSpacing", stripeSpacing); num("stripeWidth", stripeWidth); num("wear", wear); num("dirt", dirt);
    num("chestEase", chestEase); num("waistEase", waistEase); num("tuckPull", tuckPull); num("backLength", backLength);
    num("fabricWeight", fabricWeight); num("stretch", stretch); num("bend", bend); num("collarStiffness", collarStiffness);
    return true;
}

// ---------------------------------------------------------------- the build
BuildResult BuildShirt(const ShirtRecipe& R, const BuildInput& in) {
    BuildResult res;
    const auto t0 = std::chrono::steady_clock::now();
    const auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    Body B;
    if (!LoadBody(in.bodyGlb, B, res.error)) return res;
    PointGrid bodyGrid;
    bodyGrid.Build(B.p, 0.03f);
    const auto skinOf = [&](V3 q) { const int k = bodyGrid.Nearest(q); return k >= 0 ? B.skin[(size_t)k] : VertexSkin{}; };

    // torso centre (z) at the waist, then the jeans
    float czSum = 0; int czN = 0;
    for (const V3& p : B.p) if (std::fabs(p.y - (B.pelvisY + 0.12f)) < 0.03f && std::fabs(p.x) < 0.2f) { czSum += p.z; ++czN; }
    const float cz = czN ? czSum / czN : 0.0f;
    Jeans J;
    std::string jeansNote;
    LoadJeans(in.jeansGlb, J, cz, jeansNote);
    float waistY = B.pelvisY + 0.12f;
    if (J.ok) { float s = 0; for (float t : J.top) s += t; waistY = s / Jeans::A; }
    const auto topAt = [&](float th) { return J.ok ? J.Top(th) : waistY; };

    // ---- the cut
    Cut cut;
    cut.sleeve = std::clamp(R.sleeveLength, 0.15f, 1.0f);
    cut.hemY = R.tucked ? waistY - std::max(0.02f, R.tuckDepth) : std::max(B.minY + 0.01f, waistY - 0.13f);
    cut.neckBack = B.neck.y - 0.020f;
    cut.neckFront = B.clav.y + 0.012f;
    cut.neckZ = B.neck.z;
    cut.v = R.openButtons > 0;
    cut.vTopY = cut.neckFront + 0.012f;
    cut.vBottom = cut.neckFront - (0.030f + 0.068f * (float)std::max(0, R.openButtons));
    cut.vTopHalf = 0.030f + 0.010f * (float)std::max(0, R.openButtons);

    // ---- candidate triangles -> cloth mesh
    Cloth C;
    std::vector<int> bodyCls(B.p.size());
    std::vector<uint32_t> remap(B.p.size(), UINT32_MAX);
    for (size_t i = 0; i < B.p.size(); ++i) bodyCls[i] = Classify(B, cut, B.p[i]);
    for (size_t t = 0; t + 2 < B.tri.size(); t += 3) {
        bool cand = false;
        for (int k = 0; k < 3; ++k) { const V3 p = B.p[B.tri[t + k]]; cand |= ArmA(B, p, nullptr) < cut.sleeve + 0.08f && p.y > cut.hemY - 0.04f; }
        if (!cand) continue;
        for (int k = 0; k < 3; ++k) {
            uint32_t& m = remap[B.tri[t + k]];
            if (m == UINT32_MAX) m = C.Add(B.p[B.tri[t + k]], B.n[B.tri[t + k]], B.skin[B.tri[t + k]]);
            C.tri.push_back(m);
        }
    }
    if (C.tri.empty()) { res.error = "the shirt covers nothing on this body"; return res; }
    Subdivide(C, [](V3) { return true; });
    const float elbowA = Dot(B.elbow[0] - B.shoulder[0], B.dir[0]) / B.armLen[0];
    Subdivide(C, [&](V3 p) {
        const float a = ArmA(B, p, nullptr);
        if (a < 0.02f) return p.y > waistY - 0.05f && p.y < waistY + 0.16f;
        return (a > cut.sleeve - 0.22f && a < cut.sleeve + 0.05f) || std::fabs(a - elbowA) < 0.08f;
    });
    // keep covered triangles
    std::vector<int> cls(C.p0.size());
    for (size_t i = 0; i < C.p0.size(); ++i) cls[i] = Classify(B, cut, C.p0[i]);
    {
        std::vector<uint32_t> kept, used(C.p0.size(), UINT32_MAX);
        std::vector<int> reasonOf(C.p0.size(), R_None);
        for (size_t t = 0; t + 2 < C.tri.size(); t += 3) {
            const uint32_t* v = &C.tri[t];
            const bool cov = !cls[v[0]] && !cls[v[1]] && !cls[v[2]];
            if (cov) { kept.insert(kept.end(), {v[0], v[1], v[2]}); continue; }
            int why = 0;
            for (int k = 0; k < 3; ++k) if (cls[v[k]]) why = cls[v[k]];
            for (int k = 0; k < 3; ++k) if (!cls[v[k]]) reasonOf[v[k]] = why;
        }
        Cloth K;
        for (uint32_t& i : kept) {
            if (used[i] == UINT32_MAX) { used[i] = K.Add(C.p0[i], C.n0[i], C.skin[i]); K.reason.push_back(reasonOf[i]); }
            i = used[i];
        }
        K.tri = std::move(kept);
        C = std::move(K);
    }
    Neighbours(C);
    // boundary vertices: on an edge used by one triangle
    std::vector<uint8_t> boundary(C.p0.size(), 0);
    {
        std::unordered_map<uint64_t, int> edges;
        for (size_t t = 0; t + 2 < C.tri.size(); t += 3)
            for (int e = 0; e < 3; ++e) { const uint32_t a = C.tri[t + e], b = C.tri[t + (e + 1) % 3]; ++edges[a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a]; }
        for (const auto& [k, n] : edges) if (n == 1) { boundary[(size_t)(k >> 32)] = 1; boundary[(size_t)(uint32_t)k] = 1; }
        for (size_t i = 0; i < C.p0.size(); ++i) if (!boundary[i]) C.reason[i] = R_None;
    }
    // snap boundary vertices onto their cut line (clean edges)
    for (size_t i = 0; i < C.p0.size(); ++i) {
        V3& p = C.p0[i];
        V3 to = p;
        int side;
        const float a = ArmA(B, p, &side);
        switch (C.reason[i]) {
        case R_Sleeve: to = p + B.dir[side] * ((cut.sleeve - a) * B.armLen[side]); break;
        case R_Hem: to.y = cut.hemY; break;
        case R_Neck: to.y = std::max(p.y, cut.NeckY(p.x, p.z)); break;
        case R_V: to.x = (p.x >= 0 ? 1.0f : -1.0f) * cut.VHalf(p.y); break;
        default: break;
        }
        const V3 d = to - p;
        const float l = Len(d);
        p = l > 0.006f ? p + d * (0.006f / l) : to;
    }

    // ---- offset + relax
    const size_t N = C.p0.size();
    C.p.resize(N); C.ease.resize(N);
    std::vector<float> armA(N), dMax(N);
    std::vector<int> sideOf(N);
    for (size_t i = 0; i < N; ++i) {
        armA[i] = ArmA(B, C.p0[i], &sideOf[i]);
        const float wArm = Smooth(-0.04f, 0.08f, armA[i]);
        const float torso = R.ease * (1.0f - 0.7f * std::max(0.0f, C.n0[i].y));
        const float snug = Smooth(cut.sleeve - 0.16f, cut.sleeve - 0.01f, armA[i]); // a rolled sleeve is pushed up and grips the forearm
        const float arm = R.sleeveEase * (1.0f + 0.35f * std::max(0.0f, -C.n0[i].y)) * (1.0f - 0.5f * std::max(0.0f, C.n0[i].y)) * (1.0f - 0.85f * snug);
        C.ease[i] = Lerp(torso, arm, wArm);
        C.p[i] = C.p0[i] + C.n0[i] * (R.thickness + C.ease[i]);
        dMax[i] = R.thickness + C.ease[i] * 2.2f + 0.006f;
    }
    const float minGap = R.thickness + 0.003f;
    const auto constrain = [&](size_t i, float lo) {
        const float d = Dot(C.p[i] - C.p0[i], C.n0[i]);
        if (d < lo) C.p[i] += C.n0[i] * (lo - d);
        else if (d > dMax[i]) C.p[i] += C.n0[i] * (dMax[i] - d);
    };
    std::vector<V3> tmp(N);
    for (int it = 0; it < 24; ++it) {
        for (size_t i = 0; i < N; ++i) {
            if (boundary[i] || C.nb[i].empty()) { tmp[i] = C.p[i]; continue; }
            V3 avg{};
            for (uint32_t j : C.nb[i]) avg += C.p[j];
            avg = avg * (1.0f / (float)C.nb[i].size());
            tmp[i] = C.p[i] + (avg - C.p[i]) * 0.5f;
        }
        C.p.swap(tmp);
        for (size_t i = 0; i < N; ++i) constrain(i, minGap);
    }

    size_t chestN = 0, pullN = 0; float chestMax = 0, pullMax = 0;
    // ---- chest: woven cloth does not follow the chest into its hollows. Seen from the front (a heightfield of how far forward
    // the shirt is, per x and y), every row bridges the dip between the pecs in a straight line (upper concave hull), and
    // every column runs straight and taut from the chest down to the waistband (it is tucked and pulled down) instead of
    // tucking back under the pecs. Without this the shirt shrink-wraps each pec and reads as breasts.
    {
        constexpr float cell = 0.008f;
        chestN = 0; chestMax = 0;
        const float x0 = -0.26f, y0 = waistY - 0.04f, y1 = B.clav.y + 0.03f;
        const int GX = (int)std::ceil(0.52f / cell) + 1, GY = std::max(2, (int)std::ceil((y1 - y0) / cell) + 1);
        std::vector<float> zf((size_t)GX * GY, -1e9f);
        const auto front = [&](size_t i) { return armA[i] < -0.01f && C.n0[i].z > 0.15f && C.p[i].z > cz && C.p[i].y > y0 && C.p[i].y < y1; };
        for (size_t i = 0; i < N; ++i) {
            if (!front(i)) continue;
            const int gx = std::clamp((int)std::lround((C.p[i].x - x0) / cell), 0, GX - 1), gy = std::clamp((int)std::lround((C.p[i].y - y0) / cell), 0, GY - 1);
            float& z = zf[(size_t)gy * GX + gx];
            z = std::max(z, C.p[i].z);
        }
        std::vector<float> env = zf;
        for (int gy = 0; gy < GY; ++gy) { // rows: upper concave hull over the filled cells (monotone chain)
            std::vector<int> hull;
            for (int gx = 0; gx < GX; ++gx) {
                if (zf[(size_t)gy * GX + gx] < -1e8f) continue;
                while (hull.size() >= 2) {
                    const int a = hull[hull.size() - 2], b = hull.back();
                    const float za = zf[(size_t)gy * GX + a], zb = zf[(size_t)gy * GX + b], zc = zf[(size_t)gy * GX + gx];
                    if ((zb - za) * (float)(gx - a) <= (zc - za) * (float)(b - a)) hull.pop_back(); // b is under the line a..c
                    else break;
                }
                hull.push_back(gx);
            }
            for (size_t h = 0; h + 1 < hull.size(); ++h) {
                const int a = hull[h], b = hull[h + 1];
                for (int gx = a + 1; gx < b; ++gx) {
                    const size_t k = (size_t)gy * GX + gx;
                    if (zf[k] < -1e8f) continue;
                    const float t = (float)(gx - a) / (float)(b - a);
                    env[k] = std::max(env[k], Lerp(zf[(size_t)gy * GX + a], zf[(size_t)gy * GX + b], t) - 0.0015f); // a hair of sag
                }
            }
        }
        // columns: the shirt is tucked and pulled down, so from the chest to the waistband it runs in straight, taut lines
        // (upper concave hull per column), never back under the pecs
        {
            std::vector<float> col = env;
            for (int gx = 0; gx < GX; ++gx) {
                std::vector<int> hull;
                for (int gy = 0; gy < GY; ++gy) {
                    if (col[(size_t)gy * GX + gx] < -1e8f) continue;
                    const auto z = [&](int y) { return col[(size_t)y * GX + gx]; };
                    while (hull.size() >= 2) {
                        const int a2 = hull[hull.size() - 2], b2 = hull.back();
                        if ((z(b2) - z(a2)) * (float)(gy - a2) <= (z(gy) - z(a2)) * (float)(b2 - a2)) hull.pop_back();
                        else break;
                    }
                    hull.push_back(gy);
                }
                for (size_t h = 0; h + 1 < hull.size(); ++h) {
                    const int a2 = hull[h], b2 = hull[h + 1];
                    for (int gy = a2 + 1; gy < b2; ++gy) {
                        float& z = env[(size_t)gy * GX + gx];
                        if (z < -1e8f) continue;
                        const float t = (float)(gy - a2) / (float)(b2 - a2);
                        z = std::max(z, Lerp(col[(size_t)a2 * GX + gx], col[(size_t)b2 * GX + gx], t));
                    }
                }
            }
        }
        const auto sample = [&](float x, float y) { // bilinear over the filled cells
            const float fx = (x - x0) / cell, fy = (y - y0) / cell;
            const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
            float sum = 0, wsum = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int gx = ix + dx, gy = iy + dy;
                    if (gx < 0 || gy < 0 || gx >= GX || gy >= GY) continue;
                    const float z = env[(size_t)gy * GX + gx];
                    if (z < -1e8f) continue;
                    const float w = (dx ? fx - ix : 1 - (fx - ix)) * (dy ? fy - iy : 1 - (fy - iy));
                    sum += z * w; wsum += w;
                }
            return wsum > 1e-4f ? sum / wsum : -1e9f;
        };
        for (size_t i = 0; i < N; ++i) {
            if (!front(i) || boundary[i]) continue;
            const float z = sample(C.p[i].x, C.p[i].y);
            if (z < -1e8f || z <= C.p[i].z) continue;
            // full on the front, fading out toward the sides and into the waist band (the tuck takes over there)
            const float w = Smooth(0.15f, 0.45f, C.n0[i].z) * Smooth(waistY - 0.02f, waistY + 0.05f, C.p[i].y);
            if (w > 0) { ++chestN; chestMax = std::max(chestMax, (z - C.p[i].z) * w); }
            C.p[i].z += (z - C.p[i].z) * w;
        }
        for (int it = 0; it < 4; ++it) { // round the hull's creases
            for (size_t i = 0; i < N; ++i) {
                if (boundary[i] || C.nb[i].empty() || !front(i)) { tmp[i] = C.p[i]; continue; }
                V3 avg{};
                for (uint32_t j : C.nb[i]) avg += C.p[j];
                tmp[i] = C.p[i] + (avg * (1.0f / (float)C.nb[i].size()) - C.p[i]) * 0.4f;
            }
            C.p.swap(tmp);
        }
        for (size_t i = 0; i < N; ++i) { dMax[i] = std::max(dMax[i], Dot(C.p[i] - C.p0[i], C.n0[i])); constrain(i, minGap); }
    }

    // ---- waist: tucked inside the jeans, bloused over the band, gathered folds
    const uint32_t seed = R.seed * 2654435761u;
    const float W = std::clamp(R.wrinkle, 0.0f, 2.0f), ws = std::clamp(R.wrinkleScale, 0.4f, 2.5f);
    std::vector<float> foldRidge(N, 0.0f); // for the vertex shade
    for (size_t i = 0; i < N; ++i) {
        const float wT = 1.0f - Smooth(-0.06f, 0.0f, armA[i]);
        if (wT <= 0) continue;
        V3 p = C.p[i];
        const float dx = p.x, dz = p.z - cz;
        float r = std::sqrt(dx * dx + dz * dz);
        if (r < 1e-4f) continue;
        const float th = std::atan2(dx, dz); // 0 = front
        const float thPos = th < 0 ? th + 2 * kPi : th;
        const float yT = topAt(thPos);
        const float Hb = 0.06f + 0.05f * R.blouse;
        const float h = (p.y - yT) / Hb;
        const float rBody = std::sqrt(C.p0[i].x * C.p0[i].x + (C.p0[i].z - cz) * (C.p0[i].z - cz)) + minGap;
        float dy = 0;
        if (R.tucked) {
            const float rIn = J.ok ? J.R(thPos, std::min(p.y, yT - 0.003f)) - 0.004f - R.thickness : rBody + 0.004f;
            if (h < 0) {
                if (rIn > 0) r = std::min(r, rIn);
            } else if (h < 1.6f) {
                const float rInTop = J.ok && J.RTop(thPos) > 0 ? J.RTop(thPos) - 0.003f - R.thickness : rBody + 0.004f;
                const float s = Smooth(0.0f, 1.3f, h);
                if (r > rInTop) r = Lerp(rInTop, r, s);
                const float back = 1.0f + 0.35f * (0.5f - 0.5f * std::cos(th));
                r += R.blouse * 0.022f * back * std::pow(std::max(0.0f, std::sin(kPi * Clamp01(h / 1.05f))), 1.3f);
                dy = -R.blouse * 0.014f * std::max(0.0f, std::sin(kPi * Clamp01(h / 0.8f))) * (h < 0.8f ? 1.0f : 0.0f);
            }
        } else if (J.ok && p.y < yT + 0.02f) {
            const float rj = J.R(thPos, std::min(p.y, yT - 0.002f));
            if (rj > 0) r = std::max(r, rj + 0.004f + R.thickness);
        }
        // gathered folds: strongest just above the band, running up and leaning toward the front centre at the sides
        const float env = R.tucked ? Smooth(-0.6f, 0.0f, h) * (1.0f - Smooth(0.3f, 2.6f, h)) : Smooth(cut.hemY + 0.15f, cut.hemY + 0.01f, p.y); // pull lines run up from the tuck
        if (env > 0 && W > 0) {
            // whole numbers of folds around the body, so the pattern closes on itself at the back; tighter at the band
            const float k1 = std::max(6.0f, std::round(2 * kPi * 0.16f / (0.042f * ws))), k2 = std::round(k1 * 1.35f);
            const float slant = 1.3f * std::sin(th) * (0.55f + 0.45f * std::cos(th)) * (p.y - yT) / (0.042f * ws); // leaning to the buckle in front, nearly upright at the back
            const float cx = std::cos(th) * 3.5f, sx = std::sin(th) * 3.5f;
            const float n1 = (Fbm(cx + p.y * 9.0f, sx - p.y * 6.0f, seed + 1) - 0.5f) * 5.0f;
            const float g = Clamp01(h);
            const float F1 = Lerp(std::sin(k2 * th + slant + n1), std::sin(k1 * th + slant + n1), g) + 0.5f * std::sin(std::round(k1 * 1.9f) * th + 1.7f + n1 * 1.3f);
            const float ridge = std::pow(Clamp01(0.5f + F1 / 3.0f), 1.6f);
            const float amp = W * 0.0055f * env * (0.45f + 0.55f * (0.5f + 0.5f * std::cos(th))) * (0.6f + 0.8f * Fbm(cx * 0.6f + 3.0f, sx * 0.6f + p.y * 9.0f, seed + 2));
            r += amp * ridge;
            foldRidge[i] = env * (ridge - 0.5f);
        }
        r = std::max(r, rBody);
        const V3 q{std::sin(th) * r, p.y + dy, cz + std::cos(th) * r};
        C.p[i] = p + (q - p) * wT;
    }
    // sleeves: bunching above the roll, the inner-elbow crease, a soft hang underneath
    for (size_t i = 0; i < N; ++i) {
        const float wA = Smooth(-0.02f, 0.06f, armA[i]);
        if (wA <= 0 || W <= 0) continue;
        const int s = sideOf[i];
        const V3 S = B.shoulder[s], D = B.dir[s];
        const V3 rel = C.p[i] - S;
        const V3 q = rel - D * Dot(rel, D);
        const float rad = Len(q);
        if (rad < 1e-4f) continue;
        const V3 qn = q * (1.0f / rad);
        const V3 up = Norm(V3{0, 1, 0} - D * D.y), fw = Cross(D, up);
        const float psi = std::atan2(Dot(qn, fw), Dot(qn, up));
        const float dd = (cut.sleeve - armA[i]) * B.armLen[s]; // metres back from the sleeve end
        float disp = 0;
        const float bunch = std::exp(-std::max(0.0f, dd) / 0.065f) * (R.rolled ? 1.0f : 0.5f);
        const float nb1 = (Fbm(psi * 1.5f + 7, dd * 30.0f, seed + 11) - 0.5f) * 4.0f;
        const float Fb = std::sin(2 * kPi * dd / (0.024f * ws) + 1.3f * std::sin(psi * 2.0f + (float)(seed % 7)) + nb1);
        disp += W * 0.0065f * bunch * std::pow(Clamp01(0.5f + 0.5f * Fb), 1.5f);
        const float el = (armA[i] - elbowA) * B.armLen[s] / 0.045f;
        const float inner = Clamp01(qn.z); // the elbow crease faces forward in the T-pose
        const float Fe = std::sin(2 * kPi * (dd * 0.8f + psi * 0.012f) / (0.017f * ws) + nb1);
        disp += W * 0.0055f * std::exp(-el * el) * inner * std::pow(Clamp01(0.5f + 0.5f * Fe), 1.4f);
        const float under = Clamp01(-qn.y);
        const float Fu = std::sin(2 * kPi * psi * rad / (0.03f * ws) + nb1 * 0.5f);
        disp += W * 0.004f * under * Smooth(0.05f, 0.25f, armA[i]) * (0.5f + 0.5f * Fu);
        C.p[i] += qn * (disp * wA);
        foldRidge[i] += disp / 0.008f - 0.3f * wA;
    }
    // a light relax to round the folds, then outside the body again
    for (int it = 0; it < 2; ++it) {
        for (size_t i = 0; i < N; ++i) {
            if (boundary[i] || C.nb[i].empty()) { tmp[i] = C.p[i]; continue; }
            V3 avg{};
            for (uint32_t j : C.nb[i]) avg += C.p[j];
            tmp[i] = C.p[i] + (avg * (1.0f / (float)C.nb[i].size()) - C.p[i]) * 0.3f;
        }
        C.p.swap(tmp);
        for (size_t i = 0; i < N; ++i) { dMax[i] = 1e9f; constrain(i, minGap); }
    }
    // ---- drape: position-based cloth on the fitted shirt. Edges get a little more length than the fit gave them (more at
    // the waist across the body, and along the sleeve above the roll), so under gravity, pinned at the neckline, the cuffs
    // and inside the jeans, the cloth hangs off the chest and shoulder blades and buckles into its own uneven folds.
    // Collisions: the body (nearest body vertex's plane) and, below the waistband, the inside of the jeans.
    if (R.drape > 0) {
        std::vector<V3> prev = C.p, pin = C.p;
        std::vector<float> invM(N, 1.0f), pinK(N, 0.0f), rMaxJ(N, -1.0f);
        std::vector<int> nearB(N, -1);
        for (size_t i = 0; i < N; ++i) {
            pinK[i] = 0.035f; // friction: the cloth does not slide off the body
            if (boundary[i] && C.reason[i] != R_None) invM[i] = 0;
            else if (armA[i] < 0.08f && C.n0[i].y > 0.5f) pinK[i] = 0.35f;      // resting on the shoulders: friction
            else if (armA[i] > 0.0f && C.n0[i].y > 0.6f) pinK[i] = 0.12f;       // the top of the sleeve rests on the arm
        }
        struct Edge { uint32_t a, b; float rest, k; };
        std::vector<Edge> edges;
        {
            std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> opp; // edge -> its two opposite vertices
            for (size_t t = 0; t + 2 < C.tri.size(); t += 3)
                for (int e = 0; e < 3; ++e) {
                    const uint32_t a = C.tri[t + e], b = C.tri[t + (e + 1) % 3], o = C.tri[t + (e + 2) % 3];
                    const uint64_t key = a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a;
                    auto it = opp.find(key);
                    if (it == opp.end()) opp.emplace(key, std::make_pair(o, UINT32_MAX));
                    else it->second.second = o;
                }
            edges.reserve(opp.size() * 2);
            for (const auto& [key, o] : opp) {
                const uint32_t a = (uint32_t)(key >> 32), b = (uint32_t)key;
                const V3 d = C.p[b] - C.p[a];
                const float len = Len(d);
                if (len < 1e-6f) continue;
                const V3 dn = d * (1.0f / len), mid = (C.p0[a] + C.p0[b]) * 0.5f;
                float slack = 1.0f + 0.012f * R.drape;
                const float am = 0.5f * (armA[a] + armA[b]);
                if (am < 0.0f) { // across the body near the waist: the gathered fabric
                    const float h = (mid.y - waistY) / 0.10f;
                    const float front = 0.5f + 0.5f * (mid.z - cz) / std::max(0.05f, std::sqrt(mid.x * mid.x + (mid.z - cz) * (mid.z - cz))); // 1 front, 0 back
                    const float env = (R.tucked ? Smooth(-0.5f, 0.1f, h) * (1.0f - Smooth(0.6f, 1.8f, h)) : 0.0f) * (0.35f + 0.65f * front);
                    slack += env * (0.035f + 0.09f * R.blouse) * W * (1.0f - std::fabs(dn.y)) * R.drape; // spare width -> vertical folds
                    slack += env * 0.04f * R.blouse * W * std::fabs(dn.y) * R.drape;                    // a little lengthwise (the blouse)
                    slack -= 0.025f * std::fabs(dn.y) * R.drape; // tucked and pulled down: the torso is short vertically, taut
                } else { // along the sleeve above the roll, and at the elbow
                    const int sd = mid.x >= 0 ? 0 : 1;
                    const float along = std::fabs(Dot(dn, B.dir[sd]));
                    const float dd = (cut.sleeve - am) * B.armLen[sd];
                    slack += (R.rolled ? 0.12f : 0.06f) * std::exp(-std::max(0.0f, dd) / 0.07f) * along * W * R.drape;
                    slack += 0.06f * std::exp(-((am - elbowA) / 0.07f) * ((am - elbowA) / 0.07f)) * along * W * R.drape;
                }
                edges.push_back({a, b, len * slack, 0.95f});
                if (o.second != UINT32_MAX) edges.push_back({o.first, o.second, Len(C.p[o.second] - C.p[o.first]) * slack, 0.7f}); // bending: stiff enough that folds come out wide, not as needles
            }
        }
        const float gap = R.thickness + 0.0025f;
        const auto nearest = [&] { for (size_t i = 0; i < N; ++i) nearB[i] = bodyGrid.Nearest(C.p[i]); };
        nearest();
        const int steps = 48;
        const float g = 0.00012f;
        const bool tuckOn = R.tucked && J.ok;
        for (int step = 0; step < steps; ++step) {
            for (size_t i = 0; i < N; ++i) {
                if (invM[i] == 0) continue;
                const V3 v = (C.p[i] - prev[i]) * 0.85f;
                prev[i] = C.p[i];
                C.p[i] += v + V3{0, -g * (armA[i] > 0.05f ? 0.2f : 1.0f), 0};
            }
            for (int it = 0; it < 6; ++it) {
                for (const Edge& e : edges) {
                    const float wa = invM[e.a], wb = invM[e.b], ws2 = wa + wb;
                    if (ws2 == 0) continue;
                    const V3 d = C.p[e.b] - C.p[e.a];
                    const float len = Len(d);
                    if (len < 1e-7f) continue;
                    const float c = (len - e.rest) / len * e.k / ws2;
                    C.p[e.a] += d * (c * wa);
                    C.p[e.b] += d * (-c * wb);
                }
                for (size_t i = 0; i < N; ++i) {
                    if (invM[i] == 0) continue;
                    if (pinK[i] > 0) C.p[i] += (pin[i] - C.p[i]) * pinK[i];
                    const int k = nearB[i];
                    if (k >= 0) { const float d = Dot(C.p[i] - B.p[(size_t)k], B.n[(size_t)k]); if (d < gap) C.p[i] += B.n[(size_t)k] * (gap - d); }
                    if (tuckOn && armA[i] < -0.02f) { // below the waistband (where the cloth is now): inside the jeans
                        const float dx = C.p[i].x, dz = C.p[i].z - cz, r = std::sqrt(dx * dx + dz * dz);
                        const float th = std::atan2(dx, dz), thp = th < 0 ? th + 2 * kPi : th;
                        if (C.p[i].y < topAt(thp) - 0.007f && r > 1e-4f) {
                            const float rj = J.R(thp, C.p[i].y) - 0.004f - R.thickness;
                            if (rj > 0 && r > rj) { const float s2 = rj / r; C.p[i].x = dx * s2; C.p[i].z = cz + dz * s2; }
                        }
                    }
                }
            }
            if (step % 8 == 7) nearest();
        }
        for (int it = 0; it < 6; ++it) { // take out the grid-scale ripples the solver leaves, keep the folds
            for (size_t i = 0; i < N; ++i) {
                if (invM[i] == 0 || C.nb[i].empty()) { tmp[i] = C.p[i]; continue; }
                V3 avg{};
                for (uint32_t j : C.nb[i]) avg += C.p[j];
                tmp[i] = C.p[i] + (avg * (1.0f / (float)C.nb[i].size()) - C.p[i]) * 0.45f;
            }
            C.p.swap(tmp);
            for (size_t i = 0; i < N; ++i) { const int k = nearB[i]; if (k >= 0) { const float d = Dot(C.p[i] - B.p[(size_t)k], B.n[(size_t)k]); if (d < gap) C.p[i] += B.n[(size_t)k] * (gap - d); } }
        }
    }
    // ---- tuck pull lines: a tucked shirt pulled down tight has its spare width gathered into a few narrow ridges that start in
    // the waistband and run up, each its own length, leaning toward the fly at the sides (the cloth is pulled down and in).
    // Laid on after the drape so its smoothing cannot take them out. Ridges are sparse, not a wave: crest, a valley each side.
    if (R.tucked && W > 0) {
        struct Pull { float th, len, amp, width, lean; };
        std::vector<Pull> pulls;
        uint32_t rs = seed ^ 0x9E3779B9u;
        const auto rnd = [&] { rs = rs * 1664525u + 1013904223u; return (float)(rs >> 8) / 16777216.0f; };
        const int count = std::max(4, (int)std::lround(15.0f / ws));
        for (int k = 0; k < count; ++k) {
            const float base = -kPi + (k + 0.5f) * 2 * kPi / (float)count;
            const float th = base + (rnd() - 0.5f) * 1.2f * kPi / (float)count;
            const float front = 0.5f + 0.5f * std::cos(th);
            pulls.push_back({th, (0.08f + 0.10f * rnd()) * (0.7f + 0.5f * front), (0.008f + 0.006f * rnd()) * (0.6f + 0.6f * front),
                             (0.013f + 0.008f * rnd()) * ws, -std::sin(th) * (0.6f + 0.4f * rnd())});
        }
        for (size_t i = 0; i < N; ++i) {
            if (armA[i] > -0.02f) continue;
            V3 p = C.p[i];
            const float dx = p.x, dz = p.z - cz, r = std::sqrt(dx * dx + dz * dz);
            if (r < 1e-4f) continue;
            const float th = std::atan2(dx, dz), thPos = th < 0 ? th + 2 * kPi : th;
            const float up = p.y - topAt(thPos); // metres above the waistband
            if (up < -0.015f || up > 0.20f) continue;
            float disp = 0;
            for (const Pull& f : pulls) {
                if (up > f.len) continue;
                float d = th - (f.th + f.lean * up / std::max(r, 0.05f)); // leans as it rises
                d = std::remainder(d, 2 * kPi) * r;                       // metres around the body
                const float x = d / f.width;
                const float along = Smooth(-0.015f, 0.01f, up) * std::pow(1.0f - Clamp01(up / f.len), 1.4f);
                disp += f.amp * along * (std::exp(-x * x) - 0.35f * std::exp(-(x * 0.6f) * (x * 0.6f))); // crest with shallow valleys
            }
            if (disp == 0) continue;
            ++pullN; pullMax = std::max(pullMax, disp * W);
            const float s = (r + disp * W) / r;
            C.p[i].x = dx * s; C.p[i].z = cz + dz * s;
            foldRidge[i] += disp * W / 0.0009f;
        }
    }
    for (size_t i = 0; i < N; ++i) // never ship a bad vertex: back to the plain offset
        if (!std::isfinite(C.p[i].x) || !std::isfinite(C.p[i].y) || !std::isfinite(C.p[i].z)) C.p[i] = C.p0[i] + C.n0[i] * (R.thickness + C.ease[i]);
    SmoothNormals(C.p, C.tri, C.n);
    // shade: cavities darker (folds read), a shadow line under the blouse lip and along the waistband
    C.ao.assign(N, 1.0f);
    for (size_t i = 0; i < N; ++i) {
        if (C.nb[i].empty()) continue;
        V3 avg{};
        float el = 0;
        for (uint32_t j : C.nb[i]) { avg += C.p[j]; el += Len(C.p[j] - C.p[i]); }
        avg = avg * (1.0f / (float)C.nb[i].size());
        el /= (float)C.nb[i].size();
        const float cav = Dot(avg - C.p[i], C.n[i]) / std::max(1e-4f, el);
        float a = 1.0f - 1.1f * std::max(0.0f, cav) + 0.25f * std::max(0.0f, -cav);
        a -= 0.05f * std::max(0.0f, -foldRidge[i]);
        if (R.tucked && armA[i] < 0) { const float dyw = (C.p[i].y - waistY) / 0.012f; a -= 0.15f * std::exp(-dyw * dyw); }
        C.ao[i] = std::clamp(a, 0.72f, 1.03f);
    }
    for (int it = 0; it < 3; ++it) { // soften: shade follows the folds, not each triangle
        std::vector<float> ao2 = C.ao;
        for (size_t i = 0; i < N; ++i) { if (C.nb[i].empty()) continue; float s2 = 0; for (uint32_t j : C.nb[i]) s2 += C.ao[j]; ao2[i] = 0.5f * C.ao[i] + 0.5f * s2 / (float)C.nb[i].size(); }
        C.ao.swap(ao2);
    }

    // ---- output: the shirt body with chart UVs
    Mesh cloth, buttons;
    Atlas at;
    float sleeveR[2] = {0.055f, 0.055f};
    {
        float sum[2] = {0, 0}; int cnt[2] = {0, 0};
        for (size_t i = 0; i < N; ++i) if (armA[i] > 0.15f) { const int s = sideOf[i]; const V3 rel = C.p[i] - B.shoulder[s]; sum[s] += Len(rel - B.dir[s] * Dot(rel, B.dir[s])); ++cnt[s]; }
        for (int s = 0; s < 2; ++s) if (cnt[s]) sleeveR[s] = sum[s] / cnt[s];
    }
    const auto sleeveUV = [&](int s, V3 p, float& u, float& v) {
        const V3 D = B.dir[s], rel = p - B.shoulder[s];
        const V3 q = rel - D * Dot(rel, D);
        const V3 up = Norm(V3{0, 1, 0} - D * D.y), fw = Cross(D, up);
        u = std::atan2(Dot(q, fw), Dot(q, up)) * sleeveR[s] * (s == 0 ? 1.0f : -1.0f);
        v = Dot(rel, D);
    };
    for (size_t t = 0; t + 2 < C.tri.size(); t += 3) {
        const uint32_t* tv = &C.tri[t];
        const float aMean = (armA[tv[0]] + armA[tv[1]] + armA[tv[2]]) / 3.0f;
        const V3 n0 = Norm(C.n0[tv[0]] + C.n0[tv[1]] + C.n0[tv[2]]);
        const V3 cen = (C.p0[tv[0]] + C.p0[tv[1]] + C.p0[tv[2]]) * (1.0f / 3.0f);
        int chart;
        const int side = cen.x >= 0 ? 0 : 1;
        if (aMean > 0.0f) chart = side == 0 ? C_SleeveL : C_SleeveR;
        else if (n0.y > 0.72f) chart = C_Yoke;
        else {
            const float th = std::atan2(cen.x, cen.z - cz);
            chart = std::fabs(th) < 0.87f ? C_Front : std::fabs(th) > 2.27f ? C_Back : th > 0 ? C_SideL : C_SideR;
        }
        OutVert o[3];
        for (int k = 0; k < 3; ++k) {
            const uint32_t i = tv[k];
            o[k].p = C.p[i]; o[k].n = C.n[i]; o[k].ao = C.ao[i]; o[k].skin = C.skin[i]; o[k].chart = chart;
            const V3 q = C.p[i];
            switch (chart) {
            case C_Front: o[k].u = q.x; o[k].v = -q.y; break;
            case C_Back: o[k].u = -q.x; o[k].v = -q.y; break;
            case C_SideL: o[k].u = -(q.z - cz); o[k].v = -q.y; break;
            case C_SideR: o[k].u = q.z - cz; o[k].v = -q.y; break;
            case C_Yoke: o[k].u = q.x; o[k].v = q.z; break;
            default: sleeveUV(side, q, o[k].u, o[k].v); break;
            }
        }
        if (chart == C_SleeveL || chart == C_SleeveR) { // the seam under the arm: keep the triangle on one side of the wrap
            const float span = kPi * sleeveR[side];
            const float mx = std::max({o[0].u, o[1].u, o[2].u}), mn = std::min({o[0].u, o[1].u, o[2].u});
            if (mx - mn > span) for (OutVert& x : o) if (x.u < 0) x.u += 2 * span;
        }
        for (const OutVert& x : o) cloth.v.push_back(x);
    }
    C.p0.shrink_to_fit();

    // ---- rolled cuffs
    if (R.rolled)
        for (int s = 0; s < 2; ++s) {
            const V3 S = B.shoulder[s], D = B.dir[s], up = Norm(V3{0, 1, 0} - D * D.y), fw = Cross(D, up);
            constexpr int K = 72, M = 22;
            float rim[K];
            int has[K] = {};
            std::fill(rim, rim + K, 0.0f);
            for (size_t i = 0; i < N; ++i) {
                if (sideOf[i] != s || C.reason[i] != R_Sleeve) continue;
                const V3 rel = C.p[i] - S, q = rel - D * Dot(rel, D);
                float psi = std::atan2(Dot(q, fw), Dot(q, up));
                if (psi < 0) psi += 2 * kPi;
                const int k = std::min(K - 1, (int)(psi / (2 * kPi) * K));
                rim[k] = std::max(rim[k], Len(q)); has[k] = 1;
            }
            int any = 0;
            for (int k = 0; k < K; ++k) any += has[k];
            if (!any) continue;
            for (int pass = 0; pass < K; ++pass) for (int k = 0; k < K; ++k) if (!has[k] && (has[(k + 1) % K] || has[(k + K - 1) % K])) { rim[k] = std::max(rim[(k + 1) % K], rim[(k + K - 1) % K]); has[k] = 2; }
            float sm[K];
            for (int it = 0; it < 4; ++it) { for (int k = 0; k < K; ++k) sm[k] = 0.25f * rim[(k + K - 1) % K] + 0.5f * rim[k] + 0.25f * rim[(k + 1) % K]; std::copy(sm, sm + K, rim); }
            { // the roll sits on the forearm: its inside just clears the skin, never a funnel with the arm inside it
                float bodyRim[K] = {};
                for (size_t bi = 0; bi < B.p.size(); ++bi) {
                    int bs;
                    const float a = ArmA(B, B.p[bi], &bs);
                    if (bs != s || std::fabs(a - cut.sleeve) * B.armLen[s] > 0.02f) continue;
                    const V3 rel = B.p[bi] - S, q = rel - D * Dot(rel, D);
                    float psi = std::atan2(Dot(q, fw), Dot(q, up));
                    if (psi < 0) psi += 2 * kPi;
                    const int k = std::min(K - 1, (int)(psi / (2 * kPi) * K));
                    bodyRim[k] = std::max(bodyRim[k], Len(q));
                }
                for (int pass = 0; pass < K; ++pass) for (int k = 0; k < K; ++k) if (bodyRim[k] <= 0) bodyRim[k] = std::max(bodyRim[(k + 1) % K], bodyRim[(k + K - 1) % K]);
                for (int k = 0; k < K; ++k) if (bodyRim[k] > 0) rim[k] = std::clamp(rim[k], bodyRim[k] + R.thickness + 0.002f, bodyRim[k] + 0.007f);
            }
            float rimMean = 0;
            for (float x : rim) rimMean += x / K;
            const float xEnd = cut.sleeve * B.armLen[s];
            Grid g;
            g.Resize(K, M);
            g.wrapI = g.wrapJ = true;
            for (int k = 0; k < K; ++k) {
                const float psi = k * 2 * kPi / K;
                const V3 Rd = up * std::cos(psi) + fw * std::sin(psi);
                const float nA = Fbm(std::cos(psi) * 1.3f + 5, std::sin(psi) * 1.3f, seed + 51 + (uint32_t)s), nB = Fbm(std::cos(psi) * 2.1f, std::sin(psi) * 2.1f + 9, seed + 61 + (uint32_t)s);
                const float w = R.rollWidth * (0.8f + 0.4f * nA), T = R.rollThick * (0.7f + 0.6f * nB), c = xEnd - w * 0.32f + (nB - 0.5f) * 0.012f;
                const float tilt = (nA - 0.5f) * 0.25f; // the roll is not square to the arm
                const float crease = Fbm(std::cos(psi) * 6.0f, std::sin(psi) * 6.0f, seed + 71 + (uint32_t)s); // pinched spots around the roll
                const float rin = rim[k] - 0.0015f;
                float arc = 0;
                V3 prev{};
                for (int j = 0; j < M; ++j) {
                    const float t = j * 2 * kPi / M, ct = std::cos(t), st = std::sin(t);
                    const float x = c + 0.5f * w * (ct >= 0 ? 1.0f : -1.0f) * std::pow(std::fabs(ct), 0.35f);
                    float rr = rin + 0.5f * T + 0.5f * T * (st >= 0 ? 1.0f : -1.0f) * std::pow(std::fabs(st), 0.6f);
                    if (st > 0) { // the outer face: three turns of cloth, each edge a step, pinched where the roll creases
                        const float q = (x - c) / w + 0.5f; // 0 at the elbow side .. 1 at the hand side
                        rr += 0.0018f * std::sin(Clamp01(q) * 3 * kPi) * std::sin(Clamp01(q) * 3 * kPi) - 0.0012f * Clamp01(q);
                        rr -= T * 0.45f * std::pow(Clamp01((crease - 0.55f) * 3.0f), 2.0f);
                    }
                    const V3 P = S + D * (x + tilt * (rr - rin) * 4.0f + tilt * 0.012f * std::sin(psi)) + Rd * rr;
                    g.At(k, j) = P;
                    g.ref[(size_t)k * M + j] = Norm(Rd * st + D * ct);
                    if (j) arc += Len(P - prev);
                    prev = P;
                    g.U[(size_t)k * M + j] = psi * rimMean;
                    g.V[(size_t)k * M + j] = arc;
                    g.ao[(size_t)k * M + j] = st < -0.3f ? 0.7f : 1.0f;
                }
            }
            Emit(cloth, g, s == 0 ? C_CuffL : C_CuffR, skinOf);
        }

    // pushes a point out of the shirt body along the nearest shirt normal
    PointGrid clothGrid;
    clothGrid.Build(C.p, 0.02f);
    const auto pushOut = [&](V3 p, float gap) {
        const int k = clothGrid.Nearest(p);
        if (k < 0) return p;
        const float d = Dot(p - C.p[(size_t)k], C.n[(size_t)k]);
        return d < gap ? p + C.n[(size_t)k] * (gap - d) : p;
    };

    // ---- collar
    {
        struct NP { float psi, y, r; };
        std::vector<NP> pts;
        for (size_t i = 0; i < N; ++i) {
            if (C.reason[i] != R_Neck) continue;
            const V3 p = C.p[i];
            float psi = std::atan2(p.x, p.z - cut.neckZ);
            if (psi < 0) psi += 2 * kPi;
            pts.push_back({psi, p.y, std::sqrt(p.x * p.x + (p.z - cut.neckZ) * (p.z - cut.neckZ))});
        }
        if (pts.size() >= 8) {
            std::sort(pts.begin(), pts.end(), [](const NP& a, const NP& b) { return a.psi < b.psi; });
            // the opening at the front: the biggest gap in psi (wrapping) is where the collar's ends are
            float gapMax = 0; size_t gapAt = 0;
            for (size_t k = 0; k < pts.size(); ++k) {
                const float next = k + 1 < pts.size() ? pts[k + 1].psi : pts[0].psi + 2 * kPi;
                if (next - pts[k].psi > gapMax) { gapMax = next - pts[k].psi; gapAt = k; }
            }
            std::rotate(pts.begin(), pts.begin() + (std::ptrdiff_t)((gapAt + 1) % pts.size()), pts.end());
            for (size_t k = 1; k < pts.size(); ++k) while (pts[k].psi < pts[k - 1].psi) pts[k].psi += 2 * kPi;
            const float psi0 = pts.front().psi, psi1 = pts.back().psi;
            constexpr int NI = 80;
            const int nStand = 4, nFold = 6, nLeaf = 8, NJ = nStand + nFold + nLeaf;
            Grid g;
            g.Resize(NI, NJ);
            float arcU = 0;
            V3 prevBase{};
            std::vector<float> ry(NI), rr(NI);
            for (int i = 0; i < NI; ++i) { // resampled neckline (window-averaged)
                const float psi = Lerp(psi0, psi1, i / (float)(NI - 1));
                float sy = 0, sr = 0, sw = 0;
                for (const NP& q : pts) { const float d = (q.psi - psi) / 0.08f; const float w = std::exp(-d * d); sy += q.y * w; sr += q.r * w; sw += w; }
                ry[(size_t)i] = sw > 0 ? sy / sw : pts[0].y; rr[(size_t)i] = sw > 0 ? sr / sw : pts[0].r;
            }
            for (int i = 0; i < NI; ++i) {
                const float tI = i / (float)(NI - 1);
                const float psi = Lerp(psi0, psi1, tI);
                const V3 o{std::sin(psi), 0, std::cos(psi)}, upv{0, 1, 0};
                const V3 base{o.x * rr[(size_t)i], ry[(size_t)i], cut.neckZ + o.z * rr[(size_t)i]};
                const float e = Smooth(0.72f, 1.0f, std::fabs(2 * tI - 1)); // toward the front ends
                const float standH = R.collarStand * (1.0f - (R.openButtons > 0 ? 0.55f : 0.3f) * e);
                const V3 sd = Norm(upv - o * (0.18f - (R.openButtons > 0 ? 0.6f : 0.0f) * e)); // open: the front ends fall back onto the chest
                const float rf = 0.0035f;
                const float leafLen = R.collarLeaf + e * e * 0.04f, leafOut = 0.008f + 0.022f * R.collarSpread + e * 0.025f * R.collarSpread * (R.openButtons > 0 ? 1.0f : 0.4f);
                if (i) arcU += Len(base - prevBase);
                prevBase = base;
                float arcV = 0;
                V3 prevP{};
                for (int j = 0; j < NJ; ++j) {
                    V3 P;
                    if (j < nStand) P = base + o * (R.thickness * 0.5f) + sd * (standH * j / (float)(nStand - 1) * 0.92f);
                    else if (j < nStand + nFold) {
                        const float al = kPi * (j - nStand + 1) / (float)nFold;
                        const V3 top = base + sd * standH;
                        P = top + o * (rf * (1 - std::cos(al))) + upv * (rf * std::sin(al) * 0.8f);
                    } else {
                        const float s = (j - nStand - nFold + 1) / (float)nLeaf;
                        const V3 top = base + sd * standH + o * (2 * rf);
                        P = top + o * (leafOut * std::pow(s, 0.8f)) - upv * (leafLen * s);
                        P = pushOut(P, R.thickness + 0.0025f);
                    }
                    g.At(i, j) = P;
                    g.ref[(size_t)i * NJ + j] = Norm(o + upv * 0.5f);
                    if (j) arcV += Len(P - prevP);
                    prevP = P;
                    g.U[(size_t)i * NJ + j] = arcU;
                    g.V[(size_t)i * NJ + j] = arcV;
                    g.ao[(size_t)i * NJ + j] = j < nStand ? 0.8f : 1.0f;
                }
            }
            Emit(cloth, g, C_Collar, skinOf);
            for (float& a : g.ao) a *= 0.88f;
            Emit(cloth, g, C_Collar, skinOf, {}, true, -0.0012f); // the underside
        }
    }

    // ---- front pieces: placket, pockets, flaps, buttons
    FrontCaster fc;
    fc.Build(C);
    const float yVisible = R.tucked ? waistY - 0.015f : cut.hemY;
    const float yTopFront = cut.v ? cut.vBottom : cut.neckFront;
    // a raised patch on the shirt front: outline x(t) per row, rows from y0 to y1; raised by `lift`, with walls down to the shirt
    const auto patch = [&](int chart, float yLo, float yHi, const std::function<void(float, float&, float&)>& span, float lift, float aoTop) {
        constexpr int NI = 24, NJ = 8;
        Grid top;
        top.Resize(NI, NJ);
        int misses = 0;
        std::vector<uint8_t> hit((size_t)NI * NJ, 0);
        for (int i = 0; i < NI; ++i) {
            const float y = Lerp(yLo, yHi, i / (float)(NI - 1));
            float xa, xb;
            span(y, xa, xb);
            for (int j = 0; j < NJ; ++j) {
                const float x = Lerp(xa, xb, j / (float)(NJ - 1));
                V3 p{x, y, 0}, n{0, 0, 1};
                if (fc.Hit(x, y, p, n)) hit[(size_t)i * NJ + j] = 1;
                else ++misses;
                top.At(i, j) = p + n * lift;
                top.ref[(size_t)i * NJ + j] = n;
                top.U[(size_t)i * NJ + j] = x; top.V[(size_t)i * NJ + j] = -y;
                top.ao[(size_t)i * NJ + j] = aoTop;
            }
        }
        if (misses * 10 > NI * NJ) return; // mostly off the shirt: not this piece
        for (int i = 0; i < NI; ++i) // a few misses (at an edge): take the nearest hit in the row
            for (int j = 0; j < NJ; ++j) {
                if (hit[(size_t)i * NJ + j]) continue;
                for (int d = 1; d < NJ; ++d) {
                    const int a = j - d, b = j + d;
                    const int k = a >= 0 && hit[(size_t)i * NJ + a] ? a : b < NJ && hit[(size_t)i * NJ + b] ? b : -1;
                    if (k < 0) continue;
                    const V3 q = top.At(i, k);
                    top.At(i, j) = {top.U[(size_t)i * NJ + j], q.y, q.z};
                    top.ref[(size_t)i * NJ + j] = top.ref[(size_t)i * NJ + k];
                    break;
                }
            }
        Emit(cloth, top, chart, skinOf);
        // walls around the outline: two rows (on the shirt, raised)
        std::vector<std::pair<int, int>> ring;
        for (int j = 0; j < NJ; ++j) ring.push_back({0, j});
        for (int i = 1; i < NI; ++i) ring.push_back({i, NJ - 1});
        for (int j = NJ - 2; j >= 0; --j) ring.push_back({NI - 1, j});
        for (int i = NI - 2; i >= 1; --i) ring.push_back({i, 0});
        Grid wall;
        wall.Resize((int)ring.size(), 2);
        wall.wrapI = true;
        V3 mid{};
        for (const V3& p : top.P) mid += p;
        mid = mid * (1.0f / (float)top.P.size());
        for (size_t k = 0; k < ring.size(); ++k) {
            const size_t gi = (size_t)ring[k].first * NJ + ring[k].second;
            const V3 n = top.ref[gi];
            wall.P[k * 2] = top.P[gi] - n * (lift - 0.0002f);
            wall.P[k * 2 + 1] = top.P[gi];
            const V3 outv = Norm(top.P[gi] - mid);
            wall.ref[k * 2] = wall.ref[k * 2 + 1] = Norm(outv - n * Dot(outv, n));
            wall.U[k * 2] = wall.U[k * 2 + 1] = top.U[gi];
            wall.V[k * 2] = top.V[gi]; wall.V[k * 2 + 1] = top.V[gi] + lift;
            wall.ao[k * 2] = 0.55f; wall.ao[k * 2 + 1] = 0.85f;
        }
        Emit(cloth, wall, chart, skinOf);
    };
    const float plW = 0.016f;
    if (yTopFront > yVisible + 0.02f) patch(C_Placket, yVisible, yTopFront, [&](float, float& a, float& b) { a = -plW; b = plW; }, 0.0016f, 1.0f);
    if (cut.v) { // the two opened edges of the V, faced
        patch(C_Placket, cut.vBottom, cut.vTopY - 0.004f, [&](float y, float& a, float& b) { a = cut.VHalf(y) + 0.004f; b = a + 2 * plW * 0.9f; }, 0.0014f, 0.95f);
        patch(C_Placket, cut.vBottom, cut.vTopY - 0.004f, [&](float y, float& a, float& b) { b = -cut.VHalf(y) - 0.004f; a = b - 2 * plW * 0.9f; }, 0.0010f, 0.9f);
    }
    // buttons
    const auto button = [&](float x, float y, float lift) {
        V3 p, n;
        if (!fc.Hit(x, y, p, n)) return;
        const V3 c = p + n * lift;
        const V3 t1 = Norm(Cross(n, std::fabs(n.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0})), t2 = Cross(n, t1);
        const float rad = 0.0056f, hgt = 0.0024f;
        constexpr int K = 16;
        Grid g;
        g.Resize(K, 5);
        g.wrapI = true;
        const float prof[5][2] = {{1.0f, 0.0f}, {1.0f, 0.75f}, {0.82f, 1.0f}, {0.45f, 0.9f}, {0.0f, 1.08f}}; // radius, height (dished centre)
        for (int k = 0; k < K; ++k) {
            const float a = k * 2 * kPi / K;
            const V3 rd = t1 * std::cos(a) + t2 * std::sin(a);
            for (int j = 0; j < 5; ++j) {
                g.At(k, j) = c + rd * (rad * prof[j][0]) + n * (hgt * prof[j][1]);
                g.ref[(size_t)k * 5 + j] = Norm(rd * (j < 2 ? 1.0f : 0.25f) + n * (j < 2 ? 0.0f : 1.0f));
                g.ao[(size_t)k * 5 + j] = j == 0 ? 0.6f : 1.0f;
            }
        }
        Emit(buttons, g, 0, skinOf);
    };
    for (float y = yTopFront - 0.012f; y > yVisible + 0.01f; y -= 0.082f) button(0.0f, y, 0.0016f);
    for (int k = 0; k < R.openButtons; ++k) {
        const float y = cut.vBottom + 0.012f + 0.068f * (float)k;
        if (y < cut.vTopY - 0.01f) button(-cut.VHalf(y) - plW * 0.9f, y, 0.0010f);
    }
    if (R.pockets) {
        const float topY = B.clav.y - 0.105f, hgt = 0.125f, wid = 0.112f;
        for (int s = 0; s < 2; ++s) {
            const float sx = s == 0 ? 1.0f : -1.0f, cx = sx * 0.088f;
            patch(s == 0 ? C_PocketL : C_PocketR, topY - hgt, topY, [&](float y, float& a, float& b) {
                const float taper = Smooth(topY - hgt + 0.02f, topY - hgt, y) * 0.012f; // rounded bottom corners
                a = cx - wid * 0.5f + taper; b = cx + wid * 0.5f - taper;
            }, 0.0012f, 0.97f);
            const float flapH = 0.040f, tip = 0.013f;
            patch(s == 0 ? C_FlapL : C_FlapR, topY - flapH - tip, topY + 0.004f, [&](float y, float& a, float& b) {
                const float yb = topY - flapH - tip; // pointed: the flap's bottom edge runs down to a point at the centre
                const float t = Clamp01((y - yb) / tip);
                const float half = (wid * 0.5f + 0.003f) * (y < topY - flapH ? t : 1.0f);
                a = cx - std::max(half, 0.0015f); b = cx + std::max(half, 0.0015f);
            }, 0.0030f, 1.0f);
            button(cx, topY - flapH - tip * 0.25f, 0.0030f);
        }
    }

    // ---- atlas
    for (const OutVert& v : cloth.v) {
        Chart& ch = at.charts[v.chart];
        ch.used = true;
        ch.u0 = std::min(ch.u0, v.u); ch.u1 = std::max(ch.u1, v.u); ch.v0 = std::min(ch.v0, v.v); ch.v1 = std::max(ch.v1, v.v);
    }
    if (!Pack(at)) { res.error = "the atlas does not fit"; return res; }
    for (OutVert& v : cloth.v) {
        const Chart& ch = at.charts[v.chart];
        v.u = (ch.ox + at.pad + (v.u - ch.u0) * at.ppm) / at.size;
        v.v = (ch.oy + at.pad + (v.v - ch.v0) * at.ppm) / at.size;
    }
    TexCtx tc{&R, &B, {cut.sleeve * B.armLen[0], cut.sleeve * B.armLen[1]}, waistY};
    std::vector<uint8_t> albedo, normalMap;
    MakeTextures(at, tc, albedo, normalMap);

    // ---- files
    std::wstring dir = in.outDir;
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    MakeDirs(dir);
    const std::wstring albedoPath = dir + L"albedo.png", normalPath = dir + L"normal.png";
    std::vector<uint8_t> albedoPng, normalPng;
    if (!WritePngRgba(albedoPath, albedo.data(), at.size, at.size) || !WritePngRgba(normalPath, normalMap.data(), at.size, at.size) ||
        !ReadBytes(albedoPath, albedoPng) || !ReadBytes(normalPath, normalPng)) { res.error = "cannot write the textures to " + Narrow(dir); return res; }
    R.Save(dir + L"recipe.json");

    // ---- the body skin the shirt leaves showing (forearms, hands, the V, the neck): an M_Skin part like the other clothes
    std::vector<OutVert> skinPart;
    std::vector<uint8_t> skinImg, skinNrm;
    if (B.skinPart >= 0) {
        const ImportedPart& sp = B.model.parts[(size_t)B.skinPart];
        for (size_t t = sp.first; t + 2 < sp.first + sp.count; t += 3) {
            bool show = false;
            for (int k = 0; k < 3; ++k) show |= bodyCls[B.weld[t + k]] != R_None;
            if (!show) continue;
            for (int k = 0; k < 3; ++k) {
                const Vertex& v = B.flat[t + k];
                OutVert o;
                o.p = {v.px, v.py, v.pz}; o.n = {v.nx, v.ny, v.nz}; o.u = v.u; o.v = v.v; o.skin = B.flatSkin[t + k];
                skinPart.push_back(o);
            }
        }
        if (sp.albedo >= 0 && sp.albedo < (int)B.model.images.size()) skinImg = B.model.images[(size_t)sp.albedo];
        if (sp.normal >= 0 && sp.normal < (int)B.model.images.size()) skinNrm = B.model.images[(size_t)sp.normal];
    }

    // ---- GLB
    Glb g;
    const bool skinned = !B.model.joints.empty();
    std::string prims;
    const auto prim = [&](const std::vector<OutVert>& vs, int material, bool color) {
        if (vs.empty()) return;
        std::vector<float> pos, nrm, uv, col, wts;
        std::vector<uint16_t> jts;
        std::vector<uint32_t> idx;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t k = 0; k < vs.size(); ++k) {
            const OutVert& v = vs[k];
            pos.insert(pos.end(), {v.p.x, v.p.y, v.p.z});
            const V3 n = Norm(v.n);
            nrm.insert(nrm.end(), {n.x, n.y, n.z});
            uv.insert(uv.end(), {v.u, v.v});
            const float a = std::clamp(v.ao, 0.0f, 1.0f);
            col.insert(col.end(), {a, a, a, 1.0f});
            for (int c = 0; c < 4; ++c) { jts.push_back(v.skin.joints[c]); wts.push_back(v.skin.weights[c]); }
            float sw = wts[wts.size() - 1] + wts[wts.size() - 2] + wts[wts.size() - 3] + wts[wts.size() - 4];
            if (sw < 1e-6f) { wts[wts.size() - 4] = 1.0f; }
            idx.push_back((uint32_t)k);
            lo[0] = std::min(lo[0], v.p.x); lo[1] = std::min(lo[1], v.p.y); lo[2] = std::min(lo[2], v.p.z);
            hi[0] = std::max(hi[0], v.p.x); hi[1] = std::max(hi[1], v.p.y); hi[2] = std::max(hi[2], v.p.z);
        }
        const std::string mm = ",\"min\":[" + F(lo[0]) + "," + F(lo[1]) + "," + F(lo[2]) + "],\"max\":[" + F(hi[0]) + "," + F(hi[1]) + "," + F(hi[2]) + "]";
        const int aP = g.Acc(pos.data(), vs.size(), 5126, "VEC3", 3, 34962, mm), aN = g.Acc(nrm.data(), vs.size(), 5126, "VEC3", 3, 34962);
        const int aT = g.Acc(uv.data(), vs.size(), 5126, "VEC2", 2, 34962);
        std::string attrs = "\"POSITION\":" + std::to_string(aP) + ",\"NORMAL\":" + std::to_string(aN) + ",\"TEXCOORD_0\":" + std::to_string(aT);
        if (color) attrs += ",\"COLOR_0\":" + std::to_string(g.Acc(col.data(), vs.size(), 5126, "VEC4", 4, 34962));
        if (skinned) {
            attrs += ",\"JOINTS_0\":" + std::to_string(g.Acc(jts.data(), vs.size(), 5123, "VEC4", 4, 34962));
            attrs += ",\"WEIGHTS_0\":" + std::to_string(g.Acc(wts.data(), vs.size(), 5126, "VEC4", 4, 34962));
        }
        const int aI = g.Acc(idx.data(), idx.size(), 5125, "SCALAR", 1, 34963);
        if (!prims.empty()) prims += ",";
        prims += "{\"attributes\":{" + attrs + "},\"indices\":" + std::to_string(aI) + ",\"material\":" + std::to_string(material) + ",\"mode\":4}";
    };
    std::vector<std::string> images, textures;
    const auto image = [&](const std::vector<uint8_t>& bytes) {
        const bool png = bytes.size() > 4 && bytes[0] == 0x89 && bytes[1] == 'P';
        const int v = g.View(bytes.data(), bytes.size(), 0);
        images.push_back("{\"bufferView\":" + std::to_string(v) + ",\"mimeType\":\"" + (png ? "image/png" : "image/jpeg") + "\"}");
        textures.push_back("{\"sampler\":0,\"source\":" + std::to_string(images.size() - 1) + "}");
        return (int)textures.size() - 1;
    };
    const int tA = image(albedoPng), tN = image(normalPng);
    std::string mats = "{\"name\":\"M_made_shirt\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":" + std::to_string(tA) +
                       "},\"metallicFactor\":0,\"roughnessFactor\":0.86},\"normalTexture\":{\"index\":" + std::to_string(tN) + "}}";
    mats += ",{\"name\":\"M_horn_buttons\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[" + F(R.button[0]) + "," + F(R.button[1]) + "," + F(R.button[2]) + ",1],\"metallicFactor\":0,\"roughnessFactor\":0.38}}";
    prim(cloth.v, 0, true);
    prim(buttons.v, 1, true);
    if (!skinPart.empty()) {
        std::string skinMat = "{\"name\":\"M_Skin\",\"pbrMetallicRoughness\":{";
        if (!skinImg.empty()) skinMat += "\"baseColorTexture\":{\"index\":" + std::to_string(image(skinImg)) + "},";
        skinMat += "\"metallicFactor\":0,\"roughnessFactor\":0.6}";
        if (!skinNrm.empty()) skinMat += ",\"normalTexture\":{\"index\":" + std::to_string(image(skinNrm)) + "}";
        skinMat += "}";
        mats += "," + skinMat;
        prim(skinPart, 2, false);
    }
    std::string nodes, scene, skins;
    const int JN = (int)B.model.joints.size();
    if (skinned) {
        std::vector<std::vector<int>> kids((size_t)JN);
        std::vector<int> roots;
        bool rootXf = false;
        const SkinJoint* firstRoot = nullptr;
        for (int k = 0; k < JN; ++k) {
            const SkinJoint& j = B.model.joints[(size_t)k];
            if (j.parent >= 0) kids[(size_t)j.parent].push_back(k);
            else { roots.push_back(k); if (!firstRoot) firstRoot = &j; }
        }
        if (firstRoot) for (int e = 0; e < 16; ++e) rootXf |= std::fabs(firstRoot->rootParent[e] - ((e % 5) == 0 ? 1.0f : 0.0f)) > 1e-6f;
        std::vector<float> ibm;
        for (int k = 0; k < JN; ++k) {
            const SkinJoint& j = B.model.joints[(size_t)k];
            std::string kidList;
            for (int c : kids[(size_t)k]) kidList += (kidList.empty() ? "" : ",") + std::to_string(c);
            if (!nodes.empty()) nodes += ",";
            nodes += "{\"name\":\"" + j.name + "\",\"translation\":[" + F(j.t[0]) + "," + F(j.t[1]) + "," + F(j.t[2]) + "],\"rotation\":[" + F(j.q[0]) + "," + F(j.q[1]) + "," + F(j.q[2]) + "," + F(j.q[3]) +
                     "],\"scale\":[" + F(j.s[0]) + "," + F(j.s[1]) + "," + F(j.s[2]) + "]" + (kidList.empty() ? "" : ",\"children\":[" + kidList + "]") + "}";
            ibm.insert(ibm.end(), j.inverseBind, j.inverseBind + 16);
        }
        std::string rootList;
        for (int r : roots) rootList += (rootList.empty() ? "" : ",") + std::to_string(r);
        int next = JN;
        if (rootXf) {
            std::string m;
            for (int e = 0; e < 16; ++e) m += (e ? "," : "") + F(firstRoot->rootParent[e]);
            nodes += ",{\"name\":\"Armature\",\"matrix\":[" + m + "],\"children\":[" + rootList + "]}";
            scene = std::to_string(next++);
        } else scene = rootList;
        nodes += ",{\"name\":\"" + R.name + "\",\"mesh\":0,\"skin\":0}";
        scene += "," + std::to_string(next);
        std::string jl;
        for (int k = 0; k < JN; ++k) jl += (k ? "," : "") + std::to_string(k);
        skins = ",\"skins\":[{\"joints\":[" + jl + "],\"inverseBindMatrices\":" + std::to_string(g.Acc(ibm.data(), (size_t)JN, 5126, "MAT4", 16, 0)) + "}]";
    } else {
        nodes = "{\"name\":\"" + R.name + "\",\"mesh\":0}";
        scene = "0";
    }
    const auto join = [](const std::vector<std::string>& v) { std::string s; for (const auto& x : v) s += (s.empty() ? "" : ",") + x; return s; };
    while (g.bin.size() % 4) g.bin.push_back(0);
    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"TBD Clothes Maker\"},\"scene\":0,\"scenes\":[{\"nodes\":[" + scene + "]}],\"nodes\":[" + nodes +
                       "],\"meshes\":[{\"name\":\"" + R.name + "\",\"primitives\":[" + prims + "]}],\"materials\":[" + mats + "],\"textures\":[" + join(textures) +
                       "],\"images\":[" + join(images) + "],\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":10497,\"wrapT\":10497}]" + skins +
                       ",\"accessors\":[" + join(g.accessors) + "],\"bufferViews\":[" + join(g.views) + "],\"buffers\":[{\"byteLength\":" + std::to_string(g.bin.size()) + "}]}";
    while (json.size() % 4) json += ' ';
    const std::wstring glbPath = dir + meshimport_detail::Utf8ToWide(R.name) + L".glb";
    FILE* f = _wfopen(glbPath.c_str(), L"wb");
    if (!f) { res.error = "cannot write " + Narrow(glbPath); return res; }
    const auto u32 = [&](uint32_t x) { fwrite(&x, 4, 1, f); };
    u32(0x46546C67); u32(2); u32((uint32_t)(12 + 8 + json.size() + 8 + g.bin.size()));
    u32((uint32_t)json.size()); u32(0x4E4F534A); fwrite(json.data(), 1, json.size(), f);
    u32((uint32_t)g.bin.size()); u32(0x004E4942); fwrite(g.bin.data(), 1, g.bin.size(), f);
    fclose(f);

    res.ok = true;
    res.glbPath = Narrow(glbPath);
    res.triangles = (cloth.v.size() + buttons.v.size()) / 3;
    res.ms = ms();
    char sum[320];
    snprintf(sum, sizeof(sum), "%zu cloth tris, %zu button tris, %zu skin tris; atlas %d px at %.0f px/m; chest bridged at %zu points (up to %.1f mm); tuck pull lines on %zu points (up to %.1f mm); %s",
             cloth.v.size() / 3, buttons.v.size() / 3, skinPart.size() / 3, at.size, at.ppm, chestN, chestMax * 1000.0f, pullN, pullMax * 1000.0f, jeansNote.c_str());
    res.summary = sum;
    return res;
}

} // namespace tbd::character::clothes

// ---------------------------------------------------------------- the cloth builder: Blender headless, run as a child process
namespace tbd::character::clothes {
namespace {
std::mutex gProgMu;
std::string gProg;
void SetProgress(const std::string& p) { std::lock_guard<std::mutex> l(gProgMu); gProg = p; }
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string NoTag(const std::string& p) { const size_t h = p.find('#'); return h == std::string::npos ? p : p.substr(0, h); }
} // namespace

std::string ClothProgress() { std::lock_guard<std::mutex> l(gProgMu); return gProg; }

BuildResult BuildShirtCloth(const ShirtRecipe& R, const BuildInput& in) {
    const auto t0 = std::chrono::steady_clock::now();
    BuildResult res;
    SetProgress("Building the shirt: starting Blender");
    std::wstring blender = L"C:\\Program Files\\Blender Foundation\\Blender 5.2\\blender.exe";
    if (wchar_t b[MAX_PATH]; GetEnvironmentVariableW(L"TBD_BLENDER", b, MAX_PATH) > 0) blender = b;
    if (GetFileAttributesW(blender.c_str()) == INVALID_FILE_ATTRIBUTES) { res.error = "Blender 5.2 is not installed (set TBD_BLENDER to blender.exe)"; return res; }
    if (GetFileAttributesW(in.script.c_str()) == INVALID_FILE_ATTRIBUTES) { res.error = "missing " + Narrow(in.script); return res; }
    MakeDirs(in.outDir);
    const std::wstring recipe = in.outDir + L"\\recipe_in.json";
    if (!R.Save(recipe)) { res.error = "cannot write " + Narrow(recipe); return res; }
    const auto q = [](const std::wstring& v) { return L"\"" + v + L"\""; };
    std::wstring cmd = q(blender) + L" -b --factory-startup --python " + q(in.script) + L" -- --recipe " + q(recipe) +
                       L" --body " + q(Widen(in.bodyGlb)) + L" --out " + q(in.outDir);
    if (!in.legsGlb.empty()) cmd += L" --legs " + q(Widen(in.legsGlb));
    if (!in.headGlb.empty()) cmd += L" --head " + q(Widen(in.headGlb));
    if (!in.jeansGlb.empty()) cmd += L" --jeans " + q(Widen(NoTag(in.jeansGlb)));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { res.error = "cannot create a pipe"; return res; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    const std::wstring cwd = in.script.substr(0, in.script.find_last_of(L"\\/"));
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, cwd.c_str(), &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        res.error = "cannot start Blender";
        return res;
    }
    CloseHandle(wr);
    std::string buf, glb, fail;
    std::vector<std::string> tail;
    char chunk[4096];
    DWORD got = 0;
    const auto handle = [&](const std::string& l) {
        if (l.rfind("PROGRESS ", 0) == 0) {
            const float f = (float)atof(l.c_str() + 9);
            const size_t sp = l.find(' ', 9);
            char b[256];
            snprintf(b, sizeof(b), "Building the shirt (%d%%): %s", (int)std::lround(f * 100), sp == std::string::npos ? "" : l.c_str() + sp + 1);
            SetProgress(b);
        } else if (l.rfind("RESULT ok ", 0) == 0) {
            const std::string rest = l.substr(10);
            const size_t sp = rest.rfind(' ');
            glb = sp == std::string::npos ? rest : rest.substr(0, sp);
            if (sp != std::string::npos) res.triangles = (size_t)atoll(rest.c_str() + sp + 1);
        } else if (l.rfind("RESULT fail", 0) == 0) {
            fail = l.substr(11);
        }
        if (l.find("Error") != std::string::npos || l.find("Traceback") != std::string::npos) {
            tail.push_back(l);
            if (tail.size() > 6) tail.erase(tail.begin());
        }
    };
    while (ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        buf.append(chunk, got);
        for (size_t nl; (nl = buf.find('\n')) != std::string::npos;) {
            std::string l = buf.substr(0, nl);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            handle(l);
            buf.erase(0, nl + 1);
        }
    }
    if (!buf.empty()) handle(buf);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for (char& c : glb) if (c == '/') c = '\\';
    if (glb.empty() || GetFileAttributesW(Widen(glb).c_str()) == INVALID_FILE_ATTRIBUTES) {
        res.error = !fail.empty() ? fail : (!tail.empty() ? tail.back() : "Blender exited with code " + std::to_string(code));
        SetProgress("Build failed: " + res.error);
        return res;
    }
    res.ok = true;
    res.glbPath = glb;
    char b[200];
    snprintf(b, sizeof(b), "sewn, simulated and baked in Blender: %zu triangles, plus the skin it leaves showing", res.triangles);
    res.summary = b;
    SetProgress("Built.");
    return res;
}

} // namespace tbd::character::clothes
