// Face clip preview: the source video with the tracked mesh drawn over it, live head pose and channel
// values, and a scrubbable timeline of every channel. Space plays/pauses, arrows step, Home rewinds.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace tbd {

// Modeless window owned by `owner`. `videoPath` may be null: the video is then looked up next to the clip
// (same name, common extensions) and asked for if missing.
void FaceMocapPreview_Open(HWND owner, HINSTANCE instance, const wchar_t* clipPath, const wchar_t* videoPath);

// Lets the user pick a .tbdface file, then opens the preview.
void FaceMocapPreview_Pick(HWND owner, HINSTANCE instance);

// Draws frame `frameIndex` of the preview at width x height into a BMP file, with no window. For tests.
bool FaceMocapPreview_RenderToBmp(const wchar_t* clipPath, const wchar_t* videoPath, int frameIndex, int width,
                                  int height, const wchar_t* bmpPath);

} // namespace tbd
