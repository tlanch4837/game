// The terrain sandbox's toolbar, built on the in-game GUI (gui.h): brush mode, size and strength, edit on / off, save and
// load, plus a status chip with the ground under the cursor. It only edits the values it is handed and reports the button
// presses; main.cpp owns the terrain editor state (g_terrainEdit) and the file I/O (SaveTerrainEdits / LoadTerrainEdits).
// No platform headers, so tests/gui_test.cpp drives it headless too.
//
// Placement: the panel hugs the left edge, centred vertically and pushed below the host's top inset (the engine's stats
// text), clear of the orientation compass in the bottom-left corner. The status chip sits bottom-right, lifted above the
// toast chip row (`statusLift`) so a toast never covers it.
#pragma once
#include <cstddef>

#include "gui.h"

namespace tbd {
namespace gui {

enum class TerrainBrushMode { Raise = 0, Dig = 1, Flatten = 2 }; // what a plain left drag does

struct TerrainToolbarState {
    bool* editOn = nullptr;    // g_terrainEdit.on
    int* mode = nullptr;       // TerrainBrushMode as int
    float* radius = nullptr;   // brush radius, metres (4 .. 300, like [ and ])
    float* strength = nullptr; // raise / dig speed, metres per second (1 .. 20)
};

struct TerrainStatus {
    bool cursorOnGround = false;
    float east = 0.0f, north = 0.0f, height = 0.0f; // metres; world x is east, north is -z
    size_t tiles = 0;                               // edited tiles in the height field
};

enum class TerrainAction { None, Save, Load };

constexpr float kTerrainPanelWidth = 296.0f; // unscaled px: room for three segments with an icon and "Flatten"

inline TerrainAction TerrainToolbar(Context& g, const TerrainToolbarState& s, const TerrainStatus& status, float statusLift = 34.0f) {
    TerrainAction action = TerrainAction::None;
    g.BeginPanel("Terrain", Anchor::CenterLeft, 8.0f, 0.0f, kTerrainPanelWidth);
    static const char* const kModes[] = {"Raise", "Dig", "Flatten"};
    static const Icon kModeIcons[] = {Icon::Raise, Icon::Dig, Icon::Flatten};
    if (s.mode) g.Choice("Mode", kModes, 3, s.mode, kModeIcons);
    if (s.radius) g.SliderFloat("Brush size", s.radius, 4.0f, 300.0f, "%.0f m", SliderScale::Log);
    if (s.strength) g.SliderFloat("Strength", s.strength, 1.0f, 20.0f, "%.1f m/s");
    if (s.editOn) g.Toggle(*s.editOn ? "Editing on##edit" : "Editing off##edit", s.editOn);
    g.BeginRow(2);
    if (g.Button("Save", Icon::Save)) action = TerrainAction::Save;
    if (g.Button("Load", Icon::Load)) action = TerrainAction::Load;
    g.EndRow();
    g.EndPanel();

    g.BeginPanel("##terrain status", Anchor::BottomRight, 8.0f, statusLift, 0.0f, 0.0f, 8.0f);
    if (status.cursorOnGround)
        g.Text(TextStyle::Small, "E %.0f m   N %.0f m   Ground %.1f m   Tiles %zu", status.east, status.north, status.height, status.tiles);
    else
        g.Text(TextStyle::Small, "Cursor off the ground   Tiles %zu", status.tiles);
    g.EndPanel();
    return action;
}

} // namespace gui
} // namespace tbd
