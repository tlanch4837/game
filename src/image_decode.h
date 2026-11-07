// Decodes PNG / JPEG from memory into top-down RGBA8 (straight alpha).
// Windows: Windows Imaging Component. Apple: ImageIO. Both live in src/platform.
#pragma once
#include "platform/platform.h"

#if defined(_WIN32)
// Windows-only callers (src/character/creator.cpp) use WIC directly and have always got it through this header.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

#include <cstdint>
#include <vector>

namespace tbd {

inline bool DecodeImage(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height) {
    return platform::DecodeImage(data, size, rgba, width, height);
}

} // namespace tbd
