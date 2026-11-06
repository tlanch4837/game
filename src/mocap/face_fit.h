// Finds the 468 face landmarks on a head MESH: renders the mesh from the front with its textures (a small software
// rasteriser), runs the same face tracker the webcam uses on that picture, and lifts every landmark back onto the
// mesh surface. The rig then sits on the real eyes, lips and brows of whatever head it is given.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../render/renderer.h"
#include "face_clip.h"

namespace tbd {

// A run of triangles (a glTF primitive) and its color texture, RGBA8 top-down; empty rgba = untextured.
struct FitPart {
    size_t first = 0, count = 0; // vertex range in the flat list
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool hair = false; // hair cards from the importer: never fitted against, ride the skin in the rig
};

// `lm` receives kFaceLandmarks xyz points in mesh space. The head must face +Z with +Y up. When `debugBmp` is not
// empty the rendered front view (with the landmarks drawn on it) is saved there. Returns false and sets `error`
// when no face is found in the render.
bool FitFaceLandmarks(const std::vector<Vertex>& verts, const std::vector<FitPart>& parts, const wchar_t* modelDir,
                      float* lm, std::string* error, const wchar_t* debugBmp = nullptr);

// Debug: renders the mesh (turned by yawRad about Y) to a 768x768 BMP, with the same software rasteriser the fit uses.
bool RenderHeadView(const std::vector<Vertex>& verts, const std::vector<FitPart>& parts, float yawRad, const wchar_t* bmpPath);
// The same picture in memory, 768x768 BGRA top-down (for the debug video log).
void RenderHeadBgra(const std::vector<Vertex>& verts, const std::vector<FitPart>& parts, float yawRad, std::vector<uint8_t>& bgra);
// A close-up: mesh point (cx, cy) in the picture's centre, `span` mesh units across it.  768x768 BGRA top-down.
void RenderHeadCrop(const std::vector<Vertex>& verts, const std::vector<FitPart>& parts, float yawRad, float cx, float cy, float span, std::vector<uint8_t>& bgra);

} // namespace tbd
