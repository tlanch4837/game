#pragma once
// What the game asks of the platform shell that hosts it (Windows: main.cpp's window code; iOS: main_ios.mm).
// Portable declarations only. Keep it to calls the game code actually makes.
#include <string>
#include <vector>

#include "../render/vertex.h"

namespace tbd { class Renderer; }

namespace tbd::shell {

// A modal warning the player must acknowledge (Windows: MessageBox with the window title and a warning icon).
void Alert(const std::wstring& text);

// Whether this app is the one in front (Windows: the foreground window belongs to this process). Keyboard camera
// controls ignore keys while another app has focus.
bool AppHasFocus();

// The window / app title (Windows: the main window's caption).
void SetTitle(const std::wstring& title);

// Editor panels that mirror game state (Windows only; no-ops elsewhere): a scene was saved (the content browser
// lists it), the active tool changed (the toolbar shows it).
void OnSceneSaved();
void OnToolChanged();

// Scene file dialog: false when cancelled. `path` in: the suggestion, out: the choice.
bool PickSceneFile(bool save, std::wstring& path);

// Play / Stop (0 play, 2 stop): the shell owns the pointer while playing.
void Transport(int action);

// Per-frame hooks. DevReloadTick: development reloads (shaders), false when no renderer is left this frame.
// PumpRemote: external control requests. EditorTick: editor panels' per-frame work. RefreshInspector: after the
// scene is drawn. CaptureThisFrame / AfterCapture: a screenshot request for this frame. AfterFrame: after EndFrame.
bool DevReloadTick();
void PumpRemote();
void EditorTick(float dt);
void RefreshInspector();
bool CaptureThisFrame();
void AfterCapture(Renderer& r);
void AfterFrame(Renderer& r);

// The hot-reloaded gameplay code (Windows: bin\game.dll); dt is 0 outside Play.
void GameplayTick(float dt, bool playing);
// Leave the app (Windows: PostQuitMessage). Used when a capture run is finished.
void Quit();

// Editor panels that work in the 3D view (Windows: the Character Creator; elsewhere they are absent).
float EditorCameraScale(); // camera speed factor: below 1 while a panel wants fine moves
bool EditorBusy();         // a panel is still working: captures wait for it
bool EditorHidesHeadHair();
bool EditorBrushLines(std::vector<Vertex>& lines);
bool EditorClayDraw(std::vector<Vertex>& tris, std::vector<Vertex>& lines);

} // namespace tbd::shell
