// Drives a rigid head mesh from the 52 tracked expression weights by building morph targets for it on the CPU.
//
// The rig is fitted from the head's own 468 face landmarks (see face_fit.h: the tracker is run on a render of the
// mesh). Every expression is a recipe over those landmarks, e.g. smile = mouth corners up and out, blink = upper lid
// down to meet the lower lid. A recipe moves its landmarks, and the skin around each follows with a Gaussian falloff of
// the distance ALONG the skin, so a lip's move never leaks across the mouth to the other lip. The jaw is a real bone:
// a rotation about a hinge near the ears, skinned with weights solved over the surface. Distances are in the mesh's
// own units, measured from the landmarks, so any head scale works.
//
// Mapping has two modes: Auto (every channel at gain 1, one overall strength) and Manual (per-channel gain and on/off).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../render/renderer.h"
#include "face_clip.h"
#include "face_fit.h"

namespace tbd {

enum class MapMode { Auto, Manual };

struct ChannelTune {
    bool enabled = true;
    float gain = 1;
};

struct FaceMapping {
    MapMode mode = MapMode::Auto;
    ChannelTune ch[kBlendshapes];

    bool Save(const wchar_t* path) const {
        FILE* f = _wfopen(path, L"w");
        if (!f) return false;
        fprintf(f, "TBDFACEMAP2 %d\n", mode == MapMode::Manual ? 1 : 0);
        for (int i = 0; i < kBlendshapes; ++i) fprintf(f, "%s %d %g\n", kBlendshapeNames[i], ch[i].enabled, ch[i].gain);
        return fclose(f) == 0;
    }

    bool Load(const wchar_t* path) {
        FILE* f = _wfopen(path, L"r");
        if (!f) return false;
        FaceMapping m;
        int manual = 0;
        bool ok = fscanf_s(f, "TBDFACEMAP2 %d", &manual) == 1;
        for (int i = 0; ok && i < kBlendshapes; ++i) {
            char name[64];
            int en;
            ok = fscanf_s(f, "%63s %d %f", name, (unsigned)sizeof name, &en, &m.ch[i].gain) == 3;
            m.ch[i].enabled = en != 0;
        }
        fclose(f);
        if (!ok) return false;
        m.mode = manual ? MapMode::Manual : MapMode::Auto;
        *this = m;
        return true;
    }
};

// Face regions the landmarks are labelled with (the editor colors and names them).
enum class FaceRegion { Outline, Brows, Eyes, Nose, Lips, Cheeks, Jaw, Count };
inline const char* FaceRegionName(FaceRegion r) {
    static const char* n[] = {"Face outline", "Brows", "Eyes", "Nose", "Lips", "Cheeks", "Jaw / chin"};
    return n[(int)r];
}

namespace facemap {

// MediaPipe face mesh landmark groups. "Left"/"Right" are the subject's, as in ARKit: the subject's left is +X on a
// head that faces +Z.
inline const int kUpperLip[] = {61, 185, 40, 39, 37, 0, 267, 269, 270, 409, 291, 78, 191, 80, 81, 82, 13, 312, 311, 310, 415, 308};
inline const int kLowerLip[] = {146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 61, 78, 95, 88, 178, 87, 14, 317, 402, 318, 324, 308};
inline const int kBrowR[] = {70, 63, 105, 66, 107, 46, 53, 52, 65, 55};
inline const int kBrowL[] = {300, 293, 334, 296, 336, 276, 283, 282, 295, 285};
inline const int kBrowInnerR[] = {107, 55, 66, 65};
inline const int kBrowInnerL[] = {336, 285, 296, 295};
inline const int kBrowOuterR[] = {70, 63, 46, 53, 52};
inline const int kBrowOuterL[] = {300, 293, 276, 283, 282};
inline const int kLidUpR[] = {246, 161, 160, 159, 158, 157, 173};
inline const int kLidLoR[] = {33, 7, 163, 144, 145, 153, 154, 155, 133};
inline const int kLidUpL[] = {466, 388, 387, 386, 385, 384, 398};
inline const int kLidLoL[] = {263, 249, 390, 373, 374, 380, 381, 382, 362};
inline const int kCheekR[] = {50, 101, 118, 117, 111, 116, 123, 205, 36};
inline const int kCheekL[] = {280, 330, 347, 346, 340, 345, 352, 425, 266};
inline const int kAlaR[] = {129, 98, 64, 102, 49};
inline const int kAlaL[] = {358, 327, 294, 331, 279};
inline const int kNose[] = {1, 2, 4, 5, 6, 168, 195, 197, 98, 327, 129, 358, 48, 278};
inline const int kChin[] = {152, 148, 176, 149, 150, 377, 400, 378, 379};
inline const int kJawLine[] = {234, 93, 132, 58, 172, 136, 150, 149, 176, 148, 152, 377, 400, 378, 379, 365, 397, 288, 361, 323, 454};
inline const int kCornerR[] = {61, 62, 76, 78};
inline const int kCornerL[] = {291, 292, 306, 308};
// the lips' outer lines and free inner edges, corners left out
inline const int kUpperOuter[] = {185, 40, 39, 37, 0, 267, 269, 270, 409};
inline const int kLowerOuter[] = {146, 91, 181, 84, 17, 314, 405, 321, 375};
inline const int kUpperInner[] = {191, 80, 81, 82, 13, 312, 311, 310, 415};
inline const int kLowerInner[] = {95, 88, 178, 87, 14, 317, 402, 318, 324};

template <size_t N> struct Span {
    const int* p;
    size_t n;
    constexpr Span(const int (&a)[N]) : p(a), n(N) {}
};

} // namespace facemap

// What the rig needs to know about the model beyond its triangles: where the skin ends and the eyeballs are.
struct RigExtras {
    std::vector<std::pair<size_t, size_t>> eyeRanges; // (first vertex, count) of each eye part
    size_t skinCount = 0;                             // the skin is vertices 0..skinCount
    std::vector<std::pair<size_t, size_t>> hairRanges; // (first vertex, count) of each hair card part: they ride the skin
};

// Parts of a mesh as the rig wants them: the first is the skin, hair cards are hair, and the rest are the eyes.
inline RigExtras RigExtrasFromParts(const std::vector<FitPart>& parts) {
    RigExtras ex;
    if (parts.size() < 2) return ex;
    ex.skinCount = parts[0].count;
    for (size_t k = 1; k < parts.size(); ++k) (parts[k].hair ? ex.hairRanges : ex.eyeRanges).push_back({parts[k].first, parts[k].count});
    return ex;
}

// The mesh without its hair cards, for finding the face: a landmark must land on skin, not on a beard. The importer adds hair last.
inline void WithoutHair(const std::vector<Vertex>& verts, const std::vector<FitPart>& parts, std::vector<Vertex>& outVerts, std::vector<FitPart>& outParts) {
    size_t end = verts.size();
    outParts.clear();
    for (const FitPart& p : parts) {
        if (p.hair) end = std::min(end, p.first);
        else outParts.push_back(p);
    }
    outVerts.assign(verts.begin(), verts.begin() + (std::ptrdiff_t)end);
}

class FaceRig {
    struct V { float x = 0, y = 0, z = 0; };
    struct Entry { uint32_t i; V d; };

public:
    // `base`: the head's flat triangle list. `landmarks`: 468 xyz points on that mesh (FitFaceLandmarks), or null
    // for an unfitted rig that leaves the mesh alone.
    bool Init(const std::vector<Vertex>& base, const float* landmarks, const RigExtras* extras = nullptr) {
        base_ = base;
        fitted_ = false;
        cavityFirst_ = base_.size();
        jawW_.clear();
        eyes_.clear();
        if (base_.size() < 3) return false;
        lo_ = {1e30f, 1e30f, 1e30f};
        hi_ = {-1e30f, -1e30f, -1e30f};
        for (const Vertex& v : base_) {
            lo_ = {std::min(lo_.x, v.px), std::min(lo_.y, v.py), std::min(lo_.z, v.pz)};
            hi_ = {std::max(hi_.x, v.px), std::max(hi_.y, v.py), std::max(hi_.z, v.pz)};
        }
        width_ = std::max(hi_.x - lo_.x, 1e-6f);
        if (landmarks) {
            std::memcpy(lm_, landmarks, sizeof(lm_));
            fitted_ = true;
            skinEnd_ = base_.size();
            if (extras && extras->skinCount) skinEnd_ = std::min(extras->skinCount, base_.size());
            else if (extras && !extras->eyeRanges.empty()) skinEnd_ = std::min(extras->eyeRanges.front().first, base_.size());
            faceW_ = std::max(std::abs(lm_[454 * 3] - lm_[234 * 3]), 0.01f * width_);
            const size_t fileSkin = skinEnd_;
            for (int level = 0; level < kFaceDetailLevels; ++level) SubdivideFace(level);
            // the skin grew, so everything after it moved
            const size_t grew = skinEnd_ - fileSkin;
            partRanges_.assign(1, {0, skinEnd_});
            RigExtras moved;
            if (extras) {
                moved = *extras;
                for (auto& r : moved.eyeRanges) {
                    if (r.first >= fileSkin) r.first += grew;
                    partRanges_.push_back(r);
                }
                for (auto& r : moved.hairRanges) {
                    if (r.first >= fileSkin) r.first += grew;
                    partRanges_.push_back(r);
                }
            }
            BuildSurface();  // welded skin, inside and outside of it, distances along it
            BuildJawWeights();
            BuildMouthInterior(); // appends teeth, gums, palate and tongue to base_
            if (extras) FindEyes(moved);
            BindHair(moved);
            BuildHeadWeights();
        }
        // vertices at the same position share one smooth normal (the mesh is unindexed)
        group_.assign(base_.size(), 0);
        std::unordered_map<uint64_t, int> groups;
        const float q = 1.0f / (width_ * 1e-4f);
        for (size_t i = 0; i < base_.size(); ++i) {
            const uint64_t kx = (uint64_t)(int64_t)std::llround(base_[i].px * q) & 0x1FFFFF;
            const uint64_t ky = (uint64_t)(int64_t)std::llround(base_[i].py * q) & 0x1FFFFF;
            const uint64_t kz = (uint64_t)(int64_t)std::llround(base_[i].pz * q) & 0x1FFFFF;
            group_[i] = groups.emplace((kx << 42) | (ky << 21) | kz, (int)groups.size()).first->second;
        }
        groupCount_ = (int)groups.size();
        for (auto& c : sparse_) c.clear();
        mapping_ = FaceMapping();
        if (fitted_) Rebuild();
        return true;
    }

    bool Valid() const { return !base_.empty(); }
    bool Fitted() const { return fitted_; }
    const std::vector<Vertex>& Base() const { return base_; }
    float HeadWidth() const { return width_; }
    FaceMapping& Mapping() { return mapping_; }
    const FaceMapping& Mapping() const { return mapping_; }
    const float* Landmarks() const { return lm_; }
    // The vertex carrying landmark `i` (side: -1 upper lip, +1 lower lip, 0 either): where that point of the face really went.
    size_t VertexOfLandmark(int i, int side) const { const char want = side > 0 ? 1 : 0; return vertsOf_[NearestOuter(i, side != 0 ? &want : nullptr)].front(); }
    float FaceWidth() const { return faceW_; }
    // Where each of the file's material parts now sits in Base() and Pose()'s output: the rig adds skin, so they shift. Skin first.
    const std::vector<std::pair<size_t, size_t>>& PartRanges() const { return partRanges_; }
    int InsideCount() const { return insideCount_; } // skin vertices found inside the head: the mouth and eye sockets
    bool HasMouth() const { return hasMouth_; }      // false: the lips are one welded surface, so the mouth cannot open
    void BoxXY(float& x0, float& y0, float& x1, float& y1) const { x0 = lo_.x; y0 = lo_.y; x1 = hi_.x; y1 = hi_.y; }
    bool FrontFacing(const Vertex& v) const { return v.pz >= lo_.z + (hi_.z - lo_.z) * 0.45f; }

    // Which region landmark `i` belongs to (for coloring and labels).
    static FaceRegion RegionOf(int i) {
        using namespace facemap;
        auto in = [&](const auto& a) { for (int v : a) if (v == i) return true; return false; };
        if (in(kBrowR) || in(kBrowL)) return FaceRegion::Brows;
        if (in(kLidUpR) || in(kLidLoR) || in(kLidUpL) || in(kLidLoL)) return FaceRegion::Eyes;
        if (in(kUpperLip) || in(kLowerLip)) return FaceRegion::Lips;
        if (in(kNose) || in(kAlaR) || in(kAlaL)) return FaceRegion::Nose;
        if (in(kCheekR) || in(kCheekL)) return FaceRegion::Cheeks;
        if (in(kJawLine) || in(kChin)) return FaceRegion::Jaw;
        return FaceRegion::Outline;
    }

