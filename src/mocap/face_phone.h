// The face stream from Epic's Live Link Face iPhone app (ARKit mode): one UDP packet per frame, big endian, packet version 6:
//   u8 version | u32 length + device id | u32 length + subject name | u32 frame, subframe, fps, denominator | u8 count (61) | 61 floats
// The floats are ARKit's 52 expression weights in the order below, then head yaw, pitch, roll and each eye's yaw, pitch, roll.
// The layout comes from community parsers of the stream and Unreal's forum, not from a published specification.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "face_clip.h"

namespace tbd {

constexpr int kLiveLinkVersion = 6, kLiveLinkFloats = 61, kLiveLinkPort = 11111;
// What a head value of 1 means, in degrees. NOT verified on a phone: the first real take must check it, and the signs below.
constexpr float kLiveLinkHeadDegrees = 90.0f;
constexpr const char* kLiveLinkNames[52] = {
    "eyeBlinkLeft", "eyeLookDownLeft", "eyeLookInLeft", "eyeLookOutLeft", "eyeLookUpLeft", "eyeSquintLeft", "eyeWideLeft",
    "eyeBlinkRight", "eyeLookDownRight", "eyeLookInRight", "eyeLookOutRight", "eyeLookUpRight", "eyeSquintRight", "eyeWideRight",
    "jawForward", "jawLeft", "jawRight", "jawOpen", "mouthClose", "mouthFunnel", "mouthPucker", "mouthLeft", "mouthRight",
    "mouthSmileLeft", "mouthSmileRight", "mouthFrownLeft", "mouthFrownRight", "mouthDimpleLeft", "mouthDimpleRight",
    "mouthStretchLeft", "mouthStretchRight", "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper",
    "mouthPressLeft", "mouthPressRight", "mouthLowerDownLeft", "mouthLowerDownRight", "mouthUpperUpLeft", "mouthUpperUpRight",
    "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight", "cheekPuff", "cheekSquintLeft",
    "cheekSquintRight", "noseSneerLeft", "noseSneerRight", "tongueOut"};

// The channel of FaceFrame::bs a stream position feeds, -1 for tongueOut: the rig has no tongue channel.
inline int LiveLinkChannel(int position) {
    for (int c = 1; c < kBlendshapes; ++c)
        if (std::strcmp(kBlendshapeNames[c], kLiveLinkNames[position]) == 0) return c;
    return -1;
}

enum class LiveLinkResult { Ok, BadVersion, Malformed };

// Fills out.bs and out.rot (pitch, yaw, roll in degrees, as the tracker gives them). `version` receives the packet's version.
inline LiveLinkResult ParseLiveLinkPacket(const uint8_t* p, size_t n, FaceFrame& out, int* version = nullptr) {
    size_t at = 0;
    auto u32 = [&](uint32_t& v) {
        if (n - at < 4) return false;
        v = ((uint32_t)p[at] << 24) | ((uint32_t)p[at + 1] << 16) | ((uint32_t)p[at + 2] << 8) | p[at + 3];
        at += 4;
        return true;
    };
    auto skipString = [&] {
        uint32_t len = 0;
        if (!u32(len) || len > n - at) return false;
        at += len;
        return true;
    };
    if (n < 1) return LiveLinkResult::Malformed;
    if (version) *version = p[0];
    if (p[0] != kLiveLinkVersion) return LiveLinkResult::BadVersion;
    at = 1;
    if (!skipString() || !skipString()) return LiveLinkResult::Malformed;
    uint32_t skip = 0;
    for (int k = 0; k < 4; ++k)
        if (!u32(skip)) return LiveLinkResult::Malformed;
    if (n - at < 1 || p[at] != kLiveLinkFloats) return LiveLinkResult::Malformed;
    ++at;
    if (n - at < kLiveLinkFloats * 4) return LiveLinkResult::Malformed;
    float v[kLiveLinkFloats];
    for (float& f : v) {
        uint32_t bits = 0;
        u32(bits);
        std::memcpy(&f, &bits, 4);
        if (!std::isfinite(f)) return LiveLinkResult::Malformed;
    }
    std::fill(out.bs, out.bs + kBlendshapes, 0.0f);
    for (int k = 0; k < 52; ++k) {
        const int c = LiveLinkChannel(k);
        if (c >= 0) out.bs[c] = std::clamp(v[k], 0.0f, 1.0f);
    }
    out.rot[0] = v[53] * kLiveLinkHeadDegrees; // pitch
    out.rot[1] = v[52] * kLiveLinkHeadDegrees; // yaw
    out.rot[2] = v[54] * kLiveLinkHeadDegrees; // roll
    return LiveLinkResult::Ok;
}

} // namespace tbd
