// Content browser: a docked panel for the project's Assets folder. Folder tree on the left, a grid of
// asset tiles on the right, breadcrumb navigation, search, rename / delete / new folder, and
// drag-and-drop import from Explorer. Everything is real files on disk; the panel only watches them.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

namespace tbd {

struct ContentBrowserCallbacks {
    // Double-click or Enter on a file (not a folder). `path` is absolute.
    void (*openAsset)(const std::wstring& path, void* user) = nullptr;
    void* user = nullptr;
};

int ContentBrowser_DefaultHeight(int dpi);
int ContentBrowser_MinHeight(int dpi);

// `root` is the folder the browser is confined to (created if missing).
HWND ContentBrowser_Create(HWND parent, HINSTANCE instance, int dpi, const std::wstring& root,
                           const ContentBrowserCallbacks& callbacks);

// Folder currently shown (absolute path), so "save here" style commands can use it.
std::wstring ContentBrowser_CurrentFolder(HWND browser);

// Rescans the current folder and the folder tree.
void ContentBrowser_Refresh(HWND browser);

} // namespace tbd