    // Recomputes the sparse per-channel displacements from the landmarks.
    void Rebuild() {
        if (!fitted_) return;
        using namespace facemap;
        // Recipes are in head widths (w, ears included) or, around the mouth, face widths (F). Both come from the face, not
        // from the mesh's size: the mesh may be a head or a whole body.
        const float F = faceW_, w = 1.42f * F;
        auto L = [&](int i) { return V{lm_[i * 3], lm_[i * 3 + 1], lm_[i * 3 + 2]}; };
        const V mouthC = {(L(13).x + L(14).x) * 0.5f, (L(13).y + L(14).y) * 0.5f, (L(13).z + L(14).z) * 0.5f};

        // One drive: anchors (landmark, lip side) that move by d, reaching `sigma` along the skin.
        struct Drive {
            std::vector<int> idx;
            std::vector<int> side; // per anchor: -1 upper lip, +1 lower lip, 0 either
            std::vector<V> d;      // per anchor
            std::vector<float> sig; // per anchor, when it differs from sigma
            std::vector<float> pull; // per anchor, how strongly it holds the skin next to it (empty: all 1)
            float sigma = 0;
            bool noseSafe = false; // the nose base does not follow
            bool puffSafe = false; // the eyes and lips do not follow
        };
        auto drive = [&](const int* a, size_t n, float dx, float dy, float dz, float sigma) {
            Drive dr;
            dr.idx.assign(a, a + n);
            dr.side.assign(n, 0);
            dr.d.assign(n, V{dx * w, dy * w, dz * w});
            dr.sigma = sigma * w;
            return dr;
        };
        // mouth drives: every anchor has its own move in face widths, and a known lip side
        auto lipDrive = [&](float sigma) { Drive dr; dr.sigma = sigma * F; dr.noseSafe = true; return dr; };
        auto put = [&](Drive& dr, int i, int side, float dx, float dy, float dz, float sigma = 0, float pull = 1) {
            dr.idx.push_back(i); dr.side.push_back(side); dr.d.push_back({dx * F, dy * F, dz * F});
            dr.sig.push_back(sigma > 0 ? sigma * F : dr.sigma);
            dr.pull.push_back(pull);
        };
        auto size = [](const auto& a) { return sizeof(a) / sizeof(int); };

        // Blink / squint / wide: each point of the upper lid's edge travels to the lower lid's edge straight below it (and
        // the lower lid up to meet it), as fractions of the opening at that point. The skin above follows, the brow does not.
        auto lidCurve = [&](const int* a, size_t n, float x) { // the height of a lid edge at x
            std::vector<V> p;
            for (size_t k = 0; k < n; ++k) p.push_back(L(a[k]));
            std::sort(p.begin(), p.end(), [](const V& u, const V& o) { return u.x < o.x; });
            if (x <= p.front().x) return p.front().y;
            for (size_t k = 1; k < p.size(); ++k)
                if (x <= p[k].x) return p[k - 1].y + (p[k].y - p[k - 1].y) * (x - p[k - 1].x) / std::max(p[k].x - p[k - 1].x, 1e-6f);
            return p.back().y;
        };
        auto lids = [&](const int* up, size_t nu, const int* lo, size_t nl, float upFrac, float loFrac) {
            std::vector<Drive> d;
            Drive a, b;
            a.sigma = 0.03f * w;
            b.sigma = 0.024f * w;
            for (size_t k = 0; k < nu; ++k) {
                const V p = L(up[k]);
                const float gap = std::max(p.y - lidCurve(lo, nl, p.x), 0.0f);
                a.idx.push_back(up[k]); a.side.push_back(0); a.d.push_back({0, -gap * upFrac, 0.012f * F * std::max(upFrac, 0.0f)}); a.sig.push_back(a.sigma); // laps over the lower lid
            }
            for (size_t k = 0; k < nl; ++k) {
                const V p = L(lo[k]);
                const float gap = std::max(lidCurve(up, nu, p.x) - p.y, 0.0f);
                b.idx.push_back(lo[k]); b.side.push_back(0); b.d.push_back({0, gap * loFrac, -0.006f * F * std::max(loFrac, 0.0f)}); b.sig.push_back(b.sigma);
            }
            d.push_back(a);
            d.push_back(b);
            return d;
        };

        std::vector<Drive> recipe[kChannels];
        auto R = [&](int c) -> std::vector<Drive>& { return recipe[c]; };

        R(BS_browDownLeft).push_back(drive(kBrowL, size(kBrowL), 0, -0.035f, 0, 0.055f));
        R(BS_browDownRight).push_back(drive(kBrowR, size(kBrowR), 0, -0.035f, 0, 0.055f));
        R(BS_browInnerUp).push_back(drive(kBrowInnerL, size(kBrowInnerL), 0, 0.05f, 0, 0.05f));
        R(BS_browInnerUp).push_back(drive(kBrowInnerR, size(kBrowInnerR), 0, 0.05f, 0, 0.05f));
        R(BS_browOuterUpLeft).push_back(drive(kBrowOuterL, size(kBrowOuterL), 0, 0.045f, 0, 0.055f));
        R(BS_browOuterUpRight).push_back(drive(kBrowOuterR, size(kBrowOuterR), 0, 0.045f, 0, 0.055f));
        // A puffed cheek is one smooth dome over the hollow beside the mouth: air pressing out, with the lips sealed and the eyes
        // left alone. A single anchor, so the Gaussian itself is its shape.
        for (const float sx : {1.0f, -1.0f}) {
            Drive dome;
            dome.sigma = 0.135f * F;
            dome.puffSafe = true;
            dome.idx.push_back(sx > 0 ? 427 : 207);
            dome.side.push_back(0);
            dome.d.push_back({sx * 0.095f * F, -0.008f * F, 0.05f * F});
            R(BS_cheekPuff).push_back(dome);
        }
        R(BS_cheekSquintLeft).push_back(drive(kCheekL, size(kCheekL), 0.004f, 0.03f, 0.012f, 0.07f));
        R(BS_cheekSquintRight).push_back(drive(kCheekR, size(kCheekR), -0.004f, 0.03f, 0.012f, 0.07f));
        for (auto& dr : lids(kLidUpL, size(kLidUpL), kLidLoL, size(kLidLoL), 0.96f, 0.22f)) R(BS_eyeBlinkLeft).push_back(dr);
        for (auto& dr : lids(kLidUpR, size(kLidUpR), kLidLoR, size(kLidLoR), 0.96f, 0.22f)) R(BS_eyeBlinkRight).push_back(dr);
        for (auto& dr : lids(kLidUpL, size(kLidUpL), kLidLoL, size(kLidLoL), 0.25f, 0.30f)) R(BS_eyeSquintLeft).push_back(dr);
        for (auto& dr : lids(kLidUpR, size(kLidUpR), kLidLoR, size(kLidLoR), 0.25f, 0.30f)) R(BS_eyeSquintRight).push_back(dr);
        for (auto& dr : lids(kLidUpL, size(kLidUpL), kLidLoL, size(kLidLoL), -0.25f, 0.0f)) R(BS_eyeWideLeft).push_back(dr);
        for (auto& dr : lids(kLidUpR, size(kLidUpR), kLidLoR, size(kLidLoR), -0.25f, 0.0f)) R(BS_eyeWideRight).push_back(dr);
        R(BS_noseSneerLeft).push_back(drive(kAlaL, size(kAlaL), 0.003f, 0.022f, -0.004f, 0.04f));
        R(BS_noseSneerRight).push_back(drive(kAlaR, size(kAlaR), -0.003f, 0.022f, -0.004f, 0.04f));

        // ---- the mouth. Anatomy first: the lips are two free edges of one sheet, so each is driven on its own and the
        // skin between lip and nose or chin takes up the difference. Amounts are tuned with mouth_lab against the tracker.
        for (const bool left : {true, false}) {
            const float sx = left ? 1.0f : -1.0f;
            const int* corner = left ? kCornerL : kCornerR;
            const size_t nc = size(kCornerL);
            auto ch = [&](int a, int b) -> std::vector<Drive>& { return recipe[left ? a : b]; };
            auto share = [&](int i) { // this side's share of a lip point
                const float t = std::clamp((sx * (L(i).x - mouthC.x) + 0.05f * F) / (0.1f * F), 0.0f, 1.0f);
                return t * t * (3 - 2 * t);
            };
            auto lipSide = [&](Drive& dr, const int* a, size_t n, int side, float dy, float dz) {
                for (size_t j = 0; j < n; ++j) {
                    const float m = share(a[j]);
                    if (m > 1e-3f) put(dr, a[j], side, 0, dy * m, dz * m);
                }
            };
            const float halfMouth = std::max(std::abs(L(291).x - L(61).x) * 0.5f, 1e-4f);
            auto cornerDrive = [&](float dx, float dy, float dz, float sigma, float upperArc = 1.0f, float fullness = 0.0f) {
                Drive dr = lipDrive(sigma);
                for (size_t k = 0; k < nc; ++k) put(dr, corner[k], 0, sx * dx, dy, dz);
                // Along the lip the move fades to nothing at the middle. The fall is a power curve, not an S: an S rises in a
                // straight line toward the corner, which draws the lips as a V.
                auto arc = [&](const int* a, size_t n, int side, float power, float dyHere, float belly = 0.0f, float pull = 1.0f) {
                    for (size_t j = 0; j < n; ++j) {
                        const int i = a[j];
                        const float off = sx * (L(i).x - mouthC.x);
                        if (off < -0.04f * F) continue; // past the middle the lip holds still
                        const float t = std::clamp(off / halfMouth, 0.0f, 1.0f), k = std::pow(t, power) * 0.92f;
                        put(dr, i, side, sx * dx * k, dyHere * k - belly * std::sin(3.14159265f * t), dz * k, 0.035f, pull); // tight, so the arc holds its shape
                    }
                };
                // The two free edges share one curve, so lips that were shut stay shut. Only the upper lip's outer line
                // rises less, and the lower lip's outer line keeps a little belly instead of a point.
                arc(kUpperInner, size(kUpperInner), -1, kArcEdge, dy, 0.0f, kEdgePull);
                arc(kLowerInner, size(kLowerInner), 1, kArcEdge, dy, 0.0f, kEdgePull);
                arc(kUpperOuter, size(kUpperOuter), -1, kArcUpperOuter, dy * upperArc);
                arc(kLowerOuter, size(kLowerOuter), 1, kArcLowerOuter, dy, fullness);
                return dr;
            };
            // smile: corners pulled up and back along the zygomatic line, wrapping round the teeth; the cheek bunches
            ch(BS_mouthSmileLeft, BS_mouthSmileRight).push_back(cornerDrive(0.074f, 0.066f, -0.030f, 0.115f, 0.75f, 0.010f));
            {
                Drive dr = lipDrive(0.10f);
                for (int i : {left ? 425 : 205, left ? 280 : 50, left ? 411 : 187}) put(dr, i, 0, sx * 0.01f, 0.03f, 0.018f);
                ch(BS_mouthSmileLeft, BS_mouthSmileRight).push_back(dr);
            }
            ch(BS_mouthStretchLeft, BS_mouthStretchRight).push_back(cornerDrive(0.068f, -0.012f, -0.025f, 0.10f));
            // the jaw's own effect on the lips: an open mouth narrows and its corners follow the jaw down
            recipe[kJawFix].push_back(cornerDrive(-0.030f, -0.052f, 0.004f, 0.09f));
            ch(BS_mouthFrownLeft, BS_mouthFrownRight).push_back(cornerDrive(0.0f, -0.075f, -0.005f, 0.085f));
            ch(BS_mouthDimpleLeft, BS_mouthDimpleRight).push_back(cornerDrive(0.03f, 0.006f, -0.035f, 0.05f));
            // upper lip lifted off the teeth: the free edge rises most, the lip's outer line less, so the lip thins
            {
                Drive dr = lipDrive(0.05f);
                // the free edge stops at the gum line: a crown shows about 7 mm below it at most
                lipSide(dr, kUpperInner, size(kUpperInner), -1, 0.036f, 0.004f);
                lipSide(dr, kUpperOuter, size(kUpperOuter), -1, 0.024f, 0.006f);
                ch(BS_mouthUpperUpLeft, BS_mouthUpperUpRight).push_back(dr);
            }
            {
                Drive dr = lipDrive(0.055f);
                lipSide(dr, kLowerInner, size(kLowerInner), 1, -0.095f, 0.008f);
                lipSide(dr, kLowerOuter, size(kLowerOuter), 1, -0.065f, 0.010f);
                ch(BS_mouthLowerDownLeft, BS_mouthLowerDownRight).push_back(dr);
            }
            // press: both lips squeezed thin against each other
            {
                Drive dr = lipDrive(0.04f);
                lipSide(dr, kUpperOuter, size(kUpperOuter), -1, -0.022f, -0.006f);
                lipSide(dr, kLowerOuter, size(kLowerOuter), 1, 0.024f, -0.006f);
                ch(BS_mouthPressLeft, BS_mouthPressRight).push_back(dr);
            }
        }
        // ... and it lifts the upper lip off the teeth
        {
            Drive dr = lipDrive(0.05f);
            for (int v : kUpperInner) put(dr, v, -1, 0, 0.030f, 0.004f);
            for (int v : kUpperOuter) put(dr, v, -1, 0, 0.018f, 0.004f);
            recipe[kJawFix].push_back(dr);
        }
        // funnel / pucker: the lips gather toward the mouth axis and push out; the lip body thickens as it bunches
        for (int pass = 0; pass < 2; ++pass) {
            const bool funnel = pass == 0;
            const float pullX = funnel ? 0.34f : 0.40f, fwd = funnel ? 0.08f : 0.095f; // about 13 mm at full pucker
            Drive dr = lipDrive(0.075f);
            auto gather = [&](const int* a, size_t n, int side, float pullY, float bulge) {
                for (size_t k = 0; k < n; ++k) {
                    const V p = L(a[k]);
                    put(dr, a[k], side, (mouthC.x - p.x) * pullX / F, (mouthC.y - p.y) * pullY / F + bulge, fwd);
                }
            };
            gather(kUpperOuter, size(kUpperOuter), -1, funnel ? -0.1f : 0.1f, funnel ? 0.012f : 0.028f); // the lip body bunches thicker
            gather(kLowerOuter, size(kLowerOuter), 1, funnel ? -0.1f : 0.1f, funnel ? -0.012f : -0.030f);
            gather(kUpperInner, size(kUpperInner), -1, 0.0f, funnel ? 0.022f : -0.002f); // a funnel's opening stays round
            gather(kLowerInner, size(kLowerInner), 1, 0.0f, funnel ? -0.022f : 0.002f);
            R(funnel ? BS_mouthFunnel : BS_mouthPucker).push_back(dr);
        }
        for (int pass = 0; pass < 2; ++pass) {
            const float sx = pass == 0 ? 1.0f : -1.0f;
            Drive dr = lipDrive(0.11f);
            for (int v : kUpperOuter) put(dr, v, -1, sx * 0.085f, 0, 0);
            for (int v : kUpperInner) put(dr, v, -1, sx * 0.085f, 0, 0);
            for (int v : kLowerOuter) put(dr, v, 1, sx * 0.085f, 0, 0);
            for (int v : kLowerInner) put(dr, v, 1, sx * 0.085f, 0, 0);
            for (int v : kCornerL) put(dr, v, 0, sx * 0.085f, 0, 0);
            for (int v : kCornerR) put(dr, v, 0, sx * 0.085f, 0, 0);
            R(pass == 0 ? BS_mouthLeft : BS_mouthRight).push_back(dr);
        }
        {
            Drive lower = lipDrive(0.05f), upper = lipDrive(0.05f);
            for (int v : kLowerOuter) put(lower, v, 1, 0, 0.034f, -0.034f);
            for (int v : kLowerInner) put(lower, v, 1, 0, 0.006f, -0.024f);
            for (int v : kUpperOuter) put(upper, v, -1, 0, -0.030f, -0.030f);
            for (int v : kUpperInner) put(upper, v, -1, 0, -0.004f, -0.022f);
            R(BS_mouthRollLower).push_back(lower);
            R(BS_mouthRollUpper).push_back(upper);
        }
        {
            Drive lower = lipDrive(0.09f), upper = lipDrive(0.06f);
            for (int v : kLowerOuter) put(lower, v, 1, 0, 0.040f, 0.012f);
            for (int v : kLowerInner) put(lower, v, 1, 0, 0.030f, 0.010f);
            for (int v : kChin) put(lower, v, 0, 0, 0.018f, 0.006f);
            for (int v : kUpperOuter) put(upper, v, -1, 0, 0.030f, 0.004f);
            for (int v : kUpperInner) put(upper, v, -1, 0, 0.026f, 0.004f);
            R(BS_mouthShrugLower).push_back(lower);
            R(BS_mouthShrugUpper).push_back(upper);
        }
        // mouthClose only means something with the jaw open (Pose limits it to jawOpen): it brings the lips back together
        {
            const float gap = jawGapAtFull_ / F;
            Drive dr = lipDrive(0.085f);
            for (int v : kLowerOuter) put(dr, v, 1, 0, gap * 0.68f, 0.03f);
            for (int v : kLowerInner) put(dr, v, 1, 0, gap * 0.68f, 0.03f);
            for (int v : kUpperOuter) put(dr, v, -1, 0, -gap * 0.32f, 0.01f);
            for (int v : kUpperInner) put(dr, v, -1, 0, -gap * 0.32f, 0.01f);
            R(BS_mouthClose).push_back(dr);
        }

        for (auto& c : sparse_) c.clear();
        const size_t nw = surfPos_.size();
        std::vector<V> sum(nw);
        std::vector<float> reach(nw, 0.0f), wsum(nw, 0.0f);
        const float eps2 = (0.012f * F) * (0.012f * F);
        std::vector<int> touched;
        for (int c = 0; c < kChannels; ++c) {
            if (recipe[c].empty()) continue;
            touched.clear();
            for (const Drive& dr : recipe[c]) {
                for (size_t j = 0; j < dr.idx.size(); ++j) {
                    const float sg = dr.sig.empty() ? dr.sigma : dr.sig[j];
                    const float inv = 1.0f / (2 * sg * sg), cut = 3 * sg;
                    const V& d = dr.d[j];
                    for (const auto& g : AnchorGeo(dr.idx[j], dr.side[j])) {
                        if (g.second > cut) continue;
                        const float phi = std::exp(-g.second * g.second * inv);
                        if (phi < 1e-4f) continue;
                        if (reach[g.first] == 0) touched.push_back(g.first);
                        // Each anchor weighs more the nearer it is, so every anchor keeps its own move on top of it;
                        // the Gaussian alone says how far the skin follows at all.
                        const float k = (dr.pull.empty() ? 1.0f : dr.pull[j]) * phi / (g.second * g.second + eps2), m = k * (dr.noseSafe ? noseMask_[g.first] : dr.puffSafe ? puffMask_[g.first] : 1.0f);
                        sum[g.first].x += m * d.x; sum[g.first].y += m * d.y; sum[g.first].z += m * d.z;
                        wsum[g.first] += k;
                        reach[g.first] += phi;
                    }
                }
            }
            for (const int v : touched) {
                const float fall = std::min(1.0f, reach[v]); // a single anchor reaches 1 right on top of it
                const V d = {sum[v].x / wsum[v] * fall, sum[v].y / wsum[v] * fall, sum[v].z / wsum[v] * fall};
                for (const uint32_t i : vertsOf_[v]) sparse_[c].push_back({i, d});
                sum[v] = V{};
                reach[v] = wsum[v] = 0;
            }
        }
        // The jaw channels are a bone (Pose rotates it); these entries are only for the editor's influence view.
        for (size_t i = 0; i < jawW_.size(); ++i) {
            if (jawW_[i] < 1e-3f) continue;
            const V d = JawMove(base_[i].px, base_[i].py, base_[i].pz, jawW_[i], 1, 0, 0);
            sparse_[BS_jawOpen].push_back({(uint32_t)i, d});
            sparse_[BS_jawLeft].push_back({(uint32_t)i, {kJawSide * F * jawW_[i], 0, 0}});
            sparse_[BS_jawRight].push_back({(uint32_t)i, {-kJawSide * F * jawW_[i], 0, 0}});
            sparse_[BS_jawForward].push_back({(uint32_t)i, {0, 0, kJawForward * F * jawW_[i]}});
        }
        AddEyeChannels();
    }

