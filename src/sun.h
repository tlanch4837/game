// The sun: a time of day that drives the scene's light rig (direction, color, ambient) and a sky with a
// visible sun disc. Renderer-agnostic; the sky is plain vertex-colored triangles, so it draws through
// DrawTransient on either backend with no pipeline of its own.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "render/renderer.h"
#include "vecmath.h"

namespace tbd {

struct Sun {
    float hour = 13.7f;     // 0..24: 6 sunrise, 12 noon, 18 sunset, night around midnight
    float tiltDeg = 22.4f;  // how far the sun's path leans away from straight overhead (toward +Z)
    bool showSky = true;    // sky gradient and the sun disc; off leaves the plain editor background
    bool animate = false;   // run the day cycle
    bool shadows = true;    // the sun casts shadows (when it is high enough)
    float dayLengthSeconds = 90.0f;
    float wetness = 0.0f;   // 0 dry .. 1 soaked ground; the weather value, for now set by TBD_WETNESS

    // Unit vector from the world toward the sun. The sun rides a circle through east (-X at sunrise) and west.
    Vec3 Direction() const {
        const float h = Radians((hour - 12.0f) * 15.0f), tilt = Radians(tiltDeg);
        return Normalize(Vec3{std::sin(h), std::cos(h) * std::cos(tilt), std::cos(h) * std::sin(tilt)});
    }

