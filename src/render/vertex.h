// The one vertex layout every backend draws. Split out of renderer.h (which includes it) so code that only
// builds vertex lists, like the in-game GUI (src/gui) and ui_draw.h, needs no platform headers and compiles
// headless (tests) and, later, on iOS.
#pragma once

namespace tbd {

struct Vertex {
    float px, py, pz;
    float nx, ny, nz; // normal; all zero means "unlit, draw the color as is"
    float r, g, b, a;
    float u = 0.0f, v = 0.0f; // texture coordinates; trailing and defaulted so 10-value initializers still work
};
static_assert(sizeof(Vertex) == 48, "backends hard-code this layout");

} // namespace tbd