    // A game head is too coarse to bend smoothly: this one has columns 6 mm apart along the lower lip, and a line through them
    // has corners. So the skin of the lips and eyelids is split, here where the rig is built and never in the file: each
    // triangle there becomes four, and neighbours split only the edge they share, so nothing cracks. New points rise off
    // the flat triangle to follow the surface's normals (Phong tessellation), which rounds the shape as well.
    void SubdivideFace(int level) {
        auto L = [&](int i) { return V{lm_[i * 3], lm_[i * 3 + 1], lm_[i * 3 + 2]}; };
        const float mm = faceW_ / 140.0f;
        const V lipL = L(61), lipR = L(291), lipTop = L(0), lipBot = L(17), lip = L(13);
        // the first pass takes the lips with the skin round them, and the lids; the second only the lips themselves
        const float grow = level == 0 ? 8.0f : 2.0f;
        const float x0 = std::min(lipL.x, lipR.x) - grow * mm, x1 = std::max(lipL.x, lipR.x) + grow * mm;
        const float y0 = lipBot.y - grow * mm, y1 = lipTop.y + grow * mm, z0 = lip.z - 24 * mm;
        struct Box { float x0, x1, y0, y1, z0; };
        std::vector<Box> boxes = {{x0, x1, y0, y1, z0}};
        if (level == 0)
            for (const auto& eye : {std::array<int, 4>{33, 133, 159, 145}, std::array<int, 4>{263, 362, 386, 374}}) {
                const V a = L(eye[0]), b = L(eye[1]), up = L(eye[2]), lo = L(eye[3]);
                boxes.push_back({std::min(a.x, b.x) - 3 * mm, std::max(a.x, b.x) + 3 * mm, lo.y - 4 * mm, up.y + 8 * mm, std::min(a.z, b.z) - 6 * mm});
            }
        const float q = 1.0f / (width_ * 1e-4f);
        auto key = [&](const Vertex& v) {
            const uint64_t kx = (uint64_t)(int64_t)std::llround(v.px * q) & 0x1FFFFF;
            const uint64_t ky = (uint64_t)(int64_t)std::llround(v.py * q) & 0x1FFFFF;
            const uint64_t kz = (uint64_t)(int64_t)std::llround(v.pz * q) & 0x1FFFFF;
            return (kx << 42) | (ky << 21) | kz;
        };
        // one normal per position, so both triangles of an edge compute the same new point
        std::unordered_map<uint64_t, V> normal;
        for (size_t i = 0; i < skinEnd_; ++i) {
            V& n = normal[key(base_[i])];
            n.x += base_[i].nx; n.y += base_[i].ny; n.z += base_[i].nz;
        }
        for (auto& e : normal) {
            const float l = std::max(std::sqrt(e.second.x * e.second.x + e.second.y * e.second.y + e.second.z * e.second.z), 1e-6f);
            e.second = {e.second.x / l, e.second.y / l, e.second.z / l};
        }
        using Edge = std::pair<uint64_t, uint64_t>;
        auto edge = [&](const Vertex& a, const Vertex& b) { const uint64_t x = key(a), y = key(b); return x < y ? Edge{x, y} : Edge{y, x}; };
        std::set<Edge> split;
        for (size_t t = 0; t + 2 < skinEnd_; t += 3) {
            const Vertex &a = base_[t], &b = base_[t + 1], &d = base_[t + 2];
            const float cx = (a.px + b.px + d.px) / 3, cy = (a.py + b.py + d.py) / 3, cz = (a.pz + b.pz + d.pz) / 3;
            bool in = false;
            for (const Box& bx : boxes) in = in || (cx > bx.x0 && cx < bx.x1 && cy > bx.y0 && cy < bx.y1 && cz > bx.z0);
            if (!in) continue;
            split.insert(edge(a, b));
            split.insert(edge(b, d));
            split.insert(edge(d, a));
        }
        auto mid = [&](const Vertex& a, const Vertex& b) {
            Vertex m = a;
            const float* pa = &a.px; const float* pb = &b.px; float* pm = &m.px;
            for (int k = 0; k < 12; ++k) pm[k] = (pa[k] + pb[k]) * 0.5f;
            const V na = normal[key(a)], nb = normal[key(b)];
            const float da = (m.px - a.px) * na.x + (m.py - a.py) * na.y + (m.pz - a.pz) * na.z;
            const float db = (m.px - b.px) * nb.x + (m.py - b.py) * nb.y + (m.pz - b.pz) * nb.z;
            constexpr float round = 0.75f; // Phong tessellation's shape factor
            m.px -= round * 0.5f * (da * na.x + db * nb.x);
            m.py -= round * 0.5f * (da * na.y + db * nb.y);
            m.pz -= round * 0.5f * (da * na.z + db * nb.z);
            return m;
        };
        std::vector<Vertex> out;
        out.reserve(skinEnd_ + split.size() * 6);
        auto tri = [&](const Vertex& x, const Vertex& y, const Vertex& z) { out.push_back(x); out.push_back(y); out.push_back(z); };
        for (size_t t = 0; t + 2 < skinEnd_; t += 3) {
            const Vertex &a = base_[t], &b = base_[t + 1], &d = base_[t + 2];
            const bool ab = split.count(edge(a, b)) != 0, bd = split.count(edge(b, d)) != 0, da = split.count(edge(d, a)) != 0;
            if (!ab && !bd && !da) { tri(a, b, d); continue; }
            const Vertex mab = ab ? mid(a, b) : a, mbd = bd ? mid(b, d) : b, mda = da ? mid(d, a) : d;
            if (ab && bd && da) { tri(a, mab, mda); tri(mab, b, mbd); tri(mda, mbd, d); tri(mab, mbd, mda); }
            else if (ab && bd) { tri(mab, b, mbd); tri(a, mab, mbd); tri(a, mbd, d); }
            else if (bd && da) { tri(mbd, d, mda); tri(a, b, mbd); tri(a, mbd, mda); }
            else if (da && ab) { tri(a, mab, mda); tri(mab, b, d); tri(mab, d, mda); }
            else if (ab) { tri(a, mab, d); tri(mab, b, d); }
            else if (bd) { tri(a, b, mbd); tri(a, mbd, d); }
            else { tri(a, b, mda); tri(mda, b, d); }
        }
        const size_t newSkin = out.size();
        out.insert(out.end(), base_.begin() + (std::ptrdiff_t)skinEnd_, base_.end());
        base_.swap(out);
        skinEnd_ = newSkin;
    }

