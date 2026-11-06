// Outliner: docked list of the scene's entities with a search box. Click selects, double-click frames the
// entity in the viewport, Up / Down move the selection, Del deletes, right-click opens a small menu. The
// panel owns no scene data; it asks the host through the callbacks below about ten times a second.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace tbd {

enum class OutlinerCommand { Frame, Duplicate, Delete }; // all act on the selected entity

struct OutlinerSource {
    int (*count)(void* user) = nullptr;
    const char* (*name)(int index, void* user) = nullptr; // UTF-8, valid until the scene next changes
    const char* (*kind)(int index, void* user) = nullptr; // short type text shown at the right, e.g. "Cube"
    int (*selected)(void* user) = nullptr;                // entity index, -1 = none
    void (*select)(int index, void* user) = nullptr;      // -1 clears the selection
    void (*command)(OutlinerCommand command, void* user) = nullptr;
    void* user = nullptr;
};

HWND Outliner_Create(HWND parent, HINSTANCE instance, int dpi, const OutlinerSource& source);

} // namespace tbd
