// Details panel (the inspector): Location / Rotation / Scale fields for the selected entity, under a
// collapsible Transform section. The values follow the entity live (for example while a gizmo drags it);
// typing a number and pressing Enter (or leaving the field) applies it. Esc reverts the field.
// Below it, a Scripts section lists the Luau scripts on the entity (src/script) with an edit field for each declared prop.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>
#include <vector>

namespace tbd {

// Fields 0-2 position, 3-5 rotation in degrees (x = pitch, y = yaw, z = roll), 6-8 scale.
struct InspectorEdit {
    bool set[9] = {};
    float value[9] = {};
};

using InspectorApply = void (*)(const InspectorEdit& edit, void* user);

int Inspector_Height(int dpi); // height the panel needs to show the Transform section (its width follows the dock)

HWND Inspector_Create(HWND parent, HINSTANCE instance, int dpi, InspectorApply apply, void* user);

// Call every frame. `name` null means nothing is selected (fields are disabled).
void Inspector_Show(HWND inspector, const char* name, const float values[9]);

// ---- scripts
struct InspectorScriptProp {
    std::string name, type; // type: number | bool | string
    std::string value;      // what the entity uses now (its override, else the default)
    bool overridden = false;
};
struct InspectorScript {
    std::string path;       // relative to the project's Scripts folder
    bool missing = false;   // the file could not be loaded
    std::vector<InspectorScriptProp> props;
};
struct InspectorScriptEdit {
    enum Kind { Add, Remove, SetProp } kind = Add; // Add: the user pressed + (the host shows a menu of scripts)
    int script = 0;                                // index into the entity's scripts
    std::string prop, value;                       // SetProp: the new text for that prop
};
using InspectorScriptApply = void (*)(const InspectorScriptEdit& edit, void* user);

void Inspector_SetScriptHandler(HWND inspector, InspectorScriptApply apply, void* user);
// Call every frame after Inspector_Show. hasEntity false disables the section.
void Inspector_ShowScripts(HWND inspector, bool hasEntity, const std::vector<InspectorScript>& scripts);
// Pixel height that shows every script row (at least Inspector_Height). The host grows the panel to it.
int Inspector_DesiredHeight(HWND inspector);

} // namespace tbd