    // The skin as a surface: identical positions welded into one vertex, its edges, and which part of it is inside the
    // head. Only what the fit found on this mesh is used, so a differently built head still works.
    void BuildSurface() {
        using namespace facemap;
        auto L = [&](int i) { return V{lm_[i * 3], lm_[i * 3 + 1], lm_[i * 3 + 2]}; };
        mouthCx_ = (L(61).x + L(291).x) * 0.5f;

        const float q = 1.0f / (width_ * 1e-4f);
        std::unordered_map<uint64_t, int> ids;
        surfPos_.clear();
        vertsOf_.clear();
        weldOf_.assign(skinEnd_, 0);
        for (size_t i = 0; i < skinEnd_; ++i) {
            const uint64_t kx = (uint64_t)(int64_t)std::llround(base_[i].px * q) & 0x1FFFFF;
            const uint64_t ky = (uint64_t)(int64_t)std::llround(base_[i].py * q) & 0x1FFFFF;
            const uint64_t kz = (uint64_t)(int64_t)std::llround(base_[i].pz * q) & 0x1FFFFF;
            const auto it = ids.emplace((kx << 42) | (ky << 21) | kz, (int)surfPos_.size());
            if (it.second) { surfPos_.push_back({base_[i].px, base_[i].py, base_[i].pz}); vertsOf_.emplace_back(); }
            weldOf_[i] = it.first->second;
            vertsOf_[it.first->second].push_back((uint32_t)i);
        }
        const int nw = (int)surfPos_.size();
        surfNbr_.assign(nw, {});
        auto link = [&](int a, int b) {
            if (a == b) return;
            for (const auto& e : surfNbr_[a]) if (e.first == b) return;
            const V &p = surfPos_[a], &o = surfPos_[b];
            const float len = std::sqrt((p.x - o.x) * (p.x - o.x) + (p.y - o.y) * (p.y - o.y) + (p.z - o.z) * (p.z - o.z));
            surfNbr_[a].push_back({b, len});
            surfNbr_[b].push_back({a, len});
        };
        for (size_t t = 0; t + 2 < skinEnd_; t += 3) {
            link(weldOf_[t], weldOf_[t + 1]);
            link(weldOf_[t + 1], weldOf_[t + 2]);
            link(weldOf_[t + 2], weldOf_[t]);
        }
        FindInside();
        geo_.clear();
        // Which lip a vertex belongs to: whichever of the two lips' outer midpoints is nearer along the skin. The lips of
        // a mesh with a real mouth meet only at the corners, so this splits them cleanly all the way into the mouth.
        std::vector<float> dUp, dLo;
        Distances({NearestOuter(0, nullptr)}, 1e30f, dUp);
        Distances({NearestOuter(17, nullptr)}, 1e30f, dLo);
        lowerSide_.resize(nw);
        for (int v = 0; v < nw; ++v) lowerSide_[v] = dLo[v] < dUp[v] ? 1 : 0;
        // A real mouth: the lips' middles are far apart along the skin, joined only round the corners and through the cavity.
        {
            const char up = 0, lo = 1;
            std::vector<float> d;
            Distances({NearestOuter(13, &up)}, 1e30f, d);
            hasMouth_ = insideCount_ >= 30 && d[NearestOuter(14, &lo)] > 0.25f * faceW_;
        }
        // nose base: nothing a lip does should drag it
        std::vector<float> dNose;
        std::vector<int> nose;
        for (int i : {2, 94, 19, 1, 4, 141, 370}) nose.push_back(NearestOuter(i, nullptr));
        Distances(nose, 1e30f, dNose);
        // eyes and lips, for a puffed cheek
        {
            std::vector<float> dEyes, dLips;
            std::vector<int> eyes, lipLine;
            for (const int i : facemap::kLidLoL) eyes.push_back(NearestOuter(i, nullptr));
            for (const int i : facemap::kLidLoR) eyes.push_back(NearestOuter(i, nullptr));
            for (const int i : facemap::kLidUpL) eyes.push_back(NearestOuter(i, nullptr));
            for (const int i : facemap::kLidUpR) eyes.push_back(NearestOuter(i, nullptr));
            for (const int i : facemap::kUpperOuter) lipLine.push_back(NearestOuter(i, nullptr));
            for (const int i : facemap::kLowerOuter) lipLine.push_back(NearestOuter(i, nullptr));
            for (const int i : {61, 291}) lipLine.push_back(NearestOuter(i, nullptr));
            Distances(eyes, 1e30f, dEyes);
            Distances(lipLine, 1e30f, dLips);
            puffMask_.resize(nw);
            for (int v = 0; v < nw; ++v) {
                const float e = std::clamp(dEyes[v] / (0.24f * faceW_), 0.0f, 1.0f), l = std::clamp(dLips[v] / (0.11f * faceW_), 0.0f, 1.0f);
                puffMask_[v] = (e * e * (3 - 2 * e)) * (l * l * (3 - 2 * l));
            }
        }
        noseMask_.resize(nw);
        const float clear = 0.12f * faceW_;
        for (int v = 0; v < nw; ++v) {
            const float t = std::clamp(dNose[v] / clear, 0.0f, 1.0f);
            noseMask_[v] = t * t * (3 - 2 * t);
        }
    }

    // A vertex is inside when no view from outside the head can see it. Eyeballs count as blockers, so what is left is
    // the cavity reached through the lips and the eye sockets.
    void FindInside() {
        const int nw = (int)surfPos_.size();
        std::vector<char> seen(nw, 0);
        const float box = std::max({hi_.x - lo_.x, hi_.y - lo_.y, hi_.z - lo_.z}) * 1.8f;
        constexpr int G = 384;
        const float px = G / box, bias = std::min(0.004f * width_, 0.008f * faceW_);
        const V dirs[] = {{0, 0, 1}, {0.7f, 0, 0.7f}, {-0.7f, 0, 0.7f}, {1, 0, 0}, {-1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0},
                          {0.6f, -0.6f, 0.5f}, {-0.6f, -0.6f, 0.5f}, {0, -0.7f, 0.7f}, {0, 0.7f, 0.7f}, {0.6f, 0.6f, 0.5f}, {-0.6f, 0.6f, 0.5f}};
        std::vector<float> zb((size_t)G * G);
        const V mid = {(lo_.x + hi_.x) * 0.5f, (lo_.y + hi_.y) * 0.5f, (lo_.z + hi_.z) * 0.5f};
        for (V f : dirs) {
            const float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
            f = {f.x / fl, f.y / fl, f.z / fl};
            V r = std::abs(f.y) > 0.9f ? V{1, 0, 0} : V{f.z, 0, -f.x};
            const float rl = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
            r = {r.x / rl, r.y / rl, r.z / rl};
            const V u = {f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x};
            auto project = [&](float x, float y, float z, float& sx, float& sy, float& d) {
                x -= mid.x; y -= mid.y; z -= mid.z;
                sx = (x * r.x + y * r.y + z * r.z) * px + G * 0.5f;
                sy = (x * u.x + y * u.y + z * u.z) * px + G * 0.5f;
                d = x * f.x + y * f.y + z * f.z;
            };
            std::fill(zb.begin(), zb.end(), -1e30f);
            for (size_t t = 0; t + 2 < base_.size(); t += 3) {
                float sx[3], sy[3], d[3];
                for (int k = 0; k < 3; ++k) project(base_[t + k].px, base_[t + k].py, base_[t + k].pz, sx[k], sy[k], d[k]);
                const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
                if (std::abs(area) < 1e-6f) continue;
                const int x0 = std::max(0, (int)std::floor(std::min({sx[0], sx[1], sx[2]}))), x1 = std::min(G - 1, (int)std::ceil(std::max({sx[0], sx[1], sx[2]})));
                const int y0 = std::max(0, (int)std::floor(std::min({sy[0], sy[1], sy[2]}))), y1 = std::min(G - 1, (int)std::ceil(std::max({sy[0], sy[1], sy[2]})));
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const float cx = x + 0.5f, cy = y + 0.5f;
                        const float b1 = ((cx - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (cy - sy[0])) / area;
                        const float b2 = ((sx[1] - sx[0]) * (cy - sy[0]) - (cx - sx[0]) * (sy[1] - sy[0])) / area;
                        if (b1 < -0.01f || b2 < -0.01f || b1 + b2 > 1.01f) continue; // a touch fat: closes slits a pixel wide
                        const float z = d[0] + b1 * (d[1] - d[0]) + b2 * (d[2] - d[0]);
                        float& o = zb[(size_t)y * G + x];
                        if (z > o) o = z;
                    }
            }
            for (int v = 0; v < nw; ++v) {
                float sx, sy, d;
                project(surfPos_[v].x, surfPos_[v].y, surfPos_[v].z, sx, sy, d);
                const int x = std::clamp((int)sx, 0, G - 1), y = std::clamp((int)sy, 0, G - 1);
                if (d >= zb[(size_t)y * G + x] - bias) seen[v] = 1;
            }
        }
        // the largest unseen patch is the inside; lone unseen vertices are nooks of the outer skin
        inside_.assign(nw, 0);
        std::vector<int> label(nw, -1), stack, best;
        for (int s = 0; s < nw; ++s) {
            if (seen[s] || label[s] >= 0) continue;
            std::vector<int> comp;
            stack.push_back(s);
            label[s] = s;
            while (!stack.empty()) {
                const int v = stack.back();
                stack.pop_back();
                comp.push_back(v);
                for (const auto& e : surfNbr_[v])
                    if (!seen[e.first] && label[e.first] < 0) { label[e.first] = s; stack.push_back(e.first); }
            }
            if (comp.size() > best.size()) best.swap(comp);
        }
        for (const int v : best) inside_[v] = 1;
        // seen only through a slit: a vertex ringed by the inside is inside
        for (int pass = 0; pass < 3; ++pass)
            for (int v = 0; v < nw; ++v) {
                if (inside_[v] || surfNbr_[v].empty()) continue;
                bool all = true;
                for (const auto& e : surfNbr_[v]) all = all && inside_[e.first];
                if (all) inside_[v] = 1;
            }
        insideCount_ = 0;
        for (int v = 0; v < nw; ++v) insideCount_ += inside_[v];
    }

    // The welded vertex nearest landmark `i`, off the inside of the head. `accept` narrows the choice.
    int NearestOuter(int i, const char* wantLower) const {
        const V p = {lm_[i * 3], lm_[i * 3 + 1], lm_[i * 3 + 2]};
        int best = 0;
        float bestD = 1e30f;
        for (int v = 0; v < (int)surfPos_.size(); ++v) {
            if (inside_[v]) continue;
            if (wantLower && lowerSide_[v] != *wantLower) continue;
            const V& o = surfPos_[v];
            const float dx = o.x - p.x, dy = o.y - p.y, dz = (o.z - p.z) * 0.35f; // the fit's depth is its least sure axis
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < bestD) { bestD = d; best = v; }
        }
        return best;
    }

    // Distance along the skin from `seeds`, out to maxD (beyond it: 1e30). Dijkstra over the welded edges.
    void Distances(const std::vector<int>& seeds, float maxD, std::vector<float>& dist) const {
        dist.assign(surfPos_.size(), 1e30f);
        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        for (const int s : seeds) { dist[s] = 0; open.push({0.0f, s}); }
        while (!open.empty()) {
            const Item top = open.top();
            open.pop();
            if (top.first > dist[top.second]) continue;
            for (const auto& e : surfNbr_[top.second]) {
                const float d = top.first + e.second;
                if (d < dist[e.first] && d <= maxD) { dist[e.first] = d; open.push({d, e.first}); }
            }
        }
    }

    // Vertices within reach of a landmark, with their distance along the skin. Cached: a landmark is used by many channels.
    const std::vector<std::pair<int, float>>& AnchorGeo(int i, int side) {
        const int key = i * 4 + side + 1;
        auto it = geo_.find(key);
        if (it != geo_.end()) return it->second;
        const char want = side > 0 ? 1 : 0;
        std::vector<float> dist;
        Distances({NearestOuter(i, side != 0 ? &want : nullptr)}, 0.75f * faceW_, dist);
        auto& out = geo_[key];
        for (int v = 0; v < (int)dist.size(); ++v)
            if (dist[v] < 1e29f) out.push_back({v, dist[v]});
        return out;
    }

