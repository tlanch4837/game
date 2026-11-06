// The material lab: a grid of spheres, each with a full procedural map set (albedo, normal, height, cavity, roughness, metalness,
// baked occlusion, emissive), drawn through the engine's own mesh shader so what it shows is what a world asset gets.
// TBD_MATLAB=1 puts it at the TBD_SHOT target; it exists to check the material path on both APIs and to show what each map does.
// Maps are authored here in linear light and stored in display space, as the shader expects (it applies pow 2.2 itself).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render/renderer.h"
#include "surface_maps.h"

namespace tbd::material_lab {

constexpr int kSize = 512;

// One authored pixel.
struct Texel {
    float c[3] = {0.5f, 0.5f, 0.5f}; // albedo, linear
    float rough = 0.6f, metal = 0.0f, ao = 1.0f, emis = 0.0f, height = 0.5f, weather = 0.0f;
};

struct Maps {
    std::vector<uint8_t> albedo, normal, surface, extra; // RGBA, kSize square
};

inline float Hash2(int x, int y, int seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFFF) / 16777215.0f;
}
inline float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }
inline float Mix(float a, float b, float t) { return a + (b - a) * t; }
inline float Step(float a, float b, float x) { return Smooth(std::clamp((x - a) / (b - a), 0.0f, 1.0f)); }
inline int Wrap(int i, int p) { return ((i % p) + p) % p; }

// Value noise that tiles with the given integer period over u, v in [0, 1).
inline float Noise(float u, float v, int px, int py, int seed) {
    const float x = u * px, y = v * py;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = Smooth(x - x0), fy = Smooth(y - y0);
    const auto at = [&](int i, int j) { return Hash2(Wrap(i, px), Wrap(j, py), seed); };
    return Mix(Mix(at(x0, y0), at(x0 + 1, y0), fx), Mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
}
inline float Fbm(float u, float v, int period, int octaves, int seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < octaves; ++o, period *= 2, amp *= 0.5f) {
        sum += amp * Noise(u, v, period, period, seed + o * 31);
        norm += amp;
    }
    return sum / norm;
}

struct Cell {
    float d1 = 9, d2 = 9; // distance to the nearest and second nearest feature point, in cells
    int id = 0;
};
// Tiling Worley noise over a period x period grid.
inline Cell Voronoi(float u, float v, int period, int seed) {
    const float x = u * period, y = v * period;
    const int cx = (int)std::floor(x), cy = (int)std::floor(y);
    Cell best;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            const int gx = cx + i, gy = cy + j, wx = Wrap(gx, period), wy = Wrap(gy, period);
            const float px = gx + 0.15f + 0.7f * Hash2(wx, wy, seed), py = gy + 0.15f + 0.7f * Hash2(wx, wy, seed + 7);
            const float d = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
            if (d < best.d1) { best.d2 = best.d1; best.d1 = d; best.id = wy * period + wx; }
            else if (d < best.d2) best.d2 = d;
        }
    return best;
}

inline uint8_t Byte(float v) { return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); }

