// Photo fit (Character Creator, PHOTO FIT tab): photos of a person, a fixed set of easy-to-find pins marked on each photo, a camera
// solved per photo from those pins, and a software view of the head from that camera (wireframe over the photo, or shaded beside it).
// No window code here: creator.cpp draws and routes the mouse. Plan and phases: docs/photo_fit_plan.md (phase A: pins, B: camera).
//
// Pin positions on the head are the face tracker's 468 landmarks (face_fit.h) lifted onto the rest mesh, so every pin is a point a
// person can find on a face: eye corners, nose tip, mouth corners, lip centre, chin. Marks on a photo are stored as 0..1 of the image,
// so a photo can be shown at any size. Any subset of pins may be marked; the camera solve needs at least four.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../project.h"
#include "../render/renderer.h"

namespace tbd::character::photo {

constexpr int kPins = 30;     // the built-in pins
constexpr int kMaxPins = 50;  // plus up to 20 of the user's own
struct PinDef {
    const wchar_t* name;
    const wchar_t* hint;
    int mp; // index in the tracker's 468 landmarks
};
// Order = the order they are marked in (eyes, nose, mouth, brows, chin). Left / right are the PERSON's: their left is on your right
// in a front photo.
inline const PinDef& Pin(int i) {
    static const PinDef p[kPins] = {
        {L"Right eye, inner corner", L"the pink corner by the nose (their right = your left on a front photo)", 133},
        {L"Right eye, outer corner", L"where the upper and lower lid meet, temple side", 33},
        {L"Left eye, inner corner", L"the pink corner by the nose", 362},
        {L"Left eye, outer corner", L"where the upper and lower lid meet, temple side", 263},
        {L"Nose tip", L"the most forward point of the nose", 1},
        {L"Right nostril wing", L"outer edge where the nostril wing meets the cheek", 129},
        {L"Left nostril wing", L"outer edge where the nostril wing meets the cheek", 358},
        {L"Under the nose", L"where the septum meets the upper lip", 2},
        {L"Right mouth corner", L"where the upper and lower lip meet", 61},
        {L"Left mouth corner", L"where the upper and lower lip meet", 291},
        {L"Upper lip, centre", L"the dip at the top of the upper lip (Cupid's bow)", 0},
        {L"Lip meeting line, centre", L"where the lips touch, in the middle", 13},
        {L"Lower lip, centre", L"the lowest edge of the red of the lower lip", 17},
        {L"Right Cupid's bow peak", L"the top of the upper lip beside the centre dip", 37},
        {L"Left Cupid's bow peak", L"the top of the upper lip beside the centre dip", 267},
        {L"Right brow, inner end", L"where the eyebrow starts by the nose", 107},
        {L"Left brow, inner end", L"where the eyebrow starts by the nose", 336},
        {L"Right brow, outer end", L"the tail of the eyebrow", 46},
        {L"Left brow, outer end", L"the tail of the eyebrow", 276},
        {L"Chin, bottom", L"the lowest point of the chin", 152},
        {L"Right jaw corner", L"the corner of the jaw below the ear, where the jaw turns up", 172},
        {L"Left jaw corner", L"the corner of the jaw below the ear, where the jaw turns up", 397},
        {L"Right face edge at the ear", L"where the side of the face meets the front of the ear, level with the eyes", 234},
        {L"Left face edge at the ear", L"where the side of the face meets the front of the ear, level with the eyes", 454},
        {L"Right upper lid, centre", L"the middle of the upper eyelid edge (eye open)", 159},
        {L"Left upper lid, centre", L"the middle of the upper eyelid edge (eye open)", 386},
        {L"Right lower lid, centre", L"the middle of the lower eyelid edge", 145},
        {L"Left lower lid, centre", L"the middle of the lower eyelid edge", 374},
        {L"Nose bridge, between the eyes", L"the dip at the top of the nose, level with the inner eye corners", 168},
        {L"Hairline, centre", L"where the forehead skin meets the hair, in the middle (skip if hair covers it)", 10},
    };
    return p[std::clamp(i, 0, kPins - 1)];
}

// The pins on the head: the point on the rest mesh, and the nearest rest vertex so the point follows the morphs
// (now = headNow[vertex] + offset).
struct BasePins {
    bool ok = false;
    int count = kPins; // built-in pins, then the user's own
    float p[kMaxPins][3] = {};
    int vtx[kMaxPins] = {};
    float off[kMaxPins][3] = {};
    std::vector<std::wstring> custom; // names of pin kPins, kPins + 1, ...
    const wchar_t* Name(int i) const {
        if (i < kPins) return Pin(i).name;
        return (size_t)(i - kPins) < custom.size() ? custom[(size_t)(i - kPins)].c_str() : L"Custom pin";
    }
    const wchar_t* Hint(int i) const { return i < kPins ? Pin(i).hint : L"your own pin: its place on the head was picked by hand"; }
    void AttachOne(int i, const float* q, const std::vector<Vertex>& rest) {
        int best = 0;
        float bd = 1e30f;
        for (size_t v = 0; v < rest.size(); ++v) {
            const float dx = rest[v].px - q[0], dy = rest[v].py - q[1], dz = rest[v].pz - q[2], d = dx * dx + dy * dy + dz * dz;
            if (d < bd) { bd = d; best = (int)v; }
        }
        for (int k = 0; k < 3; ++k) p[i][k] = q[k];
        vtx[i] = best;
        off[i][0] = q[0] - rest[(size_t)best].px; off[i][1] = q[1] - rest[(size_t)best].py; off[i][2] = q[2] - rest[(size_t)best].pz;
    }
    void Attach(const float* lm468, const std::vector<Vertex>& rest) {
        ok = false;
        if (!lm468 || rest.empty()) return;
        for (int i = 0; i < kPins; ++i) AttachOne(i, lm468 + Pin(i).mp * 3, rest);
        ok = true;
    }
    // A pin of the user's own at a rest-mesh point; returns its index, or -1 when the list is full.
    int AddCustom(const std::wstring& name, const float* q, const std::vector<Vertex>& rest) {
        if (count >= kMaxPins || !ok) return -1;
        AttachOne(count, q, rest);
        custom.push_back(name);
        return count++;
    }
    void Now(const std::vector<Vertex>& cur, int i, float* out) const {
        const size_t v = (size_t)vtx[i];
        if (v < cur.size()) { out[0] = cur[v].px + off[i][0]; out[1] = cur[v].py + off[i][1]; out[2] = cur[v].pz + off[i][2]; }
        else { out[0] = p[i][0]; out[1] = p[i][1]; out[2] = p[i][2]; }
    }
};

struct Mark {
    float x = 0, y = 0;  // 0..1 of the image
    uint8_t state = 0;   // 0 not marked, 1 marked, 2 not visible in this photo
};

// A camera looking at the head: the head turned by yaw (about Y), pitch (about X) and roll (about the view axis), moved by tx, ty
// (mesh units, sideways and up) and placed `dist` in front of the camera. The lens is a field of view over the photo's longer side.
struct Cam {
    float yaw = 0, pitch = 0, roll = 0, tx = 0, ty = 0, dist = 0.6f, fov = 28.0f;
    bool ok = false;
    float rms = 0;                // pixels, over the marked pins
    float err[kMaxPins] = {};     // per pin, pixels (-1: not used)
};

inline float Focal(const Cam& c, int w, int h) { return 0.5f * (float)std::max(w, h) / std::tan(c.fov * 0.5f * 3.14159265f / 180.0f); }

// Mesh point -> image pixels (and the depth in front of the camera).
inline void Project(const Cam& c, int w, int h, const float* centre, const float* p, float* out) {
    const float x = p[0] - centre[0], y = p[1] - centre[1], z = p[2] - centre[2];
    const float cy = std::cos(c.yaw), sy = std::sin(c.yaw), cp = std::cos(c.pitch), sp = std::sin(c.pitch), cr = std::cos(c.roll), sr = std::sin(c.roll);
    const float x1 = cy * x + sy * z, z1 = -sy * x + cy * z;       // yaw
    const float y2 = cp * y - sp * z1, z2 = sp * y + cp * z1;      // pitch
    const float x3 = cr * x1 - sr * y2, y3 = sr * x1 + cr * y2;    // roll
    const float depth = c.dist - z2, f = Focal(c, w, h);
    const float inv = f / std::max(depth, 1e-4f);
    out[0] = 0.5f * (float)w + (x3 + c.tx) * inv;
    out[1] = 0.5f * (float)h - (y3 + c.ty) * inv;
    out[2] = depth;
}
// The rotated point (camera space before the shift), for shading.
inline void Rotate(const Cam& c, const float* centre, const float* p, float* out) {
    const float x = p[0] - centre[0], y = p[1] - centre[1], z = p[2] - centre[2];
    const float cy = std::cos(c.yaw), sy = std::sin(c.yaw), cp = std::cos(c.pitch), sp = std::sin(c.pitch), cr = std::cos(c.roll), sr = std::sin(c.roll);
    const float x1 = cy * x + sy * z, z1 = -sy * x + cy * z;
    const float y2 = cp * y - sp * z1, z2 = sp * y + cp * z1;
    out[0] = cr * x1 - sr * y2; out[1] = sr * x1 + cr * y2; out[2] = z2;
}

struct Photo {
    std::wstring path;
    std::string label = "front"; // front, 3/4, profile, other: shown only; the camera comes from the pins
    int w = 0, h = 0;            // decoded size (a long side over 2048 is shrunk when it is loaded)
    std::vector<uint8_t> rgba;
    Mark mark[kMaxPins];
    // where the photo sits in the head's frame (the frame is as big as the photo): scaled about its centre, then moved by a share of its size.
    // Marks stay relative to the photo; the camera is solved in the frame.
    float size = 1.0f, offx = 0.0f, offy = 0.0f;
    void ImgToFrame(float ix, float iy, float* fx, float* fy) const { *fx = (ix - 0.5f * (float)w) * size + 0.5f * (float)w + offx * (float)w; *fy = (iy - 0.5f * (float)h) * size + 0.5f * (float)h + offy * (float)h; }
    void FrameToImg(float fx, float fy, float* ix, float* iy) const { *ix = (fx - 0.5f * (float)w - offx * (float)w) / size + 0.5f * (float)w; *iy = (fy - 0.5f * (float)h - offy * (float)h) / size + 0.5f * (float)h; }
    void MarkPx(int i, float* x, float* y) const { ImgToFrame(mark[i].x * (float)w, mark[i].y * (float)h, x, y); }
    Cam cam;
    int Marked() const { int n = 0; for (const Mark& m : mark) n += m.state == 1; return n; }
};

// Removes one of the user's own pins (index >= kPins) and shifts the later ones down in the pin list and in every photo's marks.
inline void RemoveCustomPin(BasePins& bp, std::vector<Photo>& photos, int i) {
    if (i < kPins || i >= bp.count) return;
    for (int k = i; k + 1 < bp.count; ++k) {
        std::copy(bp.p[k + 1], bp.p[k + 1] + 3, bp.p[k]);
        std::copy(bp.off[k + 1], bp.off[k + 1] + 3, bp.off[k]);
        bp.vtx[k] = bp.vtx[k + 1];
        for (Photo& ph : photos) { ph.mark[k] = ph.mark[k + 1]; ph.cam.err[k] = ph.cam.err[k + 1]; }
    }
    for (Photo& ph : photos) { ph.mark[bp.count - 1] = Mark(); ph.cam.err[bp.count - 1] = -1; }
    bp.custom.erase(bp.custom.begin() + (i - kPins));
    --bp.count;
}

namespace detail {
// Residuals for the LM solve: (u, v) differences for every used pin, then two weak priors that keep pitch and roll sane when the pins
// leave the pose loose.
inline void Residuals(const Cam& c, int w, int h, const float* centre, const std::vector<std::array<float, 6>>& pins, std::vector<float>& r) {
    r.clear();
    for (const auto& q : pins) {
        float o[3];
        const float p[3] = {q[0], q[1], q[2]};
        Project(c, w, h, centre, p, o);
        r.push_back((o[0] - q[3]) * q[5]);
        r.push_back((o[1] - q[4]) * q[5]);
    }
    r.push_back(c.pitch * 25.0f);
    r.push_back(c.roll * 25.0f);
}
inline bool Solve6(double A[6][7]) { // Gauss-Jordan on the augmented 6x7 system
    for (int i = 0; i < 6; ++i) {
        int piv = i;
        for (int k = i + 1; k < 6; ++k) if (std::fabs(A[k][i]) > std::fabs(A[piv][i])) piv = k;
        if (std::fabs(A[piv][i]) < 1e-12) return false;
        if (piv != i) for (int j = 0; j < 7; ++j) std::swap(A[i][j], A[piv][j]);
        for (int k = 0; k < 6; ++k) {
            if (k == i) continue;
            const double f = A[k][i] / A[i][i];
            for (int j = i; j < 7; ++j) A[k][j] -= f * A[i][j];
        }
    }
    return true;
}
} // namespace detail

// Solves the photo's camera from its marked pins and the head's pin points (current positions, mesh space). `centre` is the point the
// head turns about (the middle of the face). Needs 4 marked pins; returns false (cam.ok = false) with fewer.
inline bool SolveCamera(Photo& ph, const BasePins& bp, const std::vector<Vertex>& head, const float* centre) {
    Cam& cam = ph.cam;
    for (float& e : cam.err) e = -1;
    cam.ok = false;
    if (!bp.ok || ph.w <= 0) return false;
    std::vector<std::array<float, 6>> pins; // x y z (mesh), u v (pixels)
    std::vector<int> ids;
    for (int i = 0; i < bp.count; ++i) {
        if (ph.mark[i].state != 1) continue;
        float p[3];
        bp.Now(head, i, p);
        float mpx, mpy;
        ph.MarkPx(i, &mpx, &mpy);
        pins.push_back({p[0], p[1], p[2], mpx, mpy, 1.0f});
        ids.push_back(i);
    }
    if (pins.size() < 4) return false;
    const float f = Focal(cam, ph.w, ph.h);
    Cam best;
    double bestCost = 1e300;
    std::vector<float> r, r2;
    // pass 0: several yaws (a 3/4 photo has no good guess from the label). pass 1: from the best, with pins that miss by far more than
    // the rest (a mark in the wrong place) nearly ignored, so one bad pin cannot drag the head off.
    for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1) {
        std::vector<float> miss;
        for (const auto& q : pins) { float o[3]; const float p[3] = {q[0], q[1], q[2]}; Project(best, ph.w, ph.h, centre, p, o); miss.push_back(std::hypot(o[0] - q[3], o[1] - q[4])); }
        std::vector<float> sorted = miss;
        std::sort(sorted.begin(), sorted.end());
        const float cut = std::max(3.0f * sorted[sorted.size() / 2], 6.0f);
        for (size_t k = 0; k < pins.size(); ++k) pins[k][5] = miss[k] > cut ? 0.05f : 1.0f;
        bestCost = 1e300;
    }
    for (int start = pass ? 0 : -4; start <= (pass ? 0 : 4); ++start) {
        Cam c = cam;
        c.yaw = (float)start * 0.35f; c.pitch = 0; c.roll = 0;
        // distance and shift from the spread of the marks against the spread of the pins (turned by this yaw)
        double mx = 0, my = 0, qx = 0, qy = 0;
        std::vector<std::array<float, 2>> rot;
        for (const auto& q : pins) { float o[3]; const float p[3] = {q[0], q[1], q[2]}; Rotate(c, centre, p, o); rot.push_back({o[0], o[1]}); qx += o[0]; qy += o[1]; mx += q[3]; my += q[4]; }
        const double n = (double)pins.size();
        qx /= n; qy /= n; mx /= n; my /= n;
        double s3 = 0, s2 = 0;
        for (size_t k = 0; k < pins.size(); ++k) { s3 += (rot[k][0] - qx) * (rot[k][0] - qx) + (rot[k][1] - qy) * (rot[k][1] - qy); s2 += (pins[k][3] - mx) * (pins[k][3] - mx) + (pins[k][4] - my) * (pins[k][4] - my); }
        c.dist = (float)(f * std::sqrt(s3 / std::max(s2, 1e-6)));
        c.tx = (float)((mx - 0.5 * ph.w) * c.dist / f - qx);
        c.ty = (float)(-(my - 0.5 * ph.h) * c.dist / f - qy);
        if (pass == 1) c = best;
        double lambda = 1e-2;
        detail::Residuals(c, ph.w, ph.h, centre, pins, r);
        double cost = 0;
        for (float v : r) cost += (double)v * v;
        for (int it = 0; it < 40; ++it) {
            float* par[6] = {&c.yaw, &c.pitch, &c.roll, &c.tx, &c.ty, &c.dist};
            const float step[6] = {1e-3f, 1e-3f, 1e-3f, 1e-4f, 1e-4f, 1e-3f};
            std::vector<std::vector<float>> J(6);
            for (int k = 0; k < 6; ++k) {
                const float keep = *par[k];
                *par[k] = keep + step[k]; detail::Residuals(c, ph.w, ph.h, centre, pins, r2);
                std::vector<float> rp = r2;
                *par[k] = keep - step[k]; detail::Residuals(c, ph.w, ph.h, centre, pins, r2);
                J[(size_t)k].resize(r.size());
                for (size_t q = 0; q < r.size(); ++q) J[(size_t)k][q] = (rp[q] - r2[q]) / (2 * step[k]);
                *par[k] = keep;
            }
            double A[6][7] = {};
            for (int a = 0; a < 6; ++a) {
                for (int b = 0; b < 6; ++b) { double s = 0; for (size_t q = 0; q < r.size(); ++q) s += (double)J[(size_t)a][q] * J[(size_t)b][q]; A[a][b] = s; }
                double g = 0; for (size_t q = 0; q < r.size(); ++q) g += (double)J[(size_t)a][q] * r[q];
                A[a][6] = -g;
            }
            double B[6][7];
            for (int a = 0; a < 6; ++a) for (int b = 0; b < 7; ++b) B[a][b] = A[a][b];
            for (int a = 0; a < 6; ++a) B[a][a] *= 1.0 + lambda;
            if (!detail::Solve6(B)) break;
            Cam t = c;
            float* tp[6] = {&t.yaw, &t.pitch, &t.roll, &t.tx, &t.ty, &t.dist};
            for (int a = 0; a < 6; ++a) *tp[a] += (float)(B[a][6] / B[a][a]);
            t.dist = std::max(t.dist, 0.05f);
            detail::Residuals(t, ph.w, ph.h, centre, pins, r2);
            double tc = 0;
            for (float v : r2) tc += (double)v * v;
            if (tc < cost) { c = t; cost = tc; r = r2; lambda = std::max(lambda * 0.4, 1e-6); if (std::sqrt(tc / (double)r.size()) < 1e-3) break; }
            else lambda *= 4.0;
        }
        if (cost < bestCost) { bestCost = cost; best = c; }
    }
    }
    cam = best;
    cam.ok = true;
    double sum = 0;
    for (size_t k = 0; k < pins.size(); ++k) {
        float o[3];
        const float p[3] = {pins[k][0], pins[k][1], pins[k][2]};
        Project(cam, ph.w, ph.h, centre, p, o);
        const float e = std::sqrt((o[0] - pins[k][3]) * (o[0] - pins[k][3]) + (o[1] - pins[k][4]) * (o[1] - pins[k][4]));
        cam.err[ids[k]] = e;
        sum += (double)e * e;
    }
    cam.rms = (float)std::sqrt(sum / (double)pins.size());
    return true;
}