    // Jaw skinning weights: a smooth field over the skin, solved with the lower lip and chin held at 1 and the upper
    // face, ears, back of the head and base of the neck held at 0. The solve follows the surface, so the weight drops
    // across the cheeks on its own and the two lips never mix: they are not connected except at the corners.
    void BuildJawWeights() {
        using namespace facemap;
        auto L = [&](int i) { return V{lm_[i * 3], lm_[i * 3 + 1], lm_[i * 3 + 2]}; };
        const int nw = (int)surfPos_.size();
        const V a = L(234), b = L(454);
        const float seamY = (L(13).y + L(14).y) * 0.5f, noseBase = L(2).y, chinY = L(152).y;
        const float lipZ = (L(13).z + L(14).z) * 0.5f;
        // The condyle sits about 80 mm behind the front teeth and 30 mm above the biting line (a face is about 140 mm wide).
        hinge_ = {(a.x + b.x) * 0.5f, seamY + 0.21f * faceW_, lipZ - 0.58f * faceW_};
        std::vector<float> h(nw);
        std::vector<char> fixed(nw, 0);
        auto pin = [&](int v, float val) { h[v] = val; fixed[v] = 1; };
        for (int v = 0; v < nw; ++v) {
            h[v] = lowerSide_[v] ? 0.7f : 0.0f;
            const V& p = surfPos_[v];
            if (inside_[v]) {
                if (p.y > seamY + 0.03f * faceW_) pin(v, 0);
                else if (p.y < seamY - 0.05f * faceW_ && p.z > hinge_.z + 0.5f * (lipZ - hinge_.z)) pin(v, 1);
                continue;
            }
            if (p.y > noseBase || p.z < hinge_.z - 0.02f * faceW_ || p.y < chinY - 0.42f * faceW_) pin(v, 0);
        }
        const char lower = 1, upper = 0;
        for (const int i : kUpperOuter) pin(NearestOuter(i, &upper), 0);
        for (const int i : kUpperInner) pin(NearestOuter(i, &upper), 0);
        for (const int i : kCheekL) pin(NearestOuter(i, nullptr), 0);
        for (const int i : kCheekR) pin(NearestOuter(i, nullptr), 0);
        for (const int i : {234, 454, 93, 323, 127, 356}) pin(NearestOuter(i, nullptr), 0);
        for (const int i : kLowerOuter) pin(NearestOuter(i, &lower), 1);
        for (const int i : kLowerInner) pin(NearestOuter(i, &lower), 1);
        for (const int i : kChin) pin(NearestOuter(i, nullptr), 1);
        for (const int i : {199, 200, 175, 18, 83, 313, 201, 421}) pin(NearestOuter(i, nullptr), 1);
        for (int it = 0; it < 900; ++it) // Gauss-Seidel with over-relaxation
            for (int v = 0; v < nw; ++v) {
                if (fixed[v] || surfNbr_[v].empty()) continue;
                float sw = 0, sum = 0;
                for (const auto& e : surfNbr_[v]) {
                    const float k = 1.0f / std::max(e.second, 1e-5f);
                    sw += k;
                    sum += k * h[e.first];
                }
                h[v] += 1.5f * (sum / sw - h[v]);
            }
        jawW_.assign(base_.size(), 0.0f);
        for (size_t i = 0; i < skinEnd_; ++i) jawW_[i] = std::clamp(h[weldOf_[i]], 0.0f, 1.0f);
        // full opening: the angle that drops the lower lip's edge as far as a real mouth opens
        jawGapAtFull_ = kJawGap * faceW_;
        const float ry = seamY - hinge_.y, rz = lipZ - hinge_.z;
        jawAngle_ = 0.1f;
        while (jawAngle_ < 0.65f && -(ry * std::cos(jawAngle_) - rz * std::sin(jawAngle_) - ry) < jawGapAtFull_) jawAngle_ += 0.002f;
    }

    // What the jaw bone does to a point with weight `wgt`: a true rotation, so teeth stay rigid at any opening.
    V JawMove(float x, float y, float z, float wgt, float open, float side, float forward) const {
        const float ang = jawAngle_ * open, ca = std::cos(ang), sa = std::sin(ang);
        const float ry = y - hinge_.y, rz = z - hinge_.z;
        const float slide = kJawGlide * faceW_ * open; // the condyle rides forward as the jaw opens wide
        (void)x;
        return {side * kJawSide * faceW_ * wgt, (ry * ca - rz * sa - ry) * wgt,
                (ry * sa + rz * ca - rz + slide + forward * kJawForward * faceW_) * wgt};
    }

    // Nearest skin in front of (x, y) looking along +z from zFrom: where the mouth's front wall is. Large when nothing is hit.
    float WallZ(float x, float y, float zFrom) const {
        float best = 1e30f;
        for (size_t t = 0; t + 2 < skinEnd_; t += 3) {
            const Vertex &a = base_[t], &b = base_[t + 1], &c = base_[t + 2];
            if (std::max({a.pz, b.pz, c.pz}) < zFrom) continue;
            const float d = (b.px - a.px) * (c.py - a.py) - (c.px - a.px) * (b.py - a.py);
            if (std::abs(d) < 1e-12f) continue;
            const float u = ((x - a.px) * (c.py - a.py) - (c.px - a.px) * (y - a.py)) / d;
            const float v = ((b.px - a.px) * (y - a.py) - (x - a.px) * (b.py - a.py)) / d;
            if (u < 0 || v < 0 || u + v > 1) continue;
            const float z = a.pz + u * (b.pz - a.pz) + v * (c.pz - a.pz);
            if (z > zFrom && z < best) best = z;
        }
        return best;
    }

    struct Rgb { float r, g, b; };
    // One lit triangle added behind the file's own, facing away from `from`, with its jaw weight.
    void PushTri(V a, V b, V c, Rgb ca, Rgb cb, Rgb cc, float jaw, const V& from) {
        const V u = {b.x - a.x, b.y - a.y, b.z - a.z}, v = {c.x - a.x, c.y - a.y, c.z - a.z};
        V n = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
        const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (l < 1e-14f) return;
        n = {n.x / l, n.y / l, n.z / l};
        const V m = {(a.x + b.x + c.x) / 3 - from.x, (a.y + b.y + c.y) / 3 - from.y, (a.z + b.z + c.z) / 3 - from.z};
        if (n.x * m.x + n.y * m.y + n.z * m.z < 0) { std::swap(b, c); std::swap(cb, cc); n = {-n.x, -n.y, -n.z}; }
        const V p[3] = {a, b, c};
        const Rgb col[3] = {ca, cb, cc};
        for (int k = 0; k < 3; ++k) {
            Vertex x{};
            x.px = p[k].x; x.py = p[k].y; x.pz = p[k].z;
            x.nx = n.x; x.ny = n.y; x.nz = n.z;
            x.r = col[k].r; x.g = col[k].g; x.b = col[k].b; x.a = 1;
            base_.push_back(x);
            jawW_.push_back(jaw);
        }
    }

