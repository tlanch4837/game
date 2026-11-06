// "Face Mocap Live": a browser-style tab next to the viewport. A tab strip ("Scene" | "Face Mocap Live") sits above
// the viewport; the second tab is a page with the webcam picture, the tracked mesh, and the mapping from the tracked
// expressions to a head mesh in the scene (Auto-fitted, or Manual per channel). The target mesh is deformed every
// frame by FaceRig (face_map.h); head rotation still comes from FaceDriver.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "../render/renderer.h"
#include "face_fit.h"
#include "face_live.h"

namespace tbd {

// What the tab needs from the engine; every callback runs on the UI thread.
struct FaceTabHost {
    HINSTANCE instance = nullptr;
    HWND parent = nullptr;
    LiveFace* live = nullptr;
    std::function<void()> relayout;                           // the tab strip appeared / disappeared / switched
    std::function<int()> selectedIndex;                       // selected entity, -1 = none
    std::function<std::string(int)> entityName;               // "" when the index is not an entity
    std::function<bool(int, std::vector<Vertex>*)> entityMesh; // the entity's rest-pose vertices (CPU copy)
    // The entity's own rest vertices and its closed-lid shape (<head>.lid_relaxed.glb), for FaceRig::SetLidRelax; false if none.
    std::function<bool(int, std::vector<Vertex>*, std::vector<Vertex>*)> lidRelaxed;
    std::function<void(int, const std::vector<Vertex>&, const std::vector<std::pair<size_t, size_t>>&)> setMesh; // replaces the entity's drawn mesh; the ranges are where its material parts now sit
    std::function<void(int)> restoreMesh;                     // puts the entity's own mesh back
    std::function<void(int, bool)> driveHead;                 // follow (true) / stop following the live head rotation
    std::function<void(bool)> setMirror;                      // mirror the head rotation
    std::wstring modelDir;                                    // the face models (face_detector.onnx and friends)
    std::function<bool(int, std::vector<FitPart>*)> entityParts; // per-material parts and their color textures
    std::function<void(int, const float*, float)> frameHead;  // point the viewport camera at the entity's face: its middle in mesh space, its width
    std::function<void(bool)> setMaxGraphics;                 // every shader on, editor overlays off
    std::function<bool()> maxGraphicsOn;                      // already on, from --max-graphics
    std::function<void()> stopCapture;                       // the tab was closed: stop the webcam
    std::function<void(const float*, const float*)> overrideHeadPose; // pose + neutral replace the webcam's; nulls = back to the webcam
    std::function<bool()> startVideoLog;                    // debug video of camera and viewport, false = could not start
    std::function<std::wstring()> stopVideoLog;              // returns its folder, "" = none
};

// Opens the tab (or switches to it) and makes it the active one.
void FaceTab_Open(const FaceTabHost& host);
bool FaceTab_IsOpen();
bool FaceTab_PageActive();            // the page (not the viewport) is showing
int FaceTab_StripHeight(int dpi);     // 0 while the tab is closed, so the viewport keeps its full height
// Places the strip above `page` and the page itself; hides the viewport window while the page is showing.
void FaceTab_Layout(const RECT& strip, const RECT& page, HWND viewportWindow);
// While the page shows the live character, the viewport window lives in the page's middle panel: fills `out`
// (parent client coordinates) and returns true. False when the viewport should have the whole area (or be hidden).
bool FaceTab_ViewportRect(const RECT& page, RECT* out);
// Once per frame: poses the target mesh from the newest webcam frame.
void FaceTab_Tick();
// The webcam was stopped from elsewhere; keep the tab but let go of nothing else.
void FaceTab_Close();

} // namespace tbd
