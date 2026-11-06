// Top bar of the editor: a slim menu strip (File, Edit, ...) over a toolbar. The toolbar holds the tools
// (the first tool set as buttons, the other sets as drop-downs), centred Play / Pause / Stop controls, and a
// few shortcuts on the right. It replaces the old left and right tool panels; every tool is still reachable.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "tools.h"

namespace tbd {

struct ToolbarHost {
    HMENU menu = nullptr;     // its popups become the menu strip, in order; the toolbar does not own it
    ToolSet* tools = nullptr; // owned by the caller, must outlive the toolbar
    // WM_COMMAND ids sent to the parent window (lparam 0, like a menu choice) by the shortcut buttons.
    UINT cmdSave = 0, cmdFrame = 0, cmdBrowser = 0, cmdPopout = 0;
    void (*beforeMenu)(void* user) = nullptr;           // called right before a menu opens (refresh check marks)
    void (*transport)(int action, void* user) = nullptr; // 0 play, 1 pause, 2 stop
    int (*transportState)(void* user) = nullptr;        // 0 stopped, 1 playing, 2 paused
    bool (*browserShown)(void* user) = nullptr;         // lights the Content button
    bool (*viewportFloating)(void* user) = nullptr;     // lights the Pop Out button
    void* user = nullptr;
};

int Toolbar_Height(int dpi); // wanted height of the whole bar (menu strip plus toolbar)

HWND Toolbar_Create(HWND parent, HINSTANCE instance, int dpi, const ToolbarHost& host);

// Repaints the bar after state changed behind its back (it also re-checks about ten times a second).
void Toolbar_Refresh(HWND toolbar);

} // namespace tbd