    // What the mouth holds. The skin's own inside is the mouth bag: it is tinted here, wet near the lips and black deep
    // in, and nothing is glued over the lips. Teeth are a real dental arch in real proportions (a face is about 140 mm
    // wide), with the biting line at the lips' seam: closed lips hide them, a raised lip uncovers them.
    void BuildMouthInterior() {
        auto L = [&](int i) { return V{lm_[i * 3], lm_[i * 3 + 1], lm_[i * 3 + 2]}; };
        const float mm = faceW_ / 140.0f;
        const float seamY = (L(13).y + L(14).y) * 0.5f, lipZ = (L(13).z + L(14).z) * 0.5f, eyeY = L(159).y;
        const char lower = 1, upper = 0;
        std::vector<int> rim;
        for (const int i : facemap::kUpperInner) rim.push_back(NearestOuter(i, &upper));
        for (const int i : facemap::kLowerInner) rim.push_back(NearestOuter(i, &lower));
        std::vector<float> dRim;
        Distances(rim, 1e30f, dRim);
        for (int v = 0; v < (int)surfPos_.size(); ++v) {
            if (!inside_[v]) continue;
            const float py = surfPos_[v].y;
            const float keep = std::clamp((py - (eyeY - 0.26f * faceW_)) / (0.1f * faceW_), 0.0f, 1.0f); // eye sockets stay as they are
            const float t = std::clamp(dRim[v] / (0.22f * faceW_), 0.0f, 1.0f);
            const Rgb wet = {0.72f, 0.36f, 0.36f}, deep = {0.035f, 0.012f, 0.014f};
            const float s = t * t * (3 - 2 * t);
            const Rgb c = {wet.r + (deep.r - wet.r) * s, wet.g + (deep.g - wet.g) * s, wet.b + (deep.b - wet.b) * s};
            for (const uint32_t i : vertsOf_[v]) {
                base_[i].r *= c.r + (1 - c.r) * keep;
                base_[i].g *= c.g + (1 - c.g) * keep;
                base_[i].b *= c.b + (1 - c.b) * keep;
            }
        }
        cavityFirst_ = base_.size();
        jawW_.resize(base_.size(), 0.0f);
        if (!hasMouth_) return; // welded lips: no mouth to open, nothing goes behind them
        // The wall behind the lips stands back a little: the skin at the foot of a lip is thin, and a lowered lip would
        // otherwise bring the wall through it.
        for (int v = 0; v < (int)surfPos_.size(); ++v) {
            if (!inside_[v] || surfPos_[v].z < lipZ - 0.35f * faceW_ || std::abs(surfPos_[v].y - seamY) > 0.3f * faceW_) continue;
            const float t = std::clamp(dRim[v] / (0.07f * faceW_), 0.0f, 1.0f);
            for (const uint32_t i : vertsOf_[v]) base_[i].pz -= t * t * (3 - 2 * t) * 3 * mm;
        }

        struct ToothDim { float w, h, d; int kind; };
        static const ToothDim upperTeeth[7] = {{8.6f, 10.6f, 6.6f, 0}, {6.6f, 9.2f, 6.2f, 0}, {7.6f, 10.4f, 8.0f, 1}, {7.0f, 8.4f, 9.0f, 2},
                                               {6.8f, 7.6f, 9.0f, 2}, {10.2f, 7.2f, 10.4f, 3}, {9.6f, 6.8f, 10.0f, 3}};
        static const ToothDim lowerTeeth[7] = {{5.3f, 9.0f, 6.0f, 0}, {5.8f, 9.4f, 6.2f, 0}, {6.8f, 10.0f, 7.6f, 1}, {7.0f, 8.4f, 7.8f, 2},
                                               {7.1f, 7.8f, 8.2f, 2}, {11.0f, 7.0f, 10.2f, 3}, {10.4f, 6.6f, 10.0f, 3}};
        const float zProbe = lipZ - 34 * mm; // well behind the lips, in the cavity
        // The rows part a little at rest, as real jaws do, so the slit between closed lips shows dark, not enamel.
        const float cornerY = (L(78).y + L(308).y) * 0.5f, halfMouth = std::max(std::abs(L(308).x - L(78).x) * 0.5f, 1e-4f);
        auto seamLift = [&](float x) { const float t = std::min(std::abs(x) / halfMouth, 1.0f); return (cornerY - seamY) * t * t; };
        const float edgeUpper = seamY + 2.5f * mm, edgeLower = seamY - 3.0f * mm; // at rest; see kUpperReveal
        for (int row = 0; row < 2; ++row) {
            const bool up = row == 0;
            (up ? upperRow_ : lowerRow_)[0] = base_.size();
            const ToothDim* dims = up ? upperTeeth : lowerTeeth;
            const float dir = up ? 1.0f : -1.0f, edge = up ? edgeUpper : edgeLower, jaw = up ? 0.0f : 1.0f;
            const float neckH = up ? 10.6f : 9.0f;
            // the arch's front, just behind the front wall of the mouth
            float z0 = 1e30f;
            for (const float dx : {-4.0f, 4.0f})
                for (const float dy : {1.0f, 0.6f * neckH + 3.0f})
                    z0 = std::min(z0, WallZ(mouthCx_ + dx * mm, edge + dir * dy * mm, zProbe));
            if (z0 > 1e29f) z0 = lipZ - 8 * mm;
            z0 -= (up ? 0.8f : 1.6f) * mm;
            const float halfW = (up ? 31.0f : 28.5f) * mm, depth = (up ? 47.0f : 43.0f) * mm;
            auto archZ = [&](float x) { const float t = x / halfW; return z0 - depth * t * t; };
            std::unordered_map<int, float> archCache;
            auto archFront = [&](float x) { // the wall clamps the arch where a narrow head would poke through
                const int key = (int)std::lround(x / (0.5f * mm)); // rays are costly and the wall is smooth
                const auto hit = archCache.find(key);
                if (hit != archCache.end()) return hit->second - depth * ((x / halfW) * (x / halfW) - (key * 0.5f * mm / halfW) * (key * 0.5f * mm / halfW));
                x = key * 0.5f * mm;
                float z = archZ(x);
                for (const float sgn : {-1.0f, 1.0f})
                    for (const float dy : {0.0f, 6.0f, 11.0f}) {
                        const float wall = WallZ(mouthCx_ + sgn * x, edge + dir * dy * mm, zProbe - 60 * mm);
                        if (wall < 1e29f) z = std::min(z, wall - 1.5f * mm);
                    }
                archCache[key] = z;
                return z;
            };
            auto outward = [&](float x, float& nx, float& nz) { // the arch's outward normal at x
                const float s = 2 * depth * x / (halfW * halfW), l = std::sqrt(1 + s * s);
                nx = s / l;
                nz = 1 / l;
            };
            // ---- teeth, walking along the arch from the midline
            for (const float sgn : {-1.0f, 1.0f}) {
                float x = 0;
                for (int k = 0; k < 7; ++k) {
                    const ToothDim& td = dims[k];
                    const float tw = td.w * mm;
                    float along = 0, xc = x; // step along the curve to the tooth's middle
                    while (along < tw * 0.5f) { float nx, nz; outward(xc, nx, nz); xc += 0.05f * mm * nz; along += 0.05f * mm; }
                    float nx, nz;
                    outward(xc, nx, nz);
                    const float depthFrac = std::clamp((z0 - archZ(xc)) / depth, 0.0f, 1.0f), aoT = std::clamp((depthFrac - 0.12f) / 0.75f, 0.0f, 1.0f), ao = 1.0f - 0.82f * aoT * aoT * (3 - 2 * aoT);
                    const float front = archFront(xc);
                    const V base = {mouthCx_ + sgn * xc - sgn * nx * td.d * 0.5f * mm, edge + seamLift(xc), front - nz * td.d * 0.5f * mm};
                    Tooth(base, sgn * nx, nz, tw, td.d * mm, td.h * mm, td.kind, dir, jaw, ao);
                    along = 0;
                    while (along < tw) { float ax, az; outward(x, ax, az); x += 0.05f * mm * az; along += 0.05f * mm; }
                }
            }
            // ---- gum: one band along the arch, tooth necks sunk in it; then the palate or the floor of the mouth behind it
            const size_t gumFirst = base_.size();
            constexpr int N = 22, S = 6;
            const float reach = 44.0f * mm;
            const float gumH = neckH * mm;
            auto section = [&](float xa, int s, float sgn) {
                const float x = std::min(xa, reach);
                float nx, nz;
                outward(x, nx, nz);
                const float front = archFront(x);
                static const float off[S][2] = {{-0.2f, 0.74f}, {0.5f, 0.92f}, {0.5f, 1.45f}, {-1.0f, 2.2f}, {-10.5f, 1.9f}, {-11.5f, 0.55f}};
                const float o = off[s][0] * mm, hgt = off[s][1] * gumH;
                return V{mouthCx_ + sgn * (x + nx * o), edge + seamLift(x) + dir * hgt, front + nz * o};
            };
            auto gumCol = [&](float x, int s) {
                const float t = std::clamp((z0 - archZ(x)) / depth, 0.0f, 1.0f), ao = 1.0f - 0.75f * t;
                const Rgb c = s <= 1 ? Rgb{0.76f, 0.42f, 0.42f} : s <= 3 ? Rgb{0.62f, 0.28f, 0.30f} : Rgb{0.46f, 0.18f, 0.21f};
                return Rgb{c.r * ao, c.g * ao, c.b * ao};
            };
            const V inner = {mouthCx_, edge + dir * gumH * 0.8f, z0 - 8 * mm};
            for (const float sgn : {-1.0f, 1.0f})
                for (int i = 0; i < N; ++i) {
                    const float xa = i * (halfW * 1.05f) / N, xb = (i + 1) * (halfW * 1.05f) / N;
                    for (int s = 0; s + 1 < S; ++s) {
                        const V a = section(xa, s, sgn), b = section(xb, s, sgn), c = section(xb, s + 1, sgn), d = section(xa, s + 1, sgn);
                        const Rgb ca = gumCol(xa, s), cb = gumCol(xb, s), cc = gumCol(xb, s + 1), cd = gumCol(xa, s + 1);
                        const V from = {mouthCx_ + sgn * (xa - 6 * mm), edge - dir * 2 * mm, a.z - 6 * mm};
                        PushTri(a, b, c, ca, cb, cc, jaw, from);
                        PushTri(a, c, d, ca, cc, cd, jaw, from);
                    }
                    // palate or floor: from the gum's inner edge to the midline, arched
                    constexpr int M = 5;
                    auto roof = [&](float xq, int m) {
                        const V e = section(xq, S - 1, sgn);
                        const float t = (float)m / M, dome = (up ? 9.0f : -7.0f) * mm * std::sin(t * 1.5708f);
                        return V{e.x + (mouthCx_ - e.x) * t, e.y + dome, e.z};
                    };
                    auto roofCol = [&](float xq, int m) {
                        const float t = std::clamp((z0 - archZ(xq)) / depth, 0.0f, 1.0f), ao = (1.0f - 0.85f * t) * (1.0f - 0.25f * m / M);
                        return Rgb{0.60f * ao, 0.24f * ao, 0.26f * ao};
                    };
                    const V from = {inner.x, edge - dir * 6 * mm, inner.z};
                    for (int m = 0; m < M; ++m) {
                        const V a = roof(xa, m), b = roof(xb, m), c = roof(xb, m + 1), d = roof(xa, m + 1);
                        PushTri(a, b, c, roofCol(xa, m), roofCol(xb, m), roofCol(xb, m + 1), jaw, from);
                        PushTri(a, c, d, roofCol(xa, m), roofCol(xb, m + 1), roofCol(xa, m + 1), jaw, from);
                    }
                }
            if (up) archFrontUpper_ = z0;
            else archFrontLower_ = z0;
            (up ? upperRow_ : lowerRow_)[1] = base_.size();
            for (size_t i = gumFirst; i < base_.size(); ++i) base_[i].a = 0.985f; // wet tissue, MouthWetPS
        }
        // ---- tongue, riding the jaw: lies in the floor of the mouth, tip behind the lower front teeth
        {
            constexpr int NU = 14, NV = 14;
            const float tipZ = archFrontLower_ - 8.0f * mm, len = 62 * mm, y0 = edgeLower - 3.5f * mm;
            auto P = [&](int iu, int iv) {
                const float r = -1 + 2.0f * iu / NU, a = (float)iv / NV;
                // as wide as the inside of the arch, which is narrow toward the front
                const float hw = std::max(0.92f * (28.5f * mm * std::sqrt((8 * mm + a * len) / (43 * mm)) - 5.5f * mm), 2 * mm);
                const float hump = 4.0f * mm * std::sin(a * 3.14159265f * 0.85f);
                const float groove = 0.9f * mm * std::exp(-(r / 0.2f) * (r / 0.2f));
                const float drop = 7 * mm * r * r * r * r + 1.2f * mm * r * r;
                return V{mouthCx_ + hw * r, y0 + hump - groove - drop, tipZ - a * len};
            };
            auto Col = [&](int iv) {
                const float ao = 1.0f - 0.8f * iv / NV;
                return Rgb{0.80f * ao, 0.40f * ao, 0.41f * ao};
            };
            const V from = {mouthCx_, y0 - 30 * mm, tipZ - len * 0.5f};
            for (int iv = 0; iv < NV; ++iv)
                for (int iu = 0; iu < NU; ++iu) {
                    PushTri(P(iu, iv), P(iu + 1, iv), P(iu + 1, iv + 1), Col(iv), Col(iv), Col(iv + 1), 1, from);
                    PushTri(P(iu, iv), P(iu + 1, iv + 1), P(iu, iv + 1), Col(iv), Col(iv + 1), Col(iv + 1), 1, from);
                }
        }
        mouthEnd_ = base_.size();
        for (size_t i = lowerRow_[1]; i < mouthEnd_; ++i) base_[i].a = 0.985f; // the tongue is wet tissue too
    }

    // One tooth: a stack of rounded sections from the biting edge (y = base.y) to the neck, thin at an incisor's edge and
    // blunt on a molar. (nx, nz) is the arch's outward direction, so its broad face looks out along the arch.
    void Tooth(const V& base, float nx, float nz, float tw, float td, float th, int kind, float dir, float jaw, float ao) {
        constexpr int NR = 5, NS = 10;
        const size_t firstVertex = base_.size();
        static const float edgeW[4] = {0.94f, 0.62f, 0.86f, 0.92f}, edgeD[4] = {0.34f, 0.52f, 0.72f, 0.82f};
        const V tan = {nz, 0, -nx};
        auto P = [&](int ir, int is) {
            const float t = (float)ir / NR;
            const float rise = std::min(t / 0.34f, 1.0f), neck = std::max((t - 0.48f) / 0.52f, 0.0f);
            const float ws = (edgeW[kind] + (1 - edgeW[kind]) * rise) * (1 - 0.13f * neck) * 1.03f;
            const float ds = (edgeD[kind] + (1 - edgeD[kind]) * rise) * (1 - 0.14f * neck);
            const float ph = 2 * 3.14159265f * is / NS, c = std::cos(ph), s = std::sin(ph);
            const float u = 0.5f * tw * ws * (c < 0 ? -1.0f : 1.0f) * std::pow(std::abs(c), 0.62f);
            const float v = 0.5f * td * ds * (s < 0 ? -1.0f : 1.0f) * std::pow(std::abs(s), 0.62f);
            return V{base.x + tan.x * u + nx * v, base.y + dir * th * t, base.z + tan.z * u + nz * v};
        };
        auto Col = [&](int ir, int is = 3) {
            // a tooth's sides turn away from the light and into the gap beside it
            const float side = std::abs(std::cos(2 * 3.14159265f * is / NS)), gap = 1.0f - 0.2f * side * side * side * side;
            const float t = (float)ir / NR, a = ao * (dir > 0 ? 1.0f : 0.95f) * gap;
            const Rgb edge = {0.89f, 0.87f, 0.79f}, body = {0.95f, 0.91f, 0.80f}, neck = {0.88f, 0.76f, 0.56f};
            const float m = t < 0.4f ? t / 0.4f : 1.0f, n = t > 0.55f ? (t - 0.55f) / 0.45f : 0.0f;
            const float r = edge.r + (body.r - edge.r) * m + (neck.r - body.r) * n;
            const float g = edge.g + (body.g - edge.g) * m + (neck.g - body.g) * n;
            const float b = edge.b + (body.b - edge.b) * m + (neck.b - body.b) * n;
            return Rgb{r * a, g * a, b * a};
        };
        const V mid = {base.x, base.y + dir * th * 0.5f, base.z};
        for (int ir = 0; ir < NR; ++ir)
            for (int is = 0; is < NS; ++is) {
                PushTri(P(ir, is), P(ir, is + 1), P(ir + 1, is + 1), Col(ir, is), Col(ir, is + 1), Col(ir + 1, is + 1), jaw, mid);
                PushTri(P(ir, is), P(ir + 1, is + 1), P(ir + 1, is), Col(ir, is), Col(ir + 1, is + 1), Col(ir + 1, is), jaw, mid);
            }
        for (int is = 0; is < NS; ++is) // the biting edge, closed
            PushTri(P(0, is), P(0, is + 1), {base.x, base.y - dir * 0.12f * th * 0.1f, base.z}, Col(0), Col(0), Col(0), jaw, mid);
        // For ToothPS (alpha 0.995): u runs from the biting edge to the neck, v is this tooth's own variation.
        const float seed = std::fmod(std::abs(base.x) * 917.3f + std::abs(base.z) * 311.7f + (float)kind * 0.37f, 1.0f);
        for (size_t i = firstVertex; i < base_.size(); ++i) {
            base_[i].u = std::clamp(dir * (base_[i].py - base.y) / th, 0.0f, 1.0f);
            base_[i].v = seed;
            base_[i].a = 0.995f;
        }
    }

