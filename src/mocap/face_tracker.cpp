#include "face_tracker.h"

#ifndef TBD_NO_ONNX
#include <onnxruntime_cxx_api.h>
#endif

#include <algorithm>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace tbd {

namespace {

constexpr int kDetSize = 256;
constexpr int kLmSize = 192;
constexpr int kLm478Size = 256; // the iris-aware landmark model's input
constexpr int kMeshPoints = 478; // 468 face points + 10 iris points
constexpr float kDetThreshold = 0.6f;
constexpr float kLmThreshold = 0.5f;
constexpr float kRoiScale = 1.5f;
constexpr float kPi = 3.14159265358979f;

struct Vec3 {
    float x, y, z;
};
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 Norm(Vec3 a) {
    float l = std::sqrt(Dot(a, a));
    return l > 1e-9f ? Vec3{a.x / l, a.y / l, a.z / l} : Vec3{1, 0, 0};
}

// Rotated square search region in image pixels.
struct Roi {
    float cx, cy, size, theta; // theta: radians, eye line angle in the image
};

// Samples a size x size crop of the BGRA image, transform `m` maps crop pixels to image pixels (2x3, row major).
// Writes planar RGB floats in 0..1. Pixels outside the image are black.
void WarpToCHW(const uint8_t* bgra, int w, int h, const float m[6], int size, float* out) {
    const size_t plane = (size_t)size * size;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float sx = m[0] * x + m[1] * y + m[2];
            float sy = m[3] * x + m[4] * y + m[5];
            float r = 0, g = 0, b = 0;
            if (sx >= 0 && sy >= 0 && sx <= w - 1 && sy <= h - 1) {
                int x0 = (int)sx, y0 = (int)sy;
                int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
                float fx = sx - x0, fy = sy - y0;
                auto px = [&](int xx, int yy, int c) { return (float)bgra[((size_t)yy * w + xx) * 4 + c]; };
                auto bil = [&](int c) {
                    float top = px(x0, y0, c) * (1 - fx) + px(x1, y0, c) * fx;
                    float bot = px(x0, y1, c) * (1 - fx) + px(x1, y1, c) * fx;
                    return (top * (1 - fy) + bot * fy) * (1.0f / 255.0f);
                };
                b = bil(0);
                g = bil(1);
                r = bil(2);
            }
            size_t i = (size_t)y * size + x;
            out[i] = r;
            out[plane + i] = g;
            out[2 * plane + i] = b;
        }
}

// Anchor centres of the BlazeFace detector: 16x16 grid x2 anchors, then 8x8 grid x6 anchors (normalised).
std::vector<std::pair<float, float>> MakeAnchors() {
    std::vector<std::pair<float, float>> a;
    const struct { int grid, per; } layers[] = {{16, 2}, {8, 6}};
    for (auto& l : layers)
        for (int y = 0; y < l.grid; ++y)
            for (int x = 0; x < l.grid; ++x)
                for (int k = 0; k < l.per; ++k) a.push_back({(x + 0.5f) / l.grid, (y + 0.5f) / l.grid});
    return a;
}

// The 146 mesh points MediaPipe's blendshape model reads (indices 468..477 are the two irises, which this 468-point
// mesh lacks; they are synthesised from the eye corners and lids).
constexpr int kBsPoints[146] = {
    0, 1, 4, 5, 6, 7, 8, 10, 13, 14, 17, 21, 33, 37, 39,
    40, 46, 52, 53, 54, 55, 58, 61, 63, 65, 66, 67, 70, 78, 80,
    81, 82, 84, 87, 88, 91, 93, 95, 103, 105, 107, 109, 127, 132, 133,
    136, 144, 145, 146, 148, 149, 150, 152, 153, 154, 155, 157, 158, 159, 160,
    161, 162, 163, 168, 172, 173, 176, 178, 181, 185, 191, 195, 197, 234, 246,
    249, 251, 263, 267, 269, 270, 276, 282, 283, 284, 285, 288, 291, 293, 295,
    296, 297, 300, 308, 310, 311, 312, 314, 317, 318, 321, 323, 324, 332, 334,
    336, 338, 356, 361, 362, 365, 373, 374, 375, 377, 378, 379, 380, 381, 382,
    384, 385, 386, 387, 388, 389, 390, 397, 398, 400, 402, 405, 409, 415, 454,
    466, 468, 469, 470, 471, 472, 473, 474, 475, 476, 477};