// Bakes a material: `f(u, v)` gives each texel; the normal map and cavity come from the height field.
template <class F>
Maps Bake(F&& f, float normalStrength) {
    const int n = kSize;
    std::vector<Texel> t((size_t)n * n);
    std::vector<float> height((size_t)n * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            t[(size_t)y * n + x] = f((x + 0.5f) / n, (y + 0.5f) / n);
            height[(size_t)y * n + x] = t[(size_t)y * n + x].height;
        }
    const std::vector<float> around = surface_maps::Blur(height, n, n / 64);
    Maps m;
    m.albedo.resize((size_t)n * n * 4);
    m.normal.resize((size_t)n * n * 4);
    m.surface.resize((size_t)n * n * 4);
    m.extra.resize((size_t)n * n * 4);
    const float strength = normalStrength * n / 256.0f;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const size_t k = (size_t)y * n + x;
            const Texel& e = t[k];
            const float dx = height[(size_t)y * n + (x + 1) % n] - height[(size_t)y * n + (x + n - 1) % n];
            const float dy = height[(size_t)((y + 1) % n) * n + x] - height[(size_t)((y + n - 1) % n) * n + x];
            const float vx = -dx * strength, vy = dy * strength, len = 1.0f / std::sqrt(vx * vx + vy * vy + 1.0f);
            uint8_t* a = &m.albedo[k * 4];
            for (int c = 0; c < 3; ++c) a[c] = Byte(std::pow(std::clamp(e.c[c], 0.0f, 1.0f), 1.0f / 2.2f));
            a[3] = 255;
            uint8_t* nm = &m.normal[k * 4];
            nm[0] = Byte(vx * len * 0.5f + 0.5f); nm[1] = Byte(vy * len * 0.5f + 0.5f); nm[2] = Byte(len * 0.5f + 0.5f); nm[3] = 255;
            uint8_t* s = &m.surface[k * 4]; // the layout surface_maps.h writes: R roughness / 2, G cavity, B 0 (a real map), A height
            s[0] = Byte(e.rough * 0.5f);
            s[1] = Byte(1.0f - (around[k] - e.height) * 3.0f);
            s[2] = 0;
            s[3] = Byte(e.height);
            uint8_t* x4 = &m.extra[k * 4]; // R occlusion, G weathering, B metalness, A emissive
            x4[0] = Byte(e.ao); x4[1] = Byte(e.weather); x4[2] = Byte(e.metal); x4[3] = Byte(e.emis);
        }
    return m;
}

inline void Rgb(Texel& t, float r, float g, float b) { t.c[0] = r; t.c[1] = g; t.c[2] = b; }
inline void Tint(Texel& t, float r, float g, float b, float k) { for (int c = 0; c < 3; ++c) t.c[c] = Mix(t.c[c], c == 0 ? r : c == 1 ? g : b, k); }

// --------------------------------------------------------------------------------------------- the materials
inline Texel Planks(float u, float v) {
    Texel t;
    const int boards = 6;
    const float row = v * boards;
    const int b = (int)std::floor(row);
    const float inBoard = row - b;
    const float shift = Hash2(b, 3, 11); // each board starts at its own place along its length
    const float grain = Fbm(u, inBoard * 0.12f + b * 0.173f, 3, 5, 3 + b) * 0.5f + Noise(u + shift, inBoard, 2, 48, 5 + b) * 0.5f;
    const float edge = std::min(Step(0.0f, 0.09f, inBoard), Step(0.0f, 0.09f, 1.0f - inBoard));
    const float tone = 0.7f + 0.6f * Hash2(b, 9, 2);
    Rgb(t, 0.30f * tone, 0.18f * tone, 0.095f * tone);
    for (int c = 0; c < 3; ++c) t.c[c] *= Mix(0.6f, 1.25f, grain);
    Tint(t, 0.38f, 0.37f, 0.35f, 0.25f * Step(0.4f, 0.8f, Fbm(u, v, 4, 4, 77))); // sun-bleached patches
    t.height = edge * (0.78f + 0.22f * grain);
    t.rough = Mix(0.62f, 0.9f, grain);
    t.ao = Mix(0.25f, 1.0f, edge);
    for (int c = 0; c < 3; ++c) t.c[c] *= Mix(0.35f, 1.0f, edge);
    return t;
}

