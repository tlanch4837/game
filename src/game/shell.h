#pragma once
// What the game asks of the platform shell that hosts it (Windows: main.cpp's window code; iOS: main_ios.mm).
// Portable declarations only. Keep it to calls the game code actually makes.
#include <string>

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

} // namespace tbd::shell
