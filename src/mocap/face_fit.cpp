#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "face_fit.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "face_tracker.h"

namespace tbd {
namespace {

constexpr int kSize = 768;

struct Px {
    float depth = -1e30f;
    float x = 0, y = 0, z = 0; // mesh-space position of the surface at this pixel (before any yaw)
    bool hit = false;
};

// Where the mesh sits in the picture: mesh-space point (cx, cy) is the image centre, `scale` pixels per unit.
struct View {
    float cx = 0, cy = 0, scale = 1;
};

void SampleTexture(const FitPart& p, float u, float v, float rgb[3]) {
    if (p.rgba.empty() || p.w <= 0) { rgb[0] = rgb[1] = rgb[2] = 0.78f; return; }
    u -= std::floor(u);
    v -= std::floor(v);
    const float fx = u * (p.w - 1), fy = v * (p.h - 1);
    const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, p.w - 1), y1 = std::min(y0 + 1, p.h - 1);
    const float tx = fx - x0, ty = fy - y0;
    for (int c = 0; c < 3; ++c) {
        auto at = [&](int x, int y) { return p.rgba[((size_t)y * p.w + x) * 4 + c] * (1.0f / 255.0f); };
        rgb[c] = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
    }
}

// Rasterises the mesh seen from the front (turned by yawRad about Y) with its textures, flat-lit, over a light
// background. Triangles outside every part are drawn untextured (a mouth interior added after the file's own mesh).
View RenderView(const std::vector<Vertex>& verts, const std::vector<FitPart>& parts, float yawRad, std::vector<Px>& img,
                std::vector<uint8_t>& bgra, const float* crop = nullptr) {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : verts) {
        const float p[3] = {v.px, v.py, v.pz};
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
    }
    // Neck and shoulders only waste pixels and confuse the detector: look at the upper 88% of the height.
    View view;
    view.cx = (lo[0] + hi[0]) * 0.5f;
    const float top = hi[1], bottom = lo[1] + (hi[1] - lo[1]) * 0.12f;
    const float span = std::max(hi[0] - lo[0], top - bottom) * 1.9f; // head fills about half the picture
    view.cy = (top + bottom) * 0.5f;
    view.scale = kSize / span;
    if (crop) { view.cx = crop[0]; view.cy = crop[1]; view.scale = kSize / crop[2]; }
    const float cz = (lo[2] + hi[2]) * 0.5f, cs = std::cos(yawRad), sn = std::sin(yawRad);

    img.assign((size_t)kSize * kSize, Px{});
    bgra.assign((size_t)kSize * kSize * 4, 255);
    for (size_t i = 0; i < (size_t)kSize * kSize; ++i) { // background: soft grey-blue
        bgra[i * 4] = 205; bgra[i * 4 + 1] = 196; bgra[i * 4 + 2] = 190;
    }

    // vertex positions after the yaw
    auto turn = [&](const Vertex& v, float& x, float& z, float& nz) {
        const float dx = v.px - view.cx, dz = v.pz - cz;
        x = view.cx + dx * cs + dz * sn;
        z = cz - dx * sn + dz * cs;
        nz = -v.nx * sn + v.nz * cs;
    };

    std::vector<char> covered(verts.size(), 0);
    auto rasterise = [&](const FitPart& part) {
        for (size_t t = part.first; t + 2 < part.first + part.count && t + 2 < verts.size(); t += 3) {
            covered[t] = covered[t + 1] = covered[t + 2] = 1;
            const Vertex* v[3] = {&verts[t], &verts[t + 1], &verts[t + 2]};
            float sx[3], sy[3], wz[3], wx[3], wnz[3];
            for (int k = 0; k < 3; ++k) {
                turn(*v[k], wx[k], wz[k], wnz[k]);
                sx[k] = kSize * 0.5f + (wx[k] - view.cx) * view.scale;
                sy[k] = kSize * 0.5f - (v[k]->py - view.cy) * view.scale;
            }
            const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (std::abs(area) < 1e-6f) continue;
            const int x0 = std::max(0, (int)std::floor(std::min({sx[0], sx[1], sx[2]})));
            const int x1 = std::min(kSize - 1, (int)std::ceil(std::max({sx[0], sx[1], sx[2]})));
            const int y0 = std::max(0, (int)std::floor(std::min({sy[0], sy[1], sy[2]})));
            const int y1 = std::min(kSize - 1, (int)std::ceil(std::max({sy[0], sy[1], sy[2]})));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float w1 = ((px - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (py - sy[0])) / area;
                    const float w2 = ((sx[1] - sx[0]) * (py - sy[0]) - (px - sx[0]) * (sy[1] - sy[0])) / area;
                    const float w0 = 1 - w1 - w2;
                    if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                    const float z = w0 * wz[0] + w1 * wz[1] + w2 * wz[2];
                    Px& d = img[(size_t)y * kSize + x];
                    if (z <= d.depth) continue;
                    d.depth = z;
                    d.hit = true;
                    d.x = w0 * v[0]->px + w1 * v[1]->px + w2 * v[2]->px;
                    d.y = w0 * v[0]->py + w1 * v[1]->py + w2 * v[2]->py;
                    d.z = w0 * v[0]->pz + w1 * v[1]->pz + w2 * v[2]->pz;
                    const float u = w0 * v[0]->u + w1 * v[1]->u + w2 * v[2]->u;
                    const float vv = w0 * v[0]->v + w1 * v[1]->v + w2 * v[2]->v;
                    float rgb[3];
                    SampleTexture(part, u, vv, rgb);
                    // texture times vertex color, as the engine's shader does (untextured: white times color)
                    if (part.rgba.empty()) rgb[0] = rgb[1] = rgb[2] = 1.0f;
                    rgb[0] *= w0 * v[0]->r + w1 * v[1]->r + w2 * v[2]->r;
                    rgb[1] *= w0 * v[0]->g + w1 * v[1]->g + w2 * v[2]->g;
                    rgb[2] *= w0 * v[0]->b + w1 * v[1]->b + w2 * v[2]->b;
                    const float nz = w0 * wnz[0] + w1 * wnz[1] + w2 * wnz[2];
                    const float l = 0.55f + 0.45f * std::clamp(std::abs(nz), 0.0f, 1.0f);
                    uint8_t* o = &bgra[((size_t)y * kSize + x) * 4];
                    o[0] = (uint8_t)std::clamp(rgb[2] * l * 255.0f, 0.0f, 255.0f);
                    o[1] = (uint8_t)std::clamp(rgb[1] * l * 255.0f, 0.0f, 255.0f);
                    o[2] = (uint8_t)std::clamp(rgb[0] * l * 255.0f, 0.0f, 255.0f);
                }
        }
    };
    if (parts.empty()) rasterise(FitPart{0, verts.size(), {}, 0, 0});
    else for (const FitPart& p : parts) rasterise(p);
    // anything past the file's own parts
    size_t firstUncovered = verts.size();
    for (size_t i = 0; i < verts.size(); i += 3)
        if (!covered[i]) { firstUncovered = i; break; }
    if (firstUncovered < verts.size()) rasterise(FitPart{firstUncovered, verts.size() - firstUncovered, {}, 0, 0});
    return view;
}