// ---------------------------------------------------------------- the head seen from a photo's camera
// A layer the size of the pane: RGBA over the photo (wire) or opaque shaded (side by side). `map` takes image pixels to pane pixels.
struct View { float scale = 1, ox = 0, oy = 0; };
inline View FitView(int paneW, int paneH, int imgW, int imgH, float zoom, float cx, float cy) { // cx, cy: image point (0..1) at the pane's centre
    View v;
    const float fit = std::min((float)paneW / (float)std::max(1, imgW), (float)paneH / (float)std::max(1, imgH));
    v.scale = fit * zoom;
    v.ox = 0.5f * (float)paneW - cx * (float)imgW * v.scale;
    v.oy = 0.5f * (float)paneH - cy * (float)imgH * v.scale;
    return v;
}

struct Layer {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra; // premultiplied-free: a = coverage
};

// Wireframe (front-facing triangles only) or shaded fill. `tris` is a flat triangle list, 3 vertices each.
// `tex` (RGBA, tw x th): the head's own albedo, looked up at each pixel's UV when shaded, so the render shows the skin.
inline void RenderHead(const Photo& ph, const std::vector<Vertex>& tris, const float* centre, const View& vw, int paneW, int paneH, bool shaded, Layer& out,
                       const uint8_t* tex = nullptr, int tw = 0, int th = 0) {
    out.w = paneW; out.h = paneH;
    out.bgra.assign((size_t)paneW * paneH * 4, 0);
    if (!ph.cam.ok) return;
    std::vector<float> zbuf;
    if (shaded) zbuf.assign((size_t)paneW * paneH, 1e30f);
    const auto put = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
        if (x < 0 || y < 0 || x >= paneW || y >= paneH) return;
        uint8_t* p = &out.bgra[((size_t)y * paneW + x) * 4];
        if (a > p[3]) { p[0] = b; p[1] = g; p[2] = r; p[3] = a; }
    };
    const auto line = [&](float x0, float y0, float x1, float y1) {
        const float dx = x1 - x0, dy = y1 - y0;
        const int n = std::max(1, (int)std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
        if (n > 4000) return;
        for (int i = 0; i <= n; ++i) put((int)(x0 + dx * (float)i / (float)n), (int)(y0 + dy * (float)i / (float)n), 255, 170, 60, 255); // blue-ish (BGR)
    };
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        float s[3][3], rr[3][3];
        bool bad = false;
        for (int k = 0; k < 3; ++k) {
            const float p[3] = {tris[t + (size_t)k].px, tris[t + (size_t)k].py, tris[t + (size_t)k].pz};
            Project(ph.cam, ph.w, ph.h, centre, p, s[k]);
            Rotate(ph.cam, centre, p, rr[k]);
            s[k][0] = s[k][0] * vw.scale + vw.ox; s[k][1] = s[k][1] * vw.scale + vw.oy;
            if (s[k][2] < 0.02f) bad = true;
        }
        if (bad) continue;
        const float area = (s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[2][0] - s[0][0]) * (s[1][1] - s[0][1]);
        if (area >= 0) continue; // screen y goes down, triangles face the camera when their winding is clockwise here... see below
        const float minx = std::min({s[0][0], s[1][0], s[2][0]}), maxx = std::max({s[0][0], s[1][0], s[2][0]});
        const float miny = std::min({s[0][1], s[1][1], s[2][1]}), maxy = std::max({s[0][1], s[1][1], s[2][1]});
        if (maxx < 0 || maxy < 0 || minx > paneW || miny > paneH) continue;
        if (!shaded) {
            line(s[0][0], s[0][1], s[1][0], s[1][1]);
            line(s[1][0], s[1][1], s[2][0], s[2][1]);
            line(s[2][0], s[2][1], s[0][0], s[0][1]);
            continue;
        }
        // flat shade from the camera: facing it is bright
        const float e1[3] = {rr[1][0] - rr[0][0], rr[1][1] - rr[0][1], rr[1][2] - rr[0][2]}, e2[3] = {rr[2][0] - rr[0][0], rr[2][1] - rr[0][1], rr[2][2] - rr[0][2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-12f;
        const float lam = std::clamp((n[2] * 0.8f + n[1] * 0.3f - n[0] * 0.2f) / nl, 0.0f, 1.0f);
        const uint8_t g = (uint8_t)(45 + 190 * lam);
        const int x0 = std::max(0, (int)std::floor(minx)), x1 = std::min(paneW - 1, (int)std::ceil(maxx));
        const int y0 = std::max(0, (int)std::floor(miny)), y1 = std::min(paneH - 1, (int)std::ceil(maxy));
        const float inv = 1.0f / area;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = (float)x + 0.5f, py = (float)y + 0.5f;
                const float w0 = ((s[1][0] - px) * (s[2][1] - py) - (s[2][0] - px) * (s[1][1] - py)) * inv;
                const float w1 = ((s[2][0] - px) * (s[0][1] - py) - (s[0][0] - px) * (s[2][1] - py)) * inv;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = w0 * s[0][2] + w1 * s[1][2] + w2 * s[2][2];
                float& zb = zbuf[(size_t)y * paneW + x];
                if (z >= zb) continue;
                zb = z;
                uint8_t* p = &out.bgra[((size_t)y * paneW + x) * 4];
                if (tex && tw > 0 && th > 0) {
                    const float u = w0 * tris[t].u + w1 * tris[t + 1].u + w2 * tris[t + 2].u, v = w0 * tris[t].v + w1 * tris[t + 1].v + w2 * tris[t + 2].v;
                    const int tx = std::clamp((int)(u * (float)tw), 0, tw - 1), ty = std::clamp((int)(v * (float)th), 0, th - 1);
                    const uint8_t* q = &tex[((size_t)ty * tw + (size_t)tx) * 4];
                    const float sh = 0.30f + 0.70f * lam;
                    p[0] = (uint8_t)(q[2] * sh); p[1] = (uint8_t)(q[1] * sh); p[2] = (uint8_t)(q[0] * sh); p[3] = 255;
                } else {
                    p[0] = (uint8_t)(g * 0.92f); p[1] = (uint8_t)(g * 0.95f); p[2] = g; p[3] = 255;
                }
            }
    }
}