float Sigmoid(float v) { return 1.0f / (1.0f + std::exp(-std::clamp(v, -80.0f, 80.0f))); }

float Percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0;
    size_t k = (size_t)(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

} // namespace

#ifndef TBD_NO_ONNX
struct FaceTracker::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "facemocap"};
    std::unique_ptr<Ort::Session> det, lm, bsNet; // bsNet: optional blendshape model
    std::unique_ptr<Ort::Session> lm478;           // optional: 478 landmarks including the irises (replaces `lm`)
    std::string lm478In, lm478Out0, lm478Out1;
    std::vector<float> lm478Buf = std::vector<float>(3 * kLm478Size * kLm478Size);
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<std::pair<float, float>> anchors = MakeAnchors();
    bool tracking = false;
    Roi roi{};
    std::vector<float> detIn = std::vector<float>(3 * kDetSize * kDetSize);
    std::vector<float> lmIn = std::vector<float>(3 * kLmSize * kLmSize);

    bool Detect(const uint8_t* bgra, int w, int h, Roi& out);
    bool Landmarks(const uint8_t* bgra, int w, int h, const Roi& roi, float* pts /*468*3 image px, z in px*/,
                   float& score);
    bool Landmarks478(const uint8_t* bgra, int w, int h, const Roi& roi, float* pts /*478*3*/, float& score);
    static Roi RoiFromMesh(const float* pts);
    static void FillPoseAndChannels(const float* pts, int w, int h, FaceFrame& out);
    void Blendshapes(const float* pts, FaceFrame& out);
    bool hasIris = false;
};

// Runs the blendshape model on the mesh (image pixels). The model normalises position and scale itself.
void FaceTracker::Impl::Blendshapes(const float* pts, FaceFrame& out) {
    if (!bsNet) return;
    float in[146 * 2];
    auto P = [&](int i, int c) { return pts[i * 3 + c]; };
    auto eye = [&](int outer, int inner, int up, int lo, float* c, float* r) { // iris centre and radius guess
        c[0] = (P(outer, 0) + P(inner, 0) + P(up, 0) + P(lo, 0)) * 0.25f;
        c[1] = (P(outer, 1) + P(inner, 1) + P(up, 1) + P(lo, 1)) * 0.25f;
        const float dx = P(outer, 0) - P(inner, 0), dy = P(outer, 1) - P(inner, 1);
        *r = 0.2f * std::sqrt(dx * dx + dy * dy);
    };
    float cr[2], rr, cl[2], rl;
    eye(33, 133, 159, 145, cr, &rr);   // the subject's right eye -> iris points 468..472
    eye(263, 362, 386, 374, cl, &rl);  // the subject's left eye -> 473..477
    for (int k = 0; k < 146; ++k) {
        const int i = kBsPoints[k];
        float x, y;
        if (i < kFaceLandmarks || hasIris) {
            x = P(i, 0);
            y = P(i, 1);
        } else {
            const bool left = i >= 473;
            const float* c = left ? cl : cr;
            const float r = left ? rl : rr;
            const int j = i - (left ? 473 : 468); // centre, right, up, left, down
            x = c[0] + (j == 1 ? r : j == 3 ? -r : 0.0f);
            y = c[1] + (j == 2 ? -r : j == 4 ? r : 0.0f);
        }
        in[k * 2] = x;
        in[k * 2 + 1] = y;
    }
    const int64_t shape[3] = {1, 146, 2};
    Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, in, 146 * 2, shape, 3);
    const char* inNames[] = {"t0"};
    const char* outNames[] = {"t195"};
    auto res = bsNet->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 1);
    const float* o = res[0].GetTensorData<float>();
    for (int i = 0; i < kBlendshapes; ++i) out.bs[i] = std::clamp(o[i], 0.0f, 1.0f);
    if (!hasIris)
        for (int i = 11; i <= 18; ++i) out.bs[i] = 0.0f; // eyeLook*: the iris points are synthesised, so gaze is not measured
}

