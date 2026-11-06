// Hot-reloaded gameplay (bin\game.dll). Save this file while the engine runs: it rebuilds the DLL in the background and
// swaps it in, keeping State. See docs/hot_reload.md.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_api.h"

namespace {

// Everything that must survive a reload lives here, in the engine's memory block. Change the layout and bump kLayout:
// the old contents are then cleared instead of being read as the new layout.
constexpr uint32_t kLayout = 2;
constexpr int kMaxDoors = 256;
constexpr float kDoorWidth = 0.9f, kLeafHalfThickness = 0.03f, kBodyRadius = 0.32f; // meters
constexpr float kMaxOpen = 1.75f;                 // radians either way
constexpr float kSpring = 3.0f, kDamping = 0.45f; // about a 3.6 s swing that dies away over several sways

// A swinging door: an entity under "Doors/", hinged at its origin, its leaf running 0.9 m along its own +X.
struct Door {
    int entity;
    float home[4];        // rotation when Play started, x y z w
    float position[3];    // the hinge
    float angle, speed;   // radians about the hinge from home, radians/s
};

struct State {
    uint32_t layout;
    float time;      // seconds of Play
    int spinner;     // the entity named "Spinner", or -1
    float spinnerHome[3];
    int doorCount;
    Door doors[kMaxDoors];
};

const TbdGameHost* host;
State* state;

void OnLoad(const TbdGameHost* h, void* memory, size_t size, int reloaded) {
    host = h;
    state = (State*)memory;
    if (size < sizeof(State)) { host->log("game.dll: the state block is too small"); state = nullptr; return; }
    if (!reloaded || state->layout != kLayout) {
        memset(state, 0, sizeof(State));
        state->layout = kLayout;
        state->spinner = -1;
    }
    host->log(reloaded ? "game.dll: reloaded, state kept" : "game.dll: loaded");
}

void OnPlay(int playing) {
    if (!state) return;
    state->time = 0;
    state->spinner = playing ? host->findEntity("Spinner") : -1;
    if (state->spinner >= 0) {
        float rot[4], scale[3];
        host->getTransform(state->spinner, state->spinnerHome, rot, scale);
    }
    state->doorCount = 0;
    if (!playing) return; // the engine puts every moved entity back at Stop
    const int count = host->entityCount();
    for (int i = 0; i < count && state->doorCount < kMaxDoors; ++i) {
        const char* name = host->entityName(i);
        if (!name || strncmp(name, "Doors/", 6) != 0) continue;
        Door& d = state->doors[state->doorCount++];
        float scale[3];
        host->getTransform(i, d.position, d.home, scale);
        d.entity = i;
        d.angle = d.speed = 0.0f;
    }
    char line[64];
    snprintf(line, sizeof(line), "game.dll: %d swinging doors", state->doorCount);
    host->log(line);
}

// q * (about y by angle)
void TurnAboutY(const float q[4], float angle, float out[4]) {
    const float s = sinf(angle * 0.5f), c = cosf(angle * 0.5f);
    out[0] = q[0] * c - q[2] * s;
    out[1] = q[1] * c + q[3] * s;
    out[2] = q[2] * c + q[0] * s;
    out[3] = q[3] * c - q[1] * s;
}

// Where the leaf points, flat: the rotation applied to +X, x and z only.
void LeafDirection(const float q[4], float& x, float& z) {
    x = 1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]);
    z = 2.0f * (q[0] * q[2] - q[3] * q[1]);
    const float len = sqrtf(x * x + z * z);
    if (len > 1e-5f) { x /= len; z /= len; }
}

// How far the player's feet are from the leaf at this angle, and how far along it the closest point is.
float LeafDistance(const Door& d, float angle, const float feet[3], float& along) {
    float q[4], dx, dz;
    TurnAboutY(d.home, angle, q);
    LeafDirection(q, dx, dz);
    const float vx = feet[0] - d.position[0], vz = feet[2] - d.position[2];
    along = vx * dx + vz * dz;
    const float t = along < 0.0f ? 0.0f : along > kDoorWidth ? kDoorWidth : along;
    const float ex = vx - dx * t, ez = vz - dz * t;
    return sqrtf(ex * ex + ez * ez);
}

void UpdateDoors(float dt) {
    float feet[3];
    const bool player = host->playerFeet(feet) != 0;
    for (int k = 0; k < state->doorCount; ++k) {
        Door& d = state->doors[k];
        bool pushed = false;
        if (player && feet[1] > d.position[1] - 0.6f && feet[1] < d.position[1] + 2.0f) {
            float along;
            const float gap = LeafDistance(d, d.angle, feet, along);
            const float reach = kBodyRadius + kLeafHalfThickness;
            if (gap < reach && along > -0.1f && along < kDoorWidth + 0.1f) {
                // He walks into it: the leaf turns away from him just enough to clear him, and keeps the swing that took.
                float ignored;
                const float sign = LeafDistance(d, d.angle + 0.02f, feet, ignored) > gap ? 1.0f : -1.0f;
                const float lever = along < 0.25f ? 0.25f : along;
                const float turn = sign * (reach - gap) / lever;
                d.angle += turn;
                const float push = turn / (dt > 1e-4f ? dt : 1e-4f);
                d.speed += (push - d.speed) * 0.5f;
                pushed = true;
                if (getenv("TBD_DOORLOG")) { char line[96]; snprintf(line, sizeof(line), "door %d pushed: angle %.2f", k, d.angle); host->log(line); }
            }
        }
        if (!pushed) { // a soft spring back to closed, under-damped so it sways past and settles
            d.speed += (-kSpring * d.angle - kDamping * d.speed) * dt;
            d.angle += d.speed * dt;
        }
        if (d.angle > kMaxOpen || d.angle < -kMaxOpen) { // the stop: it knocks back a little
            d.angle = d.angle > 0.0f ? kMaxOpen : -kMaxOpen;
            d.speed *= -0.3f;
        }
        float rot[4], pos[3], old[4], scale[3];
        TurnAboutY(d.home, d.angle, rot);
        host->getTransform(d.entity, pos, old, scale);
        host->setTransform(d.entity, d.position, rot, scale);
    }
}

// Example: during Play an entity named "Spinner" turns and bobs. Edit the numbers and save to see it change live.
void Update(float dt, int playing) {
    if (!playing || !state) return;
    state->time += dt;
    UpdateDoors(dt);
    if (state->spinner < 0) return;
    const float angle = state->time * 1.5f, bob = sinf(state->time * 2.0f) * 0.25f;
    const float pos[3] = {state->spinnerHome[0], state->spinnerHome[1] + bob, state->spinnerHome[2]};
    const float rot[4] = {0, sinf(angle * 0.5f), 0, cosf(angle * 0.5f)};
    float oldPos[3], oldRot[4], scale[3];
    host->getTransform(state->spinner, oldPos, oldRot, scale);
    host->setTransform(state->spinner, pos, rot, scale);
}

void OnUnload() {}

const TbdGameApi kApi = {TBD_GAME_API_VERSION, OnLoad, OnPlay, Update, OnUnload};

} // namespace

extern "C" __declspec(dllexport) const TbdGameApi* TbdGameGetApi(uint32_t hostVersion) {
    return hostVersion == TBD_GAME_API_VERSION ? &kApi : nullptr;
}
