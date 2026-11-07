// BMP file headers with the exact layout of Windows' BITMAPFILEHEADER / BITMAPINFOHEADER, for the screenshot writers.
#pragma once
#include <cstdint>

namespace tbd {
#pragma pack(push, 2)
struct BmpFileHeader {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1, bfReserved2;
    uint32_t bfOffBits;
};
#pragma pack(pop)
struct BmpInfoHeader {
    uint32_t biSize;
    int32_t biWidth, biHeight;
    uint16_t biPlanes, biBitCount;
    uint32_t biCompression, biSizeImage;
    int32_t biXPelsPerMeter, biYPelsPerMeter;
    uint32_t biClrUsed, biClrImportant;
};
static_assert(sizeof(BmpFileHeader) == 14 && sizeof(BmpInfoHeader) == 40, "BMP header layout");
constexpr uint32_t kBmpRgb = 0; // BI_RGB
} // namespace tbd