bool FaceTracker::Impl::Detect(const uint8_t* bgra, int w, int h, Roi& out) {
    const float sc = (float)kDetSize / (float)std::max(w, h);
    const float padX = (kDetSize - w * sc) * 0.5f, padY = (kDetSize - h * sc) * 0.5f;
    const float m[6] = {1 / sc, 0, -padX / sc, 0, 1 / sc, -padY / sc}; // letterboxed 256 -> image
    WarpToCHW(bgra, w, h, m, kDetSize, detIn.data());

    const int64_t shape[4] = {1, 3, kDetSize, kDetSize};
    Ort::Value in = Ort::Value::CreateTensor<float>(mem, detIn.data(), detIn.size(), shape, 4);
    const char* inNames[] = {"image"};
    const char* outNames[] = {"box_coords_1", "box_coords_2", "box_scores_1", "box_scores_2"};
    auto res = det->Run(Ort::RunOptions{nullptr}, inNames, &in, 1, outNames, 4);
    const float* box[2] = {res[0].GetTensorData<float>(), res[1].GetTensorData<float>()};
    const float* scr[2] = {res[2].GetTensorData<float>(), res[3].GetTensorData<float>()};
    const int count[2] = {512, 384};

    int best = -1, bestSet = 0, bestIdx = 0;
    float bestScore = kDetThreshold;
    int base = 0;
    for (int s = 0; s < 2; ++s) {
        for (int i = 0; i < count[s]; ++i) {
            float p = Sigmoid(scr[s][i]);
            if (p > bestScore) {
                bestScore = p;
                best = base + i;
                bestSet = s;
                bestIdx = i;
            }
        }
        base += count[s];
    }
    if (best < 0) return false;

    const float* r = box[bestSet] + (size_t)bestIdx * 16;
    const auto& a = anchors[best];
    auto toImg = [&](float nx, float ny) { return std::make_pair((nx * kDetSize - padX) / sc, (ny * kDetSize - padY) / sc); };
    auto c = toImg(r[0] / kDetSize + a.first, r[1] / kDetSize + a.second);
    float bw = r[2] / kDetSize * kDetSize / sc, bh = r[3] / kDetSize * kDetSize / sc;
    auto eyeR = toImg(r[4] / kDetSize + a.first, r[5] / kDetSize + a.second);
    auto eyeL = toImg(r[6] / kDetSize + a.first, r[7] / kDetSize + a.second);
    out.cx = c.first;
    out.cy = c.second;
    out.size = std::max(bw, bh) * kRoiScale;
    out.theta = std::atan2(eyeL.second - eyeR.second, eyeL.first - eyeR.first);
    return true;
}