    // Where each eyeball is: the eye parts are the vertices after the skin. Eye-look turns the ball about its centre, and
    // a closing lid slides over its front.
    void FindEyes(const RigExtras& ex) {
        eyeRanges_ = ex.eyeRanges;
        eyes_.clear();
        // The socket's wall stands back from the thin lid skin, or a closing lid would show it through.
        {
            std::vector<int> rim;
            for (const int i : facemap::kLidUpL) rim.push_back(NearestOuter(i, nullptr));
            for (const int i : facemap::kLidLoL) rim.push_back(NearestOuter(i, nullptr));
            for (const int i : facemap::kLidUpR) rim.push_back(NearestOuter(i, nullptr));
            for (const int i : facemap::kLidLoR) rim.push_back(NearestOuter(i, nullptr));
            std::vector<float> d;
            Distances(rim, 0.3f * faceW_, d);
            for (int v = 0; v < (int)surfPos_.size(); ++v) {
                if (!inside_[v] || d[v] > 1e29f) continue;
                const float t = std::clamp(d[v] / (0.035f * faceW_), 0.0f, 1.0f);
                for (const uint32_t i : vertsOf_[v]) base_[i].pz -= t * t * (3 - 2 * t) * 0.03f * faceW_;
            }
        }
        for (const bool left : {false, true}) {
            V lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
            size_t count = 0;
            for (const auto& r : ex.eyeRanges)
                for (size_t i = r.first; i < r.first + r.second && i < base_.size(); ++i) {
                    const Vertex& v = base_[i];
                    if ((v.px >= mouthCx_) != left) continue;
                    lo = {std::min(lo.x, v.px), std::min(lo.y, v.py), std::min(lo.z, v.pz)};
                    hi = {std::max(hi.x, v.px), std::max(hi.y, v.py), std::max(hi.z, v.pz)};
                    ++count;
                }
            if (count < 12) continue;
            Eye e;
            e.left = left;
            e.c = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
            e.r = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) * 0.5f;
            eyes_.push_back(e);
        }
    }

    // Each hair card vertex follows the nearest outside skin vertex, so a beard goes where the jaw and lips go.
    void BindHair(const RigExtras& ex) {
        hairBind_.clear();
        hairVertex_.assign(base_.size(), 0);
        if (ex.hairRanges.empty()) return;
        const float cell = std::max(0.08f * faceW_, 1e-5f);
        const auto key = [&](float x, float y, float z) {
            const auto q = [&](float a) { return (uint64_t)(int64_t)std::floor(a / cell) & 0x1FFFFF; };
            return (q(x) << 42) | (q(y) << 21) | q(z);
        };
        std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
        for (size_t i = 0; i < skinEnd_; ++i)
            if (!inside_[weldOf_[i]]) grid[key(base_[i].px, base_[i].py, base_[i].pz)].push_back((uint32_t)i);
        for (const auto& r : ex.hairRanges)
            for (size_t i = r.first; i < r.first + r.second && i < base_.size(); ++i) {
                hairVertex_[i] = 1;
                const Vertex& h = base_[i];
                uint32_t best = UINT32_MAX;
                float bestD = 1e30f;
                for (int reach = 1; reach <= 4 && best == UINT32_MAX; ++reach) // the first shell that holds any skin, then its neighbours
                    for (int dx = -reach; dx <= reach; ++dx)
                        for (int dy = -reach; dy <= reach; ++dy)
                            for (int dz = -reach; dz <= reach; ++dz) {
                                const auto it = grid.find(key(h.px + dx * cell, h.py + dy * cell, h.pz + dz * cell));
                                if (it == grid.end()) continue;
                                for (const uint32_t s : it->second) {
                                    const Vertex& b = base_[s];
                                    const float d = (b.px - h.px) * (b.px - h.px) + (b.py - h.py) * (b.py - h.py) + (b.pz - h.pz) * (b.pz - h.pz);
                                    if (d < bestD) { bestD = d; best = s; }
                                }
                            }
                if (best != UINT32_MAX) hairBind_.push_back({(uint32_t)i, best});
            }
    }

    // How much of the head's turn each vertex takes: all of it from the chin up, none low on the neck, a smooth twist between.
    void BuildHeadWeights() {
        const float chin = lm_[152 * 3 + 1];
        const float low = chin - 0.40f * faceW_, high = chin + 0.02f * faceW_;
        headW_.resize(base_.size());
        for (size_t i = 0; i < base_.size(); ++i) {
            const float t = std::clamp((base_[i].py - low) / (high - low), 0.0f, 1.0f);
            headW_[i] = t * t * (3 - 2 * t);
        }
        headPivot_ = {mouthCx_, chin - 0.12f * faceW_, (lm_[234 * 3 + 2] + lm_[454 * 3 + 2]) * 0.5f};
    }

    // Eye-look channels turn the eyeball itself: in/out about Y, up/down about X. Stored like any other displacement.
    void AddEyeChannels() {
        for (const Eye& eye : eyes_) {
            struct Look { int channel; bool aboutY; float angle; };
            const float sgn = eye.left ? -1.0f : 1.0f; // looking "in" is toward the nose: -X for the subject's left eye
            const Look looks[4] = {{eye.left ? 13 : 14, true, 0.42f * sgn}, {eye.left ? 15 : 16, true, -0.42f * sgn},
                                   {eye.left ? 17 : 18, false, -0.32f}, {eye.left ? 11 : 12, false, 0.32f}};
            for (const Look& lk : looks) {
                const float ca = std::cos(lk.angle), sa = std::sin(lk.angle);
                for (const auto& r : eyeRanges_)
                    for (size_t i = r.first; i < r.first + r.second && i < cavityFirst_; ++i) {
                        const Vertex& v = base_[i];
                        if ((v.px >= mouthCx_) != eye.left) continue;
                        const float x = v.px - eye.c.x, y = v.py - eye.c.y, z = v.pz - eye.c.z;
                        if (lk.aboutY) sparse_[lk.channel].push_back({(uint32_t)i, {x * ca + z * sa - x, 0.0f, -x * sa + z * ca - z}});
                        else sparse_[lk.channel].push_back({(uint32_t)i, {0.0f, y * ca - z * sa - y, y * sa + z * ca - z}});
                    }
            }
            // a closing lid rides over the ball: anything a lid channel would sink into it stays on its surface
            const float rr = eye.r * 1.08f;
            // ... and the ball itself draws back a little, as a real one does in a blink
            for (const auto& r : eyeRanges_)
                for (size_t i = r.first; i < r.first + r.second && i < cavityFirst_; ++i)
                    if ((base_[i].px >= mouthCx_) == eye.left) sparse_[eye.left ? BS_eyeBlinkLeft : BS_eyeBlinkRight].push_back({(uint32_t)i, {0, 0, -0.03f * faceW_}});
            for (const int c : {eye.left ? BS_eyeBlinkLeft : BS_eyeBlinkRight, eye.left ? BS_eyeSquintLeft : BS_eyeSquintRight})
                for (Entry& en : sparse_[c]) {
                    if (en.i >= skinEnd_ || inside_[weldOf_[en.i]]) continue;
                    const Vertex& v = base_[en.i];
                    const float x = v.px + en.d.x - eye.c.x, y = v.py + en.d.y - eye.c.y;
                    const float under = rr * rr - x * x - y * y;
                    if (under <= 0) continue;
                    const float front = eye.c.z + std::sqrt(under);
                    if (v.pz + en.d.z < front) en.d.z = front - v.pz;
                }
        }
    }

    // Rest pose + weighted displacements, then the jaw bone, with smooth normals recomputed. `weights` are the 52 blendshape weights.
    // `head`: pitch, yaw, roll in degrees from neutral, or null. The head turns on the neck within kHeadLimit; the neck's base stays.
    // The lids as they lie when closed: a real upper lid unrolls out of its crease as it comes down, so a closed eye shows a
    // smooth lid, not the open eye's fold. `relaxed` is the head as given to Init (same importer, same vertex order) with
    // that fold smoothed away (<head>.lid_relaxed.glb beside the head). Its offsets are carried onto this rig's vertices,
    // which Init subdivides around the lids, by inverse distance from the nearest source vertices, and Pose blends them in
    // with each eye's blink from 15% to fully shut. Call after Init; a size mismatch leaves the rig as it was.
    void SetLidRelax(const std::vector<Vertex>& rest, const std::vector<Vertex>& relaxed) {
        relax_[0].clear(); relax_[1].clear();
        if (rest.size() != relaxed.size() || rest.empty()) return;
        struct S { float x, y, z; V d; };
        std::vector<S> moved;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < rest.size(); ++i) {
            const V d{relaxed[i].px - rest[i].px, relaxed[i].py - rest[i].py, relaxed[i].pz - rest[i].pz};
            if (d.x * d.x + d.y * d.y + d.z * d.z < 1e-14f) continue;
            moved.push_back({rest[i].px, rest[i].py, rest[i].pz, d});
            const float p[3] = {rest[i].px, rest[i].py, rest[i].pz};
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
        }
        if (moved.empty()) return;
        const float pad = 0.004f * std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-6f}) / 0.03f; // ~4 mm on a head
        // the unmoved rest vertices around the moved ones anchor the falloff at zero; a flat triangle list repeats every
        // position, so each is kept once, then bucketed in a grid of `pad`-sized cells for the nearest-three search
        std::vector<S> src;
        std::unordered_map<int64_t, size_t> seen;
        const auto key = [](float x, float y, float z, float cell) {
            return ((int64_t)std::floor(x / cell) * 73856093LL) ^ ((int64_t)std::floor(y / cell) * 19349663LL) ^ ((int64_t)std::floor(z / cell) * 83492791LL);
        };
        for (const S& s : moved) if (seen.emplace(key(s.x, s.y, s.z, pad * 1e-3f), src.size()).second) src.push_back(s);
        for (const Vertex& r : rest)
            if (r.px > lo[0] - pad && r.px < hi[0] + pad && r.py > lo[1] - pad && r.py < hi[1] + pad && r.pz > lo[2] - pad && r.pz < hi[2] + pad)
                if (seen.emplace(key(r.px, r.py, r.pz, pad * 1e-3f), src.size()).second) src.push_back({r.px, r.py, r.pz, {0, 0, 0}});
        const float cell = pad * 0.5f;
        std::unordered_map<int64_t, std::vector<uint32_t>> grid;
        const auto ci = [&](float v) { return (int64_t)std::floor(v / cell); };
        const auto gk = [](int64_t x, int64_t y, int64_t z) { return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); };
        for (uint32_t k = 0; k < src.size(); ++k) grid[gk(ci(src[k].x), ci(src[k].y), ci(src[k].z))].push_back(k);
        for (size_t i = 0; i < skinEnd_ && i < base_.size(); ++i) {
            const Vertex& b = base_[i];
            if (b.px < lo[0] - pad || b.px > hi[0] + pad || b.py < lo[1] - pad || b.py > hi[1] + pad || b.pz < lo[2] - pad || b.pz > hi[2] + pad) continue;
            float best[3] = {1e30f, 1e30f, 1e30f};
            const S* who[3] = {};
            const int64_t x0 = ci(b.px), y0 = ci(b.py), z0 = ci(b.pz);
            for (int64_t dx = -1; dx <= 1; ++dx) for (int64_t dy = -1; dy <= 1; ++dy) for (int64_t dz = -1; dz <= 1; ++dz) {
                const auto it = grid.find(gk(x0 + dx, y0 + dy, z0 + dz));
                if (it == grid.end()) continue;
                for (const uint32_t k : it->second) {
                    const S& s = src[k];
                    const float dd = (s.x - b.px) * (s.x - b.px) + (s.y - b.py) * (s.y - b.py) + (s.z - b.pz) * (s.z - b.pz);
                    if (dd >= best[2]) continue;
                    int q = 2;
                    while (q > 0 && dd < best[q - 1]) { best[q] = best[q - 1]; who[q] = who[q - 1]; --q; }
                    best[q] = dd; who[q] = &s;
                }
            }
            V d{0, 0, 0};
            float wsum = 0;
            for (int k = 0; k < 3 && who[k]; ++k) {
                const float w = 1.0f / (std::sqrt(best[k]) + 1e-6f);
                d.x += who[k]->d.x * w; d.y += who[k]->d.y * w; d.z += who[k]->d.z * w; wsum += w;
            }
            if (wsum <= 0) continue;
            d = {d.x / wsum, d.y / wsum, d.z / wsum};
            if (d.x * d.x + d.y * d.y + d.z * d.z < 1e-14f) continue;
            relax_[b.px >= mouthCx_ ? 0 : 1].push_back({(uint32_t)i, d}); // 0: BS_eyeBlinkLeft's side (as the blink itself)
        }
    }

    void Pose(const float weights[kBlendshapes], std::vector<Vertex>& out, const float* head = nullptr) const {
        out = base_;
        bool any = false;
        const auto gained = [&](int c) { const ChannelTune& t = mapping_.ch[c]; return t.enabled ? weights[c] * t.gain : 0.0f; };
        const float jawOpen = gained(BS_jawOpen), jawSide = gained(BS_jawLeft) - gained(BS_jawRight), jawFwd = gained(BS_jawForward);
        for (int c = 0; c < kBlendshapes; ++c) {
            if (c == BS_jawOpen || c == BS_jawLeft || c == BS_jawRight || c == BS_jawForward) continue;
            float w = gained(c);
            if (c == BS_mouthClose) w = std::min(w, std::max(jawOpen, 0.0f)); // closes what the jaw opened, nothing more
            // a closing lid has only so far to go: squint and wide give way to the blink, or the lids would cross
            if (c == BS_eyeSquintLeft || c == BS_eyeWideLeft) w *= 1.0f - std::clamp(gained(BS_eyeBlinkLeft), 0.0f, 1.0f);
            if (c == BS_eyeSquintRight || c == BS_eyeWideRight) w *= 1.0f - std::clamp(gained(BS_eyeBlinkRight), 0.0f, 1.0f);
            if (c == BS_mouthSmileLeft || c == BS_mouthSmileRight) w *= kSmileStrength;
            if (std::abs(w) < 1e-3f) continue;
            any = true;
            for (const Entry& e : sparse_[c]) {
                out[e.i].px += e.d.x * w;
                out[e.i].py += e.d.y * w;
                out[e.i].pz += e.d.z * w;
            }
        }
        for (int s = 0; s < 2; ++s) { // the fold unrolls as the lid closes (SetLidRelax)
            const float b = std::clamp(gained(s == 0 ? BS_eyeBlinkLeft : BS_eyeBlinkRight), 0.0f, 1.0f);
            const float t = std::clamp((b - 0.15f) / 0.85f, 0.0f, 1.0f), f = t * t * (3 - 2 * t);
            if (f <= 1e-3f) continue;
            any = true;
            for (const Entry& e : relax_[s]) { out[e.i].px += e.d.x * f; out[e.i].py += e.d.y * f; out[e.i].pz += e.d.z * f; }
        }
        if (jawOpen > 1e-3f)
            for (const Entry& e : sparse_[kJawFix]) {
                out[e.i].px += e.d.x * jawOpen;
                out[e.i].py += e.d.y * jawOpen;
                out[e.i].pz += e.d.z * jawOpen;
            }
        // The rows stand apart behind closed lips, so the slit between them never shows enamel, and come to their biting
        // line as soon as the lips part.
        const float parted = std::clamp(6 * jawOpen + gained(BS_mouthUpperUpLeft) + gained(BS_mouthUpperUpRight) + gained(BS_mouthLowerDownLeft) +
                                        gained(BS_mouthLowerDownRight) + 2 * gained(BS_mouthFunnel), 0.0f, 1.0f);
        // Shut lips still leave a slit at the corners, more so in a smile, and it must look into a dark mouth, not at enamel.
        // Everything in the mouth lights up as the lips part.
        if (parted < 0.25f) {
            const float x = parted / 0.25f, seen = 0.04f + 0.96f * x * x * (3 - 2 * x);
            for (size_t i = upperRow_[0]; i < lowerRow_[1]; ++i) { out[i].r *= seen; out[i].g *= seen; out[i].b *= seen; }
            for (size_t i = lowerRow_[1]; i < mouthEnd_; ++i) { out[i].r *= seen; out[i].g *= seen; out[i].b *= seen; }
        }
        if (parted > 1e-3f) {
            const float t = parted * parted * (3 - 2 * parted) * faceW_ / 140.0f;
            for (size_t i = upperRow_[0]; i < upperRow_[1]; ++i) out[i].py -= t * kUpperReveal;
            for (size_t i = lowerRow_[0]; i < lowerRow_[1]; ++i) out[i].py += t * kLowerReveal;
        }
        if (fitted_ && (std::abs(jawOpen) > 1e-3f || std::abs(jawSide) > 1e-3f || std::abs(jawFwd) > 1e-3f)) {
            any = true;
            for (size_t i = 0; i < jawW_.size(); ++i) {
                if (jawW_[i] < 1e-3f) continue;
                const V d = JawMove(out[i].px, out[i].py, out[i].pz, jawW_[i], jawOpen, jawSide, jawFwd);
                out[i].px += d.x;
                out[i].py += d.y;
                out[i].pz += d.z;
            }
        }
        for (const HairBind& h : hairBind_) {
            out[h.vertex].px += out[h.skin].px - base_[h.skin].px;
            out[h.vertex].py += out[h.skin].py - base_[h.skin].py;
            out[h.vertex].pz += out[h.skin].pz - base_[h.skin].pz;
        }
        if (head && fitted_ && headW_.size() == out.size()) {
            // The neck gives way softly as it nears its limit, in degrees.
            const auto soft = [](float a, float limit) { return limit * std::tanh(a / limit); };
            const float pitch = soft(head[0], kHeadLimit[0]), yaw = soft(head[1], kHeadLimit[1]), roll = soft(head[2], kHeadLimit[2]);
            if (std::abs(pitch) + std::abs(yaw) + std::abs(roll) > 1e-2f) {
                any = true;
                constexpr int kSteps = 32;
                struct M3 { float m[9]; };
                M3 turns[kSteps + 1];
                for (int s = 0; s <= kSteps; ++s) { // Ry * Rx * Rz, the order QuatFromEuler uses, for this share of the turn
                    const float k = (float)s / kSteps * 3.14159265f / 180.0f;
                    const float cx = std::cos(pitch * k), sx = std::sin(pitch * k), cy = std::cos(yaw * k), sy = std::sin(yaw * k), cz = std::cos(roll * k), sz = std::sin(roll * k);
                    turns[s] = {{cy * cz + sy * sx * sz, -cy * sz + sy * sx * cz, sy * cx, cx * sz, cx * cz, -sx, -sy * cz + cy * sx * sz, sy * sz + cy * sx * cz, cy * cx}};
                }
                for (size_t i = 0; i < out.size(); ++i) {
                    if (headW_[i] <= 0) continue;
                    const float* m = turns[(int)std::lround(headW_[i] * kSteps)].m;
                    Vertex& v = out[i];
                    const float x = v.px - headPivot_.x, y = v.py - headPivot_.y, z = v.pz - headPivot_.z;
                    v.px = headPivot_.x + m[0] * x + m[1] * y + m[2] * z;
                    v.py = headPivot_.y + m[3] * x + m[4] * y + m[5] * z;
                    v.pz = headPivot_.z + m[6] * x + m[7] * y + m[8] * z;
                    if (i < hairVertex_.size() && hairVertex_[i]) { // cards keep their own normals: both sides sit at one position
                        const float nx = v.nx, ny = v.ny, nz = v.nz;
                        v.nx = m[0] * nx + m[1] * ny + m[2] * nz;
                        v.ny = m[3] * nx + m[4] * ny + m[5] * nz;
                        v.nz = m[6] * nx + m[7] * ny + m[8] * nz;
                    }
                }
            }
        }
        if (!any) return;
        // Wrinkle amount in vertex alpha: how far the most squeezed edge at each skin point has shortened.
        for (size_t s = 0; s < surfNbr_.size() && s < vertsOf_.size(); ++s) {
            if (vertsOf_[s].empty()) continue;
            const Vertex& a = out[vertsOf_[s][0]];
            float squeeze = 0;
            for (const auto& nb : surfNbr_[s]) {
                if (vertsOf_[nb.first].empty() || nb.second <= 0) continue;
                const Vertex& b = out[vertsOf_[nb.first][0]];
                const float dx = a.px - b.px, dy = a.py - b.py, dz = a.pz - b.pz;
                squeeze = std::max(squeeze, 1.0f - std::sqrt(dx * dx + dy * dy + dz * dz) / nb.second);
            }
            for (const uint32_t i : vertsOf_[s]) out[i].a = 0.5f + 0.09f * std::clamp(squeeze * 5.0f, 0.0f, 1.0f);
        }
        std::vector<V> sum(groupCount_);
        for (size_t t = 0; t + 2 < out.size(); t += 3) {
            const Vertex &a = out[t], &b = out[t + 1], &c = out[t + 2];
            const float ux = b.px - a.px, uy = b.py - a.py, uz = b.pz - a.pz;
            const float vx = c.px - a.px, vy = c.py - a.py, vz = c.pz - a.pz;
            const V nrm = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
            for (int k = 0; k < 3; ++k) {
                V& g = sum[group_[t + k]];
                g.x += nrm.x; g.y += nrm.y; g.z += nrm.z;
            }
        }
        for (size_t i = 0; i < out.size(); ++i) {
            if (base_[i].nx == 0 && base_[i].ny == 0 && base_[i].nz == 0) continue; // unlit stays unlit
            if (i < hairVertex_.size() && hairVertex_[i]) continue;
            const V& g = sum[group_[i]];
            const float l = std::sqrt(g.x * g.x + g.y * g.y + g.z * g.z);
            if (l < 1e-12f) continue;
            out[i].nx = g.x / l; out[i].ny = g.y / l; out[i].nz = g.z / l;
        }
    }

    // Largest displacement channel `c` gives any vertex (for painting the editor's influence view).
    float Influence(int c, size_t vertex) const {
        for (const Entry& e : sparse_[c])
            if (e.i == vertex) return std::sqrt(e.d.x * e.d.x + e.d.y * e.d.y + e.d.z * e.d.z) / width_;
        return 0;
    }
    // Per-vertex displacement magnitude of channel c in head widths (0 where untouched); size = vertex count.
    void InfluenceMap(int c, std::vector<float>& out) const {
        out.assign(base_.size(), 0.0f);
        for (const Entry& e : sparse_[c]) out[e.i] = std::sqrt(e.d.x * e.d.x + e.d.y * e.d.y + e.d.z * e.d.z) / width_;
    }