// ---------------------------------------------------------------- shape solve: the face sliders that bring the head's pins onto the marks
// The pins move linearly with the sliders (morphs are added deltas): pin k at values v is X0[k] + sum_i (v_i - v0_i) * D[(k*3 + c) * nS + i], where D is
// the pin's displacement for one unit of slider i (built by the caller from the head's own morph deform). Every photo keeps its own camera, solved
// again each round from the current pin positions; the sliders are shared. Robust (Huber) weights keep a wrong mark from bending the face, and a
// ridge toward the neutral values keeps depth that no photo constrains (a front photo says nothing about nose length) where it was.
namespace detail {
inline bool SolveSym(std::vector<double>& A, std::vector<double>& b, int n) { // Gaussian elimination with pivoting; b becomes the answer
    for (int i = 0; i < n; ++i) {
        int piv = i;
        for (int k = i + 1; k < n; ++k) if (std::fabs(A[(size_t)k * n + i]) > std::fabs(A[(size_t)piv * n + i])) piv = k;
        if (std::fabs(A[(size_t)piv * n + i]) < 1e-12) return false;
        if (piv != i) { for (int j = 0; j < n; ++j) std::swap(A[(size_t)i * n + j], A[(size_t)piv * n + j]); std::swap(b[(size_t)i], b[(size_t)piv]); }
        for (int k = i + 1; k < n; ++k) {
            const double f = A[(size_t)k * n + i] / A[(size_t)i * n + i];
            if (f == 0) continue;
            for (int j = i; j < n; ++j) A[(size_t)k * n + j] -= f * A[(size_t)i * n + j];
            b[(size_t)k] -= f * b[(size_t)i];
        }
    }
    for (int i = n - 1; i >= 0; --i) {
        double s = b[(size_t)i];
        for (int j = i + 1; j < n; ++j) s -= A[(size_t)i * n + j] * b[(size_t)j];
        b[(size_t)i] = s / A[(size_t)i * n + i];
    }
    return true;
}
} // namespace detail

