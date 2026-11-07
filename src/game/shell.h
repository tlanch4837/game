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

} // namespace tbd::shell