private:
    // The jaw at full open, in face widths: lip gap as measured on a real open mouth, sideways and forward travel, glide.
    static constexpr float kJawGap = 0.265f, kJawSide = 0.07f, kJawForward = 0.07f, kJawGlide = 0.035f;
    std::vector<Vertex> base_;
    static constexpr int kJawFix = kBlendshapes, kChannels = kBlendshapes + 1; // one more channel: what the jaw does to the lips
    std::vector<Entry> sparse_[kChannels];
    std::vector<Entry> relax_[2]; // SetLidRelax: per eye (0 = BS_eyeBlinkLeft's side), blended in by that eye's blink
    std::vector<int> group_;
    int groupCount_ = 0;
    FaceMapping mapping_;
    float lm_[kFaceLandmarks * 3] = {};
    static constexpr int kFaceDetailLevels = 2; // how many times the lips and eyelids are split; each level is four times the triangles
    size_t skinEnd_ = 0;              // base_[0..skinEnd_) is the skin, with its added detail
    std::vector<std::pair<size_t, size_t>> partRanges_;
    size_t cavityFirst_ = 0;          // first vertex of the mouth's contents appended by BuildMouthInterior
    std::vector<float> jawW_;         // jaw bone weight of every vertex (0 = skull, 1 = rigid with the jaw)
    // the welded skin
    std::vector<V> surfPos_;
    std::vector<std::vector<uint32_t>> vertsOf_;
    std::vector<int> weldOf_;
    std::vector<std::vector<std::pair<int, float>>> surfNbr_; // (neighbour, edge length)
    std::vector<char> inside_, lowerSide_;
    int insideCount_ = 0;
    bool hasMouth_ = false;
    std::vector<float> noseMask_, puffMask_;
    std::unordered_map<int, std::vector<std::pair<int, float>>> geo_;
    V hinge_;
    float jawAngle_ = 0.4f, jawGapAtFull_ = 0, archFrontUpper_ = 0, archFrontLower_ = 0;
    static constexpr float kArcEdge = 2.4f, kArcUpperOuter = 1.7f, kArcLowerOuter = 2.6f, kEdgePull = 30.0f; // how round each lip line stays in a smile; tuned with mouth_lab
    static constexpr float kUpperReveal = 5.5f, kLowerReveal = 0.6f; // mm the rows travel as the lips part
    size_t upperRow_[2] = {}, lowerRow_[2] = {}, mouthEnd_ = 0; // lowerRow_[1]..mouthEnd_ is the tongue
    float mouthCx_ = 0, faceW_ = 1;
    struct Eye {
        bool left = false;
        V c;       // eyeball centre
        float r = 0;
    };
    std::vector<Eye> eyes_;
    std::vector<std::pair<size_t, size_t>> eyeRanges_; // the eyeball parts' vertex ranges
    struct HairBind { uint32_t vertex, skin; };
    std::vector<HairBind> hairBind_;  // each hair card vertex and the skin vertex it rides
    std::vector<uint8_t> hairVertex_; // per vertex: is it on a hair card
    std::vector<float> headW_;        // per vertex: its share of the head's turn
    V headPivot_;
    static constexpr float kHeadLimit[3] = {35.0f, 55.0f, 28.0f}; // pitch, yaw, roll: where the neck stops, in degrees
    static constexpr float kSmileStrength = 0.65f;                // the owner found the full smile too strong
    V lo_, hi_;
    float width_ = 1;
    bool fitted_ = false;
};

// Index of the mirrored blendshape ("...Left" <-> "...Right"), or the index itself for centre shapes.
inline int MirroredBlendshape(int c) {
    static int table[kBlendshapes];
    static bool built = false;
    if (!built) {
        built = true;
        for (int i = 0; i < kBlendshapes; ++i) {
            table[i] = i;
            std::string n = kBlendshapeNames[i];
            const size_t at = n.rfind("Left") != std::string::npos ? n.rfind("Left") : std::string::npos;
            const size_t at2 = n.rfind("Right");
            std::string m = n;
            if (at != std::string::npos) m.replace(at, 4, "Right");
            else if (at2 != std::string::npos) m.replace(at2, 5, "Left");
            else continue;
            for (int j = 0; j < kBlendshapes; ++j)
                if (m == kBlendshapeNames[j]) table[i] = j;
        }
    }
    return table[c];
}

// When the webcam model is missing, the nine geometric channels still drive the matching blendshapes.
inline void BlendshapesFromChannels(const float weight[kFaceChannels], float bs[kBlendshapes]) {
    std::fill(bs, bs + kBlendshapes, 0.0f);
    bs[BS_jawOpen] = weight[0];
    bs[BS_mouthSmileLeft] = weight[1];
    bs[BS_mouthSmileRight] = weight[2];
    bs[BS_mouthPucker] = weight[3];
    bs[BS_eyeBlinkLeft] = weight[4];
    bs[BS_eyeBlinkRight] = weight[5];
    bs[BS_browDownLeft] = weight[6];
    bs[BS_browDownRight] = weight[7];
    bs[BS_browInnerUp] = weight[8];
}

} // namespace tbd
