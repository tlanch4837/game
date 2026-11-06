// The vertex of the smooth UI path (Renderer::DrawUi, shaders/ui.hlsl). Platform free: the in-game GUI core (src/gui)
// builds lists of these headless. Every primitive is a quad (two triangles); the pixel shader turns it into an
// anti-aliased shape by its signed distance, so edges are smooth at any size without textures or extra geometry.
//
// Unlike Vertex (where alpha marks materials in the scene shaders), colours here are real straight alpha: the UI
// pipeline blends SrcAlpha / InvSrcAlpha with no depth test.
#pragma once
#include <cstdint>

namespace tbd {

enum class UiMode : uint32_t {
    Rect = 0,   // rounded rectangle: local = pixel offset from the centre, a/b = half size, radius, border width
    Shadow = 1, // soft rounded rectangle: as Rect, softness = blur radius in pixels (border unused)
    Text = 2,   // SDF glyph: local = atlas uv, softness = screen pixels per atlas pixel (sharpness), border = outline (0..0.5)
    Line = 3,   // capsule: local = pixel offset from the start point, a/b = end minus start, radius = half thickness
};

struct UiVertex {
    float x, y;            // position, pixels (origin top-left)
    float localX, localY;  // see UiMode
    float a, b;            // see UiMode
    float radius, border;  // corner radius / half thickness, border width (pixels)
    float mode, softness;  // UiMode as float, shadow blur or text sharpness
    uint32_t fill;         // RGBA8, R in the low byte: the shape's colour (per vertex: gradients interpolate)
    uint32_t line;         // RGBA8: border colour (Rect), outline colour (Text)
};
static_assert(sizeof(UiVertex) == 48, "ui.hlsl and both backends hard-code this layout");

// RGBA8 with R in the low byte (DXGI_FORMAT_R8G8B8A8_UNORM / VK_FORMAT_R8G8B8A8_UNORM).
constexpr uint32_t UiRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

} // namespace tbd