bool FaceTracker::Impl::Landmarks(const uint8_t* bgra, int w, int h, const Roi& r, float* pts, float& score) {
    const float f = r.size / kLmSize, c = std::cos(r.theta), s = std::sin(r.theta);
    // crop pixel (x,y) -> image: R(theta) * ((x,y) - 96) * f + centre
    const float half = kLmSize * 0.5f;
    const float m[6] = {c * f, -s * f, r.cx - half * c * f + half * s * f,
                        s * f, c * f,  r.cy - half * s * f - half * c * f};
    WarpToCHW(bgra, w, h, m, kLmSize, lmIn.data());

    const int64_t shape[4] = {1, 3, kLmSize, kLmSize};
    Ort::Value in = Ort::Value::CreateTensor<float>(mem, lmIn.data(), lmIn.size(), shape, 4);
    const char* inNames[] = {"image"};
    const char* outNames[] = {"scores", "landmarks"};
    auto res = lm->Run(Ort::RunOptions{nullptr}, inNames, &in, 1, outNames, 2);
    score = res[0].GetTensorData<float>()[0]; // already a probability
    if (score < kLmThreshold) return false;
    const float* L = res[1].GetTensorData<float>(); // x,y,z normalised to the crop
    for (int i = 0; i < kFaceLandmarks; ++i) {
        float ox = L[i * 3 + 0] * kLmSize - half, oy = L[i * 3 + 1] * kLmSize - half;
        pts[i * 3 + 0] = c * f * ox - s * f * oy + r.cx;
        pts[i * 3 + 1] = s * f * ox + c * f * oy + r.cy;
        pts[i * 3 + 2] = L[i * 3 + 2] * kLmSize * f;
    }
    return true;
}

// Same as Landmarks, with the 478-point model: NHWC input, landmarks already in crop pixels, a logit for "a face is here".
bool FaceTracker::Impl::Landmarks478(const uint8_t* bgra, int w, int h, const Roi& r, float* pts, float& score) {
    const float f = r.size / kLm478Size, c = std::cos(r.theta), s = std::sin(r.theta);
    const float half = kLm478Size * 0.5f;
    const float m[6] = {c * f, -s * f, r.cx - half * c * f + half * s * f,
                        s * f, c * f,  r.cy - half * s * f - half * c * f};
    std::vector<float> chw(3 * kLm478Size * kLm478Size);
    WarpToCHW(bgra, w, h, m, kLm478Size, chw.data());
    const size_t plane = (size_t)kLm478Size * kLm478Size;
    for (size_t i = 0; i < plane; ++i)
        for (int ch = 0; ch < 3; ++ch) lm478Buf[i * 3 + ch] = chw[ch * plane + i];
    const int64_t shape[4] = {1, kLm478Size, kLm478Size, 3};
    Ort::Value in = Ort::Value::CreateTensor<float>(mem, lm478Buf.data(), lm478Buf.size(), shape, 4);
    const char* inNames[] = {lm478In.c_str()};
    const char* outNames[] = {lm478Out0.c_str(), lm478Out1.c_str()};
    auto res = lm478->Run(Ort::RunOptions{nullptr}, inNames, &in, 1, outNames, 2);
    score = Sigmoid(res[1].GetTensorData<float>()[0]);
    if (score < kLmThreshold) return false;
    const float* L = res[0].GetTensorData<float>(); // x,y,z in crop pixels
    for (int i = 0; i < kMeshPoints; ++i) {
        const float ox = L[i * 3 + 0] - half, oy = L[i * 3 + 1] - half;
        pts[i * 3 + 0] = c * f * ox - s * f * oy + r.cx;
        pts[i * 3 + 1] = s * f * ox + c * f * oy + r.cy;
        pts[i * 3 + 2] = L[i * 3 + 2] * f;
    }
    return true;
}

// Next frame's search region: rotated bounding box of the mesh, grown by kRoiScale.
Roi FaceTracker::Impl::RoiFromMesh(const float* pts) {
    const float theta = std::atan2(pts[263 * 3 + 1] - pts[33 * 3 + 1], pts[263 * 3 + 0] - pts[33 * 3 + 0]);
    const float c = std::cos(theta), s = std::sin(theta);
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (int i = 0; i < kFaceLandmarks; ++i) { // express in the face's own axes
        float x = c * pts[i * 3] + s * pts[i * 3 + 1], y = -s * pts[i * 3] + c * pts[i * 3 + 1];
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
    }
    const float lx = (minX + maxX) * 0.5f, ly = (minY + maxY) * 0.5f;
    return {c * lx - s * ly, s * lx + c * ly, std::max(maxX - minX, maxY - minY) * kRoiScale, theta};
}