bool SaveBmp(const std::vector<uint8_t>& bgra, const wchar_t* path) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER bi{sizeof(bi), kSize, -kSize, 1, 32, BI_RGB};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + (DWORD)bgra.size();
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(bgra.data(), 1, bgra.size(), f);
    return fclose(f) == 0;
}

} // namespace

bool RenderHeadView(const std::vector<Vertex>& verts, const std::vector<FitPart>& parts, float yawRad, const wchar_t* bmpPath) {
    std::vector<Px> img;
    std::vector<uint8_t> bgra;
    RenderView(verts, parts, yawRad, img, bgra);
    return SaveBmp(bgra, bmpPath);
}

void RenderHeadBgra(const std::vector<Vertex>& verts, const std::vector<FitPart>& parts, float yawRad, std::vector<uint8_t>& bgra) {
    std::vector<Px> img;
    RenderView(verts, parts, yawRad, img, bgra);
}

void RenderHeadCrop(const std::vector<Vertex>& verts, const std::vector<FitPart>& parts, float yawRad, float cx, float cy, float span, std::vector<uint8_t>& bgra) {
    std::vector<Px> img;
    const float crop[3] = {cx, cy, span};
    RenderView(verts, parts, yawRad, img, bgra, crop);
}

bool FitFaceLandmarks(const std::vector<Vertex>& verts, const std::vector<FitPart>& parts, const wchar_t* modelDir,
                      float* lm, std::string* error, const wchar_t* debugBmp) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (verts.size() < 3) return fail("no mesh");
    std::vector<Px> img;
    std::vector<uint8_t> bgra;
    const View view = RenderView(verts, parts, 0.0f, img, bgra);

    FaceTracker tracker;
    std::string terr;
    if (!tracker.Init(modelDir, &terr)) return fail(("cannot load the face models: " + terr).c_str());
    FaceFrame ff;
    tracker.Process(bgra.data(), kSize, kSize, ff);
    if (!ff.valid) return fail("no face found on the rendered head");

    for (int i = 0; i < kFaceLandmarks; ++i) {
        const float px = ff.lm[i * 3] * kSize, py = ff.lm[i * 3 + 1] * kSize;
        // nearest covered pixel (landmarks on the silhouette edge can fall just outside the surface)
        const Px* best = nullptr;
        float bestD = 1e30f;
        for (int r = 0; r <= 6 && !best; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    const int x = (int)px + dx, y = (int)py + dy;
                    if (x < 0 || y < 0 || x >= kSize || y >= kSize || !img[(size_t)y * kSize + x].hit) continue;
                    const float d = (float)(dx * dx + dy * dy);
                    if (d < bestD) { bestD = d; best = &img[(size_t)y * kSize + x]; }
                }
        if (!best) return fail("face landmarks fell outside the head");
        // x,y straight from the image position, z from the surface under it
        lm[i * 3] = view.cx + (px - kSize * 0.5f) / view.scale;
        lm[i * 3 + 1] = view.cy - (py - kSize * 0.5f) / view.scale;
        // depth: the front-most surface close by. A landmark on the lips' seam can fall through the slit and read the
        // far wall of the mouth.
        float frontZ = best->z;
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = (int)px + dx, y = (int)py + dy;
                if (x < 0 || y < 0 || x >= kSize || y >= kSize || !img[(size_t)y * kSize + x].hit) continue;
                frontZ = std::max(frontZ, img[(size_t)y * kSize + x].z);
            }
        lm[i * 3 + 2] = frontZ;
    }

    if (debugBmp && *debugBmp) {
        for (int i = 0; i < kFaceLandmarks; ++i) {
            const int x = (int)(ff.lm[i * 3] * kSize), y = (int)(ff.lm[i * 3 + 1] * kSize);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (x + dx >= 0 && y + dy >= 0 && x + dx < kSize && y + dy < kSize) {
                        uint8_t* o = &bgra[((size_t)(y + dy) * kSize + x + dx) * 4];
                        o[0] = 60; o[1] = 255; o[2] = 90;
                    }
        }
        SaveBmp(bgra, debugBmp);
    }
    return true;
}

} // namespace tbd
