// Face Mocap tool workflow: pick a video, pick where to save the .tbdface clip, run the tracker on a worker
// thread with a progress window.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace tbd {

// Returns immediately; the progress window is modeless and owned by `owner`.
void FaceMocap_Open(HWND owner, HINSTANCE instance);

} // namespace tbd