inline Texel Brick(float u, float v) {
    Texel t;
    const int rows = 8, per = 4;
    const float ry = v * rows;
    const int row = (int)std::floor(ry);
    const float fy = ry - row;
    const float x = u * per + (row & 1 ? 0.5f : 0.0f);
    const int col = (int)std::floor(x);
    const float fx = x - col;
    const float mortarX = 0.045f, mortarY = 0.09f;
    const float inX = std::min(Step(mortarX, mortarX + 0.03f, fx), Step(mortarX, mortarX + 0.03f, 1.0f - fx));
    const float inY = std::min(Step(mortarY, mortarY + 0.05f, fy), Step(mortarY, mortarY + 0.05f, 1.0f - fy));
    const float brick = inX * inY;
    const float id = Hash2(Wrap(col, per), row, 21);
    const float speck = Noise(u, v, 96, 96, 4) * 0.6f + Noise(u, v, 24, 24, 9) * 0.4f;
    Rgb(t, Mix(0.30f, 0.46f, id), Mix(0.085f, 0.13f, id), Mix(0.06f, 0.085f, id));
    for (int c = 0; c < 3; ++c) t.c[c] *= Mix(0.65f, 1.3f, speck);
    Tint(t, 0.07f, 0.05f, 0.04f, 0.5f * Step(0.82f, 1.0f, Hash2(Wrap(col, per), row, 5)) * Step(0.0f, 1.0f, Noise(u, v, 12, 12, 8))); // overfired, dark ends
    const float mortarTone = Mix(0.28f, 0.42f, Noise(u, v, 64, 64, 6));
    for (int c = 0; c < 3; ++c) t.c[c] = Mix(mortarTone * (c == 2 ? 0.88f : 1.0f), t.c[c], brick);
    t.height = Mix(0.12f, 0.75f + 0.25f * speck, brick);
    t.rough = Mix(0.95f, Mix(0.7f, 0.9f, speck), brick);
    t.ao = Mix(0.3f, 1.0f, brick);
    return t;
}

inline Texel Cobble(float u, float v) {
    Texel t;
    const Cell cell = Voronoi(u, v, 6, 31);
    const float edgeDist = cell.d2 - cell.d1; // zero on the line between two stones
    const float stone = Step(0.02f, 0.2f, edgeDist);
    const float id = Hash2(cell.id, 4, 17);
    const float speck = Noise(u, v, 128, 128, 2) * 0.5f + Noise(u, v, 32, 32, 3) * 0.5f;
    const float grey = Mix(0.12f, 0.3f, id);
    Rgb(t, grey * 1.04f, grey, grey * 0.9f);
    Tint(t, 0.3f, 0.19f, 0.1f, 0.3f * Hash2(cell.id, 8, 3)); // some stones are rust-stained
    for (int c = 0; c < 3; ++c) t.c[c] *= Mix(0.6f, 1.3f, speck);
    const float grout = Mix(0.07f, 0.12f, Noise(u, v, 48, 48, 9));
    for (int c = 0; c < 3; ++c) t.c[c] = Mix(grout * (c == 0 ? 1.1f : 1.0f), t.c[c], stone);
    const float dome = 1.0f - std::pow(std::clamp(cell.d1 * 1.6f, 0.0f, 1.0f), 2.0f) * 0.35f;
    t.height = Mix(0.05f, dome * Mix(0.85f, 1.0f, speck), stone);
    t.rough = Mix(0.95f, Mix(0.55f, 0.85f, speck), stone);
    t.ao = Mix(0.2f, 1.0f, stone);
    return t;
}

inline Texel RustyIron(float u, float v) {
    Texel t;
    const float blotch = Fbm(u, v, 3, 6, 12);
    const float rust = Step(0.46f, 0.62f, blotch + 0.15f * (Noise(u, v, 40, 40, 4) - 0.5f));
    const float pit = Noise(u, v, 90, 90, 7);
    const float scratch = Step(0.9f, 1.0f, Noise(u, v, 4, 220, 3)) * (1.0f - rust);
    // clean metal: grey, bright, tight lobe; rust: orange-brown, dull, not metal at all
    Rgb(t, 0.56f, 0.57f, 0.58f);
    for (int c = 0; c < 3; ++c) t.c[c] *= Mix(0.85f, 1.1f, Noise(u, v, 24, 24, 5)) + scratch * 0.12f;
    const float rustTone = Noise(u, v, 30, 30, 15);
    const float rr = Mix(0.18f, 0.42f, rustTone), rg = Mix(0.055f, 0.14f, rustTone), rb = Mix(0.03f, 0.06f, rustTone);
    for (int c = 0; c < 3; ++c) t.c[c] = Mix(t.c[c], c == 0 ? rr : c == 1 ? rg : rb, rust);
    t.metal = 1.0f - rust;
    t.rough = Mix(Mix(0.3f, 0.45f, Noise(u, v, 32, 32, 2)), Mix(0.8f, 1.0f, pit), rust);
    t.height = Mix(0.78f, 0.35f + 0.4f * pit, rust);
    t.ao = Mix(1.0f, Mix(0.55f, 1.0f, pit), rust);
    return t;
}