struct ShapeResult {
    bool ok = false;
    float before = 0, after = 0; // average pin miss in pixels over all photos, before and after
    int photos = 0, residuals = 0;
};

inline ShapeResult FitShape(std::vector<Photo>& photos, const BasePins& bp, const std::vector<float>& D, int nS, std::vector<float>& v,
                            const std::vector<float>& lo, const std::vector<float>& hi, const float* centre, const float* X0, float prior = 20.0f) {
    ShapeResult res;
    const int n = bp.count;
    if (nS <= 0 || (int)v.size() != nS || D.size() < (size_t)n * 3 * (size_t)nS) return res;
    const std::vector<float> v0 = v;
    std::vector<float> X((size_t)n * 3);
    const auto place = [&](const std::vector<float>& vv) {
        for (int k = 0; k < n; ++k)
            for (int c = 0; c < 3; ++c) {
                double a = X0[k * 3 + c];
                const float* d = &D[(size_t)(k * 3 + c) * nS];
                for (int i = 0; i < nS; ++i) a += (double)(vv[(size_t)i] - v0[(size_t)i]) * d[i];
                X[(size_t)k * 3 + c] = (float)a;
            }
    };
    BasePins tmp;
    tmp.ok = true;
    tmp.count = n;
    for (int k = 0; k < n; ++k) { tmp.vtx[k] = k; tmp.off[k][0] = tmp.off[k][1] = tmp.off[k][2] = 0; }
    std::vector<Vertex> cur((size_t)n);
    const auto solveCams = [&]() {
        place(v);
        for (int k = 0; k < n; ++k) { cur[(size_t)k].px = X[(size_t)k * 3]; cur[(size_t)k].py = X[(size_t)k * 3 + 1]; cur[(size_t)k].pz = X[(size_t)k * 3 + 2]; }
        int used = 0;
        for (Photo& ph : photos) if (ph.Marked() >= 4 && SolveCamera(ph, tmp, cur, centre)) ++used;
        return used;
    };
    // miss of every marked pin in every photo at the current cameras, and the Huber weights from them
    struct Item { int photo, pin; float w; };
    std::vector<Item> items;
    const auto miss = [&](const std::vector<float>& vv, double* sumW, double* mean) {
        place(vv);
        double sw = 0, sm = 0;
        int cnt = 0;
        for (const Item& it : items) {
            const Photo& ph = photos[(size_t)it.photo];
            float o[3];
            const float p[3] = {X[(size_t)it.pin * 3], X[(size_t)it.pin * 3 + 1], X[(size_t)it.pin * 3 + 2]};
            Project(ph.cam, ph.w, ph.h, centre, p, o);
            float mpx, mpy;
            ph.MarkPx(it.pin, &mpx, &mpy);
            const float dx = o[0] - mpx, dy = o[1] - mpy;
            sw += (double)it.w * (dx * dx + dy * dy);
            sm += std::sqrt(dx * dx + dy * dy);
            ++cnt;
        }
        double pr = 0;
        for (int i = 0; i < nS; ++i) pr += (double)vv[(size_t)i] * vv[(size_t)i];
        *sumW = sw + (double)prior * pr;
        *mean = cnt ? sm / cnt : 0;
    };
    const auto collect = [&]() {
        items.clear();
        std::vector<float> errs;
        for (size_t pi = 0; pi < photos.size(); ++pi) {
            const Photo& ph = photos[pi];
            if (!ph.cam.ok) continue;
            for (int k = 0; k < n; ++k) if (ph.mark[k].state == 1 && ph.cam.err[k] >= 0) { items.push_back({(int)pi, k, 1.0f}); errs.push_back(ph.cam.err[k]); }
        }
        if (errs.empty()) return;
        std::vector<float> sorted = errs;
        std::sort(sorted.begin(), sorted.end());
        const float c = std::max(2.0f * sorted[sorted.size() / 2], 5.0f);
        for (size_t i = 0; i < items.size(); ++i) items[i].w = errs[i] <= c ? 1.0f : c / errs[i];
    };
    if (!solveCams()) return res;
    collect();
    if (items.empty()) return res;
    double cost = 0, mean = 0;
    miss(v, &cost, &mean);
    res.before = (float)mean;
    std::vector<int> photoSeen;
    for (const Item& it : items) if (std::find(photoSeen.begin(), photoSeen.end(), it.photo) == photoSeen.end()) photoSeen.push_back(it.photo);
    res.photos = (int)photoSeen.size();
    res.residuals = (int)items.size() * 2;
    for (int round = 0; round < 8; ++round) {
        // normal equations at the current pin positions and cameras
        std::vector<double> A((size_t)nS * nS, 0.0), b((size_t)nS, 0.0);
        place(v);
        std::vector<float> col((size_t)nS * 2);
        for (const Item& it : items) {
            const Photo& ph = photos[(size_t)it.photo];
            const float p[3] = {X[(size_t)it.pin * 3], X[(size_t)it.pin * 3 + 1], X[(size_t)it.pin * 3 + 2]};
            float o0[3];
            Project(ph.cam, ph.w, ph.h, centre, p, o0);
            float mpx, mpy;
            ph.MarkPx(it.pin, &mpx, &mpy);
            const float r0 = o0[0] - mpx, r1 = o0[1] - mpy;
            std::vector<int> act;
            const float h = 0.05f;
            for (int i = 0; i < nS; ++i) {
                const float* d0 = &D[(size_t)(it.pin * 3) * nS];
                const float dx = d0[i], dy = D[(size_t)(it.pin * 3 + 1) * nS + i], dz = D[(size_t)(it.pin * 3 + 2) * nS + i];
                if (std::fabs(dx) + std::fabs(dy) + std::fabs(dz) < 1e-7f) continue;
                const float q[3] = {p[0] + h * dx, p[1] + h * dy, p[2] + h * dz};
                float o1[3];
                Project(ph.cam, ph.w, ph.h, centre, q, o1);
                col[(size_t)i * 2] = (o1[0] - o0[0]) / h;
                col[(size_t)i * 2 + 1] = (o1[1] - o0[1]) / h;
                act.push_back(i);
            }
            for (int a : act) {
                b[(size_t)a] += it.w * (col[(size_t)a * 2] * r0 + col[(size_t)a * 2 + 1] * r1);
                for (int c : act) A[(size_t)a * nS + c] += it.w * (col[(size_t)a * 2] * col[(size_t)c * 2] + col[(size_t)a * 2 + 1] * col[(size_t)c * 2 + 1]);
            }
        }
        for (int i = 0; i < nS; ++i) { A[(size_t)i * nS + i] += prior; b[(size_t)i] += (double)prior * v[(size_t)i]; }
        bool accepted = false;
        double mu = 1e-3;
        for (int tries = 0; tries < 7 && !accepted; ++tries, mu *= 8) {
            std::vector<double> A2 = A, x((size_t)nS);
            for (int i = 0; i < nS; ++i) { A2[(size_t)i * nS + i] += mu * A[(size_t)i * nS + i] + 1e-9; x[(size_t)i] = -b[(size_t)i]; }
            if (!detail::SolveSym(A2, x, nS)) continue;
            std::vector<float> nv = v;
            for (int i = 0; i < nS; ++i) nv[(size_t)i] = std::clamp(v[(size_t)i] + std::clamp((float)x[(size_t)i], -0.35f, 0.35f), lo[(size_t)i], hi[(size_t)i]);
            double c2, m2;
            miss(nv, &c2, &m2);
            if (c2 < cost - 1e-6) { v = nv; cost = c2; mean = m2; accepted = true; }
        }
        if (!accepted) break;
        solveCams(); // the cameras follow the new face
        collect();
        miss(v, &cost, &mean);
    }
    solveCams();
    collect();
    double c3, m3;
    miss(v, &c3, &m3);
    res.after = (float)m3;
    res.ok = true;
    return res;
}

