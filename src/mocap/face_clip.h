// Face motion-capture clip: per-frame head pose, expression channels and the 468-point mesh.
// Binary file format ".tbdface" (little endian), see Save()/Load().
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tbd {

constexpr int kFaceLandmarks = 468;
constexpr int kFaceChannels = 9;
// MediaPipe's trained blendshape model (face_blendshapes.onnx): 52 ARKit-style weights, in its output order.
constexpr int kBlendshapes = 52;
constexpr const char* kBlendshapeNames[kBlendshapes] = {
    "_neutral", "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight", "cheekPuff",
    "cheekSquintLeft", "cheekSquintRight", "eyeBlinkLeft", "eyeBlinkRight", "eyeLookDownLeft", "eyeLookDownRight",
    "eyeLookInLeft", "eyeLookInRight", "eyeLookOutLeft", "eyeLookOutRight", "eyeLookUpLeft", "eyeLookUpRight",
    "eyeSquintLeft", "eyeSquintRight", "eyeWideLeft", "eyeWideRight", "jawForward", "jawLeft", "jawOpen", "jawRight",
    "mouthClose", "mouthDimpleLeft", "mouthDimpleRight", "mouthFrownLeft", "mouthFrownRight", "mouthFunnel",
    "mouthLeft", "mouthLowerDownLeft", "mouthLowerDownRight", "mouthPressLeft", "mouthPressRight", "mouthPucker",
    "mouthRight", "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper", "mouthSmileLeft",
    "mouthSmileRight", "mouthStretchLeft", "mouthStretchRight", "mouthUpperUpLeft", "mouthUpperUpRight",
    "noseSneerLeft", "noseSneerRight"};
enum Blendshape { // the ones the rig reaches for by name
    BS_browDownLeft = 1, BS_browDownRight = 2, BS_browInnerUp = 3, BS_browOuterUpLeft = 4, BS_browOuterUpRight = 5,
    BS_cheekPuff = 6, BS_cheekSquintLeft = 7, BS_cheekSquintRight = 8, BS_eyeBlinkLeft = 9, BS_eyeBlinkRight = 10,
    BS_eyeSquintLeft = 19, BS_eyeSquintRight = 20, BS_eyeWideLeft = 21, BS_eyeWideRight = 22, BS_jawForward = 23,
    BS_jawLeft = 24, BS_jawOpen = 25, BS_jawRight = 26, BS_mouthClose = 27, BS_mouthDimpleLeft = 28,
    BS_mouthDimpleRight = 29, BS_mouthFrownLeft = 30, BS_mouthFrownRight = 31, BS_mouthFunnel = 32, BS_mouthLeft = 33,
    BS_mouthLowerDownLeft = 34, BS_mouthLowerDownRight = 35, BS_mouthPressLeft = 36, BS_mouthPressRight = 37,
    BS_mouthPucker = 38, BS_mouthRight = 39, BS_mouthRollLower = 40, BS_mouthRollUpper = 41, BS_mouthShrugLower = 42,
    BS_mouthShrugUpper = 43, BS_mouthSmileLeft = 44, BS_mouthSmileRight = 45, BS_mouthStretchLeft = 46,
    BS_mouthStretchRight = 47, BS_mouthUpperUpLeft = 48, BS_mouthUpperUpRight = 49, BS_noseSneerLeft = 50,
    BS_noseSneerRight = 51
};
// ARKit-style names. Weights are geometric estimates from the mesh, not a trained blendshape model.
constexpr const char* kFaceChannelNames[kFaceChannels] = {
    "jawOpen",       "mouthSmileLeft", "mouthSmileRight", "mouthPucker",   "eyeBlinkLeft",
    "eyeBlinkRight", "browDownLeft",   "browDownRight",   "browInnerUp"};

struct FaceFrame {
    double time = 0;   // seconds from the start of the video
    bool valid = false; // false: no face found; the other fields are zero
    float score = 0;   // landmark confidence 0..1
    float pos[3] = {}; // face centre: x,y in 0..1 of the frame, z = frame height / eye distance (bigger = farther)
    float rot[3] = {}; // pitch, yaw, roll in degrees. pitch>0: looking up; yaw>0: head turned to its own left;
                       // roll>0: clockwise on screen
    float raw[kFaceChannels] = {};    // unnormalised measures, larger = more active
    float weight[kFaceChannels] = {}; // raw mapped to 0..1 over the clip's 5th..95th percentile
    float bs[kBlendshapes] = {};      // trained blendshape weights 0..1 (zero when the blendshape model is not installed)
    float lm[kFaceLandmarks * 3] = {}; // x/width, y/height, z/width
};

struct FaceClip {
    float fps = 30;
    uint32_t width = 0, height = 0;
    std::vector<FaceFrame> frames;

    bool Save(const wchar_t* path) const {
        FILE* f = _wfopen(path, L"wb");
        if (!f) return false;
        const char magic[8] = {'T', 'B', 'D', 'F', 'A', 'C', 'E', '2'};
        uint32_t hdr[5] = {(uint32_t)frames.size(), (uint32_t)kFaceChannels, (uint32_t)kFaceLandmarks, width, height};
        bool ok = fwrite(magic, 1, 8, f) == 8 && fwrite(hdr, 4, 5, f) == 5 && fwrite(&fps, 4, 1, f) == 1;
        for (int i = 0; ok && i < kFaceChannels; ++i) {
            char name[32] = {};
            strncpy_s(name, kFaceChannelNames[i], _TRUNCATE);
            ok = fwrite(name, 1, 32, f) == 32;
        }
        for (const FaceFrame& fr : frames) {
            if (!ok) break;
            uint8_t v = fr.valid ? 1 : 0;
            ok = fwrite(&fr.time, 8, 1, f) == 1 && fwrite(&v, 1, 1, f) == 1 && fwrite(&fr.score, 4, 1, f) == 1 &&
                 fwrite(fr.pos, 4, 3, f) == 3 && fwrite(fr.rot, 4, 3, f) == 3 &&
                 fwrite(fr.raw, 4, kFaceChannels, f) == (size_t)kFaceChannels &&
                 fwrite(fr.weight, 4, kFaceChannels, f) == (size_t)kFaceChannels &&
                 fwrite(fr.bs, 4, kBlendshapes, f) == (size_t)kBlendshapes &&
                 fwrite(fr.lm, 4, kFaceLandmarks * 3, f) == (size_t)kFaceLandmarks * 3;
        }
        return fclose(f) == 0 && ok;
    }

    bool Load(const wchar_t* path) {
        FILE* f = _wfopen(path, L"rb");
        if (!f) return false;
        char magic[8];
        uint32_t hdr[5];
        bool ok = fread(magic, 1, 8, f) == 8 && (memcmp(magic, "TBDFACE1", 8) == 0 || memcmp(magic, "TBDFACE2", 8) == 0) &&
                  fread(hdr, 4, 5, f) == 5 &&
                  hdr[1] == kFaceChannels && hdr[2] == kFaceLandmarks && fread(&fps, 4, 1, f) == 1 &&
                  fseek(f, 32 * kFaceChannels, SEEK_CUR) == 0;
        const bool v2 = memcmp(magic, "TBDFACE2", 8) == 0;
        if (ok) {
            width = hdr[3];
            height = hdr[4];
            frames.assign(hdr[0], FaceFrame{});
            for (FaceFrame& fr : frames) {
                uint8_t v = 0;
                ok = fread(&fr.time, 8, 1, f) == 1 && fread(&v, 1, 1, f) == 1 && fread(&fr.score, 4, 1, f) == 1 &&
                     fread(fr.pos, 4, 3, f) == 3 && fread(fr.rot, 4, 3, f) == 3 &&
                     fread(fr.raw, 4, kFaceChannels, f) == (size_t)kFaceChannels &&
                     fread(fr.weight, 4, kFaceChannels, f) == (size_t)kFaceChannels &&
                     (!v2 || fread(fr.bs, 4, kBlendshapes, f) == (size_t)kBlendshapes) &&
                     fread(fr.lm, 4, kFaceLandmarks * 3, f) == (size_t)kFaceLandmarks * 3;
                fr.valid = v != 0;
                if (!ok) break;
            }
        }
        fclose(f);
        if (!ok) frames.clear();
        return ok;
    }
};

} // namespace tbd
