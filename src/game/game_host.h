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
// The app is active again, or an audio-session interruption ended: restart the sound device if the OS stopped it.
void ResumeAudio();

// Pointer input in device pixels. Touch: one finger is the left button; two-finger pan and pinch drive the camera.
enum class Button { Left = 0, Right = 1, Middle = 2 };
void PointerDown(Button b, float x, float y);
void PointerMove(float x, float y);
void PointerUp(Button b, float x, float y);
void PointerCancel(); // the system took the touch (a gesture, a call): drags end
// Zoom in notches at a point (a mouse wheel's unit; pinch converts to it). Positive = closer.
void Zoom(float notches, float x, float y);
// Touch, SimCity style: does a finger landing at (x, y) belong to the GUI or a build tool (true: it is the left button)
// or to the camera (false: one-finger drag pans, a tap selects)?
bool TouchBuilds(float x, float y);
// A finger held in the screen's edge band while it drags keeps the camera moving: the platform calls this each frame with the
// extra drag, in device pixels, the finger would have made (same sign as a finger moving that way). Honours the Edge pan setting.
void EdgePan(float dxPx, float dyPx);
// Does a finger landing at (x, y) hit the GUI (as opposed to a build tool or the city)? Edge pan never runs for a GUI drag.
bool TouchOnGui(float x, float y);
// [safe area] The screen's safe area in device pixels: the strips the Dynamic Island / notch, the rounded corners and the
// home indicator cover (UIView safeAreaInsets times the scale), and the display's corner radius. The GUI keeps out of them.
void SetSafeArea(float left, float top, float right, float bottom, float cornerRadius);
// Turns the camera about its target by `radians` (a two-finger twist).
void Twist(float radians);
// Tilts the camera about its target by `radians`: positive towards straight down, negative towards the horizon (a two-finger
// vertical drag). The city clamps it from just above level (street view) to straight down (src/city_camera.h).
void Tilt(float radians);
// Keys from a hardware keyboard: tbd::platform::Key values; ch is the character typed (0 if none).
void KeyDown(int key, bool repeat, unsigned ch);
void KeyUp(int key);

} // namespace tbd::host
