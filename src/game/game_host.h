#pragma once
// The game without the editor, for platform shells that host only the game (iOS: src/platform/ios/main_ios.mm).
// Plain C++: the shell creates the renderer on its surface and feeds touches, size changes and frames.
#include <memory>
#include <string>

namespace tbd {
class Renderer;
}

namespace tbd::host {

// Before anything else: where the assets are (read-only on iOS), the clock and the worker threads.
void Init(const std::wstring& assetsRoot);
// A renderer the shell created and initialised on its surface; size in device pixels.
void AttachRenderer(std::unique_ptr<Renderer> renderer, int width, int height);
bool HasRenderer();
// The city game on a new random map.
void StartCity();
// The surface changed size (device pixels) or the screen's points-to-pixels scale changed.
void Resize(int width, int height, float scale);
// One frame (simulation, streaming, drawing, present). Call from the display link.
void Frame();
// The app is going away.
void Shutdown();

// Pointer input in device pixels. Touch: one finger is the left button; two-finger pan and pinch drive the camera.
enum class Button { Left = 0, Right = 1, Middle = 2 };
void PointerDown(Button b, float x, float y);
void PointerMove(float x, float y);
void PointerUp(Button b, float x, float y);
void PointerCancel(); // the system took the touch (a gesture, a call): drags end
// Zoom in notches at a point (a mouse wheel's unit; pinch converts to it). Positive = closer.
void Zoom(float notches, float x, float y);
// Keys from a hardware keyboard: tbd::platform::Key values; ch is the character typed (0 if none).
void KeyDown(int key, bool repeat, unsigned ch);
void KeyUp(int key);

} // namespace tbd::host
