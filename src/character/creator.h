// Character Creator workspace (top-level "Character Creator" menu, F7 toggles): the place to create, modify and see a character.
// A panel docked on the left of the viewport (scrolls, full viewport height) with three big tabs:
//   CLOTHES: the Clothes Maker (clothes_maker.h). A button-up shirt built from a recipe (fit, collar, undone buttons, pockets,
//         rolled sleeves, tuck into the scene's jeans, folds, pinstripe fabric, wear) and fitted to body_torso.glb on a worker
//         thread; written to Assets\clothes\<name>\ and put on the scene's "Shirt" entity (Restore puts the old file back).
//         Save / Load at the top read and write the recipe. TBD_CHARTAB=clothes, TBD_CLOTHBUILD=1|<recipe.json> build headless.
//   BODY: body_maker's 17 sliders (out\body_morphs_{torso,legs,head}.json, one values vector for all parts, the limiter's
//         displacement cap over all parts together; the head part, the neck, is added to the face deltas) plus a uniform
//         height scale (entity scale); Randomize / Reset. Says "body morphs not built yet" until the files exist (polled).
//         Adds the torso and legs next to the head if the scene has only the head. Camera framed on the whole body.
//   HEAD also has HAIR painting (hair_paint.h): density + flow maps over the head's UV atlas, Paint / Erase / Comb / Smooth
//         on the 3D head, Fill defaults, colour; baked into a copy of the skin albedo (TBD_CHARHAIRTEST=0|1 scripted test).
//   HEAD: the face maker's 30 identity sliders (morphs.json), Randomize (seeded, limiter on) and Age; Skin presets from
//         gameenginetools\skin_tex\out (points head / torso / legs at that preset's GLBs and reloads them; values shown read
//         only); Hair grooms, Mouth, Lips and Wrinkles are listed but marked NOT WIRED. Camera framed on the head.
// Top row: New, Save, Load (preset JSON: character_preset.schema.json plus a "body" block), Reset (the current tab).
// Meshes go back through Host::setMesh (main.cpp ReplaceEntityVertices: the same CreateMesh path on Direct3D 12 and Vulkan).
// Headless: TBD_CHARPRESET=<preset.json>, TBD_CHARSEED=<n>, TBD_CHARTAB=body|head (opens the workspace once the scene has
// loaded), TBD_MORPHS / TBD_BODYMORPHS=<file> override the data files. "Character:" lines in engine.log carry the timings.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "../mesh_import.h"

namespace tbd::character {

constexpr UINT kMenuCommand = 0x7C10; // toggle the workspace (F7)
constexpr UINT kMenuBody = 0x7C11, kMenuHead = 0x7C12;

// What the workspace may do to the scene; every callback runs on the UI thread, between frames.
struct Host {
    HINSTANCE instance = nullptr;
    HWND parent = nullptr;
    std::function<void()> relayout;
    std::function<int()> entityCount, selectedIndex;
    std::function<std::string(int)> entityName, entityPath; // path "" when not an imported mesh
    std::function<bool(int, std::vector<Vertex>*)> entityMesh; // rest vertices (EntityVertices)
    std::function<const ImportedModel*(int)> entityModel;
    std::function<void(int, const std::vector<Vertex>&)> setMesh; // replaces the drawn mesh, same layout as the rest list
    std::function<void(int)> restoreMesh;                       // the entity's own mesh back
    std::function<void(int, const std::string&)> setEntityPath; // another file for the entity (a skin preset's GLB), reloaded
    std::function<void(int)> reloadEntity;                      // the entity's own file imported again (a hair style changed on disk)
    std::function<int(int, const std::string&, const std::string&)> addEntity; // copy of entity i (-1: a new one at the origin) with a name and file; its index
    std::function<void(int, const float*, float*)> toWorld;     // entity i's mesh-space point to world
    std::function<void(int, float)> setEntityScale;             // uniform
    std::function<void(const float*, float)> frame;             // camera on a world point from the front (+Z), at a distance
    std::function<bool()> sceneReady;
    std::function<bool()> viewportFloating; // the 3D viewport is popped out: the 2D view takes all of the docked space
    // hair painting
    std::function<bool(int, int, int, float*, float*)> entityRay; // viewport pixel -> ray (origin, direction) in entity i's mesh space
    std::function<TextureId(int)> skinAlbedo;                    // entity i's skin albedo texture as drawn now (kNoTexture: none)
    std::function<void(int, TextureId, TextureId)> swapAlbedo;   // entity i's parts drawn with albedo `from` use `to` instead
    std::function<TextureId(const uint8_t*, int, int)> createTexture;
    std::function<void(TextureId)> destroyTexture;
    // skin paint (tint, spots, shine) and the eye look
    std::function<TextureId(int, size_t)> partAlbedo;                // entity i's drawn albedo of the part that starts at vertex `first` (the eyes)
    std::function<bool(int, std::vector<uint8_t>*, int*, int*)> skinSurfaceImage; // entity i's skin surface map decoded (RGBA, w, h)
    std::function<TextureId(int)> skinSurface;                       // the skin surface texture as drawn now (kNoTexture: none)
    std::function<bool(int, std::vector<float>*)> faceLandmarks;      // entity i's 468 face landmarks on its rest mesh (the face tracker run on a render of it)
    std::function<void(int, TextureId, TextureId)> swapSurface;      // entity i's skin parts drawn with surface `from` use `to` instead
};

void SetHost(const Host& h);
bool HostSet();
// While the workspace is open the editor camera moves slower (close-up work on a head): this scale for orbit, pan, zoom, fly.
float CameraScale();
void AddMenu(HMENU bar);     // the top-level "Character Creator" popup
void Command(UINT id);       // kMenuCommand / kMenuBody / kMenuHead
void Toggle();
// Layout: while the workspace is open it takes the left of `viewport` (shrunk in place). PlaceDock moves it there.
void Dock(RECT& viewport, int dpi);
void PlaceDock();
// The viewport's mouse while a hair tool is on (HEAD tab): true = used (a paint stroke), so the editor ignores it.
bool ViewportMouse(HWND viewport, UINT msg, int x, int y);
// FaceRig hook: when entity `i` is the workspace's head with a face set, `v` (its rest vertices) becomes the morphed head.
void MorphEntityVertices(int i, std::vector<Vertex>& v);
// The hair brush on the head while a tool is on and the mouse is over it: world-space line pairs (outer ring = radius, inner rings
// fade with the falloff, so strength and hardness show). False when there is nothing to draw.
bool BrushLines(std::vector<Vertex>& out);
// The Hair Maker's clay (lit triangles) and comb lines, world space, while it is loaded on the HEAD tab. False: nothing to draw.
bool ClayDraw(std::vector<Vertex>& tris, std::vector<Vertex>& lines);
// True while the clay is being worked with the hair hidden: the editor skips the head's hair parts.
bool HideHeadHair();
void Tick(); // once per frame, before the frame opens
// True while a Clothes Maker build is running or waiting to land (a TBD_CAPTURE waits for it).
bool Busy();
void Shutdown();

} // namespace tbd::character