// ---------------------------------------------------------------- session file (photos by path, marks and lens)
inline std::string Esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 32) o += c; }
    return o;
}
inline std::string ToJson(const std::vector<Photo>& photos, int active, const BasePins& bp) {
    std::string j = "{\n \"format\": \"photo_fit\", \"version\": 2, \"active\": " + std::to_string(active) + ",\n \"custom\": [";
    for (int i = kPins; i < bp.count; ++i) {
        char b[160];
        snprintf(b, sizeof b, "%s{\"p\": [%.6f,%.6f,%.6f], \"name\": \"", i > kPins ? "," : "", bp.p[i][0], bp.p[i][1], bp.p[i][2]);
        j += b + Esc(WideToUtf8(bp.Name(i))) + "\"}";
    }
    j += "],\n \"photos\": [\n";
    for (size_t i = 0; i < photos.size(); ++i) {
        const Photo& p = photos[i];
        j += "  {\"path\": \"" + Esc(WideToUtf8(p.path)) + "\", \"label\": \"" + Esc(p.label) + "\", \"fov\": " + std::to_string(p.cam.fov) + ", \"size\": " + std::to_string(p.size) + ", \"offx\": " + std::to_string(p.offx) + ", \"offy\": " + std::to_string(p.offy) + ", \"marks\": [";
        for (int k = 0; k < bp.count; ++k) {
            char b[64];
            snprintf(b, sizeof b, "%s[%d,%.5f,%.5f]", k ? "," : "", (int)p.mark[k].state, p.mark[k].x, p.mark[k].y);
            j += b;
        }
        j += std::string("]}") + (i + 1 < photos.size() ? "," : "") + "\n";
    }
    return j + " ]\n}\n";
}

