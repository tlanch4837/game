// Window for a running LiveFace: the (mirrored) camera picture with the tracked mesh, head pose and channel bars.
// R starts/stops recording a clip (asks where to save it when stopped), C re-measures the neutral pose, Esc closes
// the window (capture keeps running until it is stopped elsewhere).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "face_live.h"

namespace tbd {

// Opens the window, or brings the existing one to the front. `live` must outlive it; the window closes
// itself when `live` stops running.
void FaceLiveWindow_Open(HWND owner, HINSTANCE instance, LiveFace* live);

// Draws the current state at width x height into a BMP, with no window. For tests.
bool FaceLiveWindow_RenderToBmp(LiveFace& live, int width, int height, const wchar_t* bmpPath);

} // namespace tbd
