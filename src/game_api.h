#pragma once
// The gameplay DLL's interface (bin\game.dll, built from game\ by build_game.bat). Plain C types only: the DLL is swapped
// while the engine runs (hot_reload.h), so nothing here may own memory across the boundary. The game keeps its state in
// the block the engine hands it in onLoad, which survives every reload; a function pointer or a pointer to a string
// literal stored there goes stale when the DLL is swapped, so keep the state plain data.
#include <stddef.h>
#include <stdint.h>

#define TBD_GAME_API_VERSION 1u

struct TbdGameHost {
    uint32_t version; // TBD_GAME_API_VERSION
    void (*log)(const char* text);
    // Scene entities, by index (0 .. entityCount-1). Moves made during Play are undone when Play stops.
    int (*entityCount)(void);
    int (*findEntity)(const char* name); // -1 when there is none
    const char* (*entityName)(int index);
    void (*getTransform)(int index, float position[3], float rotation[4], float scale[3]); // rotation is x, y, z, w
    void (*setTransform)(int index, const float position[3], const float rotation[4], const float scale[3]);
    int (*keyDown)(int virtualKey);          // a Windows VK_ code or 'A'..'Z'
    int (*playerFeet)(float position[3]);    // 0 while there is no walking player
    float (*getHour)(void);                  // the sun's hour, 0..24
    void (*setHour)(float hour);
    void (*playSound)(const char* name, const float position[3], float volume, float pitch);
};

struct TbdGameApi {
    uint32_t version; // TBD_GAME_API_VERSION
    // reloaded is 1 when this DLL replaces an earlier one and memory already holds that one's state.
    void (*onLoad)(const TbdGameHost* host, void* memory, size_t memorySize, int reloaded);
    void (*onPlay)(int playing);   // 1 when Play starts, 0 when it stops
    void (*update)(float dt, int playing);
    void (*onUnload)(void);        // before the DLL is swapped out or the engine closes
};

typedef const TbdGameApi* (*TbdGameGetApiFn)(uint32_t hostVersion);
#define TBD_GAME_ENTRY "TbdGameGetApi"