inline Texel Gold(float u, float v) {
    Texel t;
    const Cell dent = Voronoi(u, v, 14, 51);
    const float bowl = std::clamp(dent.d1 * 1.5f, 0.0f, 1.0f);
    Rgb(t, 1.0f, 0.77f, 0.34f);
    for (int c = 0; c < 3; ++c) t.c[c] *= Mix(0.9f, 1.0f, Noise(u, v, 20, 20, 3));
    t.metal = 1.0f;
    t.rough = Mix(0.13f, 0.26f, Noise(u, v, 40, 40, 6) * 0.6f + bowl * 0.4f); // polished by hand: not one value
    t.height = 1.0f - bowl * bowl * 0.5f; // a hammered finish
    t.ao = Mix(0.82f, 1.0f, bowl);
    return t;
}

inline Texel ChippedPaint(float u, float v) {
    Texel t;
    const float field = Fbm(u, v, 4, 6, 3) + 0.25f * (Noise(u, v, 70, 70, 9) - 0.5f);
    const float chip = Step(0.6f, 0.64f, field); // 1 where the paint is gone
    const float edge = Step(0.56f, 0.62f, field) * (1.0f - chip); // thin lip of paint at the break
    // paint: red enamel; under it, bare grey steel with a film of rust at the edges
    Rgb(t, 0.62f, 0.045f, 0.035f);
    for (int c = 0; c < 3; ++c) t.c[c] *= Mix(0.9f, 1.05f, Noise(u, v, 16, 16, 4));
    float steel[3] = {0.48f, 0.49f, 0.5f};
    const float film = Step(0.0f, 1.0f, Noise(u, v, 36, 36, 8)) * 0.6f;
    steel[0] = Mix(steel[0], 0.3f, film); steel[1] = Mix(steel[1], 0.13f, film); steel[2] = Mix(steel[2], 0.07f, film);
    for (int c = 0; c < 3; ++c) t.c[c] = Mix(t.c[c], steel[c], chip);
    t.metal = chip * (1.0f - film);
    t.rough = Mix(0.3f, Mix(0.45f, 0.9f, film), chip);
    t.height = Mix(0.82f + 0.04f * edge, 0.5f, chip);
    t.ao = Mix(1.0f, 0.8f, chip);
    return t;
}

inline Texel BrushedSteel(float u, float v) {
    Texel t;
    const float streak = Noise(u, v, 3, 300, 5) * 0.6f + Noise(u, v, 6, 140, 6) * 0.4f; // long along u
    Rgb(t, 0.58f, 0.59f, 0.6f);
    for (int c = 0; c < 3; ++c) t.c[c] *= Mix(0.85f, 1.1f, streak);
    t.metal = 1.0f;
    t.rough = Mix(0.28f, 0.5f, streak);
    t.height = 0.5f + 0.5f * streak * 0.5f;
    return t;
}