    void Advance(float dt) {
        hour = std::fmod(hour + dt * 24.0f / dayLengthSeconds, 24.0f);
        if (hour < 0.0f) hour += 24.0f;
    }
};

namespace sun_detail {

inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float SmoothStep(float a, float b, float x) {
    const float t = Clamp01((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline Vec3 Mul(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

// Sun color: orange on the horizon, warm white high up; zero once it is down.
inline Vec3 SunTint(float up) {
    return Lerp({1.0f, 0.50f, 0.22f}, {1.0f, 0.96f, 0.88f}, SmoothStep(0.0f, 0.55f, up));
}

constexpr Vec3 kEditorBackground = {24.0f / 255.0f, 20.0f / 255.0f, 16.0f / 255.0f}; // matches kClearColor

} // namespace sun_detail

namespace sun_detail {

// CPU copy of post.hlsl's single-scattering sky (Nishita), coarse: enough to light with.
inline Vec3 SkyRadianceCoarse(Vec3 dir, Vec3 toSun) {
    constexpr float kE = 6360e3f, kT = 6420e3f, kI = 12.0f;
    const Vec3 betaR = {5.8e-6f, 13.5e-6f, 33.1e-6f};
    constexpr float betaM = 21e-6f;
    const auto sphereFar = [](Vec3 o, Vec3 d, float r) { const float b = Dot(o, d); const float h = b * b - Dot(o, o) + r * r; return h < 0 ? -1.0f : -b + std::sqrt(h); };
    const auto transmit = [&](float odR, float odM) { const float m = betaM * 1.1f * odM; return Vec3{std::exp(-((betaR.x + 0.73e-6f) * odR + m)), std::exp(-((betaR.y + 2.1e-6f) * odR + m)), std::exp(-((betaR.z + 0.1e-6f) * odR + m))}; };
    const Vec3 o = {0.0f, kE + 100.0f, 0.0f};
    const float mu = Dot(dir, toSun), g = 0.76f;
    const float pr = 3.0f / (16.0f * kPi) * (1.0f + mu * mu);
    const float pm = 3.0f / (8.0f * kPi) * (1.0f - g * g) * (1.0f + mu * mu) / ((2.0f + g * g) * std::pow(1.0f + g * g - 2.0f * g * mu, 1.5f));
    const float step = sphereFar(o, dir, kT) / 6.0f;
    float odR = 0, odM = 0;
    Vec3 sumR = {}, sumM = {};
    for (int i = 0; i < 6; ++i) {
        const Vec3 p = o + dir * ((float)i + 0.5f) * step;
        const float h = std::fmax(std::sqrt(Dot(p, p)) - kE, 0.0f), rhoR = std::exp(-h / 8000.0f) * step, rhoM = std::exp(-h / 1200.0f) * step;
        odR += rhoR; odM += rhoM;
        float lr = 1e9f, lm = 1e9f;
        const float b = Dot(p, toSun);
        if (b >= 0.0f || Dot(p, p) - b * b >= kE * kE) {
            lr = lm = 0.0f;
            const float ls = sphereFar(p, toSun, kT) / 3.0f;
            for (int k = 0; k < 3; ++k) {
                const Vec3 q = p + toSun * ((float)k + 0.5f) * ls; const float hs = std::fmax(std::sqrt(Dot(q, q)) - kE, 0.0f);
                lr += std::exp(-hs / 8000.0f) * ls; lm += std::exp(-hs / 1200.0f) * ls;
            }
        }
        const Vec3 t = transmit(odR + lr, odM + lm);
        sumR = sumR + t * rhoR;
        sumM = sumM + t * rhoM;
    }
    return (Mul(sumR, betaR) * pr + sumM * (betaM * pm) + betaR * (odR * 0.016f * SmoothStep(-0.18f, 0.08f, toSun.y))) * kI; // the last term: post.hlsl's AirMultiScatter
}

// The sky and ground as light: radiance gathered over the sphere into a mean and a gradient, again only when the hour changes.
inline void FillSkyLight(const Sun& sun, Vec3 sunColor, Vec3 fallbackSky, float day, Lighting& l) {
    static float cachedHour = -100.0f, cachedTilt = 0.0f;
    static float sh[4][4], grad[3][4];
    if (cachedHour != sun.hour || cachedTilt != sun.tiltDeg) {
        cachedHour = sun.hour; cachedTilt = sun.tiltDeg;
        const Vec3 s = sun.Direction();
        constexpr float kCloudCover = 0.15f; // a mostly clear desert sky; clouds whiten what they cover
        constexpr int kDirs = 128;
        Vec3 mean = {}, gx = {}, gy = {}, gz = {}, up = {};
        Vec3 zenith = {}, horizon = {};
        for (int pass = 0; pass < 2; ++pass) { // sky first; the ground's radiance needs the light the sky sends it
            const Vec3 sunE = sunColor * (3.2f * std::fmax(s.y, 0.0f));
            const Vec3 ground = Mul(up + sunE, {0.42f, 0.33f, 0.24f});
            for (int i = 0; i < kDirs; ++i) { // spherical Fibonacci
                const float y = 1.0f - (2.0f * (float)i + 1.0f) / (float)kDirs, r = std::sqrt(std::fmax(1.0f - y * y, 0.0f)), a = (float)i * 2.399963f;
                const Vec3 d = {r * std::cos(a), y, r * std::sin(a)};
                Vec3 L;
                if (y >= 0.0f) {
                    L = SkyRadianceCoarse(d, s);
                    const float lum = 0.2126f * L.x + 0.7152f * L.y + 0.0722f * L.z;
                    L = Lerp(L, Vec3{lum, lum, lum} * 1.2f, kCloudCover); // no gain: noon sun to sky comes to about 12:1 on level ground
                    L = L + fallbackSky * (0.35f * (1.0f - day)); // night keeps a moonlit floor
                    if (pass == 0) { up = up + L * (std::fmax(y, 0.0f) * 2.0f / (float)kDirs * 2.0f); continue; }
                } else {
                    if (pass == 0) continue;
                    L = ground;
                }
                mean = mean + L * (1.0f / (float)kDirs);
                gx = gx + L * (3.0f * d.x / (float)kDirs);
                gy = gy + L * (3.0f * d.y / (float)kDirs);
                gz = gz + L * (3.0f * d.z / (float)kDirs);
            }
            if (pass == 1) {
                zenith = SkyRadianceCoarse({0, 1, 0}, s);
                for (int k = 0; k < 8; ++k) {
                    const float a = (float)k * 0.7853982f;
                    horizon = horizon + SkyRadianceCoarse(Normalize(Vec3{std::cos(a), 0.08f, std::sin(a)}), s) * 0.125f;
                }
                const Vec3 floor = fallbackSky * (0.35f * (1.0f - day));
                const auto put4 = [](float* dst, Vec3 v) { dst[0] = v.x; dst[1] = v.y; dst[2] = v.z; dst[3] = 0.0f; };
                put4(sh[0], mean); put4(sh[1], gx); put4(sh[2], gy); put4(sh[3], gz);
                put4(grad[0], zenith + floor); put4(grad[1], horizon + floor); put4(grad[2], ground);
            }
        }
    }
    std::copy(&sh[0][0], &sh[0][0] + 16, &l.skySh[0][0]);
    std::copy(&grad[0][0], &grad[0][0] + 12, &l.skyGrad[0][0]);
}

} // namespace sun_detail

// The light rig for the shaders. At the default hour this equals the engine's original fixed light.
inline Lighting ComputeLighting(const Sun& sun) {
    using namespace sun_detail;
    const Vec3 d = sun.Direction();
    const float day = SmoothStep(-0.18f, 0.25f, d.y);     // how much of the sky is lit
    const float direct = SmoothStep(-0.02f, 0.12f, d.y);  // the sun itself above the horizon
    const float dusk = (1.0f - SmoothStep(0.0f, 0.35f, std::fabs(d.y))) * day;

    const Vec3 sunColor = SunTint(d.y) * (0.85f * direct * (0.55f + 0.45f * SmoothStep(0.0f, 0.5f, d.y))); // the long air path takes about half the beam at low sun
    Vec3 sky = Lerp({0.10f, 0.12f, 0.22f}, Vec3{0.42f, 0.52f, 0.68f} * 0.9f, day); // night keeps a moonlit floor
    Vec3 ground = Lerp({0.05f, 0.05f, 0.07f}, Vec3{0.26f, 0.22f, 0.18f} * 0.9f, day);
    sky = Lerp(sky, Mul(sky, {1.25f, 0.9f, 0.75f}), dusk); // warm the ambient around sunrise and sunset

    Lighting l;
    auto put = [](float* dst, Vec3 v) {
        dst[0] = v.x;
        dst[1] = v.y;
        dst[2] = v.z;
        dst[3] = 0.0f;
    };
    put(l.sunDir, d);
    put(l.sunColor, sunColor);
    put(l.skyColor, sky);
    put(l.groundColor, ground);
    sun_detail::FillSkyLight(sun, sunColor, sky, day, l);
    return l;
}

// Places the probe grid around `focus`, in whole cells. A cell keeps its atlas texel while the grid slides over it.
inline void ComputeProbeGrid(Vec3 focus, bool on, Lighting& l) {
    static float prev[3] = {1e9f, 1e9f, 1e9f};
    const float o[3] = {std::floor(focus.x / kProbeSpacing) - (float)(kProbeX / 2), std::floor(focus.y / kProbeSpacingY) - 3.0f,
                        std::floor(focus.z / kProbeSpacing) - (float)(kProbeZ / 2)};
    for (int k = 0; k < 3; ++k) { l.probeOrigin[k] = o[k]; l.probePrev[k] = prev[k]; prev[k] = o[k]; }
    l.probeOrigin[3] = kProbeSpacing;
    l.probeGrid[0] = (float)kProbeX; l.probeGrid[1] = (float)kProbeY; l.probeGrid[2] = (float)kProbeZ; l.probeGrid[3] = kProbeSpacingY;
    l.probeUpdate[1] = (float)kProbesPerFrame; l.probeUpdate[2] = (float)kProbeRays; l.probeUpdate[3] = on ? 1.0f : 0.0f;
}

// Centres the cloud shadow map near `focus`, in whole texels so its edges do not crawl as the camera moves.
inline void ComputeCloudMap(Vec3 focus, bool on, Lighting& l) {
    const float texel = 2.0f * kCloudShadowExtent / (float)kCloudShadowSize;
    l.cloudMap[0] = std::floor(focus.x / texel) * texel;
    l.cloudMap[1] = std::floor(focus.z / texel) * texel;
    l.cloudMap[2] = kCloudShadowExtent;
    l.cloudMap[3] = on ? 1.0f : 0.0f;
}

// Fits the sun's shadow map (an orthographic view along the sun's rays) around a sphere of `radius` centered on
// `focus` and writes it into `l`. Returns false, leaving shadows off, when the sun is too low to cast any.
// The map slides in whole-texel steps so shadow edges don't shimmer as the camera moves.
inline bool ComputeShadow(const Sun& sun, Vec3 focus, float radius, Lighting& l) {
    const Vec3 d = sun.Direction();
    if (d.y < 0.03f) return false;

    constexpr float kMargin = 250.0f; // room for tall casters up the sun's side of the sphere (terrain peaks, trees)
    const float texel = 2.0f * radius / (float)kShadowMapSize;
    const Vec3 up = std::fabs(d.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const Vec3 forward = d * -1.0f;
    const Vec3 right = Normalize(Cross(forward, up)), upAxis = Cross(right, forward);
    const Vec3 center = focus - right * std::fmod(Dot(focus, right), texel) - upAxis * std::fmod(Dot(focus, upAxis), texel);

    const Mat4 view = LookAtRH(center + d * (radius + kMargin), center, up);
    const Mat4 proj = OrthoRH(radius, radius, 0.0f, 2.0f * radius + kMargin + 100.0f);
    const Mat4 viewProj = proj * view;
    std::copy(viewProj.m, viewProj.m + 16, l.shadowVP);
    l.shadowParams[1] = 1.0f / (float)kShadowMapSize;
    l.shadowParams[2] = texel * 1.5f;
    l.shadowParams[3] = 1.0f;
    return true;
}

// Fits the tight character shadow map: an orthographic box around a sphere of `radius` (a few meters) centered on
// `focus`, from the same sun direction, and writes it into `l`. Returns false, leaving it off, when the sun is too low
// to cast shadows. It slides in whole-texel steps like the sun's map. The box reaches `kMargin` toward the sun so
// casters just above the sphere still land in it, and a little past the sphere's far side for receivers behind it.
inline bool ComputeCharacterShadow(const Sun& sun, Vec3 focus, float radius, Lighting& l) {
    const Vec3 d = sun.Direction();
    if (d.y < 0.03f) return false;

    constexpr float kMargin = 16.0f, kFarPad = 5.0f;
    const float texel = 2.0f * radius / (float)kTightShadowMapSize;
    const Vec3 up = std::fabs(d.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const Vec3 forward = d * -1.0f;
    const Vec3 right = Normalize(Cross(forward, up)), upAxis = Cross(right, forward);
    const Vec3 center = focus - right * std::fmod(Dot(focus, right), texel) - upAxis * std::fmod(Dot(focus, upAxis), texel);

    const Mat4 view = LookAtRH(center + d * (radius + kMargin), center, up);
    const Mat4 proj = OrthoRH(radius, radius, 0.0f, 2.0f * radius + kMargin + kFarPad);
    const Mat4 viewProj = proj * view;
    std::copy(viewProj.m, viewProj.m + 16, l.tightVP);
    l.tightParams[0] = 2.0f * radius + kMargin + kFarPad; // metres the map's depth spans
    l.tightParams[0] = 2.0f * radius + kMargin + kFarPad; // metres the map's depth spans
    l.tightParams[1] = 1.0f / (float)kTightShadowMapSize;
    l.tightParams[2] = texel * 2.0f; // the 5x5 filter reaches two texels, so the normal offset is a bit larger than the sun map's
    l.tightParams[3] = 1.0f;
    return true;
}

namespace sun_detail {

// Sky color for a unit view direction, for the current sun.
class SkyModel {
public:
    explicit SkyModel(const Sun& sun) : sunDir_(sun.Direction()) {
        const float up = sunDir_.y;
        day_ = SmoothStep(-0.18f, 0.2f, up);
        glow_ = std::exp(-(up / 0.2f) * (up / 0.2f));
        zenith_ = Lerp({0.005f, 0.008f, 0.03f}, {0.16f, 0.34f, 0.70f}, day_);
        horizon_ = Lerp({0.02f, 0.03f, 0.07f}, {0.66f, 0.78f, 0.92f}, day_);
    }

    Vec3 Color(Vec3 dir) const {
        const float h = std::clamp(dir.y, -1.0f, 1.0f), hu = std::fmax(h, 0.0f);
        Vec3 c = Lerp(horizon_, zenith_, std::pow(hu, 0.5f));

        // The horizon glows around the sun's bearing while it is low.
        const float len = std::sqrt(dir.x * dir.x + dir.z * dir.z), sunLen = std::sqrt(sunDir_.x * sunDir_.x + sunDir_.z * sunDir_.z);
        const float toSun = len > 1e-4f && sunLen > 1e-4f
                                ? std::fmax(0.0f, (dir.x * sunDir_.x + dir.z * sunDir_.z) / (len * sunLen))
                                : 0.0f;
        const float w = glow_ * std::pow(toSun, 3.0f) * std::pow(1.0f - hu, 3.0f) * 0.85f;
        c = Lerp(c, {1.0f, 0.52f, 0.25f}, Clamp01(w));

        // Below the horizon the sky fades to the editor's background, so looking down keeps the old look.
        if (h < 0.0f) c = Lerp(c, kEditorBackground, Clamp01(-h * 2.5f));
        return c;
    }

private:
    Vec3 sunDir_, zenith_, horizon_;
    float day_ = 1.0f, glow_ = 0.0f;
};

inline Vertex SkyVertex(Vec3 dir, Vec3 color, float radius) {
    return {dir.x * radius, dir.y * radius, dir.z * radius, 0, 0, 0, color.x, color.y, color.z, 1.0f};
}

} // namespace sun_detail

// A dome around the origin (so draw it with a model matrix that translates to the eye) and, on top of it, the
// sun: a bright disc with a glow that fades into the sky color. Unlit colors, drawn without depth.
inline std::vector<Vertex> BuildSky(const Sun& sun, float radius) {
    using namespace sun_detail;
    const SkyModel sky(sun);
    std::vector<Vertex> out;

    // Rings are packed toward the horizon, where the gradient changes fastest.
    static const float kElevations[] = {-90, -50, -25, -10, -3, 0, 3, 7, 12, 20, 30, 42, 56, 72, 90};
    constexpr int kRows = (int)(sizeof(kElevations) / sizeof(kElevations[0])), kSectors = 48;
    auto dirAt = [](float elevDeg, int sector) {
        const float e = Radians(elevDeg), a = 2.0f * kPi * (float)sector / (float)kSectors;
        return Vec3{std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a)};
    };
    for (int j = 0; j + 1 < kRows; ++j)
        for (int i = 0; i < kSectors; ++i) {
            const Vec3 a = dirAt(kElevations[j], i), b = dirAt(kElevations[j], i + 1), c = dirAt(kElevations[j + 1], i + 1),
                       d = dirAt(kElevations[j + 1], i);
            const Vertex va = SkyVertex(a, sky.Color(a), radius), vb = SkyVertex(b, sky.Color(b), radius),
                         vc = SkyVertex(c, sky.Color(c), radius), vd = SkyVertex(d, sky.Color(d), radius);
            for (const Vertex* v : {&va, &vb, &vc, &va, &vc, &vd}) out.push_back(*v);
        }

    // The sun fades into the sky as it sets, so it never shows below the horizon.
    const Vec3 s = sun.Direction();
    const float visible = SmoothStep(-0.05f, 0.03f, s.y);
    if (visible <= 0.0f) return out;
    const Vec3 disc = Lerp({1.0f, 0.62f, 0.30f}, {1.0f, 0.97f, 0.88f}, SmoothStep(0.0f, 0.3f, s.y));
    const Vec3 u = Normalize(Cross(s, std::fabs(s.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0})), v = Cross(s, u);

    // Angular radius of each ring: the disc, a hard edge, the inner glow, the fade to sky.
    constexpr float kRadii[] = {0.0f, 0.030f, 0.0315f, 0.14f, 0.38f};
    constexpr int kRings = (int)(sizeof(kRadii) / sizeof(kRadii[0])), kSegments = 32;
    auto ringDir = [&](int ring, int seg) {
        if (ring == 0) return s;
        const float a = 2.0f * kPi * (float)seg / (float)kSegments, r = kRadii[ring];
        return Normalize(s * std::cos(r) + (u * std::cos(a) + v * std::sin(a)) * std::sin(r));
    };
    auto ringColor = [&](int ring, Vec3 dir) {
        const Vec3 base = sky.Color(dir);
        switch (ring) {
        case 0:
        case 1: return Lerp(base, disc, visible);
        case 2: return Lerp(base, disc, 0.55f * visible);
        case 3: return Lerp(base, disc, 0.12f * visible);
        default: return base;
        }
    };
    for (int ring = 0; ring + 1 < kRings; ++ring)
        for (int seg = 0; seg < kSegments; ++seg) {
            const Vec3 a = ringDir(ring, seg), b = ringDir(ring, seg + 1), c = ringDir(ring + 1, seg + 1),
                       d = ringDir(ring + 1, seg);
            const Vertex va = SkyVertex(a, ringColor(ring, a), radius), vb = SkyVertex(b, ringColor(ring, b), radius),
                         vc = SkyVertex(c, ringColor(ring + 1, c), radius), vd = SkyVertex(d, ringColor(ring + 1, d), radius);
            for (const Vertex* x : {&va, &vc, &vd}) out.push_back(*x);
            if (ring > 0)
                for (const Vertex* x : {&va, &vb, &vc}) out.push_back(*x);
        }
    return out;
}

// Draws the sky first in the frame, centered on the eye. No depth test and no depth write.
inline void DrawSky(Renderer& r, const Mat4& viewProj, Vec3 eye, const Sun& sun) {
    if (!sun.showSky) return;
    const std::vector<Vertex> sky = BuildSky(sun, 4500.0f); // inside the camera's 6000 far plane
    const Mat4 model = TRS(eye, Quat{}, {1, 1, 1});
    r.DrawTransient(sky.data(), (uint32_t)sky.size(), Topology::Triangles, viewProj * model, model, false);
}

} // namespace tbd