void FaceTracker::Impl::FillPoseAndChannels(const float* pts, int w, int h, FaceFrame& out) {
    auto P = [&](int i) { return Vec3{pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]}; };

    // Head frame: x = right eye -> left eye, y = forehead -> chin, z = cross (away from camera).
    Vec3 ex = Norm(P(263) - P(33));
    Vec3 ez = Norm(Cross(ex, P(152) - P(10)));
    Vec3 ey = Cross(ez, ex);
    out.rot[0] = std::atan2(ey.z, ey.y) * 180.0f / kPi;  // pitch
    out.rot[1] = std::asin(std::clamp(ex.z, -1.0f, 1.0f)) * 180.0f / kPi; // yaw
    out.rot[2] = std::atan2(ex.y, ex.x) * 180.0f / kPi;  // roll

    float cx = 0, cy = 0;
    for (int i = 0; i < kFaceLandmarks; ++i) { cx += pts[i * 3]; cy += pts[i * 3 + 1]; }
    out.pos[0] = cx / kFaceLandmarks / w;
    out.pos[1] = cy / kFaceLandmarks / h;
    const float iod = std::sqrt(Dot(P(263) - P(33), P(263) - P(33)));
    out.pos[2] = iod > 1e-3f ? h / iod : 0;

    // Measures in head space, in units of eye distance, so they survive head turns and distance.
    const Vec3 origin = {(P(33).x + P(263).x) * 0.5f, (P(33).y + P(263).y) * 0.5f, (P(33).z + P(263).z) * 0.5f};
    auto L = [&](int i) {
        Vec3 d = P(i) - origin;
        return Vec3{Dot(d, ex) / iod, Dot(d, ey) / iod, Dot(d, ez) / iod};
    };
    auto eyeOpen = [&](int up, int lo, int outer, int inner) {
        Vec3 a = P(up) - P(lo), b = P(outer) - P(inner);
        return std::sqrt(Dot(a, a)) / std::max(std::sqrt(Dot(b, b)), 1e-3f);
    };
    const float lipMid = (L(13).y + L(14).y) * 0.5f;
    float* r = out.raw;
    r[0] = L(14).y - L(13).y;                       // jawOpen: inner lip gap
    r[1] = lipMid - L(291).y;                       // mouthSmileLeft: corner above the lip line
    r[2] = lipMid - L(61).y;                        // mouthSmileRight
    r[3] = -(L(291).x - L(61).x);                   // mouthPucker: narrow mouth
    r[4] = -eyeOpen(386, 374, 263, 362);            // eyeBlinkLeft
    r[5] = -eyeOpen(159, 145, 33, 133);             // eyeBlinkRight
    r[6] = -(L(334).y < L(386).y ? L(386).y - L(334).y : 0.0f); // browDownLeft: brow close to lid
    r[7] = -(L(105).y < L(159).y ? L(159).y - L(105).y : 0.0f); // browDownRight
    r[8] = ((L(159).y - L(107).y) + (L(386).y - L(336).y)) * 0.5f; // browInnerUp
}

FaceTracker::FaceTracker() : impl_(new Impl) {}
FaceTracker::~FaceTracker() = default;