inline Texel Lantern(float u, float v) {
    Texel t;
    const int panes = 2;
    const float fu = u * panes - std::floor(u * panes), fv = v * panes - std::floor(v * panes);
    const float bar = 0.09f;
    const float glass = std::min(Step(bar, bar + 0.03f, fu), std::min(Step(bar, bar + 0.03f, 1.0f - fu), std::min(Step(bar, bar + 0.03f, fv), Step(bar, bar + 0.03f, 1.0f - fv))));
    const float flame = std::exp(-((fu - 0.5f) * (fu - 0.5f) + (fv - 0.55f) * (fv - 0.55f)) * 9.0f); // the glow is brightest behind the flame
    const float soot = Noise(u, v, 28, 28, 3);
    Rgb(t, 1.0f, 0.56f, 0.16f);
    for (int c = 0; c < 3; ++c) t.c[c] = Mix(0.035f * (c == 0 ? 1.4f : 1.0f) * Mix(0.7f, 1.3f, soot), t.c[c] * Mix(0.75f, 1.0f, flame), glass);
    t.metal = 1.0f - glass;
    t.rough = Mix(0.55f, 0.1f, glass);
    t.emis = glass * Mix(0.45f, 1.0f, flame);
    t.height = Mix(1.0f, 0.35f, glass);
    t.ao = Mix(0.7f, 1.0f, glass);
    return t;
}

inline Texel Denim(float u, float v) {
    Texel t;
    const float g = 2.0f * 3.14159265f * 90.0f;
    const float warp = 0.5f + 0.5f * std::sin(g * (u + v) * 0.5f), weft = 0.5f + 0.5f * std::sin(g * (u - v) * 0.5f);
    const float thread = Mix(warp, weft, 0.5f);
    const float fade = Noise(u, v, 5, 5, 4) * 0.6f + Noise(u, v, 40, 40, 8) * 0.4f;
    Rgb(t, Mix(0.03f, 0.08f, fade), Mix(0.05f, 0.11f, fade), Mix(0.14f, 0.25f, fade)); // indigo, worn paler in patches
    for (int c = 0; c < 3; ++c) t.c[c] *= Mix(0.75f, 1.15f, thread) * Mix(0.9f, 1.1f, Noise(u, v, 256, 256, 2));
    t.height = thread;
    t.rough = 0.9f;
    return t;
}

inline Texel Leather(float u, float v) {
    Texel t;
    const Cell grain = Voronoi(u, v, 36, 61);
    const float crease = Step(0.04f, 0.22f, grain.d2 - grain.d1);
    const float wear = Fbm(u, v, 3, 5, 14);
    Rgb(t, Mix(0.2f, 0.085f, wear), Mix(0.1f, 0.04f, wear), Mix(0.05f, 0.022f, wear));
    for (int c = 0; c < 3; ++c) t.c[c] *= Mix(0.7f, 1.0f, crease) * Mix(0.9f, 1.1f, Noise(u, v, 64, 64, 3));
    t.height = Mix(0.2f, 0.8f, crease) * Mix(0.85f, 1.0f, wear);
    t.rough = Mix(0.4f, 0.65f, crease);
    t.ao = Mix(0.55f, 1.0f, crease);
    return t;
}

// --------------------------------------------------------------------------------------------- the scene
struct Entry {
    const char* name;
    Texel (*fn)(float, float);
    float normalStrength;
    float alpha; // the vertex alpha class: 0.91 = world PBR with metal bit, 0.61 cloth, 0.63 leather
};

inline const std::vector<Entry>& Entries() {
    static const std::vector<Entry> list = {
        {"Weathered planks", Planks, 3.0f, 0.91f},        {"Red brick", Brick, 3.0f, 0.91f},       {"Cobblestone", Cobble, 3.0f, 0.91f},
        {"Rusted iron", RustyIron, 2.5f, 0.91f},          {"Hammered gold", Gold, 2.0f, 0.91f},    {"Chipped red paint", ChippedPaint, 4.0f, 0.91f},
        {"Brushed steel", BrushedSteel, 1.5f, 0.91f},     {"Lantern glass", Lantern, 3.0f, 0.91f}, {"Indigo denim", Denim, 1.2f, 0.61f},
        {"Tooled leather", Leather, 3.0f, 0.63f},
    };
    return list;
}