// The nearest front-facing triangle under a pane pixel (the same view the head is drawn in): its first vertex in `tris` and the
// barycentric weights of the point.
inline bool PickHead(const Photo& ph, const std::vector<Vertex>& tris, const float* centre, const View& vw, float x, float y, size_t* tri, float* bary) {
    if (!ph.cam.ok) return false;
    float bestZ = 1e30f;
    bool hit = false;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        float s[3][3];
        bool bad = false;
        for (int k = 0; k < 3; ++k) {
            const float p[3] = {tris[t + (size_t)k].px, tris[t + (size_t)k].py, tris[t + (size_t)k].pz};
            Project(ph.cam, ph.w, ph.h, centre, p, s[k]);
            s[k][0] = s[k][0] * vw.scale + vw.ox; s[k][1] = s[k][1] * vw.scale + vw.oy;
            if (s[k][2] < 0.02f) bad = true;
        }
        if (bad) continue;
        const float area = (s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[2][0] - s[0][0]) * (s[1][1] - s[0][1]);
        if (area >= 0) continue;
        const float inv = 1.0f / area;
        const float w0 = ((s[1][0] - x) * (s[2][1] - y) - (s[2][0] - x) * (s[1][1] - y)) * inv;
        const float w1 = ((s[2][0] - x) * (s[0][1] - y) - (s[0][0] - x) * (s[2][1] - y)) * inv;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        const float z = w0 * s[0][2] + w1 * s[1][2] + w2 * s[2][2];
        if (z < bestZ) { bestZ = z; *tri = t; bary[0] = w0; bary[1] = w1; bary[2] = w2; hit = true; }
    }
    return hit;
}

} // namespace tbd::character::photo