bool FaceTracker::Init(const wchar_t* modelDir, std::string* error) {
    try {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(2);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        std::wstring dir = modelDir;
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
        impl_->det = std::make_unique<Ort::Session>(impl_->env, (dir + L"face_detector.onnx").c_str(), so);
        impl_->lm = std::make_unique<Ort::Session>(impl_->env, (dir + L"face_landmark_detector.onnx").c_str(), so);
        try { // optional: the 478-point model with irises; without it the older 468-point model is used
            const std::wstring l478 = dir + L"face_landmark_478.onnx";
            if (GetFileAttributesW(l478.c_str()) != INVALID_FILE_ATTRIBUTES) {
                auto net = std::make_unique<Ort::Session>(impl_->env, l478.c_str(), so);
                Ort::AllocatorWithDefaultOptions alloc;
                impl_->lm478In = net->GetInputNameAllocated(0, alloc).get();
                impl_->lm478Out0 = net->GetOutputNameAllocated(0, alloc).get(); // landmarks (1434)
                impl_->lm478Out1 = net->GetOutputNameAllocated(1, alloc).get(); // face logit
                impl_->lm478 = std::move(net);
                impl_->hasIris = true;
            }
        } catch (const Ort::Exception&) {
            impl_->lm478.reset();
            impl_->hasIris = false;
        }
        try { // optional: without it the nine geometric channels still work
            const std::wstring bs = dir + L"face_blendshapes.onnx";
            if (GetFileAttributesW(bs.c_str()) != INVALID_FILE_ATTRIBUTES)
                impl_->bsNet = std::make_unique<Ort::Session>(impl_->env, bs.c_str(), so);
        } catch (const Ort::Exception&) {
            impl_->bsNet.reset();
        }
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

void FaceTracker::Reset() { impl_->tracking = false; }

void FaceTracker::Process(const uint8_t* bgra, int w, int h, FaceFrame& out) {
    Impl& s = *impl_;
    const double t = out.time;
    out = FaceFrame{};
    out.time = t;
    std::vector<float> pts(kMeshPoints * 3);
    float score = 0;
    bool ok = false;
    try {
        auto landmarks = [&]() { return s.lm478 ? s.Landmarks478(bgra, w, h, s.roi, pts.data(), score) : s.Landmarks(bgra, w, h, s.roi, pts.data(), score); };
        if (s.tracking) ok = landmarks();
        if (!ok && s.Detect(bgra, w, h, s.roi)) ok = landmarks();
    } catch (const Ort::Exception&) {
        ok = false;
    }
    s.tracking = ok;
    if (!ok) return;
    s.roi = Impl::RoiFromMesh(pts.data());
    out.valid = true;
    out.score = score;
    Impl::FillPoseAndChannels(pts.data(), w, h, out);
    try { s.Blendshapes(pts.data(), out); } catch (const Ort::Exception&) {}
    for (int i = 0; i < kFaceLandmarks; ++i) {
        out.lm[i * 3 + 0] = pts[i * 3 + 0] / w;
        out.lm[i * 3 + 1] = pts[i * 3 + 1] / h;
        out.lm[i * 3 + 2] = pts[i * 3 + 2] / w;
    }
}

#else
// Built without ONNX Runtime (TBD_NO_ONNX): the tracker never finds a face. Smoothing and normalisation below still work.
struct FaceTracker::Impl {};
FaceTracker::FaceTracker() : impl_(new Impl) {}
FaceTracker::~FaceTracker() = default;
bool FaceTracker::Init(const wchar_t*, std::string* error) {
    if (error) *error = "built without ONNX Runtime (TBD_NO_ONNX)";
    return false;
}
void FaceTracker::Reset() {}
void FaceTracker::Process(const uint8_t*, int, int, FaceFrame& out) {
    const double t = out.time;
    out = FaceFrame{};
    out.time = t;
}
#endif // TBD_NO_ONNX

// Casiez et al. One-Euro filter: a low-pass whose cutoff rises with the signal's speed.
float FaceSmoother::OneEuro::Step(float v, float dt, float minCutoff, float beta) {
    auto alpha = [dt](float cutoff) { return 1.0f / (1.0f + 1.0f / (2.0f * kPi * cutoff * dt)); };
    if (!primed) {
        primed = true;
        x = v;
        dx = 0;
        return v;
    }
    dx += alpha(1.0f) * ((v - x) / dt - dx); // derivative cutoff 1 Hz
    x += alpha(minCutoff + beta * std::fabs(dx)) * (v - x);
    return x;
}

namespace {
constexpr int kSignals = 3 + 3 + kFaceChannels + kBlendshapes + kFaceLandmarks * 3;
float& SignalAt(FaceFrame& f, int i) {
    if (i < 3) return f.pos[i];
    if (i < 6) return f.rot[i - 3];
    if (i < 6 + kFaceChannels) return f.raw[i - 6];
    if (i < 6 + kFaceChannels + kBlendshapes) return f.bs[i - 6 - kFaceChannels];
    return f.lm[i - 6 - kFaceChannels - kBlendshapes];
}
} // namespace

std::vector<float> FaceSmoother::DefaultScales() {
    // Typical 5th-95th percentile spread of each signal on a talking head, from a tracked sample clip.
    const float raw[kFaceChannels] = {0.18f, 0.05f, 0.05f, 0.14f, 0.065f, 0.06f, 0.065f, 0.065f, 0.055f};
    std::vector<float> s(kSignals, 0.1f); // mesh points: a face is about 0.1 of the frame wide
    s[0] = s[1] = 0.15f;
    s[2] = 3.0f;
    s[3] = s[4] = s[5] = 30.0f; // degrees
    for (int c = 0; c < kFaceChannels; ++c) s[6 + c] = raw[c];
    for (int c = 0; c < kBlendshapes; ++c) s[6 + kFaceChannels + c] = 0.25f;
    return s;
}

FaceSmoother::FaceSmoother(std::vector<float> scales, const SmoothParams& params)
    : scale_(std::move(scales)), params_(params), filters_(kSignals) {}

void FaceSmoother::Reset() {
    for (OneEuro& f : filters_) f.primed = false;
}

void FaceSmoother::Apply(FaceFrame& f, float dt) {
    if (!params_.enabled || !f.valid) return;
    dt = std::max(dt, 1e-3f);
    for (int i = 0; i < kSignals; ++i) {
        float& v = SignalAt(f, i);
        v = filters_[i].Step(v / scale_[i], dt, params_.minCutoff, params_.beta) * scale_[i];
    }
}

void FaceTracker::Smooth(std::vector<FaceFrame>& frames, const SmoothParams& p) {
    if (!p.enabled || frames.size() < 2) return;
    // Range of every signal over the clip, so each is filtered in units of its own scale.
    std::vector<float> scale = FaceSmoother::DefaultScales();
    for (int i = 0; i < 6 + kFaceChannels; ++i) {
        std::vector<float> v;
        for (FaceFrame& f : frames)
            if (f.valid) v.push_back(SignalAt(f, i));
        if (v.size() >= 2) scale[i] = std::max(Percentile(v, 0.95f) - Percentile(v, 0.05f), 1e-4f);
    }
    FaceSmoother smoother(scale, p);
    double prevTime = 0;
    bool prevValid = false;
    for (FaceFrame& f : frames) {
        const double gap = f.time - prevTime;
        if (!f.valid || !prevValid || gap > 0.25) smoother.Reset();
        prevTime = f.time;
        prevValid = f.valid;
        smoother.Apply(f, (float)gap);
    }
}

void FaceTracker::NormalizeChannels(std::vector<FaceFrame>& frames) {
    for (int c = 0; c < kFaceChannels; ++c) {
        std::vector<float> v;
        for (const FaceFrame& f : frames)
            if (f.valid) v.push_back(f.raw[c]);
        if (v.empty()) continue;
        const float lo = Percentile(v, 0.05f), hi = Percentile(v, 0.95f);
        const float span = std::max(hi - lo, 1e-4f);
        for (FaceFrame& f : frames)
            if (f.valid) f.weight[c] = std::clamp((f.raw[c] - lo) / span, 0.0f, 1.0f);
    }
}

} // namespace tbd