struct Lab {
    struct Sphere {
        TextureId albedo = kNoTexture, normal = kNoTexture, surface = kNoTexture, extra = kNoTexture;
        MeshId mesh = kInvalidMesh;
    };
    std::vector<Sphere> spheres;
    MeshId slab = kInvalidMesh;
    bool built = false;

    static std::vector<Vertex> SphereMesh(float alpha, int u, int v) {
        std::vector<Vertex> out;
        const auto at = [&](int i, int j) {
            const float a = 6.2831853f * i / u, b = 3.1415927f * j / v;
            const float x = std::cos(a) * std::sin(b), y = std::cos(b), z = std::sin(a) * std::sin(b);
            Vertex p = {x * 0.5f, y * 0.5f, z * 0.5f, x, y, z, 1.0f, 1.0f, 1.0f, alpha};
            p.u = 2.0f * i / u; // twice around, once pole to pole: the tile stays near square at the equator
            p.v = (float)j / v;
            return p;
        };
        for (int j = 0; j < v; ++j)
            for (int i = 0; i < u; ++i) {
                const Vertex a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
                out.insert(out.end(), {a, d, c, a, c, b});
            }
        return out;
    }

    // Builds every texture and mesh; call once, outside a frame.
    void Build(Renderer& r) {
        if (built) return;
        built = true;
        for (const Entry& e : Entries()) {
            const Maps m = Bake(e.fn, e.normalStrength);
            Sphere s;
            s.albedo = r.CreateTexture(m.albedo.data(), kSize, kSize);
            s.normal = r.CreateTexture(m.normal.data(), kSize, kSize);
            if (e.alpha > 0.9f) { // only world PBR materials read the surface and extra maps
                s.surface = r.CreateTexture(m.surface.data(), kSize, kSize);
                s.extra = r.CreateTexture(m.extra.data(), kSize, kSize);
                r.SetSurfaceExtra(s.surface, s.extra);
            }
            const std::vector<Vertex> verts = SphereMesh(e.alpha, 96, 48);
            s.mesh = r.CreateMesh(verts.data(), (uint32_t)verts.size(), Topology::Triangles);
            spheres.push_back(s);
        }
        const float h = 4.0f, y = -0.5f;
        const auto slabVertex = [&](float x, float z) { return Vertex{x, y, z, 0, 1, 0, 0.30f, 0.30f, 0.32f, 0.9f}; };
        const Vertex slabV[6] = {slabVertex(-h * 1.6f, -h), slabVertex(-h * 1.6f, h), slabVertex(h * 1.6f, h), slabVertex(-h * 1.6f, -h), slabVertex(h * 1.6f, h), slabVertex(h * 1.6f, -h)};
        slab = r.CreateMesh(slabV, 6, Topology::Triangles);
    }

    // `center` is the middle of the grid; two rows of five, the first row in front.
    void Draw(Renderer& r, const Mat4& viewProj, Vec3 center) {
        const auto place = [&](float x, float y, float z) {
            Mat4 m = Mat4::Identity();
            m.At(0, 3) = center.x + x; m.At(1, 3) = center.y + y; m.At(2, 3) = center.z + z;
            return m;
        };
        r.SetMaterial(kNoTexture, kNoTexture);
        const Mat4 slabModel = place(0, 0, 0);
        r.Draw(slab, viewProj * slabModel, slabModel, true, true);
        for (size_t k = 0; k < spheres.size(); ++k) {
            const Sphere& s = spheres[k];
            const float col = (float)(k % 5) - 2.0f, row = (float)(k / 5);
            const Mat4 model = place(col * 1.3f, 0.0f, (row - 0.5f) * 1.5f);
            r.SetMaterial(s.albedo, s.normal, s.surface);
            r.Draw(s.mesh, viewProj * model, model, true, true);
        }
        r.SetMaterial(kNoTexture, kNoTexture);
    }
};

} // namespace tbd::material_lab
